#!/bin/bash
set -e

DISPLAY_NUM="${DISPLAY_NUM:-1}"
VNC_PORT=$((5900 + DISPLAY_NUM))
NOVNC_PORT="${NOVNC_PORT:-6080}"
LOG_FILE="$HOME/.vnc/startup.log"

mkdir -p "$HOME/.vnc"

{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting VNC server"

    if [ -z "${VNC_PASSWORD:-}" ]; then
        echo "ERROR: VNC_PASSWORD is not set."
        exit 1
    fi

    # Check if VNC server is already running
    if ! pgrep -f "Xtigervnc.*:${DISPLAY_NUM}" >/dev/null 2>&1; then
        echo "Setting up VNC password..."
        printf '%s\n' "$VNC_PASSWORD" | vncpasswd -f > "$HOME/.vnc/passwd"
        chmod 600 "$HOME/.vnc/passwd"

        echo "Starting vncserver on display :${DISPLAY_NUM}..."
        vncserver ":${DISPLAY_NUM}" \
            -geometry 1280x800 \
            -depth 24 \
            -localhost yes

        # Verify vncserver started
        sleep 1
        if ! pgrep -f "Xtigervnc.*:${DISPLAY_NUM}" >/dev/null 2>&1; then
            echo "ERROR: Failed to start vncserver"
            exit 1
        fi
        echo "vncserver started successfully"
    else
        echo "vncserver already running on :${DISPLAY_NUM}"
    fi

    # Check if websockify is already running
    if ! pgrep -f "websockify.*${NOVNC_PORT}" >/dev/null 2>&1; then
        echo "Starting websockify on port ${NOVNC_PORT}..."
        websockify \
            --web=/usr/share/novnc/ \
            "$NOVNC_PORT" \
            "localhost:${VNC_PORT}" \
            >>"$HOME/.novnc.log" 2>&1 &

        # Verify websockify started
        sleep 1
        if ! pgrep -f "websockify.*${NOVNC_PORT}" >/dev/null 2>&1; then
            echo "ERROR: Failed to start websockify"
            echo "Check log: $HOME/.novnc.log"
            exit 1
        fi
        echo "websockify started successfully"
    else
        echo "websockify already running on port ${NOVNC_PORT}"
    fi

    echo ""
    echo "======================================"
    echo "VNC server ready"
    echo "======================================"
    echo "Display:  :${DISPLAY_NUM}"
    echo "VNC Port: ${VNC_PORT}"
    echo "Web URL:  http://localhost:${NOVNC_PORT}"
    echo "======================================"
    echo "Startup log: ${LOG_FILE}"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Startup complete"
} | tee -a "$LOG_FILE"
