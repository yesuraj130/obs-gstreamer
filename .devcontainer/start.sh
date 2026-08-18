#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for script in \
    "$SCRIPT_DIR/start/tailscale.sh" \
    "$SCRIPT_DIR/start/xstartup.sh" \
    "$SCRIPT_DIR/start/vnc.sh" \
    "$SCRIPT_DIR/start/novnc.sh"
do
    echo "========================================"
    echo "Running ${script##*/}"
    echo "========================================"
    bash "$script"
done
