#!/usr/bin/env bash
set -Eeuo pipefail

LOG_FILE="$HOME/.vnc/startup.log"

{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting Web/WHEP dev server on port 3000"

    if ! pgrep -f "node.*/app/applet/server.js" >/dev/null 2>&1; then
        echo "Starting Node dev server..."
        node /app/applet/server.js >>"$HOME/.server.log" 2>&1 &

        sleep 1
        if ! pgrep -f "node.*/app/applet/server.js" >/dev/null 2>&1; then
            echo "ERROR: Failed to start Node dev server."
            echo "Check log: $HOME/.server.log"
            exit 1
        fi
    else
        echo "Node dev server already running on port 3000"
    fi
} | tee -a "$LOG_FILE"
