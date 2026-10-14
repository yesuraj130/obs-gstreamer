#!/usr/bin/env bash
set -Eeuo pipefail

NOVNC_PORT="${NOVNC_PORT:-6080}"
LOG_FILE="$HOME/.vnc/startup.log"

{
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Web client served natively by KasmVNC on port ${NOVNC_PORT}"
} | tee -a "$LOG_FILE"
