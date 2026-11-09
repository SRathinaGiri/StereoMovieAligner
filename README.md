# Stereo Aligner

Stereo Aligner is a Qt 6 Widgets application for synchronizing and aligning two videos recorded by separate cameras. It is designed for matching-resolution phone videos (including 4K), while keeping preview decoding lightweight.

## Features

- Load independent left-eye and right-eye video files.
- Step either video backward or forward by exactly one frame.
- Optional linked stepping for moving both timelines together.
- Centered stereo-window and vertical alignment: every change moves the two eyes by equal amounts in opposite directions.
- Automatic portrait/landscape detection with compact side-by-side portrait previews.
- Preview-first workspace with a StereoMask-style left icon rail and dockable, scrollable controls drawer.
- Complementary center zoom: reducing one eye increases the other by the same percentage, keeping the adjustment centered.
- Fine, Medium and Long adjustment modes set zoom steps to 0.1%, 1% or 10% and movement steps to 1, 10 or 100 pixels.
- Sliders and precise spin boxes stay synchronized; previews can also be dragged to position the eyes and scrolled to adjust zoom.
- Optional full 10×10 alignment grid over the previews, with highlighted center axes.
- SIFT/RANSAC automatic alignment, inspired by the Autopano-Sift workflow historically used by StereoPhoto Maker. It analyzes three nearby frame pairs and offers zoom-only or zoom-plus-X/Y correction with a confidence report.
- Shared left, right, top and bottom cropping for both eyes, with automatic removal of black borders after auto alignment.
- Automatic RGB color-balance and contrast matching using either the left or right selected frame as the reference; the correction is included in previews and export.
- Build a project from multiple frame ranges, each with its own centered stereo-window, vertical and zoom adjustment.
- Shared synchronized timeline beneath the previews, with a gold pending-start marker and a different colored band for every project range.
- Editable range table with automatic `Range 1`, `Range 2`, … names and paired From/To frame columns.
- Save and reopen portable JSON projects containing both video paths, frame ranges, transforms, crop, color correction and interface/output choices.
- Start a clean project with **New** (`Ctrl+N`); an existing project receives a Save, Discard, or Cancel prompt before it is reset.
- Side-by-side, red/cyan anaglyph, and difference previews.
- Light and dark themes, with light selected by default.
- English/US number formatting independent of Windows regional settings.
- Parallel full side-by-side, cross-view full side-by-side, half side-by-side (HSBS), and red/cyan anaglyph MP4 export.
- Optional output eye-swap button for reversing the selected format without changing source assignments.
- Export includes only project frame ranges, concatenated in list order, with one largest common black-border-free crop across the whole movie.
- Choose audio from the left video, right video, or no audio.
- Keyboard shortcuts for quick synchronization and alignment.

## Requirements

- Qt 6 Widgets
- OpenCV 4 with Core, Imgproc, Features2D and Calib3D
- CMake 3.21 or newer
- FFmpeg available in `PATH`, or at `D:/stereo/ffmpeg.exe`

## Build on Windows

```powershell
$env:Path='D:\Qt\6.11.0\mingw_64\bin;D:\Qt\Tools\mingw1310_64\bin;D:\Qt\Tools\Ninja;' + $env:Path
& 'D:\Qt\Tools\CMake_64\bin\cmake.exe' -S . -B build-qt -G Ninja -DCMAKE_PREFIX_PATH='D:\Qt\6.11.0\mingw_64' -DOpenCV_DIR='D:\vcpkg\installed\x64-mingw-dynamic\share\opencv4' -DCMAKE_BUILD_TYPE=Release
& 'D:\Qt\Tools\CMake_64\bin\cmake.exe' --build build-qt
```

To make a portable folder, run `windeployqt` on the resulting executable and include the OpenCV runtime DLL and its `libz.dll` dependency. FFmpeg is not bundled.

## Basic workflow

1. Load the left and right videos.
2. Find the same visual event with the independent frame controls. `A/D` step the left video and `J/L` step the right video.
3. Click **Auto align selected frames** to measure relative zoom and positioning. Automatic results are divided equally and oppositely between both eyes.
4. Fine-tune the left, right, top and bottom crop values, or use **Auto crop black borders** again after manual transform changes.
5. Choose the left or right eye as the color reference and click **Auto match selected frames**. The reference eye is preserved while the other eye is corrected.
6. Choose **Fine**, **Medium**, or **Long**, then fine-tune the **Stereo window**, **Vertical**, and **Zoom balance** sliders or spin boxes. Dragging a preview uses the selected movement resolution. Over a preview, each wheel event changes zoom by one selected step; **Ctrl+wheel** changes horizontal position by one selected step and **Shift+wheel** changes vertical position by one selected step. Enable **Grid** for full-image alignment guides. A 90% left zoom, for example, always produces a 110% right zoom.
7. Use the shared timeline beneath the previews to scrub both synchronized eyes. Click **Mark start** to place the gold marker, move to the desired end, set the adjustment, and click **Add range end**. The new range appears as a colored timeline band and as a named row in the range table; double-click its name to rename it.
8. Select an output format and audio source, then click **Export project ranges**. Only listed ranges are exported; the app concatenates them and applies one common borderless crop.

Use **Save JSON** (`Ctrl+S`) to preserve the complete working state. **Open JSON** (`Ctrl+O`) restores it later. Video paths are stored both relative to the project file and as an absolute fallback, so a project can be moved together with its source videos.

The source videos are never modified.
