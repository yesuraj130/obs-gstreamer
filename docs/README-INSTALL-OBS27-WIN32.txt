=============================================================
obs-gstreamer for OBS Studio 27.2.4 (32-bit / Windows 7 SP1)
Target: x86 (32-bit), Intel Core i3-3217U / Ivy Bridge HD 4000
=============================================================

Prerequisites:
1. Windows 7 SP1 with KB4474419 (SHA-2) and Universal CRT (KB2999226).
2. Official 32-bit GStreamer Runtime (MinGW version 1.18.6 or 1.20.7 x86):
   https://gstreamer.freedesktop.org/data/pkg/windows/1.18.6/mingw/gstreamer-1.0-mingw-x86-1.18.6.msi
   * Ensure the GStreamer bin folder (e.g. C:\gstreamer\1.0\mingw_x86\bin) is added to your Windows PATH.
3. OBS Studio 27.2.4 (32-bit):
   https://github.com/obsproject/obs-studio/releases/tag/27.2.4

Installation:
Copy 'obs-plugins/32bit/obs-gstreamer.dll' into your OBS Studio 32-bit plugin folder:
C:\Program Files (x86)\obs-studio\obs-plugins\32bit\obs-gstreamer.dll

Performance Tips for Intel Core i3-3217U:
- Set OBS canvas and output to 1280x720 @ 30 FPS.
- Use Intel Quick Sync hardware elements (qsvh264dec / qsvh264enc) for low CPU usage.
