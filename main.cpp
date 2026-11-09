#include <QtWidgets>
#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

struct VideoInfo
{
    QString path;
    double duration = 0.0;
    double fps = 30.0;
    int width = 0;
    int height = 0;
    int displayWidth = 0;
    int displayHeight = 0;
    int rotation = 0;
    int totalFrames = 0;
    bool hasAudio = false;

    bool valid() const { return !path.isEmpty() && duration > 0.0 && width > 0 && height > 0; }
};

struct EyeControls
{
    QLabel *fileName = nullptr;
    QLabel *details = nullptr;
    QPushButton *open = nullptr;
    QPushButton *previous = nullptr;
    QPushButton *next = nullptr;
    QSlider *slider = nullptr;
    QSpinBox *frame = nullptr;
    QSpinBox *x = nullptr;
    QSpinBox *y = nullptr;
    QDoubleSpinBox *zoom = nullptr;
};

struct AlignmentEstimate
{
    double scale = 1.0;
    double x = 0.0;
    double y = 0.0;
    double rotationDegrees = 0.0;
    int matches = 0;
    int inliers = 0;
    bool valid = false;
};

struct ColorCorrection
{
    double gain[3] {1.0, 1.0, 1.0};
    double offset[3] {0.0, 0.0, 0.0};

    bool isIdentity() const
    {
        for (int channel = 0; channel < 3; ++channel)
            if (qAbs(gain[channel] - 1.0) > 0.0001 || qAbs(offset[channel]) > 0.0001) return false;
        return true;
    }
};

struct CenteredAdjustment
{
    int stereoWindow = 0;
    int vertical = 0;
    double zoomBalance = 0.0;
};

struct FrameSegment
{
    QString name;
    int leftStart = 0;
    int rightStart = 0;
    int leftEnd = 0;
    int rightEnd = 0;
    CenteredAdjustment adjustment;
};

class RangeTimelineSlider final : public QSlider
{
public:
    struct Span { int first = 0; int last = 0; QColor color; };

    explicit RangeTimelineSlider(QWidget *parent = nullptr) : QSlider(Qt::Horizontal, parent)
    {
        setMinimumHeight(30);
        setToolTip("Shared synchronized timeline. Colored bands are project ranges.");
    }

    void setSpans(const QVector<Span> &value) { spans = value; update(); }
    void setPendingStart(int value) { pendingStart = value; update(); }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QSlider::paintEvent(event);
        QStyleOptionSlider option;
        initStyleOption(&option);
        QRect groove = style()->subControlRect(QStyle::CC_Slider, &option,
                                               QStyle::SC_SliderGroove, this);
        if (maximum() <= minimum() || groove.width() <= 0) return;
        auto xForValue = [&](int value) {
            const double ratio = (qBound(minimum(), value, maximum()) - minimum()) /
                                 static_cast<double>(maximum() - minimum());
            return groove.left() + qRound(ratio * groove.width());
        };
        QPainter painter(this);
        painter.setPen(Qt::NoPen);
        for (const Span &span : spans) {
            const int x1 = xForValue(span.first);
            const int x2 = xForValue(span.last);
            QColor fill = span.color;
            fill.setAlpha(210);
            painter.setBrush(fill);
            painter.drawRoundedRect(QRect(qMin(x1, x2), height() - 7,
                                          qMax(3, qAbs(x2 - x1)), 6), 2, 2);
        }
        if (pendingStart >= minimum()) {
            const int x = xForValue(pendingStart);
            painter.setPen(QPen(QColor(255, 190, 30), 2));
            painter.drawLine(x, 2, x, height() - 2);
        }
    }

private:
    QVector<Span> spans;
    int pendingStart = -1;
};

class StereoAlignerWindow final : public QMainWindow
{
public:
    StereoAlignerWindow()
    {
        ffmpegPath = QStandardPaths::findExecutable("ffmpeg");
        if (ffmpegPath.isEmpty() && QFileInfo::exists("D:/stereo/ffmpeg.exe"))
            ffmpegPath = "D:/stereo/ffmpeg.exe";

        previewTimer.setSingleShot(true);
        previewTimer.setInterval(120);
        connect(&previewTimer, &QTimer::timeout, this, [this] { updatePreview(); });

        buildUi();
        connectUi();
        QSettings settings;
        const int savedTheme = qBound(0, settings.value("appearance/theme", 0).toInt(), 1);
        themeSelector->setCurrentIndex(savedTheme);
        applyTheme(savedTheme == 1);
        setWindowTitle("Stereo Aligner");
        resize(1500, 940);
        setMinimumSize(1050, 720);
        setAcceptDrops(true);
        updateReadyState();
        QSettings windowSettings;
        restoreGeometry(windowSettings.value("window/geometry").toByteArray());
        restoreState(windowSettings.value("window/state").toByteArray(), 1);

        const QStringList arguments = QCoreApplication::arguments();
        if (arguments.size() > 1) {
            QTimer::singleShot(0, this, [this, arguments] {
                loadVideo(true, arguments.at(1));
                if (arguments.size() > 2) loadVideo(false, arguments.at(2));
            });
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const bool preview = watched == leftPreview || watched == rightPreview || watched == singlePreview;
        if (!preview) return QMainWindow::eventFilter(watched, event);
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton || !leftInfo.valid() || !rightInfo.valid())
                return QMainWindow::eventFilter(watched, event);
            previewDragActive = true;
            previewDragTarget = watched;
            previewDragStart = mouse->position();
            previewDragAdjustment = currentAdjustment();
            static_cast<QWidget*>(watched)->setCursor(Qt::ClosedHandCursor);
            mouse->accept();
            return true;
        }
        if (event->type() == QEvent::MouseMove && previewDragActive && watched == previewDragTarget) {
            auto *mouse = static_cast<QMouseEvent*>(event);
            const QPointF delta = mouse->position() - previewDragStart;
            CenteredAdjustment adjustment = previewDragAdjustment;
            const int eyeDirection = watched == leftPreview ? -1 : 1;
            const int step = pixelAdjustmentStep();
            const double rawX = previewDragAdjustment.stereoWindow + eyeDirection * delta.x() * previewPixelsPerDisplayX;
            const double rawY = previewDragAdjustment.vertical + eyeDirection * delta.y() * previewPixelsPerDisplayY;
            adjustment.stereoWindow = qBound(-2000, qRound(rawX / step) * step, 2000);
            adjustment.vertical = qBound(-2000, qRound(rawY / step) * step, 2000);
            setCurrentAdjustment(adjustment);
            mouse->accept();
            return true;
        }
        if (event->type() == QEvent::Wheel && leftInfo.valid() && rightInfo.valid()) {
            auto *wheel = static_cast<QWheelEvent*>(event);
            const int wheelDelta = wheel->angleDelta().y();
            if (wheelDelta == 0) return true;
            const int direction = wheelDelta > 0 ? 1 : -1;
            const int eyeDirection = watched == leftPreview ? -1 : 1;
            CenteredAdjustment adjustment = currentAdjustment();
            if (wheel->modifiers().testFlag(Qt::ControlModifier)) {
                adjustment.stereoWindow = qBound(-2000,
                    adjustment.stereoWindow + eyeDirection * direction * pixelAdjustmentStep(), 2000);
                status->setText("Preview horizontal step applied");
            } else if (wheel->modifiers().testFlag(Qt::ShiftModifier)) {
                adjustment.vertical = qBound(-2000,
                    adjustment.vertical - eyeDirection * direction * pixelAdjustmentStep(), 2000);
                status->setText("Preview vertical step applied");
            } else {
                adjustment.zoomBalance = qBound(-75.0,
                    adjustment.zoomBalance + eyeDirection * direction * zoomAdjustmentStep(), 75.0);
                status->setText("Preview wheel zoom applied");
            }
            setCurrentAdjustment(adjustment);
            wheel->accept();
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && previewDragActive && watched == previewDragTarget) {
            previewDragActive = false;
            previewDragTarget = nullptr;
            static_cast<QWidget*>(watched)->setCursor(Qt::OpenHandCursor);
            status->setText("Interactive adjustment applied");
            event->accept();
            return true;
        }
        return QMainWindow::eventFilter(watched, event);
    }

    void closeEvent(QCloseEvent *event) override
    {
        QSettings settings;
        settings.setValue("window/geometry", saveGeometry());
        settings.setValue("window/state", saveState(1));
        QMainWindow::closeEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QMainWindow::resizeEvent(event);
        requestPreview();
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls())
            event->acceptProposedAction();
    }

    void dropEvent(QDropEvent *event) override
    {
        const auto urls = event->mimeData()->urls();
        if (urls.isEmpty()) return;
        if (!leftInfo.valid()) loadVideo(true, urls.first().toLocalFile());
        else loadVideo(false, urls.first().toLocalFile());
    }

private:
    VideoInfo leftInfo;
    VideoInfo rightInfo;
    EyeControls leftUi;
    EyeControls rightUi;
    ColorCorrection leftColor;
    ColorCorrection rightColor;
    QString projectPath;
    QString ffmpegPath;

    QLabel *leftPreview = nullptr;
    QLabel *rightPreview = nullptr;
    QLabel *singlePreview = nullptr;
    QStackedWidget *previewStack = nullptr;
    QComboBox *previewMode = nullptr;
    QComboBox *themeSelector = nullptr;
    QComboBox *outputMode = nullptr;
    QComboBox *audioSource = nullptr;
    QPushButton *swapOutputEyes = nullptr;
    QCheckBox *linkStepping = nullptr;
    QPushButton *newProjectButton = nullptr;
    QPushButton *openProjectButton = nullptr;
    QPushButton *saveProjectButton = nullptr;
    QPushButton *resetAlignment = nullptr;
    QPushButton *autoAlignButton = nullptr;
    QSlider *stereoWindowSlider = nullptr;
    QSlider *verticalSlider = nullptr;
    QSlider *zoomBalanceSlider = nullptr;
    QSpinBox *stereoWindowSpin = nullptr;
    QSpinBox *verticalSpin = nullptr;
    QDoubleSpinBox *zoomBalanceSpin = nullptr;
    QLabel *stereoWindowValue = nullptr;
    QLabel *verticalValue = nullptr;
    QLabel *zoomBalanceValue = nullptr;
    QTableWidget *segmentTable = nullptr;
    RangeTimelineSlider *commonTimeline = nullptr;
    QPushButton *timelineMarkStartButton = nullptr;
    QPushButton *timelineAddEndButton = nullptr;
    QLabel *commonTimelinePosition = nullptr;
    QPushButton *markSegmentStartButton = nullptr;
    QPushButton *addSegmentButton = nullptr;
    QPushButton *updateSegmentButton = nullptr;
    QPushButton *removeSegmentButton = nullptr;
    QButtonGroup *adjustmentStepGroup = nullptr;
    QCheckBox *showGrid = nullptr;
    QSpinBox *cropLeft = nullptr;
    QSpinBox *cropRight = nullptr;
    QSpinBox *cropTop = nullptr;
    QSpinBox *cropBottom = nullptr;
    QPushButton *autoCropButton = nullptr;
    QPushButton *resetCropButton = nullptr;
    QComboBox *colorReference = nullptr;
    QLabel *colorSummary = nullptr;
    QPushButton *autoColorButton = nullptr;
    QPushButton *resetColorButton = nullptr;
    QPushButton *exportButton = nullptr;
    QPushButton *cancelButton = nullptr;
    QProgressBar *progress = nullptr;
    QLabel *status = nullptr;
    QLabel *syncSummary = nullptr;
    QTimer previewTimer;
    QProcess *exportProcess = nullptr;
    QString exportOutput;
    double exportDuration = 0.0;
    bool cancelRequested = false;
    std::vector<FrameSegment> segments;
    int markedLeftStart = -1;
    int markedRightStart = -1;
    bool previewDragActive = false;
    QObject *previewDragTarget = nullptr;
    QPointF previewDragStart;
    CenteredAdjustment previewDragAdjustment;
    double previewPixelsPerDisplayX = 1.0;
    double previewPixelsPerDisplayY = 1.0;
    bool updatingCommonTimeline = false;
    double commonSyncOffsetSeconds = 0.0;

    static QString secondsText(double seconds)
    {
        const int ms = qMax(0, qRound(seconds * 1000.0));
        const int hours = ms / 3600000;
        const int minutes = (ms / 60000) % 60;
        const int secs = (ms / 1000) % 60;
        const int millis = ms % 1000;
        return QString("%1:%2:%3.%4")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'))
            .arg(millis, 3, 10, QLatin1Char('0'));
    }

    static QString orientationName(const VideoInfo &info)
    {
        if (info.displayHeight > info.displayWidth) return "Portrait";
        if (info.displayWidth > info.displayHeight) return "Landscape";
        return "Square";
    }

    void applyTheme(bool dark)
    {
        if (dark) {
            setStyleSheet(R"(
                QMainWindow, QWidget { background: #11161e; color: #e7edf5; font-size: 13px; }
                QLabel#title { font-size: 28px; font-weight: 700; color: #ffffff; }
                QLabel#drawerTitle { font-size: 21px; font-weight: 700; color: #ffffff; }
                QLabel#workspaceTitle { font-size: 17px; font-weight: 650; color: #e7edf5; }
                QLabel#subtitle, QLabel#details { color: #8fa1b7; }
                QLabel#summary { color: #a9bad0; padding: 3px; }
                QGroupBox { border: 1px solid #2c3949; border-radius: 7px; margin-top: 9px; padding-top: 11px; font-weight: 600; }
                QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; color: #9fb4cb; }
                QPushButton, QComboBox, QSpinBox, QDoubleSpinBox { background: #202a37; border: 1px solid #39495c; border-radius: 5px; padding: 6px 9px; }
                QPushButton:hover { background: #2a3949; border-color: #5b7692; }
                QPushButton:disabled { color: #657181; background: #19212b; }
                QPushButton#primaryButton { background: #1769aa; border-color: #3187c9; color: white; font-weight: 700; }
                QPushButton#primaryButton:hover { background: #207dc4; }
                QSlider::groove:horizontal { height: 5px; background: #2b3745; border-radius: 2px; }
                QSlider::handle:horizontal { width: 15px; margin: -5px 0; border-radius: 7px; background: #42a5e8; }
                QProgressBar { border: 1px solid #344253; border-radius: 4px; background: #19212b; text-align: center; }
                QProgressBar::chunk { background: #287eb9; }
                QDockWidget::title { background: #202a37; padding: 6px; }
                QToolBar { background: #19212b; border-right: 1px solid #39495c; spacing: 5px; padding: 4px; }
                QToolTip { background: #273342; color: white; border: 1px solid #5b6c80; }
            )");
        } else {
            setStyleSheet(R"(
                QMainWindow, QWidget { background: #f5f7fa; color: #182433; font-size: 13px; }
                QLabel#title { font-size: 28px; font-weight: 700; color: #132033; }
                QLabel#drawerTitle { font-size: 21px; font-weight: 700; color: #132033; }
                QLabel#workspaceTitle { font-size: 17px; font-weight: 650; color: #24364a; }
                QLabel#subtitle, QLabel#details { color: #5e7085; }
                QLabel#summary { color: #465b72; padding: 3px; }
                QGroupBox { border: 1px solid #cbd5e1; border-radius: 7px; margin-top: 9px; padding-top: 11px; font-weight: 600; background: #ffffff; }
                QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; color: #41566e; background: #f5f7fa; }
                QPushButton, QComboBox, QSpinBox, QDoubleSpinBox { background: #ffffff; border: 1px solid #b9c6d4; border-radius: 5px; padding: 6px 9px; }
                QPushButton:hover { background: #edf4fa; border-color: #7195b7; }
                QPushButton:disabled { color: #9aa7b4; background: #edf0f3; }
                QPushButton#primaryButton { background: #1672b8; border-color: #12639f; color: white; font-weight: 700; }
                QPushButton#primaryButton:hover { background: #2184cc; }
                QSlider::groove:horizontal { height: 5px; background: #d4dde6; border-radius: 2px; }
                QSlider::handle:horizontal { width: 15px; margin: -5px 0; border-radius: 7px; background: #1672b8; }
                QProgressBar { border: 1px solid #b9c6d4; border-radius: 4px; background: #ffffff; text-align: center; }
                QProgressBar::chunk { background: #2b83bf; }
                QDockWidget::title { background: #e7edf3; padding: 6px; }
                QToolBar { background: #e8edf2; border-right: 1px solid #c4ced8; spacing: 5px; padding: 4px; }
                QToolTip { background: #ffffff; color: #182433; border: 1px solid #9cabb9; }
            )");
        }
    }

    void buildUi()
    {
        // Preview-first workspace. Editing controls live in a dockable side
        // drawer, following the compact toolbox approach used by StereoMask.
        auto *root = new QWidget;
        setCentralWidget(root);
        auto *main = new QVBoxLayout(root);
        main->setContentsMargins(8, 8, 8, 8);
        main->setSpacing(6);

        auto *header = new QHBoxLayout;
        auto *workspaceTitle = new QLabel("Stereo workspace");
        workspaceTitle->setObjectName("workspaceTitle");
        header->addWidget(workspaceTitle);
        header->addSpacing(12);
        header->addWidget(new QLabel("Adjustment:"));
        adjustmentStepGroup = new QButtonGroup(this);
        auto *fineStep = new QRadioButton("Fine");
        auto *mediumStep = new QRadioButton("Medium");
        auto *longStep = new QRadioButton("Long");
        adjustmentStepGroup->addButton(fineStep, 0);
        adjustmentStepGroup->addButton(mediumStep, 1);
        adjustmentStepGroup->addButton(longStep, 2);
        fineStep->setChecked(true);
        fineStep->setToolTip("Zoom 0.1%; movement 1 pixel.");
        mediumStep->setToolTip("Zoom 1%; movement 10 pixels.");
        longStep->setToolTip("Zoom 10%; movement 100 pixels.");
        header->addWidget(fineStep);
        header->addWidget(mediumStep);
        header->addWidget(longStep);
        showGrid = new QCheckBox("Grid");
        showGrid->setToolTip("Show or hide alignment guides over the previews.");
        header->addWidget(showGrid);
        header->addStretch();
        previewMode = new QComboBox;
        previewMode->addItems({"Side by side", "Red / cyan anaglyph", "Difference"});
        themeSelector = new QComboBox;
        themeSelector->addItems({"Light", "Dark"});
        header->addWidget(new QLabel("Preview:"));
        header->addWidget(previewMode);
        header->addSpacing(8);
        header->addWidget(new QLabel("Theme:"));
        header->addWidget(themeSelector);
        main->addLayout(header);

        previewStack = new QStackedWidget;
        previewStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto *sbsPage = new QWidget;
        auto *sbs = new QHBoxLayout(sbsPage);
        sbs->setContentsMargins(0, 0, 0, 0);
        sbs->setSpacing(6);
        sbs->setAlignment(Qt::AlignCenter);
        leftPreview = makePreviewLabel("LEFT");
        rightPreview = makePreviewLabel("RIGHT");
        leftPreview->installEventFilter(this);
        rightPreview->installEventFilter(this);
        sbs->addWidget(leftPreview);
        sbs->addWidget(rightPreview);
        singlePreview = makePreviewLabel("PREVIEW");
        singlePreview->installEventFilter(this);
        previewStack->addWidget(sbsPage);
        previewStack->addWidget(singlePreview);
        main->addWidget(previewStack, 1);

        auto *timelineRow = new QHBoxLayout;
        timelineMarkStartButton = new QPushButton("Mark start");
        timelineAddEndButton = new QPushButton("Add range end");
        commonTimeline = new RangeTimelineSlider;
        commonTimeline->setRange(0, 0);
        commonTimelinePosition = new QLabel("00:00:00.000");
        commonTimelinePosition->setMinimumWidth(92);
        timelineMarkStartButton->setToolTip("Mark the current synchronized frame pair as the range start.");
        timelineAddEndButton->setToolTip("Add a range from the marked start to the current synchronized frame pair.");
        timelineRow->addWidget(timelineMarkStartButton);
        timelineRow->addWidget(commonTimeline, 1);
        timelineRow->addWidget(commonTimelinePosition);
        timelineRow->addWidget(timelineAddEndButton);
        main->addLayout(timelineRow);

        syncSummary = new QLabel("Load both videos to begin.");
        syncSummary->setObjectName("summary");
        main->addWidget(syncSummary);
        auto *statusRow = new QHBoxLayout;
        progress = new QProgressBar;
        progress->setRange(0, 1000);
        progress->setValue(0);
        status = new QLabel(ffmpegPath.isEmpty() ? "FFmpeg not found" : "Ready");
        status->setMinimumWidth(180);
        statusRow->addWidget(progress, 1);
        statusRow->addWidget(status);
        main->addLayout(statusRow);

        auto *drawer = new QWidget;
        auto *drawerLayout = new QVBoxLayout(drawer);
        drawerLayout->setContentsMargins(8, 8, 8, 8);
        drawerLayout->setSpacing(7);
        auto *title = new QLabel("Stereo Aligner");
        title->setObjectName("drawerTitle");
        auto *subtitle = new QLabel("Load, synchronize, align and export");
        subtitle->setObjectName("subtitle");
        subtitle->setWordWrap(true);
        drawerLayout->addWidget(title);
        drawerLayout->addWidget(subtitle);

        auto *projectGroup = new QGroupBox("Project");
        auto *projectLayout = new QHBoxLayout(projectGroup);
        newProjectButton = new QPushButton("New");
        openProjectButton = new QPushButton("Open JSON…");
        saveProjectButton = new QPushButton("Save JSON…");
        newProjectButton->setToolTip("Close the current work and reset an empty project.");
        openProjectButton->setToolTip("Open a saved Stereo Aligner project.");
        saveProjectButton->setToolTip("Save video paths and all current alignment settings as JSON.");
        projectLayout->addWidget(newProjectButton);
        projectLayout->addWidget(openProjectButton);
        projectLayout->addWidget(saveProjectButton);
        drawerLayout->addWidget(projectGroup);

        drawerLayout->addWidget(makeFileCard(true));
        drawerLayout->addWidget(makeFileCard(false));
        drawerLayout->addWidget(makeTimelineCard(true));
        drawerLayout->addWidget(makeTimelineCard(false));

        auto *alignGroup = new QGroupBox("Centered stereo adjustment");
        auto *align = new QGridLayout(alignGroup);
        align->setColumnStretch(1, 1);
        setupOffsetSpin(leftUi.x);
        setupOffsetSpin(leftUi.y);
        setupOffsetSpin(rightUi.x);
        setupOffsetSpin(rightUi.y);
        setupZoomSpin(leftUi.zoom);
        setupZoomSpin(rightUi.zoom);
        const QList<QWidget*> hiddenEyeControls {leftUi.x, leftUi.y, rightUi.x, rightUi.y,
                                                 leftUi.zoom, rightUi.zoom};
        for (QWidget *hidden : hiddenEyeControls) hidden->hide();
        auto makeFineSlider = [](int minimum, int maximum, int step) {
            auto *slider = new QSlider(Qt::Horizontal);
            slider->setRange(minimum, maximum);
            slider->setSingleStep(step);
            slider->setPageStep(step * 10);
            slider->setTracking(true);
            return slider;
        };
        stereoWindowSlider = makeFineSlider(-2000, 2000, 1);
        verticalSlider = makeFineSlider(-2000, 2000, 1);
        zoomBalanceSlider = makeFineSlider(-750, 750, 1); // tenths of one percent
        stereoWindowSpin = new QSpinBox;
        stereoWindowSpin->setRange(-2000, 2000);
        stereoWindowSpin->setSuffix(" px");
        stereoWindowSpin->setKeyboardTracking(false);
        verticalSpin = new QSpinBox;
        verticalSpin->setRange(-2000, 2000);
        verticalSpin->setSuffix(" px");
        verticalSpin->setKeyboardTracking(false);
        zoomBalanceSpin = new QDoubleSpinBox;
        zoomBalanceSpin->setRange(-75.0, 75.0);
        zoomBalanceSpin->setDecimals(1);
        zoomBalanceSpin->setSuffix(" %");
        zoomBalanceSpin->setKeyboardTracking(false);
        stereoWindowValue = new QLabel("0 px");
        verticalValue = new QLabel("0 px");
        zoomBalanceValue = new QLabel("L 100.0% / R 100.0%");
        stereoWindowSlider->setToolTip("Move the left and right images horizontally by equal amounts in opposite directions.");
        verticalSlider->setToolTip("Move the left and right images vertically by equal amounts in opposite directions.");
        zoomBalanceSlider->setToolTip("Change both zooms equally around 100%. One eye shrinks by the amount the other grows.");
        align->addWidget(new QLabel("Stereo window"), 0, 0);
        align->addWidget(stereoWindowSlider, 0, 1);
        align->addWidget(stereoWindowSpin, 0, 2);
        align->addWidget(stereoWindowValue, 0, 3);
        align->addWidget(new QLabel("Vertical"), 1, 0);
        align->addWidget(verticalSlider, 1, 1);
        align->addWidget(verticalSpin, 1, 2);
        align->addWidget(verticalValue, 1, 3);
        align->addWidget(new QLabel("Zoom balance"), 2, 0);
        align->addWidget(zoomBalanceSlider, 2, 1);
        align->addWidget(zoomBalanceSpin, 2, 2);
        align->addWidget(zoomBalanceValue, 2, 3);
        autoAlignButton = new QPushButton("Auto align selected frames");
        autoAlignButton->setToolTip("Estimate zoom and X/Y alignment, then split every correction equally around the stereo center.");
        align->addWidget(autoAlignButton, 3, 0, 1, 4);
        resetAlignment = new QPushButton("Center all adjustments");
        align->addWidget(resetAlignment, 4, 0, 1, 4);
        drawerLayout->addWidget(alignGroup);

        auto *segmentsGroup = new QGroupBox("Project frame ranges");
        auto *segmentsLayout = new QGridLayout(segmentsGroup);
        segmentTable = new QTableWidget(0, 4);
        segmentTable->setHorizontalHeaderLabels({"Name", "From", "To", "Adjustment"});
        segmentTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        segmentTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        segmentTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        segmentTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        segmentTable->verticalHeader()->setVisible(false);
        segmentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        segmentTable->setSelectionMode(QAbstractItemView::SingleSelection);
        segmentTable->setMinimumHeight(130);
        segmentTable->setToolTip("Only ranges in this table are exported, in this order. Double-click a name to edit it.");
        markSegmentStartButton = new QPushButton("Mark range start");
        addSegmentButton = new QPushButton("Add range to current frames");
        updateSegmentButton = new QPushButton("Update selected adjustment");
        removeSegmentButton = new QPushButton("Remove selected range");
        segmentsLayout->addWidget(segmentTable, 0, 0, 1, 2);
        segmentsLayout->addWidget(markSegmentStartButton, 1, 0);
        segmentsLayout->addWidget(addSegmentButton, 1, 1);
        segmentsLayout->addWidget(updateSegmentButton, 2, 0);
        segmentsLayout->addWidget(removeSegmentButton, 2, 1);
        auto *rangeHint = new QLabel("Mark a synchronized start, move to the end, set the three adjustments, then add the range.");
        rangeHint->setObjectName("details");
        rangeHint->setWordWrap(true);
        segmentsLayout->addWidget(rangeHint, 3, 0, 1, 2);
        drawerLayout->addWidget(segmentsGroup);

        auto *cropGroup = new QGroupBox("Crop aligned pair");
        auto *cropGrid = new QGridLayout(cropGroup);
        setupCropSpin(cropLeft, "Left");
        setupCropSpin(cropRight, "Right");
        setupCropSpin(cropTop, "Top");
        setupCropSpin(cropBottom, "Bottom");
        cropGrid->addWidget(new QLabel("Left"), 0, 0);
        cropGrid->addWidget(cropLeft, 0, 1);
        cropGrid->addWidget(new QLabel("Right"), 0, 2);
        cropGrid->addWidget(cropRight, 0, 3);
        cropGrid->addWidget(new QLabel("Top"), 1, 0);
        cropGrid->addWidget(cropTop, 1, 1);
        cropGrid->addWidget(new QLabel("Bottom"), 1, 2);
        cropGrid->addWidget(cropBottom, 1, 3);
        autoCropButton = new QPushButton("Auto crop black borders");
        autoCropButton->setToolTip("Find the largest common rectangle covered by both transformed videos.");
        resetCropButton = new QPushButton("Reset crop");
        cropGrid->addWidget(autoCropButton, 2, 0, 1, 4);
        cropGrid->addWidget(resetCropButton, 3, 0, 1, 4);
        drawerLayout->addWidget(cropGroup);

        auto *colorGroup = new QGroupBox("Color match");
        auto *colorGrid = new QGridLayout(colorGroup);
        colorReference = new QComboBox;
        colorReference->addItems({"Left eye as reference", "Right eye as reference"});
        colorReference->setToolTip("The reference eye remains unchanged; the other eye is corrected to match it.");
        colorSummary = new QLabel("No color correction");
        colorSummary->setObjectName("details");
        colorSummary->setWordWrap(true);
        autoColorButton = new QPushButton("Auto match selected frames");
        autoColorButton->setToolTip("Match the other eye's RGB color balance and contrast to the selected reference eye.");
        resetColorButton = new QPushButton("Reset color");
        colorGrid->addWidget(new QLabel("Reference"), 0, 0);
        colorGrid->addWidget(colorReference, 0, 1);
        colorGrid->addWidget(colorSummary, 1, 0, 1, 2);
        colorGrid->addWidget(autoColorButton, 2, 0, 1, 2);
        colorGrid->addWidget(resetColorButton, 3, 0, 1, 2);
        drawerLayout->addWidget(colorGroup);

        auto *exportGroup = new QGroupBox("Output");
        auto *exportGrid = new QGridLayout(exportGroup);
        outputMode = new QComboBox;
        outputMode->addItems({"Parallel full SBS", "Cross-view full SBS",
                              "Red / cyan anaglyph", "Half SBS (HSBS)"});
        audioSource = new QComboBox;
        audioSource->addItems({"Left video", "Right video", "No audio"});
        swapOutputEyes = new QPushButton("Swap output eyes: Off");
        swapOutputEyes->setCheckable(true);
        swapOutputEyes->setToolTip("Reverse left and right at export. Cross-view already reverses the normal parallel order.");
        exportGrid->addWidget(new QLabel("Format"), 0, 0);
        exportGrid->addWidget(outputMode, 0, 1);
        exportGrid->addWidget(new QLabel("Audio"), 1, 0);
        exportGrid->addWidget(audioSource, 1, 1);
        exportGrid->addWidget(swapOutputEyes, 2, 0, 1, 2);
        exportButton = new QPushButton("Export project ranges");
        exportButton->setObjectName("primaryButton");
        cancelButton = new QPushButton("Cancel");
        cancelButton->setVisible(false);
        exportGrid->addWidget(exportButton, 3, 0, 1, 2);
        exportGrid->addWidget(cancelButton, 4, 0, 1, 2);
        drawerLayout->addWidget(exportGroup);

        auto *shortcutHint = new QLabel("Drag: horizontal/vertical  •  Wheel: zoom\nCtrl+wheel: horizontal  •  Shift+wheel: vertical");
        shortcutHint->setObjectName("details");
        shortcutHint->setWordWrap(true);
        drawerLayout->addWidget(shortcutHint);
        drawerLayout->addStretch();

        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(drawer);
        auto *controlsDock = new QDockWidget("Controls", this);
        controlsDock->setObjectName("controlsDock");
        controlsDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        controlsDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
        controlsDock->setWidget(scroll);
        controlsDock->setMinimumWidth(410);
        controlsDock->resize(430, height());
        addDockWidget(Qt::LeftDockWidgetArea, controlsDock);

        auto *rail = new QToolBar("Toolbox rail", this);
        rail->setObjectName("toolboxRail");
        rail->setAllowedAreas(Qt::LeftToolBarArea);
        rail->setMovable(false);
        rail->setIconSize(QSize(24, 24));
        rail->setToolButtonStyle(Qt::ToolButtonIconOnly);
        addToolBar(Qt::LeftToolBarArea, rail);
        QAction *drawerAction = controlsDock->toggleViewAction();
        drawerAction->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
        drawerAction->setToolTip("Show or hide controls");
        rail->addAction(drawerAction);
        rail->addSeparator();
        QAction *newProjectAction = rail->addAction(style()->standardIcon(QStyle::SP_FileIcon), "New project");
        QAction *openProjectAction = rail->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), "Open JSON project");
        QAction *saveProjectAction = rail->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton), "Save JSON project");
        rail->addSeparator();
        QAction *openLeftAction = rail->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), "Open left video");
        QAction *openRightAction = rail->addAction(style()->standardIcon(QStyle::SP_DirOpenIcon), "Open right video");
        rail->addSeparator();
        QAction *exportAction = rail->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton), "Export aligned video");
        connect(openLeftAction, &QAction::triggered, this, [this] { chooseVideo(true); });
        connect(openRightAction, &QAction::triggered, this, [this] { chooseVideo(false); });
        connect(newProjectAction, &QAction::triggered, this, [this] { newProject(); });
        connect(openProjectAction, &QAction::triggered, this, [this] { openProject(); });
        connect(saveProjectAction, &QAction::triggered, this, [this] { saveProject(); });
        connect(exportAction, &QAction::triggered, this, [this] { startExport(); });
    }

    QWidget *makeFileCard(bool left)
    {
        EyeControls &ui = left ? leftUi : rightUi;
        auto *box = new QGroupBox(left ? "Left camera" : "Right camera");
        auto *layout = new QGridLayout(box);
        ui.open = new QPushButton(left ? "Open left video…" : "Open right video…");
        ui.fileName = new QLabel("No video loaded");
        ui.fileName->setTextInteractionFlags(Qt::TextSelectableByMouse);
        ui.fileName->setWordWrap(true);
        ui.details = new QLabel("—");
        ui.details->setObjectName("details");
        ui.details->setWordWrap(true);
        layout->addWidget(ui.open, 0, 0, 1, 2);
        layout->addWidget(ui.fileName, 1, 0, 1, 2);
        layout->addWidget(ui.details, 2, 0, 1, 2);
        return box;
    }

    QWidget *makeTimelineCard(bool left)
    {
        EyeControls &ui = left ? leftUi : rightUi;
        auto *box = new QGroupBox(left ? "Left timeline" : "Right timeline");
        auto *layout = new QGridLayout(box);
        ui.previous = new QPushButton("◀  Previous");
        ui.next = new QPushButton("Next  ▶");
        ui.slider = new QSlider(Qt::Horizontal);
        ui.frame = new QSpinBox;
        ui.frame->setPrefix("Frame ");
        ui.frame->setKeyboardTracking(false);
        layout->addWidget(ui.previous, 0, 0);
        layout->addWidget(ui.next, 0, 1);
        if (left) {
            linkStepping = new QCheckBox("Link stepping");
            linkStepping->setToolTip("Previous/Next buttons move both videos by one frame.");
            layout->addWidget(linkStepping, 1, 0, 1, 2);
        } else {
            auto *independent = new QLabel("Independent by default");
            independent->setObjectName("details");
            layout->addWidget(independent, 1, 0, 1, 2);
        }
        layout->addWidget(ui.slider, 2, 0, 1, 2);
        layout->addWidget(ui.frame, 3, 0, 1, 2);
        layout->setColumnStretch(1, 1);
        return box;
    }

    QLabel *makePreviewLabel(const QString &name)
    {
        auto *label = new QLabel(name + "\n\nLoad a video");
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumHeight(250);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        label->setCursor(Qt::OpenHandCursor);
        label->setToolTip("Drag to position. Wheel: zoom. Ctrl+wheel: horizontal. Shift+wheel: vertical. All use the selected adjustment step.");
        label->setStyleSheet("background:#05070a; border:1px solid #2a3543; border-radius:6px; color:#65758a; font-size:16px;");
        return label;
    }

    static void setupOffsetSpin(QSpinBox *&spin)
    {
        spin = new QSpinBox;
        spin->setRange(-2000, 2000);
        spin->setSuffix(" px");
        spin->setKeyboardTracking(false);
    }

    static void setupZoomSpin(QDoubleSpinBox *&spin)
    {
        spin = new QDoubleSpinBox;
        spin->setRange(25.0, 400.0);
        spin->setDecimals(1);
        spin->setSingleStep(0.5);
        spin->setValue(100.0);
        spin->setSuffix(" %");
        spin->setKeyboardTracking(false);
        spin->setToolTip("Scale this camera around the center. Below 100% zooms out; above 100% zooms in.");
    }

    static void setupCropSpin(QSpinBox *&spin, const QString &side)
    {
        spin = new QSpinBox;
        spin->setRange(0, 8000);
        spin->setSuffix(" px");
        spin->setKeyboardTracking(false);
        spin->setToolTip(QString("Crop %1 pixels from the %2 side of both aligned eyes.")
                         .arg(side.toLower(), side.toLower()));
    }

    int pixelAdjustmentStep() const
    {
        const int level = adjustmentStepGroup ? adjustmentStepGroup->checkedId() : 0;
        return level == 2 ? 100 : (level == 1 ? 10 : 1);
    }

    double zoomAdjustmentStep() const
    {
        const int level = adjustmentStepGroup ? adjustmentStepGroup->checkedId() : 0;
        return level == 2 ? 10.0 : (level == 1 ? 1.0 : 0.1);
    }

    void updateAdjustmentSteps()
    {
        const int pixelStep = pixelAdjustmentStep();
        const int zoomStep = qRound(zoomAdjustmentStep() * 10.0);
        stereoWindowSlider->setSingleStep(pixelStep);
        stereoWindowSlider->setPageStep(pixelStep);
        verticalSlider->setSingleStep(pixelStep);
        verticalSlider->setPageStep(pixelStep);
        zoomBalanceSlider->setSingleStep(zoomStep);
        zoomBalanceSlider->setPageStep(zoomStep);
        stereoWindowSpin->setSingleStep(pixelStep);
        verticalSpin->setSingleStep(pixelStep);
        zoomBalanceSpin->setSingleStep(zoomAdjustmentStep());
        status->setText(QString("Adjustment step: zoom %1%, movement %2 px")
                        .arg(zoomAdjustmentStep(), 0, 'f', 1).arg(pixelStep));
    }

    CenteredAdjustment currentAdjustment() const
    {
        return {stereoWindowSlider->value(), verticalSlider->value(),
                zoomBalanceSlider->value() / 10.0};
    }

    void applyCenteredAdjustment(const CenteredAdjustment &adjustment)
    {
        const double balance = qBound(-75.0, adjustment.zoomBalance, 75.0);
        const QList<QObject*> controls {leftUi.x, leftUi.y, rightUi.x, rightUi.y,
                                        leftUi.zoom, rightUi.zoom};
        std::vector<std::unique_ptr<QSignalBlocker>> blockers;
        for (QObject *control : controls) blockers.emplace_back(std::make_unique<QSignalBlocker>(control));
        leftUi.x->setValue(-adjustment.stereoWindow);
        rightUi.x->setValue(adjustment.stereoWindow);
        leftUi.y->setValue(-adjustment.vertical);
        rightUi.y->setValue(adjustment.vertical);
        leftUi.zoom->setValue(100.0 - balance);
        rightUi.zoom->setValue(100.0 + balance);
        const QSignalBlocker sw(stereoWindowSpin), sv(verticalSpin), sz(zoomBalanceSpin);
        stereoWindowSpin->setValue(adjustment.stereoWindow);
        verticalSpin->setValue(adjustment.vertical);
        zoomBalanceSpin->setValue(balance);
        stereoWindowValue->setText(QString("L %1 / R %2 px").arg(-adjustment.stereoWindow).arg(adjustment.stereoWindow));
        verticalValue->setText(QString("L %1 / R %2 px").arg(-adjustment.vertical).arg(adjustment.vertical));
        zoomBalanceValue->setText(QString("L %1% / R %2%")
                                  .arg(100.0 - balance, 0, 'f', 1).arg(100.0 + balance, 0, 'f', 1));
        updateSummary();
        requestPreview();
    }

    void setCurrentAdjustment(const CenteredAdjustment &adjustment)
    {
        const QSignalBlocker a(stereoWindowSlider), b(verticalSlider), c(zoomBalanceSlider);
        const QSignalBlocker d(stereoWindowSpin), e(verticalSpin), f(zoomBalanceSpin);
        stereoWindowSlider->setValue(qBound(-2000, adjustment.stereoWindow, 2000));
        verticalSlider->setValue(qBound(-2000, adjustment.vertical, 2000));
        zoomBalanceSlider->setValue(qBound(-750, qRound(adjustment.zoomBalance * 10.0), 750));
        stereoWindowSpin->setValue(stereoWindowSlider->value());
        verticalSpin->setValue(verticalSlider->value());
        zoomBalanceSpin->setValue(zoomBalanceSlider->value() / 10.0);
        applyCenteredAdjustment(currentAdjustment());
    }

    QString segmentAdjustmentText(const FrameSegment &segment) const
    {
        return QString("W %1  V %2  Z %3%")
            .arg(segment.adjustment.stereoWindow).arg(segment.adjustment.vertical)
            .arg(segment.adjustment.zoomBalance, 0, 'f', 1);
    }

    QColor segmentColor(int index) const
    {
        return QColor::fromHsv((index * 67 + 15) % 360, 190, 225);
    }

    void refreshSegmentList(int selected = -1)
    {
        const QSignalBlocker blocker(segmentTable);
        segmentTable->setRowCount(static_cast<int>(segments.size()));
        QVector<RangeTimelineSlider::Span> spans;
        for (int i = 0; i < static_cast<int>(segments.size()); ++i) {
            FrameSegment &segment = segments[i];
            if (segment.name.trimmed().isEmpty()) segment.name = QString("Range %1").arg(i + 1);
            const QColor color = segmentColor(i);
            auto *name = new QTableWidgetItem(segment.name);
            name->setBackground(QBrush(QColor(color.red(), color.green(), color.blue(), 90)));
            auto makeReadOnly = [](const QString &text) {
                auto *item = new QTableWidgetItem(text);
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                return item;
            };
            segmentTable->setItem(i, 0, name);
            segmentTable->setItem(i, 1, makeReadOnly(QString("L %1 / R %2").arg(segment.leftStart).arg(segment.rightStart)));
            segmentTable->setItem(i, 2, makeReadOnly(QString("L %1 / R %2").arg(segment.leftEnd).arg(segment.rightEnd)));
            segmentTable->setItem(i, 3, makeReadOnly(segmentAdjustmentText(segment)));
            const int firstMs = leftInfo.valid() ? qRound(segment.leftStart * 1000.0 / leftInfo.fps) : 0;
            const int lastMs = leftInfo.valid() ? qRound(segment.leftEnd * 1000.0 / leftInfo.fps) : 0;
            spans.push_back({firstMs, lastMs, color});
        }
        commonTimeline->setSpans(spans);
        if (selected >= 0 && selected < segmentTable->rowCount()) segmentTable->setCurrentCell(selected, 0);
        updateReadyState();
    }

    int selectedSegmentRow() const
    {
        return segmentTable->selectionModel()->selectedRows().isEmpty()
            ? -1 : segmentTable->selectionModel()->selectedRows().first().row();
    }

    void updateCommonTimelineFromFrames()
    {
        if (updatingCommonTimeline || !leftInfo.valid() || !rightInfo.valid()) return;
        const double leftSeconds = leftUi.frame->value() / leftInfo.fps;
        const double rightSeconds = rightUi.frame->value() / rightInfo.fps;
        commonSyncOffsetSeconds = rightSeconds - leftSeconds;
        const int minimumMs = qMax(0, qCeil(-commonSyncOffsetSeconds * 1000.0));
        const int maximumMs = qMax(minimumMs, qFloor(qMin(leftInfo.duration,
            rightInfo.duration - commonSyncOffsetSeconds) * 1000.0));
        const QSignalBlocker blocker(commonTimeline);
        commonTimeline->setRange(minimumMs, maximumMs);
        commonTimeline->setValue(qBound(minimumMs, qRound(leftSeconds * 1000.0), maximumMs));
        commonTimelinePosition->setText(secondsText(leftSeconds));
        refreshTimelineMarkers();
    }

    void refreshTimelineMarkers()
    {
        commonTimeline->setPendingStart(markedLeftStart >= 0 && leftInfo.valid()
            ? qRound(markedLeftStart * 1000.0 / leftInfo.fps) : -1);
    }

    void markSegmentStart()
    {
        if (!leftInfo.valid() || !rightInfo.valid()) return;
        markedLeftStart = leftUi.frame->value();
        markedRightStart = rightUi.frame->value();
        refreshTimelineMarkers();
        status->setText(QString("Range start marked: L %1 / R %2").arg(markedLeftStart).arg(markedRightStart));
    }

    void addSegment()
    {
        if (markedLeftStart < 0 || markedRightStart < 0) {
            QMessageBox::information(this, "Mark a range start", "Mark the start frames before adding a project range.");
            return;
        }
        const int leftEnd = leftUi.frame->value();
        const int rightEnd = rightUi.frame->value();
        if (leftEnd <= markedLeftStart || rightEnd <= markedRightStart) {
            QMessageBox::warning(this, "Invalid frame range", "Both current frames must be later than their marked start frames.");
            return;
        }
        FrameSegment segment;
        segment.name = QString("Range %1").arg(segments.size() + 1);
        segment.leftStart = markedLeftStart;
        segment.rightStart = markedRightStart;
        segment.leftEnd = leftEnd;
        segment.rightEnd = rightEnd;
        segment.adjustment = currentAdjustment();
        segments.push_back(segment);
        markedLeftStart = markedRightStart = -1;
        refreshTimelineMarkers();
        refreshSegmentList(static_cast<int>(segments.size()) - 1);
        status->setText("Project range added");
    }

    void connectUi()
    {
        connect(newProjectButton, &QPushButton::clicked, this, [this] { newProject(); });
        connect(openProjectButton, &QPushButton::clicked, this, [this] { openProject(); });
        connect(saveProjectButton, &QPushButton::clicked, this, [this] { saveProject(); });
        connect(leftUi.open, &QPushButton::clicked, this, [this] { chooseVideo(true); });
        connect(rightUi.open, &QPushButton::clicked, this, [this] { chooseVideo(false); });
        connect(leftUi.previous, &QPushButton::clicked, this, [this] { stepFrame(true, -1); });
        connect(leftUi.next, &QPushButton::clicked, this, [this] { stepFrame(true, 1); });
        connect(rightUi.previous, &QPushButton::clicked, this, [this] { stepFrame(false, -1); });
        connect(rightUi.next, &QPushButton::clicked, this, [this] { stepFrame(false, 1); });

        connectTimeline(leftUi);
        connectTimeline(rightUi);
        for (QSpinBox *spin : {leftUi.x, leftUi.y, rightUi.x, rightUi.y})
            connect(spin, &QSpinBox::valueChanged, this, [this] { updateSummary(); requestPreview(); });
        for (QDoubleSpinBox *spin : {leftUi.zoom, rightUi.zoom})
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { updateSummary(); requestPreview(); });
        connect(stereoWindowSlider, &QSlider::valueChanged, this, [this](int value) {
            const int step = pixelAdjustmentStep();
            const int snapped = qBound(stereoWindowSlider->minimum(),
                                       qRound(value / static_cast<double>(step)) * step,
                                       stereoWindowSlider->maximum());
            if (snapped != value) { stereoWindowSlider->setValue(snapped); return; }
            applyCenteredAdjustment(currentAdjustment());
        });
        connect(verticalSlider, &QSlider::valueChanged, this, [this](int value) {
            const int step = pixelAdjustmentStep();
            const int snapped = qBound(verticalSlider->minimum(),
                                       qRound(value / static_cast<double>(step)) * step,
                                       verticalSlider->maximum());
            if (snapped != value) { verticalSlider->setValue(snapped); return; }
            applyCenteredAdjustment(currentAdjustment());
        });
        connect(zoomBalanceSlider, &QSlider::valueChanged, this, [this](int value) {
            const int step = qRound(zoomAdjustmentStep() * 10.0);
            const int snapped = qBound(zoomBalanceSlider->minimum(),
                                       qRound(value / static_cast<double>(step)) * step,
                                       zoomBalanceSlider->maximum());
            if (snapped != value) { zoomBalanceSlider->setValue(snapped); return; }
            applyCenteredAdjustment(currentAdjustment());
        });
        connect(stereoWindowSpin, &QSpinBox::valueChanged, this, [this](int value) { stereoWindowSlider->setValue(value); });
        connect(verticalSpin, &QSpinBox::valueChanged, this, [this](int value) { verticalSlider->setValue(value); });
        connect(zoomBalanceSpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            zoomBalanceSlider->setValue(qRound(value * 10.0));
        });
        connect(adjustmentStepGroup, &QButtonGroup::idClicked, this, [this] { updateAdjustmentSteps(); });
        connect(showGrid, &QCheckBox::toggled, this, [this] { requestPreview(); });
        connect(swapOutputEyes, &QPushButton::toggled, this, [this](bool checked) {
            swapOutputEyes->setText(checked ? "Swap output eyes: On" : "Swap output eyes: Off");
            status->setText(checked ? "Output eye order reversed" : "Normal output eye order");
        });
        updateAdjustmentSteps();
        for (QSpinBox *spin : {cropLeft, cropRight, cropTop, cropBottom})
            connect(spin, &QSpinBox::valueChanged, this, [this] { updateSummary(); requestPreview(); });

        connect(previewMode, &QComboBox::currentIndexChanged, this, [this] {
            previewStack->setCurrentIndex(previewMode->currentIndex() == 0 ? 0 : 1);
            requestPreview();
        });
        connect(themeSelector, &QComboBox::currentIndexChanged, this, [this](int index) {
            QSettings().setValue("appearance/theme", index);
            applyTheme(index == 1);
        });
        connect(resetAlignment, &QPushButton::clicked, this, [this] {
            setCurrentAdjustment({});
        });
        connect(markSegmentStartButton, &QPushButton::clicked, this, [this] { markSegmentStart(); });
        connect(addSegmentButton, &QPushButton::clicked, this, [this] { addSegment(); });
        connect(timelineMarkStartButton, &QPushButton::clicked, this, [this] { markSegmentStart(); });
        connect(timelineAddEndButton, &QPushButton::clicked, this, [this] { addSegment(); });
        connect(commonTimeline, &QSlider::valueChanged, this, [this](int milliseconds) {
            if (updatingCommonTimeline || !leftInfo.valid() || !rightInfo.valid()) return;
            updatingCommonTimeline = true;
            const double leftSeconds = milliseconds / 1000.0;
            const double rightSeconds = leftSeconds + commonSyncOffsetSeconds;
            leftUi.frame->setValue(qBound(0, qRound(leftSeconds * leftInfo.fps), leftInfo.totalFrames - 1));
            rightUi.frame->setValue(qBound(0, qRound(rightSeconds * rightInfo.fps), rightInfo.totalFrames - 1));
            updatingCommonTimeline = false;
            commonTimelinePosition->setText(secondsText(leftSeconds));
            updateSummary();
            requestPreview();
        });
        connect(updateSegmentButton, &QPushButton::clicked, this, [this] {
            const int row = selectedSegmentRow();
            if (row < 0 || row >= static_cast<int>(segments.size())) return;
            segments[row].adjustment = currentAdjustment();
            refreshSegmentList(row);
            status->setText("Selected range adjustment updated");
        });
        connect(removeSegmentButton, &QPushButton::clicked, this, [this] {
            const int row = selectedSegmentRow();
            if (row < 0 || row >= static_cast<int>(segments.size())) return;
            segments.erase(segments.begin() + row);
            refreshSegmentList(qMin(row, static_cast<int>(segments.size()) - 1));
            status->setText("Project range removed");
        });
        connect(segmentTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item) {
            const int row = item->row();
            if (row < 0 || row >= static_cast<int>(segments.size())) return;
            const FrameSegment &segment = segments[row];
            leftUi.frame->setValue(segment.leftStart);
            rightUi.frame->setValue(segment.rightStart);
            setCurrentAdjustment(segment.adjustment);
        });
        connect(segmentTable, &QTableWidget::itemSelectionChanged, this, [this] { updateReadyState(); });
        connect(segmentTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
            if (!item || item->column() != 0 || item->row() < 0 || item->row() >= static_cast<int>(segments.size())) return;
            const QString name = item->text().trimmed();
            segments[item->row()].name = name.isEmpty() ? QString("Range %1").arg(item->row() + 1) : name;
        });
        connect(autoAlignButton, &QPushButton::clicked, this, [this] { autoAlign(); });
        connect(autoCropButton, &QPushButton::clicked, this, [this] { autoCropBorders(); });
        connect(resetCropButton, &QPushButton::clicked, this, [this] {
            cropLeft->setValue(0); cropRight->setValue(0);
            cropTop->setValue(0); cropBottom->setValue(0);
        });
        connect(autoColorButton, &QPushButton::clicked, this, [this] { autoMatchColor(); });
        connect(resetColorButton, &QPushButton::clicked, this, [this] {
            leftColor = ColorCorrection{};
            rightColor = ColorCorrection{};
            updateColorSummary();
            requestPreview();
        });
        connect(exportButton, &QPushButton::clicked, this, [this] { startExport(); });
        connect(cancelButton, &QPushButton::clicked, this, [this] {
            if (exportProcess) {
                cancelRequested = true;
                status->setText("Cancelling export…");
                exportProcess->kill();
            }
        });

        addShortcut(QKeySequence(Qt::Key_A), [this] { stepFrame(true, -1, false); });
        addShortcut(QKeySequence(Qt::Key_D), [this] { stepFrame(true, 1, false); });
        addShortcut(QKeySequence(Qt::Key_J), [this] { stepFrame(false, -1, false); });
        addShortcut(QKeySequence(Qt::Key_L), [this] { stepFrame(false, 1, false); });
        addShortcut(QKeySequence::New, [this] { newProject(); });
        addShortcut(QKeySequence::Open, [this] { openProject(); });
        addShortcut(QKeySequence::Save, [this] { saveProject(); });
        addAlignmentShortcuts();
    }

    void connectTimeline(EyeControls &ui)
    {
        connect(ui.slider, &QSlider::valueChanged, this, [this, &ui](int value) {
            if (ui.frame->value() != value) ui.frame->setValue(value);
            updateSummary(); requestPreview();
        });
        connect(ui.frame, &QSpinBox::valueChanged, this, [this, &ui](int value) {
            if (ui.slider->value() != value) ui.slider->setValue(value);
            updateCommonTimelineFromFrames();
        });
    }

    void addShortcut(const QKeySequence &sequence, std::function<void()> callback)
    {
        auto *shortcut = new QShortcut(sequence, this);
        connect(shortcut, &QShortcut::activated, this, std::move(callback));
    }

    void addAlignmentShortcuts()
    {
        auto nudge = [](QSlider *slider, int amount) { slider->setValue(slider->value() + amount); };
        addShortcut(QKeySequence("Ctrl+Left"), [=] { nudge(stereoWindowSlider, -pixelAdjustmentStep()); });
        addShortcut(QKeySequence("Ctrl+Right"), [=] { nudge(stereoWindowSlider, pixelAdjustmentStep()); });
        addShortcut(QKeySequence("Ctrl+Up"), [=] { nudge(verticalSlider, -pixelAdjustmentStep()); });
        addShortcut(QKeySequence("Ctrl+Down"), [=] { nudge(verticalSlider, pixelAdjustmentStep()); });
        addShortcut(QKeySequence("Ctrl+Shift+Left"), [=] { nudge(stereoWindowSlider, -pixelAdjustmentStep() * 10); });
        addShortcut(QKeySequence("Ctrl+Shift+Right"), [=] { nudge(stereoWindowSlider, pixelAdjustmentStep() * 10); });
        addShortcut(QKeySequence("Ctrl+Shift+Up"), [=] { nudge(verticalSlider, -pixelAdjustmentStep() * 10); });
        addShortcut(QKeySequence("Ctrl+Shift+Down"), [=] { nudge(verticalSlider, pixelAdjustmentStep() * 10); });
    }

    static QJsonObject colorToJson(const ColorCorrection &correction)
    {
        QJsonArray gain;
        QJsonArray offset;
        for (int channel = 0; channel < 3; ++channel) {
            gain.append(correction.gain[channel]);
            offset.append(correction.offset[channel]);
        }
        return {{"gain", gain}, {"offset", offset}};
    }

    static ColorCorrection colorFromJson(const QJsonObject &object)
    {
        ColorCorrection correction;
        const QJsonArray gain = object.value("gain").toArray();
        const QJsonArray offset = object.value("offset").toArray();
        if (gain.size() != 3 || offset.size() != 3) return correction;
        for (int channel = 0; channel < 3; ++channel) {
            correction.gain[channel] = qBound(0.1, gain.at(channel).toDouble(1.0), 4.0);
            correction.offset[channel] = qBound(-255.0, offset.at(channel).toDouble(0.0), 255.0);
        }
        return correction;
    }

    static QString storedVideoPath(const QString &videoPath, const QDir &projectDirectory)
    {
        const QString relative = projectDirectory.relativeFilePath(QFileInfo(videoPath).absoluteFilePath());
        return QDir::fromNativeSeparators(relative);
    }

    static QString resolvedVideoPath(const QJsonObject &eye, const QDir &projectDirectory)
    {
        const QString stored = eye.value("path").toString();
        if (!stored.isEmpty()) {
            const QString candidate = QFileInfo(stored).isAbsolute()
                ? stored : projectDirectory.absoluteFilePath(stored);
            if (QFileInfo::exists(candidate)) return QFileInfo(candidate).absoluteFilePath();
        }
        const QString absolute = eye.value("absolutePath").toString();
        if (QFileInfo::exists(absolute)) return QFileInfo(absolute).absoluteFilePath();
        return {};
    }

    QJsonObject eyeToJson(const VideoInfo &info, const EyeControls &ui,
                          const ColorCorrection &color, const QDir &projectDirectory) const
    {
        return {
            {"path", storedVideoPath(info.path, projectDirectory)},
            {"absolutePath", QDir::fromNativeSeparators(QFileInfo(info.path).absoluteFilePath())},
            {"frame", ui.frame->value()},
            {"x", ui.x->value()},
            {"y", ui.y->value()},
            {"zoomPercent", ui.zoom->value()},
            {"color", colorToJson(color)}
        };
    }

    QJsonArray segmentsToJson() const
    {
        QJsonArray array;
        for (const FrameSegment &segment : segments) {
            array.append(QJsonObject {
                {"name", segment.name},
                {"leftStart", segment.leftStart}, {"rightStart", segment.rightStart},
                {"leftEnd", segment.leftEnd}, {"rightEnd", segment.rightEnd},
                {"stereoWindow", segment.adjustment.stereoWindow},
                {"vertical", segment.adjustment.vertical},
                {"zoomBalance", segment.adjustment.zoomBalance}
            });
        }
        return array;
    }

    void newProject()
    {
        if (exportProcess) {
            QMessageBox::information(this, "Export in progress",
                                     "Finish or cancel the current export before starting a new project.");
            return;
        }
        const bool hasCurrentProject = leftInfo.valid() || rightInfo.valid() ||
                                       !segments.empty() || !projectPath.isEmpty();
        if (hasCurrentProject) {
            QMessageBox prompt(this);
            prompt.setWindowTitle("Start a new project?");
            prompt.setIcon(QMessageBox::Question);
            prompt.setText("Do you want to save the current project before starting a new one?");
            prompt.setInformativeText("Unsaved ranges and adjustments will be lost if you choose Discard.");
            prompt.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
            prompt.setDefaultButton(QMessageBox::Save);
            const int choice = prompt.exec();
            if (choice == QMessageBox::Cancel) return;
            if (choice == QMessageBox::Save && !saveProject()) return;
        }

        previewTimer.stop();
        leftInfo = VideoInfo{};
        rightInfo = VideoInfo{};
        projectPath.clear();
        segments.clear();
        markedLeftStart = markedRightStart = -1;
        commonSyncOffsetSeconds = 0.0;

        auto resetEye = [](EyeControls &ui) {
            ui.fileName->setText("No video loaded");
            ui.fileName->setToolTip({});
            ui.details->setText("—");
            ui.slider->setRange(0, 0);
            ui.slider->setValue(0);
            ui.frame->setRange(0, 0);
            ui.frame->setValue(0);
        };
        resetEye(leftUi);
        resetEye(rightUi);
        setCurrentAdjustment({});
        cropLeft->setValue(0); cropRight->setValue(0);
        cropTop->setValue(0); cropBottom->setValue(0);
        leftColor = ColorCorrection{};
        rightColor = ColorCorrection{};
        updateColorSummary();

        linkStepping->setChecked(false);
        previewMode->setCurrentIndex(0);
        outputMode->setCurrentIndex(0);
        audioSource->setCurrentIndex(0);
        swapOutputEyes->setChecked(false);
        colorReference->setCurrentIndex(0);
        if (QAbstractButton *fine = adjustmentStepGroup->button(0)) fine->setChecked(true);
        updateAdjustmentSteps();
        showGrid->setChecked(false);

        segmentTable->setRowCount(0);
        commonTimeline->setSpans({});
        commonTimeline->setPendingStart(-1);
        commonTimeline->setRange(0, 0);
        commonTimeline->setValue(0);
        commonTimelinePosition->setText(secondsText(0.0));
        progress->setValue(0);

        auto resetPreview = [](QLabel *label, const QString &name) {
            label->clear();
            label->setText(name + "\n\nLoad a video");
            label->setMinimumSize(0, 250);
            label->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        };
        resetPreview(leftPreview, "LEFT");
        resetPreview(rightPreview, "RIGHT");
        resetPreview(singlePreview, "PREVIEW");

        setWindowTitle("Stereo Aligner");
        status->setText(ffmpegPath.isEmpty() ? "FFmpeg not found" : "New project ready");
        updateReadyState();
    }

    bool saveProject()
    {
        if (!leftInfo.valid() || !rightInfo.valid()) {
            QMessageBox::information(this, "Load both videos",
                                     "Load both the left and right videos before saving a project.");
            return false;
        }
        QString suggested;
        if (!projectPath.isEmpty()) suggested = projectPath;
        else suggested = QFileInfo(leftInfo.path).dir().filePath(
            QFileInfo(leftInfo.path).completeBaseName() + "_StereoAligner.json");
        QString path = QFileDialog::getSaveFileName(this, "Save Stereo Aligner project", suggested,
                                                    "Stereo Aligner project (*.json)");
        if (path.isEmpty()) return false;
        if (QFileInfo(path).suffix().isEmpty()) path += ".json";
        const QDir projectDirectory = QFileInfo(path).absoluteDir();
        QJsonObject root {
            {"application", "Stereo Aligner"},
            {"version", 2},
            {"leftEye", eyeToJson(leftInfo, leftUi, leftColor, projectDirectory)},
            {"rightEye", eyeToJson(rightInfo, rightUi, rightColor, projectDirectory)},
            {"crop", QJsonObject {{"left", cropLeft->value()}, {"right", cropRight->value()},
                                   {"top", cropTop->value()}, {"bottom", cropBottom->value()}}},
            {"centeredAdjustment", QJsonObject {
                {"stereoWindow", currentAdjustment().stereoWindow},
                {"vertical", currentAdjustment().vertical},
                {"zoomBalance", currentAdjustment().zoomBalance}
            }},
            {"segments", segmentsToJson()},
            {"interface", QJsonObject {
                {"linkedStepping", linkStepping->isChecked()},
                {"previewMode", previewMode->currentIndex()},
                {"theme", themeSelector->currentIndex()},
                {"outputMode", outputMode->currentIndex()},
                {"audioSource", audioSource->currentIndex()},
                {"swapOutputEyes", swapOutputEyes->isChecked()},
                {"colorReference", colorReference->currentIndex()},
                {"adjustmentStep", adjustmentStepGroup->checkedId()},
                {"showGrid", showGrid->isChecked()}
            }}
        };
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
            QMessageBox::critical(this, "Could not save project",
                                  "The project file could not be written:\n" + QDir::toNativeSeparators(path));
            return false;
        }
        projectPath = QFileInfo(path).absoluteFilePath();
        setWindowTitle("Stereo Aligner — " + QFileInfo(projectPath).completeBaseName());
        status->setText("Project saved: " + QFileInfo(projectPath).fileName());
        return true;
    }

    void restoreEye(const QJsonObject &eye, EyeControls &ui, ColorCorrection &color)
    {
        ui.frame->setValue(eye.value("frame").toInt(0));
        ui.x->setValue(eye.value("x").toInt(0));
        ui.y->setValue(eye.value("y").toInt(0));
        ui.zoom->setValue(eye.value("zoomPercent").toDouble(100.0));
        color = colorFromJson(eye.value("color").toObject());
    }

    void openProject()
    {
        const QString path = QFileDialog::getOpenFileName(this, "Open Stereo Aligner project", projectPath,
                                                         "Stereo Aligner project (*.json);;JSON files (*.json)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::critical(this, "Could not open project", "The selected JSON file could not be read.");
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            QMessageBox::critical(this, "Invalid project",
                                  "The selected file is not valid project JSON.\n\n" + parseError.errorString());
            return;
        }
        const QJsonObject root = document.object();
        const int projectVersion = root.value("version").toInt();
        if (root.value("application").toString() != "Stereo Aligner" ||
            (projectVersion != 1 && projectVersion != 2) ||
            !root.value("leftEye").isObject() || !root.value("rightEye").isObject()) {
            QMessageBox::critical(this, "Invalid project",
                                  "This is not a supported Stereo Aligner project file.");
            return;
        }
        const QJsonObject leftEye = root.value("leftEye").toObject();
        const QJsonObject rightEye = root.value("rightEye").toObject();
        const QDir projectDirectory = QFileInfo(path).absoluteDir();
        const QString leftPath = resolvedVideoPath(leftEye, projectDirectory);
        const QString rightPath = resolvedVideoPath(rightEye, projectDirectory);
        if (leftPath.isEmpty() || rightPath.isEmpty()) {
            QStringList missing;
            if (leftPath.isEmpty()) missing << "Left video: " + leftEye.value("path").toString();
            if (rightPath.isEmpty()) missing << "Right video: " + rightEye.value("path").toString();
            QMessageBox::warning(this, "Project videos not found",
                                 "The project was not changed because these videos could not be found:\n\n" +
                                 missing.join('\n'));
            return;
        }

        previewTimer.stop();
        loadVideo(true, leftPath);
        loadVideo(false, rightPath);
        if (!leftInfo.valid() || !rightInfo.valid() ||
            QFileInfo(leftInfo.path).absoluteFilePath() != leftPath ||
            QFileInfo(rightInfo.path).absoluteFilePath() != rightPath) {
            QMessageBox::critical(this, "Could not restore project",
                                  "One of the project videos could not be opened.");
            return;
        }
        restoreEye(leftEye, leftUi, leftColor);
        restoreEye(rightEye, rightUi, rightColor);
        if (projectVersion >= 2) {
            const QJsonObject adjustment = root.value("centeredAdjustment").toObject();
            setCurrentAdjustment({adjustment.value("stereoWindow").toInt(),
                                  adjustment.value("vertical").toInt(),
                                  adjustment.value("zoomBalance").toDouble()});
        } else {
            setCurrentAdjustment({qRound((rightUi.x->value() - leftUi.x->value()) / 2.0),
                                  qRound((rightUi.y->value() - leftUi.y->value()) / 2.0),
                                  (rightUi.zoom->value() - leftUi.zoom->value()) / 2.0});
        }
        segments.clear();
        for (const QJsonValue &value : root.value("segments").toArray()) {
            const QJsonObject object = value.toObject();
            FrameSegment segment;
            segment.name = object.value("name").toString(QString("Range %1").arg(segments.size() + 1));
            segment.leftStart = object.value("leftStart").toInt();
            segment.rightStart = object.value("rightStart").toInt();
            segment.leftEnd = object.value("leftEnd").toInt();
            segment.rightEnd = object.value("rightEnd").toInt();
            segment.adjustment = {object.value("stereoWindow").toInt(), object.value("vertical").toInt(),
                                  object.value("zoomBalance").toDouble()};
            if (segment.leftEnd > segment.leftStart && segment.rightEnd > segment.rightStart)
                segments.push_back(segment);
        }
        refreshSegmentList();
        const QJsonObject crop = root.value("crop").toObject();
        cropLeft->setValue(crop.value("left").toInt(0));
        cropRight->setValue(crop.value("right").toInt(0));
        cropTop->setValue(crop.value("top").toInt(0));
        cropBottom->setValue(crop.value("bottom").toInt(0));
        const QJsonObject interface = root.value("interface").toObject();
        linkStepping->setChecked(interface.value("linkedStepping").toBool(false));
        previewMode->setCurrentIndex(qBound(0, interface.value("previewMode").toInt(0), previewMode->count() - 1));
        themeSelector->setCurrentIndex(qBound(0, interface.value("theme").toInt(0), themeSelector->count() - 1));
        int savedOutputMode = interface.value("outputMode").toInt(0);
        if (!interface.contains("swapOutputEyes") && savedOutputMode == 1) savedOutputMode = 3;
        outputMode->setCurrentIndex(qBound(0, savedOutputMode, outputMode->count() - 1));
        audioSource->setCurrentIndex(qBound(0, interface.value("audioSource").toInt(0), audioSource->count() - 1));
        swapOutputEyes->setChecked(interface.value("swapOutputEyes").toBool(false));
        colorReference->setCurrentIndex(qBound(0, interface.value("colorReference").toInt(0), colorReference->count() - 1));
        const int stepLevel = qBound(0, interface.value("adjustmentStep").toInt(0), 2);
        if (QAbstractButton *button = adjustmentStepGroup->button(stepLevel)) button->setChecked(true);
        updateAdjustmentSteps();
        showGrid->setChecked(interface.value("showGrid").toBool(false));
        projectPath = QFileInfo(path).absoluteFilePath();
        setWindowTitle("Stereo Aligner — " + QFileInfo(projectPath).completeBaseName());
        updateColorSummary();
        updateReadyState();
        requestPreview();
        status->setText("Project restored: " + QFileInfo(projectPath).fileName());
    }

    void chooseVideo(bool left)
    {
        const QString path = QFileDialog::getOpenFileName(this, left ? "Open left video" : "Open right video", QString(),
            "Video files (*.mp4 *.mov *.mkv *.avi *.m4v *.mts *.webm);;All files (*.*)");
        if (!path.isEmpty()) loadVideo(left, path);
    }

    void loadVideo(bool left, const QString &path)
    {
        if (ffmpegPath.isEmpty()) {
            QMessageBox::critical(this, "FFmpeg not found", "Add ffmpeg.exe to PATH or place it at D:/stereo/ffmpeg.exe.");
            return;
        }
        status->setText(QString("Reading %1 video…").arg(left ? "left" : "right"));
        QApplication::setOverrideCursor(Qt::WaitCursor);
        VideoInfo info;
        const bool ok = probeVideo(path, info);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            status->setText("Could not read video");
            QMessageBox::critical(this, "Could not open video", "FFmpeg could not read the selected file's video metadata.");
            return;
        }

        VideoInfo &target = left ? leftInfo : rightInfo;
        EyeControls &ui = left ? leftUi : rightUi;
        target = info;
        segments.clear();
        markedLeftStart = markedRightStart = -1;
        if (segmentTable) segmentTable->setRowCount(0);
        if (commonTimeline) commonTimeline->setSpans({});
        leftColor = ColorCorrection{};
        rightColor = ColorCorrection{};
        updateColorSummary();
        ui.fileName->setText(QFileInfo(path).fileName());
        ui.fileName->setToolTip(path);
        ui.details->setText(QString("%1  •  %2 × %3 display  •  %4 fps  •  %5  •  approximately %6 frames")
            .arg(orientationName(info)).arg(info.displayWidth).arg(info.displayHeight).arg(info.fps, 0, 'f', 3)
            .arg(secondsText(info.duration)).arg(info.totalFrames));
        ui.slider->setRange(0, qMax(0, info.totalFrames - 1));
        ui.frame->setRange(0, qMax(0, info.totalFrames - 1));
        ui.slider->setValue(0);
        ui.frame->setValue(0);
        status->setText("Ready");
        updateReadyState();
        updateCommonTimelineFromFrames();
        requestPreview();
    }

    bool probeVideo(const QString &path, VideoInfo &info)
    {
        QProcess process;
        process.setProgram(ffmpegPath);
        process.setArguments({"-hide_banner", "-i", path});
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start();
        if (!process.waitForFinished(15000)) {
            process.kill(); process.waitForFinished(); return false;
        }
        const QString text = QString::fromUtf8(process.readAll());
        const auto durationMatch = QRegularExpression(R"(Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?))").match(text);
        const auto videoMatch = QRegularExpression(R"(Video:.*?(\d{2,5})x(\d{2,5}).*?(\d+(?:\.\d+)?)\s*fps)").match(text);
        if (!durationMatch.hasMatch() || !videoMatch.hasMatch()) return false;

        info.path = path;
        info.duration = durationMatch.captured(1).toInt() * 3600.0 + durationMatch.captured(2).toInt() * 60.0 + durationMatch.captured(3).toDouble();
        info.width = videoMatch.captured(1).toInt();
        info.height = videoMatch.captured(2).toInt();
        info.fps = videoMatch.captured(3).toDouble();
        if (info.fps <= 0.0) info.fps = 30.0;
        info.hasAudio = text.contains("Audio:", Qt::CaseInsensitive);
        const auto rotateTag = QRegularExpression(R"(rotate\s*:\s*(-?\d+))").match(text);
        const auto matrix = QRegularExpression(R"(rotation of\s*(-?\d+(?:\.\d+)?)\s*degrees)").match(text);
        if (rotateTag.hasMatch()) info.rotation = rotateTag.captured(1).toInt();
        else if (matrix.hasMatch()) info.rotation = qRound(matrix.captured(1).toDouble());
        info.rotation = ((info.rotation % 360) + 360) % 360;
        const bool sideways = info.rotation == 90 || info.rotation == 270;
        info.displayWidth = sideways ? info.height : info.width;
        info.displayHeight = sideways ? info.width : info.height;
        info.totalFrames = qMax(1, qRound(info.duration * info.fps));
        return true;
    }

    void updateReadyState()
    {
        const bool leftReady = leftInfo.valid();
        const bool rightReady = rightInfo.valid();
        const QList<QWidget*> leftWidgets {leftUi.previous, leftUi.next, leftUi.slider, leftUi.frame, leftUi.x, leftUi.y, leftUi.zoom};
        const QList<QWidget*> rightWidgets {rightUi.previous, rightUi.next, rightUi.slider, rightUi.frame, rightUi.x, rightUi.y, rightUi.zoom};
        for (QWidget *w : leftWidgets) w->setEnabled(leftReady);
        for (QWidget *w : rightWidgets) w->setEnabled(rightReady);
        newProjectButton->setEnabled(!exportProcess);
        exportButton->setEnabled(leftReady && rightReady && !segments.empty() && !exportProcess);
        autoAlignButton->setEnabled(leftReady && rightReady && !exportProcess);
        autoCropButton->setEnabled(leftReady && rightReady && !exportProcess);
        autoColorButton->setEnabled(leftReady && rightReady && !exportProcess);
        colorReference->setEnabled(leftReady && rightReady && !exportProcess);
        resetColorButton->setEnabled(leftReady && rightReady);
        saveProjectButton->setEnabled(leftReady || rightReady);
        resetCropButton->setEnabled(leftReady && rightReady);
        for (QSpinBox *spin : {cropLeft, cropRight, cropTop, cropBottom})
            spin->setEnabled(leftReady && rightReady);
        resetAlignment->setEnabled(leftReady || rightReady);
        const QList<QWidget*> centeredWidgets {stereoWindowSlider, verticalSlider, zoomBalanceSlider,
                                               stereoWindowSpin, verticalSpin, zoomBalanceSpin,
                                               markSegmentStartButton, addSegmentButton};
        for (QWidget *widget : centeredWidgets)
            widget->setEnabled(leftReady && rightReady && !exportProcess);
        updateSegmentButton->setEnabled(leftReady && rightReady && selectedSegmentRow() >= 0 && !exportProcess);
        removeSegmentButton->setEnabled(leftReady && rightReady && selectedSegmentRow() >= 0 && !exportProcess);
        commonTimeline->setEnabled(leftReady && rightReady && !exportProcess);
        timelineMarkStartButton->setEnabled(leftReady && rightReady && !exportProcess);
        timelineAddEndButton->setEnabled(leftReady && rightReady && !exportProcess);
        updateSummary();
    }

    void stepFrame(bool left, int delta, bool honorLink = true)
    {
        EyeControls &ui = left ? leftUi : rightUi;
        const VideoInfo &info = left ? leftInfo : rightInfo;
        if (!info.valid()) return;
        ui.frame->setValue(qBound(0, ui.frame->value() + delta, info.totalFrames - 1));
        if (honorLink && linkStepping->isChecked()) {
            EyeControls &otherUi = left ? rightUi : leftUi;
            const VideoInfo &otherInfo = left ? rightInfo : leftInfo;
            if (otherInfo.valid()) otherUi.frame->setValue(qBound(0, otherUi.frame->value() + delta, otherInfo.totalFrames - 1));
        }
    }

    void requestPreview()
    {
        if (!exportProcess) previewTimer.start();
    }

    QImage extractFrame(const VideoInfo &info, int frameIndex, int maxWidth)
    {
        if (!info.valid()) return {};
        QProcess process;
        process.setProgram(ffmpegPath);
        const double time = frameIndex / info.fps;
        process.setArguments({"-hide_banner", "-loglevel", "error", "-ss", QString::number(time, 'f', 6), "-i", info.path,
                              "-frames:v", "1", "-vf", QString("scale='min(%1,iw)':-2").arg(maxWidth),
                              "-f", "image2pipe", "-vcodec", "png", "pipe:1"});
        process.start();
        if (!process.waitForFinished(12000)) {
            process.kill(); process.waitForFinished(); return {};
        }
        QImage image;
        image.loadFromData(process.readAllStandardOutput(), "PNG");
        return image;
    }

    static cv::Mat grayMat(const QImage &image)
    {
        const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
        cv::Mat wrapped(rgb.height(), rgb.width(), CV_8UC3,
                        const_cast<uchar*>(rgb.constBits()), rgb.bytesPerLine());
        cv::Mat gray;
        cv::cvtColor(wrapped, gray, cv::COLOR_RGB2GRAY);
        return gray;
    }

    AlignmentEstimate estimateAlignment(const QImage &leftImage, const QImage &rightImage) const
    {
        AlignmentEstimate result;
        if (leftImage.isNull() || rightImage.isNull()) return result;

        cv::Mat leftGray = grayMat(leftImage);
        cv::Mat rightGray = grayMat(rightImage);
        if (leftGray.empty() || rightGray.empty()) return result;
        if (rightGray.size() != leftGray.size())
            cv::resize(rightGray, rightGray, leftGray.size(), 0.0, 0.0, cv::INTER_AREA);

        // Local contrast normalization makes cross-camera exposure and tone
        // differences less important to feature detection.
        auto clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
        clahe->apply(leftGray, leftGray);
        clahe->apply(rightGray, rightGray);

        std::vector<cv::KeyPoint> leftKeys, rightKeys;
        cv::Mat leftDescriptors, rightDescriptors;
        auto sift = cv::SIFT::create(3500, 3, 0.025, 10.0, 1.6);
        sift->detectAndCompute(leftGray, cv::noArray(), leftKeys, leftDescriptors);
        sift->detectAndCompute(rightGray, cv::noArray(), rightKeys, rightDescriptors);
        if (leftDescriptors.empty() || rightDescriptors.empty()) return result;

        cv::BFMatcher matcher(cv::NORM_L2);
        std::vector<std::vector<cv::DMatch>> candidates;
        matcher.knnMatch(rightDescriptors, leftDescriptors, candidates, 2);
        std::vector<cv::DMatch> good;
        good.reserve(candidates.size());
        for (const auto &pair : candidates) {
            if (pair.size() >= 2 && pair[0].distance < 0.72f * pair[1].distance)
                good.push_back(pair[0]);
        }
        if (good.size() < 18) return result;
        std::sort(good.begin(), good.end(), [](const cv::DMatch &a, const cv::DMatch &b) {
            return a.distance < b.distance;
        });
        if (good.size() > 1800) good.resize(1800);

        std::vector<cv::Point2f> rightPoints, leftPoints;
        rightPoints.reserve(good.size());
        leftPoints.reserve(good.size());
        for (const cv::DMatch &match : good) {
            rightPoints.push_back(rightKeys[match.queryIdx].pt);
            leftPoints.push_back(leftKeys[match.trainIdx].pt);
        }

        cv::Mat inlierMask;
        cv::Mat affine = cv::estimateAffinePartial2D(
            rightPoints, leftPoints, inlierMask, cv::RANSAC, 3.0, 5000, 0.995, 25);
        if (affine.empty()) return result;
        affine.convertTo(affine, CV_64F);
        const int inliers = cv::countNonZero(inlierMask);
        const double inlierRatio = inliers / static_cast<double>(good.size());
        if (inliers < 18 || inlierRatio < 0.20) return result;

        const double a = affine.at<double>(0, 0);
        const double b = affine.at<double>(0, 1);
        const double c = affine.at<double>(1, 0);
        const double d = affine.at<double>(1, 1);
        const double tx = affine.at<double>(0, 2);
        const double ty = affine.at<double>(1, 2);
        const double scale = std::sqrt(a * a + c * c);
        const double rotation = std::atan2(c, a) * 180.0 / M_PI;
        if (!std::isfinite(scale) || scale < 0.60 || scale > 1.60 || std::abs(rotation) > 10.0)
            return result;

        const double centerX = leftGray.cols / 2.0;
        const double centerY = leftGray.rows / 2.0;
        const double mappedCenterX = a * centerX + b * centerY + tx;
        const double mappedCenterY = c * centerX + d * centerY + ty;
        result.scale = scale;
        result.x = (mappedCenterX - centerX) * leftInfo.displayWidth / leftGray.cols;
        result.y = (mappedCenterY - centerY) * leftInfo.displayHeight / leftGray.rows;
        result.rotationDegrees = rotation;
        result.matches = static_cast<int>(good.size());
        result.inliers = inliers;
        result.valid = true;
        return result;
    }

    static double medianValue(std::vector<double> values)
    {
        if (values.empty()) return 0.0;
        std::sort(values.begin(), values.end());
        const size_t middle = values.size() / 2;
        return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
    }

    void autoAlign()
    {
        if (!leftInfo.valid() || !rightInfo.valid() || exportProcess) return;
        autoAlignButton->setEnabled(false);
        status->setText("Finding matching features…");
        QApplication::setOverrideCursor(Qt::WaitCursor);

        std::vector<AlignmentEstimate> estimates;
        const int candidateOffsets[] = {0, -6, 6, -12, 12};
        int samplesAnalyzed = 0;
        try {
            for (int offset : candidateOffsets) {
                const int leftFrame = leftUi.frame->value() + offset;
                const int rightFrame = rightUi.frame->value() + offset;
                if (leftFrame < 0 || leftFrame >= leftInfo.totalFrames ||
                    rightFrame < 0 || rightFrame >= rightInfo.totalFrames)
                    continue;
                status->setText(QString("Auto alignment sample %1 of 3…").arg(samplesAnalyzed + 1));
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                const QImage left = extractFrame(leftInfo, leftFrame, 1280);
                const QImage right = extractFrame(rightInfo, rightFrame, 1280);
                AlignmentEstimate estimate = estimateAlignment(left, right);
                if (estimate.valid) estimates.push_back(estimate);
                if (++samplesAnalyzed == 3) break;
            }
        } catch (const cv::Exception &error) {
            QApplication::restoreOverrideCursor();
            autoAlignButton->setEnabled(true);
            status->setText("Auto alignment failed");
            QMessageBox::warning(this, "Auto alignment failed", QString::fromUtf8(error.what()));
            return;
        }

        QApplication::restoreOverrideCursor();
        autoAlignButton->setEnabled(true);
        status->setText("Ready");
        if (estimates.empty()) {
            QMessageBox::warning(this, "Could not auto align",
                "Not enough reliable matching features were found. Try a frame with clear, static background detail and similar exposure in both cameras.");
            return;
        }

        std::vector<double> scales, xs, ys, rotations, ratios;
        int totalMatches = 0;
        int totalInliers = 0;
        for (const AlignmentEstimate &estimate : estimates) {
            scales.push_back(estimate.scale);
            xs.push_back(estimate.x);
            ys.push_back(estimate.y);
            rotations.push_back(estimate.rotationDegrees);
            ratios.push_back(estimate.inliers / static_cast<double>(estimate.matches));
            totalMatches += estimate.matches;
            totalInliers += estimate.inliers;
        }
        const double scale = medianValue(scales);
        const double x = medianValue(xs);
        const double y = medianValue(ys);
        const double rotation = medianValue(rotations);
        const double ratio = medianValue(ratios);
        const QString confidence = estimates.size() == 3 && ratio >= 0.45 ? "High" : (ratio >= 0.28 ? "Medium" : "Low");

        QMessageBox box(this);
        box.setWindowTitle("Automatic alignment result");
        box.setIcon(QMessageBox::Information);
        box.setTextFormat(Qt::RichText);
        box.setText(QString(
            "<b>Relative right-eye zoom:</b> %1%<br>"
            "<b>Relative center shift:</b> X %2 px, Y %3 px<br>"
            "<b>Residual rotation:</b> %4° (reported, not applied)<br>"
            "<b>Confidence:</b> %5 — %6/%7 inliers from %8 usable frame pairs<br><br>"
            "Choose whether to apply only the lens/zoom correction or also center the dominant matched scene plane.")
            .arg(scale * 100.0, 0, 'f', 2).arg(x, 0, 'f', 1).arg(y, 0, 'f', 1)
            .arg(rotation, 0, 'f', 3).arg(confidence).arg(totalInliers).arg(totalMatches).arg(estimates.size()));
        QPushButton *applyAll = box.addButton("Apply zoom + X/Y", QMessageBox::AcceptRole);
        QPushButton *zoomOnly = box.addButton("Apply zoom only", QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(applyAll);
        box.exec();
        if (box.clickedButton() != applyAll && box.clickedButton() != zoomOnly) return;

        // Keep the stereo midpoint invariant.  The complementary zoom balance
        // also preserves the measured right/left scale ratio exactly.
        CenteredAdjustment adjustment = currentAdjustment();
        adjustment.zoomBalance = qBound(-75.0, 100.0 * (scale - 1.0) / (scale + 1.0), 75.0);
        if (box.clickedButton() == applyAll) {
            adjustment.stereoWindow = qRound(x / 2.0);
            adjustment.vertical = qRound(y / 2.0);
        }
        setCurrentAdjustment(adjustment);
        autoCropBorders();
        updateSummary();
        requestPreview();
    }

    QRect effectiveCropRect(int width, int height) const
    {
        if (width < 2 || height < 2) return QRect(0, 0, qMax(1, width), qMax(1, height));
        const int left = qBound(0, cropLeft->value(), width - 2);
        const int top = qBound(0, cropTop->value(), height - 2);
        const int right = qBound(0, cropRight->value(), width - left - 2);
        const int bottom = qBound(0, cropBottom->value(), height - top - 2);
        int croppedWidth = width - left - right;
        int croppedHeight = height - top - bottom;
        if (croppedWidth % 2) --croppedWidth;
        if (croppedHeight % 2) --croppedHeight;
        return QRect(left, top, qMax(2, croppedWidth), qMax(2, croppedHeight));
    }

    QRect safeCropForAdjustment(int width, int height, const CenteredAdjustment &adjustment) const
    {
        auto bounds = [width, height](int x, int y, double zoomPercent) {
            const double zoom = zoomPercent / 100.0;
            const int transformedWidth = qMax(2, qRound((width * zoom) / 2.0) * 2);
            const int transformedHeight = qMax(2, qRound((height * zoom) / 2.0) * 2);
            return QRect((width - transformedWidth) / 2 + x,
                         (height - transformedHeight) / 2 + y,
                         transformedWidth, transformedHeight);
        };
        const QRect leftBounds = bounds(-adjustment.stereoWindow, -adjustment.vertical,
                                        100.0 - adjustment.zoomBalance);
        const QRect rightBounds = bounds(adjustment.stereoWindow, adjustment.vertical,
                                         100.0 + adjustment.zoomBalance);
        const int x1 = qMax(0, qMax(leftBounds.x(), rightBounds.x()));
        const int y1 = qMax(0, qMax(leftBounds.y(), rightBounds.y()));
        int x2 = qMin(width, qMin(leftBounds.x() + leftBounds.width(), rightBounds.x() + rightBounds.width()));
        int y2 = qMin(height, qMin(leftBounds.y() + leftBounds.height(), rightBounds.y() + rightBounds.height()));
        if ((x2 - x1) % 2) --x2;
        if ((y2 - y1) % 2) --y2;
        return QRect(x1, y1, qMax(0, x2 - x1), qMax(0, y2 - y1));
    }

    QRect projectSafeCrop(int width, int height) const
    {
        QRect common(0, 0, width, height);
        for (const FrameSegment &segment : segments)
            common = common.intersected(safeCropForAdjustment(width, height, segment.adjustment));
        int w = common.width();
        int h = common.height();
        if (w % 2) --w;
        if (h % 2) --h;
        return QRect(common.x(), common.y(), qMax(0, w), qMax(0, h));
    }

    void autoCropBorders()
    {
        if (!leftInfo.valid() || !rightInfo.valid()) return;
        const int width = qMin(leftInfo.displayWidth, rightInfo.displayWidth);
        const int height = qMin(leftInfo.displayHeight, rightInfo.displayHeight);
        const QRect safe = safeCropForAdjustment(width, height, currentAdjustment());
        const int commonLeft = safe.x();
        const int commonTop = safe.y();
        const int commonRight = safe.x() + safe.width();
        const int commonBottom = safe.y() + safe.height();
        if (commonRight - commonLeft < 2 || commonBottom - commonTop < 2) {
            QMessageBox::warning(this, "Cannot auto crop",
                                 "The transformed videos do not have a usable common image area.");
            return;
        }
        cropLeft->setValue(commonLeft);
        cropRight->setValue(width - commonRight);
        cropTop->setValue(commonTop);
        cropBottom->setValue(height - commonBottom);
        const QRect crop = effectiveCropRect(width, height);
        cropRight->setValue(width - crop.x() - crop.width());
        cropBottom->setValue(height - crop.y() - crop.height());
        status->setText(QString("Auto crop: %1 × %2 per eye").arg(crop.width()).arg(crop.height()));
    }

    static QImage applyColorCorrection(const QImage &source, const ColorCorrection &correction)
    {
        if (source.isNull() || correction.isIdentity()) return source;
        QImage result = source.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < result.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb*>(result.scanLine(y));
            for (int x = 0; x < result.width(); ++x) {
                const QRgb pixel = line[x];
                const int red = qBound(0, qRound(qRed(pixel) * correction.gain[0] + correction.offset[0]), 255);
                const int green = qBound(0, qRound(qGreen(pixel) * correction.gain[1] + correction.offset[1]), 255);
                const int blue = qBound(0, qRound(qBlue(pixel) * correction.gain[2] + correction.offset[2]), 255);
                line[x] = qRgb(red, green, blue);
            }
        }
        return result;
    }

    static int histogramPercentile(const std::array<int, 256> &histogram, int count, double percentile)
    {
        const int target = qRound(qBound(0.0, percentile, 1.0) * qMax(0, count - 1));
        int accumulated = 0;
        for (int value = 0; value < 256; ++value) {
            accumulated += histogram[value];
            if (accumulated > target) return value;
        }
        return 255;
    }

    static bool estimateColorCorrection(const QImage &reference, const QImage &target,
                                        ColorCorrection &correction, QString &error)
    {
        if (reference.isNull() || target.isNull() || reference.size() != target.size()) {
            error = "The aligned frame images could not be compared.";
            return false;
        }
        std::array<std::array<int, 256>, 3> referenceHistogram {};
        std::array<std::array<int, 256>, 3> targetHistogram {};
        int count = 0;
        for (int y = 0; y < reference.height(); y += 2) {
            const QRgb *referenceLine = reinterpret_cast<const QRgb*>(reference.constScanLine(y));
            const QRgb *targetLine = reinterpret_cast<const QRgb*>(target.constScanLine(y));
            for (int x = 0; x < reference.width(); x += 2) {
                const QRgb base = referenceLine[x];
                const QRgb other = targetLine[x];
                const int baseLuma = qGray(base);
                const int otherLuma = qGray(other);
                if (baseLuma < 8 || otherLuma < 8 || baseLuma > 247 || otherLuma > 247) continue;
                const int baseChannels[3] {qRed(base), qGreen(base), qBlue(base)};
                const int otherChannels[3] {qRed(other), qGreen(other), qBlue(other)};
                for (int channel = 0; channel < 3; ++channel) {
                    ++referenceHistogram[channel][baseChannels[channel]];
                    ++targetHistogram[channel][otherChannels[channel]];
                }
                ++count;
            }
        }
        if (count < 1000) {
            error = "Not enough non-black, non-clipped image detail was available for reliable color matching.";
            return false;
        }
        correction = ColorCorrection{};
        for (int channel = 0; channel < 3; ++channel) {
            const int baseLow = histogramPercentile(referenceHistogram[channel], count, 0.10);
            const int baseMiddle = histogramPercentile(referenceHistogram[channel], count, 0.50);
            const int baseHigh = histogramPercentile(referenceHistogram[channel], count, 0.90);
            const int targetLow = histogramPercentile(targetHistogram[channel], count, 0.10);
            const int targetMiddle = histogramPercentile(targetHistogram[channel], count, 0.50);
            const int targetHigh = histogramPercentile(targetHistogram[channel], count, 0.90);
            if (targetHigh - targetLow < 8 || baseHigh - baseLow < 8) {
                error = "The selected frames do not contain enough tonal range for reliable color matching.";
                return false;
            }
            correction.gain[channel] = qBound(0.60,
                (baseHigh - baseLow) / static_cast<double>(targetHigh - targetLow), 1.67);
            correction.offset[channel] = qBound(-64.0,
                baseMiddle - correction.gain[channel] * targetMiddle, 64.0);
        }
        return true;
    }

    void updateColorSummary()
    {
        if (leftColor.isIdentity() && rightColor.isIdentity()) {
            colorSummary->setText("No color correction");
            return;
        }
        const bool rightWasCorrected = !rightColor.isIdentity();
        const ColorCorrection &correction = rightWasCorrected ? rightColor : leftColor;
        colorSummary->setText(QString("%1 corrected to %2  •  R %3/%4  G %5/%6  B %7/%8")
            .arg(rightWasCorrected ? "Right" : "Left", rightWasCorrected ? "left" : "right")
            .arg(correction.gain[0], 0, 'f', 3).arg(correction.offset[0], 0, 'f', 1)
            .arg(correction.gain[1], 0, 'f', 3).arg(correction.offset[1], 0, 'f', 1)
            .arg(correction.gain[2], 0, 'f', 3).arg(correction.offset[2], 0, 'f', 1));
    }

    void autoMatchColor()
    {
        if (!leftInfo.valid() || !rightInfo.valid()) return;
        status->setText("Analyzing color balance…");
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const int fullWidth = qMin(leftInfo.displayWidth, rightInfo.displayWidth);
        const int fullHeight = qMin(leftInfo.displayHeight, rightInfo.displayHeight);
        const double scale = qMin(1.0, 1200.0 / fullWidth);
        const QSize canvas(qMax(2, qRound(fullWidth * scale)), qMax(2, qRound(fullHeight * scale)));
        QImage leftRaw = extractFrame(leftInfo, leftUi.frame->value(), canvas.width());
        QImage rightRaw = extractFrame(rightInfo, rightUi.frame->value(), canvas.width());
        QImage left = alignedImage(leftRaw, leftUi.x->value(), leftUi.y->value(), leftUi.zoom->value(),
                                   fullWidth, fullHeight, canvas);
        QImage right = alignedImage(rightRaw, rightUi.x->value(), rightUi.y->value(), rightUi.zoom->value(),
                                    fullWidth, fullHeight, canvas);
        const QRect crop = effectiveCropRect(fullWidth, fullHeight);
        const int x1 = qRound(crop.x() * canvas.width() / static_cast<double>(fullWidth));
        const int y1 = qRound(crop.y() * canvas.height() / static_cast<double>(fullHeight));
        const int x2 = qRound((crop.x() + crop.width()) * canvas.width() / static_cast<double>(fullWidth));
        const int y2 = qRound((crop.y() + crop.height()) * canvas.height() / static_cast<double>(fullHeight));
        const QRect scaledCrop(x1, y1, qMax(1, x2 - x1), qMax(1, y2 - y1));
        left = left.copy(scaledCrop.intersected(left.rect()));
        right = right.copy(scaledCrop.intersected(right.rect()));

        ColorCorrection correction;
        QString error;
        const bool leftReference = colorReference->currentIndex() == 0;
        const bool ok = estimateColorCorrection(leftReference ? left : right,
                                                leftReference ? right : left, correction, error);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            status->setText("Color matching failed");
            QMessageBox::warning(this, "Color matching failed", error);
            return;
        }
        if (leftReference) {
            leftColor = ColorCorrection{};
            rightColor = correction;
        } else {
            rightColor = ColorCorrection{};
            leftColor = correction;
        }
        updateColorSummary();
        requestPreview();
        status->setText(QString("Matched %1 eye colors to the %2 eye")
                        .arg(leftReference ? "right" : "left", leftReference ? "left" : "right"));
    }

    static QImage alignedImage(const QImage &source, int x, int y, double zoomPercent,
                               int fullWidth, int fullHeight, QSize canvas)
    {
        QImage result(canvas, QImage::Format_RGB32);
        result.fill(Qt::black);
        if (source.isNull() || fullWidth <= 0 || fullHeight <= 0) return result;
        const double sx = canvas.width() / static_cast<double>(fullWidth);
        const double sy = canvas.height() / static_cast<double>(fullHeight);
        const double zoom = zoomPercent / 100.0;
        QPainter painter(&result);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSize target(qMax(1, qRound(canvas.width() * zoom)), qMax(1, qRound(canvas.height() * zoom)));
        const int drawX = qRound((canvas.width() - target.width()) / 2.0 + x * sx);
        const int drawY = qRound((canvas.height() - target.height()) / 2.0 + y * sy);
        painter.drawImage(QRect(drawX, drawY, target.width(), target.height()), source);
        return result;
    }

    static void overlayAlignmentGrid(QImage &image)
    {
        if (image.isNull()) return;
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, false);
        QPen minor(QColor(255, 255, 255, 95));
        minor.setWidth(1);
        painter.setPen(minor);
        // A full 10 x 10 field is more useful for judging small stereo drift
        // than the previous rule-of-thirds overlay.
        for (int part = 1; part < 10; ++part) {
            if (part == 5) continue;
            painter.drawLine(image.width() * part / 10, 0, image.width() * part / 10, image.height() - 1);
            painter.drawLine(0, image.height() * part / 10, image.width() - 1, image.height() * part / 10);
        }
        QPen center(QColor(0, 220, 255, 190));
        center.setWidth(1);
        painter.setPen(center);
        painter.drawLine(image.width() / 2, 0, image.width() / 2, image.height() - 1);
        painter.drawLine(0, image.height() / 2, image.width() - 1, image.height() / 2);
    }

    void updatePreview()
    {
        if (!leftInfo.valid() && !rightInfo.valid()) return;
        status->setText("Updating preview…");
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const int canvasW = leftInfo.valid() && rightInfo.valid() ? qMin(leftInfo.displayWidth, rightInfo.displayWidth)
                                                                 : (leftInfo.valid() ? leftInfo.displayWidth : rightInfo.displayWidth);
        const int canvasH = leftInfo.valid() && rightInfo.valid() ? qMin(leftInfo.displayHeight, rightInfo.displayHeight)
                                                                 : (leftInfo.valid() ? leftInfo.displayHeight : rightInfo.displayHeight);
        int availableW = previewMode->currentIndex() == 0 ? qMax(320, previewStack->width() / 2 - 10) : qMax(500, previewStack->width() - 10);
        int availableH = qMax(220, previewStack->height() - 8);
        double scale = qMin(availableW / static_cast<double>(canvasW), availableH / static_cast<double>(canvasH));
        scale = qMin(scale, 1.0);
        const QSize canvas(qMax(2, qRound(canvasW * scale)), qMax(2, qRound(canvasH * scale)));
        previewPixelsPerDisplayX = canvasW / static_cast<double>(canvas.width());
        previewPixelsPerDisplayY = canvasH / static_cast<double>(canvas.height());
        const int decodeWidth = qMin(1600, qMax(2, canvas.width()));
        QImage leftRaw = extractFrame(leftInfo, leftUi.frame->value(), decodeWidth);
        QImage rightRaw = extractFrame(rightInfo, rightUi.frame->value(), decodeWidth);
        leftRaw = applyColorCorrection(leftRaw, leftColor);
        rightRaw = applyColorCorrection(rightRaw, rightColor);
        QImage left = alignedImage(leftRaw, leftUi.x->value(), leftUi.y->value(), leftUi.zoom->value(), canvasW, canvasH, canvas);
        QImage right = alignedImage(rightRaw, rightUi.x->value(), rightUi.y->value(), rightUi.zoom->value(), canvasW, canvasH, canvas);
        const QRect sourceCrop = effectiveCropRect(canvasW, canvasH);
        const int previewLeft = qRound(sourceCrop.left() * canvas.width() / static_cast<double>(canvasW));
        const int previewTop = qRound(sourceCrop.top() * canvas.height() / static_cast<double>(canvasH));
        const int previewRight = qRound((sourceCrop.x() + sourceCrop.width()) * canvas.width() / static_cast<double>(canvasW));
        const int previewBottom = qRound((sourceCrop.y() + sourceCrop.height()) * canvas.height() / static_cast<double>(canvasH));
        const QRect previewCrop(previewLeft, previewTop, qMax(1, previewRight - previewLeft),
                                qMax(1, previewBottom - previewTop));
        left = left.copy(previewCrop.intersected(left.rect()));
        right = right.copy(previewCrop.intersected(right.rect()));
        const QSize croppedCanvas = left.size();

        if (previewMode->currentIndex() == 0) {
            // Keep portrait eyes as a compact centered pair instead of stretching
            // their labels across the full width of the window.
            if (showGrid->isChecked()) {
                overlayAlignmentGrid(left);
                overlayAlignmentGrid(right);
            }
            leftPreview->setFixedSize(croppedCanvas);
            rightPreview->setFixedSize(croppedCanvas);
            leftPreview->setPixmap(QPixmap::fromImage(left));
            rightPreview->setPixmap(QPixmap::fromImage(right));
        } else {
            QImage combined(croppedCanvas, QImage::Format_RGB32);
            if (previewMode->currentIndex() == 1) {
                for (int y = 0; y < croppedCanvas.height(); ++y) {
                    const QRgb *l = reinterpret_cast<const QRgb*>(left.constScanLine(y));
                    const QRgb *r = reinterpret_cast<const QRgb*>(right.constScanLine(y));
                    QRgb *out = reinterpret_cast<QRgb*>(combined.scanLine(y));
                    for (int x = 0; x < croppedCanvas.width(); ++x) out[x] = qRgb(qGray(l[x]), qGreen(r[x]), qBlue(r[x]));
                }
            } else {
                for (int y = 0; y < croppedCanvas.height(); ++y) {
                    const QRgb *l = reinterpret_cast<const QRgb*>(left.constScanLine(y));
                    const QRgb *r = reinterpret_cast<const QRgb*>(right.constScanLine(y));
                    QRgb *out = reinterpret_cast<QRgb*>(combined.scanLine(y));
                    for (int x = 0; x < croppedCanvas.width(); ++x) {
                        const int d = (qAbs(qRed(l[x])-qRed(r[x])) + qAbs(qGreen(l[x])-qGreen(r[x])) + qAbs(qBlue(l[x])-qBlue(r[x]))) / 3;
                        out[x] = qRgb(d, d, d);
                    }
                }
            }
            if (showGrid->isChecked()) overlayAlignmentGrid(combined);
            singlePreview->setPixmap(QPixmap::fromImage(combined));
        }
        QApplication::restoreOverrideCursor();
        status->setText("Ready");
    }

    void updateSummary()
    {
        if (!leftInfo.valid() || !rightInfo.valid()) {
            syncSummary->setText("Load both videos to begin.");
            return;
        }
        const double lt = leftUi.frame->value() / leftInfo.fps;
        const double rt = rightUi.frame->value() / rightInfo.fps;
        double projectLength = 0.0;
        for (const FrameSegment &segment : segments)
            projectLength += qMin((segment.leftEnd - segment.leftStart + 1) / leftInfo.fps,
                                  (segment.rightEnd - segment.rightStart + 1) / rightInfo.fps);
        const QString layout = QString("%1 preview").arg(
            leftInfo.displayHeight > leftInfo.displayWidth ? "Portrait" : "Landscape");
        const QRect crop = effectiveCropRect(qMin(leftInfo.displayWidth, rightInfo.displayWidth),
                                             qMin(leftInfo.displayHeight, rightInfo.displayHeight));
        syncSummary->setText(QString("%1  •  L frame %2 @ %3, zoom %4%  •  R frame %5 @ %6, zoom %7%  •  Crop %8×%9  •  Offset R−L: %10 s  •  Project: %11 ranges / %12")
            .arg(layout)
            .arg(leftUi.frame->value()).arg(secondsText(lt)).arg(leftUi.zoom->value(), 0, 'f', 1)
            .arg(rightUi.frame->value()).arg(secondsText(rt)).arg(rightUi.zoom->value(), 0, 'f', 1)
            .arg(crop.width()).arg(crop.height())
            .arg(rt - lt, 0, 'f', 3).arg(static_cast<int>(segments.size())).arg(secondsText(projectLength)));
    }

    static QString ffmpegColorFilter(const ColorCorrection &correction)
    {
        if (correction.isIdentity()) return {};
        const auto expression = [&correction](int channel) {
            const QString gain = QString::number(correction.gain[channel], 'f', 6);
            const QString offset = QString::number(correction.offset[channel], 'f', 6);
            return QString("clip(val*%1%2%3,0,255)")
                .arg(gain, correction.offset[channel] >= 0.0 ? "+" : "", offset);
        };
        return QString(",lutrgb=r='%1':g='%2':b='%3'")
            .arg(expression(0), expression(1), expression(2));
    }

    void startExport()
    {
        if (!leftInfo.valid() || !rightInfo.valid() || exportProcess) return;
        if (segments.empty()) {
            QMessageBox::information(this, "No project ranges",
                                     "Add at least one frame range to the project before exporting.");
            return;
        }
        exportDuration = 0.0;
        for (const FrameSegment &segment : segments)
            exportDuration += qMin((segment.leftEnd - segment.leftStart + 1) / leftInfo.fps,
                                   (segment.rightEnd - segment.rightStart + 1) / rightInfo.fps);
        QString suggested = QFileInfo(leftInfo.path).dir().filePath(QFileInfo(leftInfo.path).completeBaseName() + "_StereoAligned.mp4");
        exportOutput = QFileDialog::getSaveFileName(this, "Export aligned stereo video", suggested, "MP4 video (*.mp4)");
        if (exportOutput.isEmpty()) return;
        if (QFileInfo(exportOutput).suffix().isEmpty()) exportOutput += ".mp4";

        const int width = qMin(leftInfo.displayWidth, rightInfo.displayWidth);
        const int height = qMin(leftInfo.displayHeight, rightInfo.displayHeight);
        const QRect crop = projectSafeCrop(width, height);
        if (crop.width() < 2 || crop.height() < 2) {
            QMessageBox::warning(this, "No common image area",
                                 "The project ranges do not share a usable black-border-free crop. Reduce the adjustments in the extreme range.");
            return;
        }
        const double fps = qMin(leftInfo.fps, rightInfo.fps);
        auto eyeChain = [&](int input, double start, double duration, int x, int y,
                            double zoomPercent, const QString &name) {
            const double zoom = zoomPercent / 100.0;
            const int zoomWidth = qMax(2, qRound((width * zoom) / 2.0) * 2);
            const int zoomHeight = qMax(2, qRound((height * zoom) / 2.0) * 2);
            const int centeredX = (width - zoomWidth) / 2 + x;
            const int centeredY = (height - zoomHeight) / 2 + y;
            const ColorCorrection &correction = input == 0 ? leftColor : rightColor;
            return QString("[%1:v]trim=start=%2:duration=%3,setpts=PTS-STARTPTS,scale=%4:%5%8,setsar=1,fps=%6[eye%7];")
                .arg(input).arg(start, 0, 'f', 6).arg(duration, 0, 'f', 6).arg(zoomWidth).arg(zoomHeight)
                .arg(fps, 0, 'f', 6).arg(name).arg(ffmpegColorFilter(correction))
                + QString("color=c=black:s=%1x%2:r=%3:d=%4[base%5];[base%5][eye%5]overlay=x=%6:y=%7:shortest=1[placed%5];"
                          "[placed%5]crop=%8:%9:%10:%11[%5]")
                .arg(width).arg(height).arg(fps, 0, 'f', 6).arg(duration, 0, 'f', 6).arg(name)
                .arg(centeredX).arg(centeredY).arg(crop.width()).arg(crop.height()).arg(crop.x()).arg(crop.y());
        };
        QStringList chains;
        QStringList videoLabels;
        QStringList audioLabels;
        const int audioInput = audioSource->currentIndex() == 0 ? 0 : 1;
        const bool mapAudio = (audioInput == 0 && leftInfo.hasAudio && audioSource->currentIndex() != 2) ||
                              (audioInput == 1 && rightInfo.hasAudio && audioSource->currentIndex() != 2);
        for (int i = 0; i < static_cast<int>(segments.size()); ++i) {
            const FrameSegment &segment = segments[i];
            const double leftStart = segment.leftStart / leftInfo.fps;
            const double rightStart = segment.rightStart / rightInfo.fps;
            const double duration = qMin((segment.leftEnd - segment.leftStart + 1) / leftInfo.fps,
                                         (segment.rightEnd - segment.rightStart + 1) / rightInfo.fps);
            const QString l = QString("l%1").arg(i);
            const QString r = QString("r%1").arg(i);
            const QString v = QString("v%1").arg(i);
            chains << eyeChain(0, leftStart, duration, -segment.adjustment.stereoWindow,
                               -segment.adjustment.vertical, 100.0 - segment.adjustment.zoomBalance, l);
            chains << eyeChain(1, rightStart, duration, segment.adjustment.stereoWindow,
                               segment.adjustment.vertical, 100.0 + segment.adjustment.zoomBalance, r);
            const int mode = outputMode->currentIndex();
            const bool reverseEyes = (mode == 1) != swapOutputEyes->isChecked();
            const QString first = reverseEyes ? r : l;
            const QString second = reverseEyes ? l : r;
            if (mode == 0 || mode == 1)
                chains << QString("[%1][%2]hstack[%3]").arg(first, second, v);
            else if (mode == 3)
                chains << QString("[%1][%2]hstack,scale=%3:%4[%5]").arg(first, second).arg(crop.width()).arg(crop.height()).arg(v);
            else
                chains << QString("[%1][%2]hstack[sbs%3];[sbs%3]stereo3d=sbsl:arcd[%4]").arg(first, second).arg(i).arg(v);
            videoLabels << QString("[%1]").arg(v);
            if (mapAudio) {
                const double audioStart = audioInput == 0 ? leftStart : rightStart;
                const QString a = QString("a%1").arg(i);
                chains << QString("[%1:a]atrim=start=%2:duration=%3,asetpts=PTS-STARTPTS[%4]")
                              .arg(audioInput).arg(audioStart, 0, 'f', 6).arg(duration, 0, 'f', 6).arg(a);
                audioLabels << QString("[%1]").arg(a);
            }
        }
        if (segments.size() == 1) chains << "[v0]null[v]";
        else chains << videoLabels.join("") + QString("concat=n=%1:v=1:a=0[v]").arg(segments.size());
        if (mapAudio) {
            if (segments.size() == 1) chains << "[a0]anull[a]";
            else chains << audioLabels.join("") + QString("concat=n=%1:v=0:a=1[a]").arg(segments.size());
        }
        const QString filter = chains.join(';');

        QStringList args {"-progress", "pipe:1", "-y", "-i", leftInfo.path, "-i", rightInfo.path,
                          "-filter_complex", filter, "-map", "[v]"};
        if (mapAudio) args << "-map" << "[a]" << "-c:a" << "aac" << "-b:a" << "192k";
        args << "-c:v" << "libx264" << "-preset" << "fast" << "-crf" << "18" << "-pix_fmt" << "yuv420p"
             << "-movflags" << "+faststart" << exportOutput;

        exportProcess = new QProcess(this);
        cancelRequested = false;
        exportProcess->setProgram(ffmpegPath);
        exportProcess->setArguments(args);
        exportProcess->setProcessChannelMode(QProcess::MergedChannels);
        connect(exportProcess, &QProcess::readyReadStandardOutput, this, [this] { readExportProgress(); });
        connect(exportProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus state) { exportFinished(code, state); });
        progress->setValue(0);
        exportButton->setEnabled(false);
        cancelButton->setVisible(true);
        status->setText("Exporting…");
        exportProcess->start();
    }

    void readExportProgress()
    {
        if (!exportProcess) return;
        const QString text = QString::fromUtf8(exportProcess->readAllStandardOutput());
        const auto match = QRegularExpression(R"(out_time_ms=(\d+))").match(text);
        if (match.hasMatch() && exportDuration > 0.0) {
            const double seconds = match.captured(1).toDouble() / 1000000.0;
            progress->setValue(qBound(0, qRound(seconds * 1000.0 / exportDuration), 1000));
            status->setText(QString("Exporting %1%  •  %2 / %3").arg(progress->value() / 10.0, 0, 'f', 1)
                .arg(secondsText(seconds)).arg(secondsText(exportDuration)));
        }
    }

    void exportFinished(int code, QProcess::ExitStatus state)
    {
        const QString remaining = exportProcess ? QString::fromUtf8(exportProcess->readAll()) : QString();
        if (exportProcess) exportProcess->deleteLater();
        exportProcess = nullptr;
        cancelButton->setVisible(false);
        updateReadyState();
        if (cancelRequested) {
            status->setText("Export cancelled");
            progress->setValue(0);
        } else if (state == QProcess::NormalExit && code == 0) {
            progress->setValue(1000);
            status->setText("Export complete");
            QMessageBox::information(this, "Export complete", "Aligned stereo video saved to:\n" + QDir::toNativeSeparators(exportOutput));
        } else {
            status->setText("Export failed");
            QMessageBox::critical(this, "Export failed", "FFmpeg could not create the output.\n\n" + remaining.right(2500));
        }
        requestPreview();
    }
};

int main(int argc, char *argv[])
{
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    QApplication app(argc, argv);
    QApplication::setApplicationName("Stereo Aligner");
    QApplication::setOrganizationName("Stereo Tools");
    app.setStyle("Fusion");
    StereoAlignerWindow window;
    window.show();
    return app.exec();
}
