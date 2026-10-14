#!/usr/bin/env bash
set -Eeuo pipefail

export DEBIAN_FRONTEND=noninteractive
export DEBIAN_PRIORITY=critical
export LANGUAGE=C.UTF-8
export LC_ALL=C.UTF-8

# 1. Install desktop environment & prerequisites
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    xfce4 \
    xfce4-terminal \
    dbus-x11 \
    dbus-user-session \
    ssl-cert \
    libxfont2 \
    autocutsel

# 2. Remove TigerVNC if present to avoid binary conflicts
sudo apt-get remove -y tigervnc-standalone-server tigervnc-tools 2>/dev/null || true

# 3. Install KasmVNC Server for Ubuntu Noble
if ! command -v kasmvncserver >/dev/null 2>&1; then
    echo "Downloading and installing KasmVNC Server 1.5.0..."
    KASM_DEB="/tmp/kasmvncserver_noble.deb"
    curl -fsSL -o "$KASM_DEB" https://github.com/kasmtech/KasmVNC/releases/download/v1.5.0/kasmvncserver_noble_1.5.0_amd64.deb
    sudo apt-get install -y "$KASM_DEB"
    rm -f "$KASM_DEB"
fi

# Ensure /usr/bin/vncserver points to kasmvncserver
sudo ln -sf /usr/bin/kasmvncserver /usr/local/bin/vncserver

# 3. Add user to ssl-cert group
CURRENT_USER="${USER:-codespace}"
sudo adduser "$CURRENT_USER" ssl-cert || true
