#!/usr/bin/env bash
set -Eeuo pipefail

DISPLAY_NUM="${DISPLAY_NUM:-1}"
NOVNC_PORT="${NOVNC_PORT:-6080}"
VNC_PORT=$((5900 + DISPLAY_NUM))
LOG_FILE="$HOME/.vnc/startup.log"

{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting noVNC bridge"

    if ! pgrep -f "websockify.*${NOVNC_PORT}" >/dev/null 2>&1; then
        echo "Starting websockify on port ${NOVNC_PORT}..."
        websockify \
            --web=/usr/share/novnc/ \
            "$NOVNC_PORT" \
            "localhost:${VNC_PORT}" \
            >>"$HOME/.novnc.log" 2>&1 &

        sleep 1
        if ! pgrep -f "websockify.*${NOVNC_PORT}" >/dev/null 2>&1; then
            echo "ERROR: Failed to start websockify."
            echo "Check log: $HOME/.novnc.log"
            exit 1
        fi
    else
        echo "websockify already running on port ${NOVNC_PORT}"
    fi

    echo "Web URL: http://localhost:${NOVNC_PORT}"
    echo "Startup log: ${LOG_FILE}"
} | tee -a "$LOG_FILE"
