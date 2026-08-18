#!/usr/bin/env bash
set -Eeuo pipefail

if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    if systemctl is-system-running >/dev/null 2>&1 || systemctl show >/dev/null 2>&1; then
        sudo systemctl enable --now tailscaled 2>/dev/null || true
        sudo systemctl start tailscaled 2>/dev/null || true
    fi
fi

if command -v service >/dev/null 2>&1; then
    sudo service tailscaled start 2>/dev/null || true
fi

if ! pgrep -x tailscaled >/dev/null 2>&1; then
    echo "Starting Tailscale daemon directly in this container..."
    sudo tailscaled >/tmp/tailscaled.log 2>&1 &
fi

if ! tailscale status >/dev/null 2>&1; then
    echo "Tailscale is not connected."
    if [ -n "${TAILSCALE_AUTH_KEY:-}" ]; then
        echo "Authenticating Tailscale with TAILSCALE_AUTH_KEY..."
        sudo tailscale up --authkey="${TAILSCALE_AUTH_KEY}"
    else
        echo "TAILSCALE_AUTH_KEY is not set; skipping Tailscale auth."
    fi
else
    echo "Tailscale already connected."
fi
