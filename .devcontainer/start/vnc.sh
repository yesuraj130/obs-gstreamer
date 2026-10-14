#!/usr/bin/env bash
set -Eeuo pipefail

DISPLAY_NUM="${DISPLAY_NUM:-1}"
VNC_PORT=$((5900 + DISPLAY_NUM))
NOVNC_PORT="${NOVNC_PORT:-6080}"
VNC_USER="${USER:-codespace}"
VNC_PASSWORD="${VNC_PASSWORD:-vncpassword}"
LOG_FILE="$HOME/.vnc/startup.log"

mkdir -p "$HOME/.vnc"

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
    require_ssl: false
EOF

{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting KasmVNC server"

    # Configure user password non-interactively if command exists
    if command -v kasmvncpasswd >/dev/null 2>&1; then
        echo -e "${VNC_PASSWORD}\n${VNC_PASSWORD}\n" | kasmvncpasswd -u "$VNC_USER" -wo >/dev/null 2>&1 || true
    fi

    if ! pgrep -f "Xkasmvnc.*:${DISPLAY_NUM}" >/dev/null 2>&1; then
        echo "Starting KasmVNC on display :${DISPLAY_NUM} (web port ${NOVNC_PORT})..."
        vncserver ":${DISPLAY_NUM}" \
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
