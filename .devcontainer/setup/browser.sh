#!/usr/bin/env bash
set -Eeuo pipefail

export DEBIAN_FRONTEND=noninteractive
export DEBIAN_PRIORITY=critical
export LANGUAGE=C.UTF-8
export LC_ALL=C.UTF-8

sudo apt-get update
sudo apt-get install -y --no-install-recommends wget gnupg ca-certificates

sudo install -d -m 0755 /etc/apt/keyrings
wget -q -O - https://dl.google.com/linux/linux_signing_key.pub | sudo gpg --dearmor -o /etc/apt/keyrings/google-chrome.gpg
sudo bash -c 'echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/google-chrome.gpg] https://dl.google.com/linux/chrome/deb/ stable main" > /etc/apt/sources.list.d/google-chrome.list'

sudo apt-get update
sudo apt-get install -y --no-install-recommends google-chrome-stable

if [ -x /usr/bin/google-chrome-stable ] && [ ! -e /usr/local/bin/google-chrome ]; then
    sudo ln -sf /usr/bin/google-chrome-stable /usr/local/bin/google-chrome
fi

if [ -x /usr/bin/google-chrome-stable ] && [ ! -e /usr/local/bin/chrome ]; then
    sudo ln -sf /usr/bin/google-chrome-stable /usr/local/bin/chrome
fi

sudo mkdir -p /usr/local/bin
sudo tee /usr/local/bin/x-www-browser >/dev/null <<'EOF'
#!/usr/bin/env bash
exec /usr/bin/google-chrome --password-store=basic --no-sandbox --disable-gpu --disable-dev-shm-usage "$@"
EOF
sudo chmod +x /usr/local/bin/x-www-browser

mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/web-browser.desktop" <<'EOF'
[Desktop Entry]
Version=1.0
Type=Application
Name=Web Browser
Comment=Open the default web browser
Exec=/usr/local/bin/x-www-browser %U
Icon=web-browser
Terminal=false
Categories=Network;WebBrowser;
StartupNotify=true
EOF
