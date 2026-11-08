# Stereo Aligner

Stereo Aligner is a Qt 6 Widgets application for synchronizing and aligning two videos recorded by separate cameras. It is designed for matching-resolution phone videos (including 4K), while keeping preview decoding lightweight.

## Features

- Load independent left-eye and right-eye video files.
- Step either video backward or forward by exactly one frame.
- Optional linked stepping for moving both timelines together.
- Independent horizontal (X) and vertical (Y) alignment for both eyes.
- Automatic portrait/landscape detection with compact side-by-side portrait previews.
- Preview-first workspace with a StereoMask-style left icon rail and dockable, scrollable controls drawer.
- Independent 25–400% center zoom for each camera to compensate for different lenses and fields of view; zoom is included in export.
- SIFT/RANSAC automatic alignment, inspired by the Autopano-Sift workflow historically used by StereoPhoto Maker. It analyzes three nearby frame pairs and offers zoom-only or zoom-plus-X/Y correction with a confidence report.
- Shared left, right, top and bottom cropping for both eyes, with automatic removal of black borders after auto alignment.
- Automatic RGB color-balance and contrast matching using either the left or right selected frame as the reference; the correction is included in previews and export.
- Side-by-side, red/cyan anaglyph, and difference previews.
- Light and dark themes, with light selected by default.
- English/US number formatting independent of Windows regional settings.
- Full side-by-side, half side-by-side, and red/cyan anaglyph MP4 export.
- Export begins at the frame pair currently selected in the UI.
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
3. Click **Auto align selected frames** to measure the relative camera zoom. Review the confidence report, then apply zoom only or zoom with X/Y positioning. The largest safe common crop is applied automatically.
4. Fine-tune the left, right, top and bottom crop values, or use **Auto crop black borders** again after manual transform changes.
5. Choose the left or right eye as the color reference and click **Auto match selected frames**. The reference eye is preserved while the other eye is corrected.
6. Fine-tune horizontal, vertical and zoom values manually if needed. Hold `Shift` while using alignment shortcuts for 10-pixel steps.
7. Select an output format and audio source.
8. Click **Export from selected frames**. The current left and right frames become time zero in the output.

The source videos are never modified.
