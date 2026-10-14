#!/usr/bin/env bash
set -Eeuo pipefail

DISPLAY_NUM="${DISPLAY_NUM:-1}"
VNC_PORT=$((5900 + DISPLAY_NUM))
NOVNC_PORT="${NOVNC_PORT:-6080}"
VNC_USER="${USER:-codespace}"
VNC_PASSWORD="${VNC_PASSWORD:-vncpassword}"
LOG_FILE="$HOME/.vnc/startup.log"

mkdir -p "$HOME/.vnc"

# Ensure SSL certificate files exist for KasmVNC web server
if [ ! -f "$HOME/.vnc/self.pem" ] || [ ! -f "$HOME/.vnc/self.key" ]; then
    openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
        -keyout "$HOME/.vnc/self.key" \
        -out "$HOME/.vnc/self.pem" \
        -subj "/CN=localhost" 2>/dev/null || true
    chmod 600 "$HOME/.vnc/self.key" 2>/dev/null || true
    chmod 644 "$HOME/.vnc/self.pem" 2>/dev/null || true
fi

# Ensure default snakeoil exists in /etc/ssl if system requires it
if [ ! -f "/etc/ssl/private/ssl-cert-snakeoil.key" ] && command -v sudo >/dev/null 2>&1; then
    sudo mkdir -p /etc/ssl/private /etc/ssl/certs 2>/dev/null || true
    sudo cp "$HOME/.vnc/self.key" /etc/ssl/private/ssl-cert-snakeoil.key 2>/dev/null || true
    sudo cp "$HOME/.vnc/self.pem" /etc/ssl/certs/ssl-cert-snakeoil.pem 2>/dev/null || true
    sudo chmod 640 /etc/ssl/private/ssl-cert-snakeoil.key 2>/dev/null || true
    sudo chmod 644 /etc/ssl/certs/ssl-cert-snakeoil.pem 2>/dev/null || true
    sudo chown root:ssl-cert /etc/ssl/private/ssl-cert-snakeoil.key 2>/dev/null || true
fi

# Configure KasmVNC: HTTP mode on web port (Codespaces handles HTTPS edge)
cat > "$HOME/.vnc/kasmvnc.yaml" <<EOF
desktop:
  resolution:
    width: 1280
    height: 800
  allow_resize: true
network:
  protocol: http
  interface: 0.0.0.0
  websocket_port: ${NOVNC_PORT}
  ssl:
    pem_certificate: $HOME/.vnc/self.pem
    pem_key: $HOME/.vnc/self.key
    require_ssl: false
EOF

{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting KasmVNC server"

    # Auto-install KasmVNC if not yet installed
    if ! command -v kasmvncserver >/dev/null 2>&1; then
        echo "kasmvncserver not found. Automatically installing KasmVNC 1.5.0..."
        sudo apt-get remove -y tigervnc-standalone-server tigervnc-tools 2>/dev/null || true
        KASM_DEB="/tmp/kasmvncserver_noble.deb"
        curl -fsSL -o "$KASM_DEB" https://github.com/kasmtech/KasmVNC/releases/download/v1.5.0/kasmvncserver_noble_1.5.0_amd64.deb
        sudo apt-get update && sudo apt-get install -y "$KASM_DEB" ssl-cert
        rm -f "$KASM_DEB"
        sudo adduser "$VNC_USER" ssl-cert 2>/dev/null || true
        sudo ln -sf /usr/bin/kasmvncserver /usr/local/bin/vncserver 2>/dev/null || true
    fi

    # Configure user password non-interactively if command exists
    if command -v kasmvncpasswd >/dev/null 2>&1; then
        echo -e "${VNC_PASSWORD}\n${VNC_PASSWORD}\n" | kasmvncpasswd -u "$VNC_USER" -wo >/dev/null 2>&1 || true
    fi

    if ! pgrep -f "Xkasmvnc.*:${DISPLAY_NUM}" >/dev/null 2>&1; then
        echo "Starting KasmVNC on display :${DISPLAY_NUM} (web port ${NOVNC_PORT})..."
        kasmvncserver ":${DISPLAY_NUM}" \
            -select-de xfce \
            -websocketPort "${NOVNC_PORT}" \
            -httpd /usr/share/kasmvnc/www \
            -interface 0.0.0.0

        sleep 1
        if ! pgrep -f "Xkasmvnc.*:${DISPLAY_NUM}" >/dev/null 2>&1; then
            echo "ERROR: Failed to start KasmVNC."
            exit 1
        fi
    else
        echo "KasmVNC already running on :${DISPLAY_NUM}"
    fi

    echo "KasmVNC ready on :${DISPLAY_NUM}"
    echo "Web URL: http://localhost:${NOVNC_PORT}"
    echo "Startup log: ${LOG_FILE}"
} | tee -a "$LOG_FILE"
