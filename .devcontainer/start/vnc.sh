#!/usr/bin/env bash
set -Eeuo pipefail

DISPLAY_NUM="${DISPLAY_NUM:-1}"
VNC_PORT=$((5900 + DISPLAY_NUM))
PASSWD_FILE="$HOME/.vnc/passwd"
LOG_FILE="$HOME/.vnc/startup.log"

if [ -z "${VNC_PASSWORD:-}" ]; then
    echo "ERROR: VNC_PASSWORD is not set."
    exit 1
fi

{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting VNC server"

    if ! pgrep -f "Xtigervnc.*:${DISPLAY_NUM}" >/dev/null 2>&1; then
        printf '%s\n' "$VNC_PASSWORD" | vncpasswd -f > "$PASSWD_FILE"
        chmod 600 "$PASSWD_FILE"

        echo "Starting vncserver on display :${DISPLAY_NUM}..."
        vncserver ":${DISPLAY_NUM}" \
            -geometry 1280x800 \
            -depth 24 \
            -localhost no

        sleep 1
        if ! pgrep -f "Xtigervnc.*:${DISPLAY_NUM}" >/dev/null 2>&1; then
            echo "ERROR: Failed to start vncserver."
            exit 1
        fi
    else
        echo "vncserver already running on :${DISPLAY_NUM}"
    fi

    echo "VNC server ready on :${DISPLAY_NUM}"
    echo "VNC Port: ${VNC_PORT}"
    echo "Startup log: ${LOG_FILE}"
} | tee -a "$LOG_FILE"
