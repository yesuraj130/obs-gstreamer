#!/usr/bin/env bash
set -Eeuo pipefail

XSTARTUP="$HOME/.vnc/xstartup"

mkdir -p "$HOME/.vnc"
chmod 700 "$HOME/.vnc"

echo "Creating VNC xstartup script..."
cat > "$XSTARTUP" <<'EOF'
#!/bin/sh
unset SESSION_MANAGER
unset DBUS_SESSION_BUS_ADDRESS
exec dbus-launch --exit-with-session xfce4-session
EOF
chmod +x "$XSTARTUP"
