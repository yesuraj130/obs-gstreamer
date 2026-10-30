#!/usr/bin/env bash
set -Eeuo pipefail

export DEBIAN_FRONTEND=noninteractive
export DEBIAN_PRIORITY=critical
export LANGUAGE=C.UTF-8
export LC_ALL=C.UTF-8

# Add official OBS Studio PPA to install latest OBS 32.X (upgraded from 30.X)
if ! grep -rq "obsproject/obs-studio" /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
    sudo DEBIAN_FRONTEND=noninteractive apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends software-properties-common
    sudo add-apt-repository -y ppa:obsproject/obs-studio
    sudo DEBIAN_FRONTEND=noninteractive apt-get update
fi

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    gcc \
    g++ \
    gdb \
    valgrind \
    git \
    pkg-config \
    meson \
    ninja-build \
    libglib2.0-dev \
    libobs-dev \
    obs-studio \
    qt6-base-dev

# Ensure libobs 32 libraries and headers are active
if [ -d /opt/obs-studio-32/AppDir/.junest/usr/lib ]; then
    echo "Updating system libobs to OBS 32..."
    sudo cp /opt/obs-studio-32/AppDir/.junest/usr/lib/libobs.so.30 /usr/lib/x86_64-linux-gnu/ 2>/dev/null || true
    sudo cp /opt/obs-studio-32/AppDir/.junest/usr/lib/libobs-frontend-api.so.30 /usr/lib/x86_64-linux-gnu/ 2>/dev/null || true
    sudo cp /opt/obs-studio-32/AppDir/.junest/usr/lib/libobs-opengl.so.30 /usr/lib/x86_64-linux-gnu/ 2>/dev/null || true
    sudo cp /opt/obs-studio-32/AppDir/.junest/usr/lib/libobs-scripting.so.30 /usr/lib/x86_64-linux-gnu/ 2>/dev/null || true
    sudo ln -sf libobs.so.30 /usr/lib/x86_64-linux-gnu/libobs.so
    sudo ln -sf libobs.so.30 /usr/lib/x86_64-linux-gnu/libobs.so.0
    sudo ln -sf libobs-frontend-api.so.30 /usr/lib/x86_64-linux-gnu/libobs-frontend-api.so
    sudo ln -sf libobs-frontend-api.so.30 /usr/lib/x86_64-linux-gnu/libobs-frontend-api.so.0
    sudo ldconfig
fi

# Install Move Transition plugin (obs-move-transition by Exeldro)
if [ ! -f /opt/obs-studio-32/AppDir/.junest/usr/lib/obs-plugins/move-transition.so ]; then
    echo "Installing Move Transition plugin..."
    curl -sL -o /tmp/move-transition.deb "https://github.com/exeldro/obs-move-transition/releases/download/3.2.1/move-transition-3.2.1-x86_64-linux-gnu.deb"
    sudo dpkg -i /tmp/move-transition.deb || true
    sudo mkdir -p /opt/obs-studio-32/AppDir/.junest/usr/lib/obs-plugins
    sudo cp /usr/lib/x86_64-linux-gnu/obs-plugins/move-transition.so /opt/obs-studio-32/AppDir/.junest/usr/lib/obs-plugins/
    sudo chmod 755 /opt/obs-studio-32/AppDir/.junest/usr/lib/obs-plugins/move-transition.so
    sudo mkdir -p /opt/obs-studio-32/AppDir/.junest/usr/share/obs/obs-plugins/move-transition
    sudo cp -r /usr/share/obs/obs-plugins/move-transition/* /opt/obs-studio-32/AppDir/.junest/usr/share/obs/obs-plugins/move-transition/ 2>/dev/null || true
    rm -f /tmp/move-transition.deb
fi

# Apply saved OBS configuration (settings, docks, profile, scenes & sources) on initial OBS install
REPO_OBS_CFG="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/obs-studio-config"
if [ -d "$REPO_OBS_CFG" ] && [ ! -d /root/.config/obs-studio ]; then
    echo "Applying initial OBS settings, docks, scenes, and sources..."
    mkdir -p /root/.config/obs-studio
    cp -rf "$REPO_OBS_CFG"/* /root/.config/obs-studio/ 2>/dev/null || true
fi
