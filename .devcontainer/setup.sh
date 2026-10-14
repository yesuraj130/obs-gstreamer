#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export DEBIAN_FRONTEND=noninteractive
export DEBIAN_PRIORITY=critical
export LANGUAGE=C.UTF-8
export LC_ALL=C.UTF-8

sudo apt-get update

for script in \
    "$SCRIPT_DIR/setup/obs.sh" \
    "$SCRIPT_DIR/setup/gstreamer.sh" \
    "$SCRIPT_DIR/setup/novnc.sh" \
    "$SCRIPT_DIR/setup/tigervnc.sh" \
    "$SCRIPT_DIR/setup/tailscale.sh" \
    "$SCRIPT_DIR/setup/browser.sh" \
    "$SCRIPT_DIR/setup/clear-cache.sh"
do
    echo "========================================"
    echo "Running ${script##*/}"
    echo "========================================"
    bash "$script"
done

echo
echo "========================================"
echo "One-time environment setup complete"
echo "========================================"

echo "Run .devcontainer/start.sh after each codespace reconnect or restart."
