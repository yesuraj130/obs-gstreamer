#!/bin/bash
set -e

DISPLAY_NUM="${DISPLAY_NUM:-1}"
VNC_PORT=$((5900 + DISPLAY_NUM))
NOVNC_PORT="${NOVNC_PORT:-6080}"

mkdir -p "$HOME/.vnc"

if [ -z "${VNC_PASSWORD:-}" ]; then
    echo "ERROR: VNC_PASSWORD is not set."
    exit 1
fi

if ! pgrep -f "Xtigervnc.*:${DISPLAY_NUM}" >/dev/null; then
    printf '%s\n' "$VNC_PASSWORD" | vncpasswd -f > "$HOME/.vnc/passwd"
    chmod 600 "$HOME/.vnc/passwd"

    vncserver ":${DISPLAY_NUM}" \
        -geometry 1280x800 \
        -depth 24 \
        -localhost yes
fi

if ! pgrep -f "websockify.*${NOVNC_PORT}" >/dev/null; then
    websockify \
        --web=/usr/share/novnc/ \
        "$NOVNC_PORT" \
        "localhost:${VNC_PORT}" \
        >"$HOME/.novnc.log" 2>&1 &
fi

echo "VNC display :${DISPLAY_NUM}"
echo "VNC port     :${VNC_PORT}"
echo "noVNC port   :${NOVNC_PORT}"
