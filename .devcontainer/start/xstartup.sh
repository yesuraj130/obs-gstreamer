#!/usr/bin/env bash
set -Eeuo pipefail

XSTARTUP="$HOME/.vnc/xstartup"

mkdir -p "$HOME/.vnc"
chmod 700 "$HOME/.vnc"

if [ ! -x "$XSTARTUP" ]; then
    echo "Creating VNC xstartup script..."
    cat > "$XSTARTUP" <<'EOF'
#!/bin/sh
unset SESSION_MANAGER
unset DBUS_SESSION_BUS_ADDRESS
exec startxfce4
EOF
    chmod +x "$XSTARTUP"
else
    echo "VNC xstartup script already exists"
fi
