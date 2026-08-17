#!/usr/bin/env bash
set -e

echo "========================================"
echo "Installing OBS/GStreamer environment"
echo "========================================"

sudo apt-get update

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    gcc \
    git \
    pkg-config \
    meson \
    ninja-build \
    libglib2.0-dev \
    libobs-dev \
    obs-studio \
    gstreamer1.0-tools \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-x \
    gstreamer1.0-gl \
    xfce4 \
    xfce4-terminal \
    dbus-x11 \
    dbus-user-session \
    tigervnc-standalone-server \
    tigervnc-tools \
    novnc \
    websockify

echo
echo "========================================"
echo "Environment installed"
echo "========================================"

gcc --version | head -1
meson --version
ninja --version
gst-launch-1.0 --version | head -1
obs --version || true
