#include <QtWidgets>
#include <cmath>

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
    QString ffmpegPath;

    QLabel *leftPreview = nullptr;
    QLabel *rightPreview = nullptr;
    QLabel *singlePreview = nullptr;
    QStackedWidget *previewStack = nullptr;
    QComboBox *previewMode = nullptr;
    QComboBox *themeSelector = nullptr;
    QComboBox *outputMode = nullptr;
    QComboBox *audioSource = nullptr;
    QCheckBox *linkStepping = nullptr;
    QPushButton *resetAlignment = nullptr;
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
        sbs->addWidget(leftPreview);
        sbs->addWidget(rightPreview);
        singlePreview = makePreviewLabel("PREVIEW");
        previewStack->addWidget(sbsPage);
        previewStack->addWidget(singlePreview);
        main->addWidget(previewStack, 1);

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
        drawerLayout->addWidget(makeFileCard(true));
        drawerLayout->addWidget(makeFileCard(false));
        drawerLayout->addWidget(makeTimelineCard(true));
        drawerLayout->addWidget(makeTimelineCard(false));

        auto *alignGroup = new QGroupBox("Transform");
        auto *align = new QGridLayout(alignGroup);
        align->setColumnStretch(1, 1);
        align->setColumnStretch(2, 1);
        align->addWidget(new QLabel("Eye"), 0, 0);
        align->addWidget(new QLabel("Left / right"), 0, 1);
        align->addWidget(new QLabel("Up / down"), 0, 2);
        setupOffsetSpin(leftUi.x);
        setupOffsetSpin(leftUi.y);
        setupOffsetSpin(rightUi.x);
        setupOffsetSpin(rightUi.y);
        setupZoomSpin(leftUi.zoom);
        setupZoomSpin(rightUi.zoom);
        align->addWidget(new QLabel("Left"), 1, 0);
        align->addWidget(leftUi.x, 1, 1);
        align->addWidget(leftUi.y, 1, 2);
        align->addWidget(new QLabel("Zoom"), 2, 0);
        align->addWidget(leftUi.zoom, 2, 1, 1, 2);
        align->addWidget(new QLabel("Right"), 3, 0);
        align->addWidget(rightUi.x, 3, 1);
        align->addWidget(rightUi.y, 3, 2);
        align->addWidget(new QLabel("Zoom"), 4, 0);
        align->addWidget(rightUi.zoom, 4, 1, 1, 2);
        resetAlignment = new QPushButton("Reset transforms");
        align->addWidget(resetAlignment, 5, 0, 1, 3);
        drawerLayout->addWidget(alignGroup);

        auto *exportGroup = new QGroupBox("Output");
        auto *exportGrid = new QGridLayout(exportGroup);
        outputMode = new QComboBox;
        outputMode->addItems({"Full side by side", "Half side by side", "Red / cyan anaglyph"});
        audioSource = new QComboBox;
        audioSource->addItems({"Left video", "Right video", "No audio"});
        exportGrid->addWidget(new QLabel("Format"), 0, 0);
        exportGrid->addWidget(outputMode, 0, 1);
        exportGrid->addWidget(new QLabel("Audio"), 1, 0);
        exportGrid->addWidget(audioSource, 1, 1);
        exportButton = new QPushButton("Export from selected frames");
        exportButton->setObjectName("primaryButton");
        cancelButton = new QPushButton("Cancel");
        cancelButton->setVisible(false);
        exportGrid->addWidget(exportButton, 2, 0, 1, 2);
        exportGrid->addWidget(cancelButton, 3, 0, 1, 2);
        drawerLayout->addWidget(exportGroup);

        auto *shortcutHint = new QLabel("A/D: left frame   J/L: right frame\nCtrl+arrows: left XY   Alt+arrows: right XY");
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
        controlsDock->setMinimumWidth(310);
        controlsDock->resize(340, height());
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
        QAction *openLeftAction = rail->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), "Open left video");
        QAction *openRightAction = rail->addAction(style()->standardIcon(QStyle::SP_DirOpenIcon), "Open right video");
        rail->addSeparator();
        QAction *exportAction = rail->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton), "Export aligned video");
        connect(openLeftAction, &QAction::triggered, this, [this] { chooseVideo(true); });
        connect(openRightAction, &QAction::triggered, this, [this] { chooseVideo(false); });
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

    void connectUi()
    {
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

        connect(previewMode, &QComboBox::currentIndexChanged, this, [this] {
            previewStack->setCurrentIndex(previewMode->currentIndex() == 0 ? 0 : 1);
            requestPreview();
        });
        connect(themeSelector, &QComboBox::currentIndexChanged, this, [this](int index) {
            QSettings().setValue("appearance/theme", index);
            applyTheme(index == 1);
        });
        connect(resetAlignment, &QPushButton::clicked, this, [this] {
            leftUi.x->setValue(0); leftUi.y->setValue(0);
            rightUi.x->setValue(0); rightUi.y->setValue(0);
            leftUi.zoom->setValue(100.0); rightUi.zoom->setValue(100.0);
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
        addAlignmentShortcuts();
    }

    void connectTimeline(EyeControls &ui)
    {
        connect(ui.slider, &QSlider::valueChanged, this, [this, &ui](int value) {
            if (ui.frame->value() != value) ui.frame->setValue(value);
            updateSummary(); requestPreview();
        });
        connect(ui.frame, &QSpinBox::valueChanged, this, [&ui](int value) {
            if (ui.slider->value() != value) ui.slider->setValue(value);
        });
    }

    void addShortcut(const QKeySequence &sequence, std::function<void()> callback)
    {
        auto *shortcut = new QShortcut(sequence, this);
        connect(shortcut, &QShortcut::activated, this, std::move(callback));
    }

    void addAlignmentShortcuts()
    {
        auto nudge = [this](QSpinBox *spin, int amount) { spin->setValue(spin->value() + amount); };
        addShortcut(QKeySequence("Ctrl+Left"), [=] { nudge(leftUi.x, -1); });
        addShortcut(QKeySequence("Ctrl+Right"), [=] { nudge(leftUi.x, 1); });
        addShortcut(QKeySequence("Ctrl+Up"), [=] { nudge(leftUi.y, -1); });
        addShortcut(QKeySequence("Ctrl+Down"), [=] { nudge(leftUi.y, 1); });
        addShortcut(QKeySequence("Alt+Left"), [=] { nudge(rightUi.x, -1); });
        addShortcut(QKeySequence("Alt+Right"), [=] { nudge(rightUi.x, 1); });
        addShortcut(QKeySequence("Alt+Up"), [=] { nudge(rightUi.y, -1); });
        addShortcut(QKeySequence("Alt+Down"), [=] { nudge(rightUi.y, 1); });
        addShortcut(QKeySequence("Ctrl+Shift+Left"), [=] { nudge(leftUi.x, -10); });
        addShortcut(QKeySequence("Ctrl+Shift+Right"), [=] { nudge(leftUi.x, 10); });
        addShortcut(QKeySequence("Ctrl+Shift+Up"), [=] { nudge(leftUi.y, -10); });
        addShortcut(QKeySequence("Ctrl+Shift+Down"), [=] { nudge(leftUi.y, 10); });
        addShortcut(QKeySequence("Alt+Shift+Left"), [=] { nudge(rightUi.x, -10); });
        addShortcut(QKeySequence("Alt+Shift+Right"), [=] { nudge(rightUi.x, 10); });
        addShortcut(QKeySequence("Alt+Shift+Up"), [=] { nudge(rightUi.y, -10); });
        addShortcut(QKeySequence("Alt+Shift+Down"), [=] { nudge(rightUi.y, 10); });
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
        exportButton->setEnabled(leftReady && rightReady && !exportProcess);
        resetAlignment->setEnabled(leftReady || rightReady);
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
        const int decodeWidth = qMin(1600, qMax(2, canvas.width()));
        QImage leftRaw = extractFrame(leftInfo, leftUi.frame->value(), decodeWidth);
        QImage rightRaw = extractFrame(rightInfo, rightUi.frame->value(), decodeWidth);
        QImage left = alignedImage(leftRaw, leftUi.x->value(), leftUi.y->value(), leftUi.zoom->value(), canvasW, canvasH, canvas);
        QImage right = alignedImage(rightRaw, rightUi.x->value(), rightUi.y->value(), rightUi.zoom->value(), canvasW, canvasH, canvas);

        if (previewMode->currentIndex() == 0) {
            // Keep portrait eyes as a compact centered pair instead of stretching
            // their labels across the full width of the window.
            leftPreview->setFixedSize(canvas);
            rightPreview->setFixedSize(canvas);
            leftPreview->setPixmap(QPixmap::fromImage(left));
            rightPreview->setPixmap(QPixmap::fromImage(right));
        } else {
            QImage combined(canvas, QImage::Format_RGB32);
            if (previewMode->currentIndex() == 1) {
                for (int y = 0; y < canvas.height(); ++y) {
                    const QRgb *l = reinterpret_cast<const QRgb*>(left.constScanLine(y));
                    const QRgb *r = reinterpret_cast<const QRgb*>(right.constScanLine(y));
                    QRgb *out = reinterpret_cast<QRgb*>(combined.scanLine(y));
                    for (int x = 0; x < canvas.width(); ++x) out[x] = qRgb(qGray(l[x]), qGreen(r[x]), qBlue(r[x]));
                }
            } else {
                for (int y = 0; y < canvas.height(); ++y) {
                    const QRgb *l = reinterpret_cast<const QRgb*>(left.constScanLine(y));
                    const QRgb *r = reinterpret_cast<const QRgb*>(right.constScanLine(y));
                    QRgb *out = reinterpret_cast<QRgb*>(combined.scanLine(y));
                    for (int x = 0; x < canvas.width(); ++x) {
                        const int d = (qAbs(qRed(l[x])-qRed(r[x])) + qAbs(qGreen(l[x])-qGreen(r[x])) + qAbs(qBlue(l[x])-qBlue(r[x]))) / 3;
                        out[x] = qRgb(d, d, d);
                    }
                }
            }
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
        const double remaining = qMin(leftInfo.duration - lt, rightInfo.duration - rt);
        const QString layout = QString("%1 preview").arg(
            leftInfo.displayHeight > leftInfo.displayWidth ? "Portrait" : "Landscape");
        syncSummary->setText(QString("%1  •  L frame %2 @ %3, zoom %4%  •  R frame %5 @ %6, zoom %7%  •  Offset R−L: %8 s  •  Export: %9")
            .arg(layout)
            .arg(leftUi.frame->value()).arg(secondsText(lt)).arg(leftUi.zoom->value(), 0, 'f', 1)
            .arg(rightUi.frame->value()).arg(secondsText(rt)).arg(rightUi.zoom->value(), 0, 'f', 1)
            .arg(rt - lt, 0, 'f', 3).arg(secondsText(remaining)));
    }

    void startExport()
    {
        if (!leftInfo.valid() || !rightInfo.valid() || exportProcess) return;
        const double leftStart = leftUi.frame->value() / leftInfo.fps;
        const double rightStart = rightUi.frame->value() / rightInfo.fps;
        exportDuration = qMin(leftInfo.duration - leftStart, rightInfo.duration - rightStart);
        if (exportDuration <= 0.05) {
            QMessageBox::warning(this, "Nothing to export", "The selected frame pair is too close to the end of a video.");
            return;
        }
        QString suggested = QFileInfo(leftInfo.path).dir().filePath(QFileInfo(leftInfo.path).completeBaseName() + "_StereoAligned.mp4");
        exportOutput = QFileDialog::getSaveFileName(this, "Export aligned stereo video", suggested, "MP4 video (*.mp4)");
        if (exportOutput.isEmpty()) return;
        if (QFileInfo(exportOutput).suffix().isEmpty()) exportOutput += ".mp4";

        const int width = qMin(leftInfo.displayWidth, rightInfo.displayWidth);
        const int height = qMin(leftInfo.displayHeight, rightInfo.displayHeight);
        const double fps = qMin(leftInfo.fps, rightInfo.fps);
        auto eyeChain = [&](int input, double start, int x, int y, double zoomPercent, const QString &name) {
            const double zoom = zoomPercent / 100.0;
            const int zoomWidth = qMax(2, qRound((width * zoom) / 2.0) * 2);
            const int zoomHeight = qMax(2, qRound((height * zoom) / 2.0) * 2);
            const int centeredX = (width - zoomWidth) / 2 + x;
            const int centeredY = (height - zoomHeight) / 2 + y;
            return QString("[%1:v]trim=start=%2:duration=%3,setpts=PTS-STARTPTS,scale=%4:%5,setsar=1,fps=%6[eye%7];")
                .arg(input).arg(start, 0, 'f', 6).arg(exportDuration, 0, 'f', 6).arg(zoomWidth).arg(zoomHeight).arg(fps, 0, 'f', 6).arg(name)
                + QString("color=c=black:s=%1x%2:r=%3:d=%4[base%5];[base%5][eye%5]overlay=x=%6:y=%7:shortest=1[%5]")
                .arg(width).arg(height).arg(fps, 0, 'f', 6).arg(exportDuration, 0, 'f', 6).arg(name).arg(centeredX).arg(centeredY);
        };
        QString filter = eyeChain(0, leftStart, leftUi.x->value(), leftUi.y->value(), leftUi.zoom->value(), "l") + ";"
                       + eyeChain(1, rightStart, rightUi.x->value(), rightUi.y->value(), rightUi.zoom->value(), "r") + ";";
        if (outputMode->currentIndex() == 0) filter += "[l][r]hstack[v]";
        else if (outputMode->currentIndex() == 1) filter += QString("[l][r]hstack,scale=%1:%2[v]").arg(width).arg(height);
        else filter += "[l][r]hstack[sbs];[sbs]stereo3d=sbsl:arcd[v]";

        bool mapAudio = false;
        if (audioSource->currentIndex() == 0 && leftInfo.hasAudio) {
            filter += QString(";[0:a]atrim=start=%1:duration=%2,asetpts=PTS-STARTPTS[a]").arg(leftStart, 0, 'f', 6).arg(exportDuration, 0, 'f', 6);
            mapAudio = true;
        } else if (audioSource->currentIndex() == 1 && rightInfo.hasAudio) {
            filter += QString(";[1:a]atrim=start=%1:duration=%2,asetpts=PTS-STARTPTS[a]").arg(rightStart, 0, 'f', 6).arg(exportDuration, 0, 'f', 6);
            mapAudio = true;
        }

        QStringList args {"-progress", "pipe:1", "-y", "-i", leftInfo.path, "-i", rightInfo.path,
                          "-filter_complex", filter, "-map", "[v]"};
        if (mapAudio) args << "-map" << "[a]" << "-c:a" << "aac" << "-b:a" << "192k";
        args << "-c:v" << "libx264" << "-preset" << "fast" << "-crf" << "18" << "-pix_fmt" << "yuv420p"
             << "-movflags" << "+faststart" << "-t" << QString::number(exportDuration, 'f', 6) << exportOutput;

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
        exportButton->setEnabled(leftInfo.valid() && rightInfo.valid());
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
