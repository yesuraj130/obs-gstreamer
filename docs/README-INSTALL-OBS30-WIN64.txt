=============================================================
obs-gstreamer for OBS Studio 30+ / 32+ (64-bit / Windows 10/11)
Target: x86_64 (64-bit) with Native Qt6 Dock & GPU Encoders
=============================================================

Prerequisites:
1. Official 64-bit GStreamer MSVC Runtime (1.28+ unified installer):
   https://gstreamer.freedesktop.org/data/pkg/windows/
   * Download and run the 64-bit MSVC unified installer (gstreamer-1.0-msvc-x86_64-*.exe).
   * Ensure the GStreamer bin folder (e.g. C:\gstreamer\1.0\msvc_x86_64\bin) is added to your Windows PATH environment variable.
2. OBS Studio 30+ / 32+ (64-bit).

Installation:
1. Copy 'obs-plugins/64bit/obs-gstreamer.dll' into your OBS Studio 64-bit plugin folder:
   C:\Program Files\obs-studio\obs-plugins\64bit\obs-gstreamer.dll
2. Launch OBS Studio.
3. Access the dock from the top menu:
   Docks -> GStreamer Output
4. Click "+" in the dock to add your RTSP, WebRTC, or custom GStreamer pipeline output.
