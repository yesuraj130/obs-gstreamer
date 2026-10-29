const http = require('http');
const fs = require('fs');
const path = require('path');
const net = require('net');
const { exec, spawn } = require('child_process');

const PORT = 3000;
const WEB_ROOT = path.join(__dirname, '.webrtc-webroot');
const TARGET_HOST = '127.0.0.1';
const TARGET_PORT = 8888;
const NOVNC_PORT = 6080;
const VNC_PORT = 5901;

let setupProcess = null;
let setupLogs = [];
let isSettingUp = false;

function addLog(msg) {
  const line = `[${new Date().toLocaleTimeString()}] ${msg}`;
  setupLogs.push(line);
  if (setupLogs.length > 100) setupLogs.shift();
  console.log(line);
}

let tcpTunnelInfo = {
  host: '',
  port: '',
  full: '',
  status: 'connecting',
  lastUpdated: null
};

let tunnelProcess = null;

function startTCPTunnel() {
  if (tunnelProcess) {
    try { tunnelProcess.kill(); } catch (e) {}
    tunnelProcess = null;
  }

  addLog('Starting public TCP tunnel for VNC (port 5901)...');
  tcpTunnelInfo.status = 'connecting';

  try {
    const proc = spawn('ssh', [
      '-p', '443',
      '-o', 'StrictHostKeyChecking=no',
      '-o', 'ServerAliveInterval=30',
      '-o', 'ServerAliveCountMax=3',
      '-R', '0:localhost:5901',
      'tcp@a.pinggy.io'
    ]);

    tunnelProcess = proc;

    const parseOutput = (chunk) => {
      const text = chunk.toString();
      const match = text.match(/tcp:\/\/([^:\s]+):(\d+)/);
      if (match) {
        tcpTunnelInfo = {
          host: match[1],
          port: match[2],
          full: `${match[1]}:${match[2]}`,
          status: 'online',
          lastUpdated: new Date().toISOString()
        };
        addLog(`Public VNC TCP Tunnel online: ${tcpTunnelInfo.full}`);
      }
    };

    proc.stdout.on('data', parseOutput);
    proc.stderr.on('data', parseOutput);

    proc.on('close', () => {
      addLog('TCP tunnel closed. Reconnecting in 5s...');
      tcpTunnelInfo.status = 'offline';
      tunnelProcess = null;
      setTimeout(startTCPTunnel, 5000);
    });

    proc.on('error', (err) => {
      addLog(`TCP tunnel error: ${err.message}`);
      tcpTunnelInfo.status = 'offline';
    });
  } catch (e) {
    addLog(`TCP tunnel spawn error: ${e.message}`);
  }
}

// Start public tunnel on server init
startTCPTunnel();

function checkPort(port, host, callback) {
  const socket = new net.Socket();
  let reported = false;
  socket.setTimeout(800);

  socket.on('connect', () => {
    reported = true;
    socket.destroy();
    callback(null, true);
  });

  socket.on('timeout', () => {
    reported = true;
    socket.destroy();
    callback(null, false, 'Timed out');
  });

  socket.on('error', (err) => {
    if (!reported) {
      reported = true;
      callback(null, false, err.message);
    }
  });

  socket.connect(port, host);
}

let isRestarting = false;

function ensureVNC(callback) {
  if (isRestarting) {
    if (callback) callback();
    return;
  }
  exec('pgrep -x Xtigervnc >/dev/null && echo "1" || echo "0"', (err, stdout) => {
    const vncOnline = stdout && stdout.trim() === '1';
    if (vncOnline) {
      ensureWebsockify(callback);
      return;
    }
    addLog('TigerVNC offline. Starting TigerVNC on :1...');
    const startVncScript = `
      mkdir -p /root/.vnc /root/.config/tigervnc
      echo '#!/bin/sh
unset SESSION_MANAGER
unset DBUS_SESSION_BUS_ADDRESS
exec startxfce4' > /root/.vnc/xstartup
      chmod +x /root/.vnc/xstartup

      echo "vncpassword" | vncpasswd -f > /root/.vnc/passwd 2>/dev/null || true
      chmod 600 /root/.vnc/passwd 2>/dev/null || true
      cp /root/.vnc/passwd /root/.config/tigervnc/passwd 2>/dev/null || true
      chmod 600 /root/.config/tigervnc/passwd 2>/dev/null || true

      vncserver -kill :1 >/dev/null 2>&1 || true
      rm -rf /tmp/.X1-lock /tmp/.X11-unix/X1 2>/dev/null || true

      vncserver :1 -geometry 1280x800 -depth 24 -localhost no -PasswordFile /root/.vnc/passwd -SecurityTypes VncAuth -UseBlacklist=0 -AlwaysShared=1 >/tmp/vnc.log 2>&1 || \
      vncserver :1 -geometry 1280x800 -depth 24 -localhost no -UseBlacklist=0 -AlwaysShared=1 >/tmp/vnc.log 2>&1 || true
    `;
    exec(startVncScript, (startErr) => {
      if (startErr) addLog('VNC startup error: ' + startErr.message);
      else addLog('TigerVNC started on :1');
      ensureWebsockify(callback);
    });
  });
}

function ensureWebsockify(callback) {
  exec('pgrep -x websockify >/dev/null && echo "1" || echo "0"', (err, stdout) => {
    const novncOnline = stdout && stdout.trim() === '1';
    if (novncOnline) {
      if (callback) callback();
      return;
    }
    addLog('Websockify offline. Starting Websockify on 6080 -> 5901...');
    exec('setsid websockify --web=/usr/share/novnc/ 6080 localhost:5901 >/tmp/websockify.log 2>&1 < /dev/null &', (wsErr) => {
      if (wsErr) addLog('Websockify start error: ' + wsErr.message);
      else addLog('Websockify launched');
      setTimeout(() => {
        if (callback) callback();
      }, 500);
    });
  });
}

function restartOBSOnly(callback) {
  addLog('Restarting OBS Studio (preserving desktop & VNC connection)...');
  isRestarting = true;
  const restartCmd = `
    pkill -9 -x obs 2>/dev/null || true
    rm -rf /root/.config/obs-studio/.sentinel 2>/dev/null || true
    sleep 0.5
    DISPLAY=:1 setsid obs >/tmp/obs.log 2>&1 < /dev/null &
    sleep 2.5
    DISPLAY=:1 wmctrl -r "OBS" -b add,maximized_vert,maximized_horz 2>/dev/null || true
  `;
  exec(restartCmd, (err) => {
    isRestarting = false;
    if (err) addLog('OBS restart error: ' + err.message);
    else addLog('OBS Studio restarted and active on desktop');
    if (callback) callback(err);
  });
}

function restartFullPC(callback) {
  if (isSettingUp) {
    if (callback) callback(null, 'Setup/Restart already in progress');
    return;
  }
  addLog('Checking, installing & configuring all required tools (noVNC, XFCE, GStreamer, OBS 32, Plugin) and restarting PC...');
  isRestarting = true;
  isSettingUp = true;

  const fullProvisionAndRestartCmd = `
    export DEBIAN_FRONTEND=noninteractive

    # 1. Check & install system packages (VNC, noVNC, XFCE, File Manager, Zip GUI, Browser, GStreamer, WebRTC ICE, OpenGL, Qt6, Build tools)
    if ! which Xtigervnc websockify startxfce4 wmctrl xdotool xrandr thunar xarchiver firefox-esr meson ninja gst-launch-1.0 ssh dbus-launch git >/dev/null 2>&1 || \
       ! gst-inspect-1.0 nicesrc >/dev/null 2>&1 || \
       ! gst-inspect-1.0 glupload >/dev/null 2>&1 || \
       [ ! -f /usr/include/obs/obs-module.h ] || \
       [ ! -f /usr/lib/x86_64-linux-gnu/libQt6Widgets.so ]; then
      echo "[Setup] Installing missing system packages..."
      dpkg --remove --force-all lightdm 2>/dev/null || true
      dpkg --configure -a 2>/dev/null || true
      apt-get update
      apt-get install -y --no-install-recommends \
        xfce4-session xfwm4 xfce4-panel xfdesktop4 xfce4-terminal dbus-x11 adwaita-icon-theme \
        thunar thunar-archive-plugin xarchiver zip unzip p7zip-full firefox-esr \
        tigervnc-standalone-server tigervnc-tools novnc websockify wmctrl xdotool x11-xserver-utils \
        openssh-client procps psmisc git curl ca-certificates pulseaudio \
        obs-studio libobs-dev meson ninja-build build-essential pkg-config \
        squashfs-tools zstd fuse3 libfuse2 \
        gstreamer1.0-tools libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-ugly gstreamer1.0-libav libgstreamer-plugins-bad1.0-dev \
        gstreamer1.0-nice gstreamer1.0-gl gstreamer1.0-vaapi gstreamer1.0-x \
        gstreamer1.0-pulseaudio gstreamer1.0-alsa \
        libgstrtspserver-1.0-0 libgstrtspserver-1.0-dev gstreamer1.0-rtsp \
        libsoup-3.0-dev qt6-base-dev
    fi

    # 2. Check & install OBS Studio 32.2.2 AppImage
    if [ ! -x /opt/obs-studio-32/AppDir/AppRun ]; then
      echo "[Setup] Downloading and extracting OBS Studio 32.2.2..."
      mkdir -p /opt/obs-studio-32
      APPIMAGE_URL="https://github.com/ivan-hc/OBS-Studio-appimage/releases/download/continuous/OBS-Studio_32.2.2-1-archimage5.0-lite-x86_64.AppImage"
      curl -L -s --retry 3 -o /tmp/obs32.AppImage "$APPIMAGE_URL"
      chmod +x /tmp/obs32.AppImage
      cd /opt/obs-studio-32
      /tmp/obs32.AppImage --appimage-extract >/dev/null 2>&1 || true
      if [ -d /opt/obs-studio-32/squashfs-root ] && [ ! -d /opt/obs-studio-32/AppDir ]; then
        mv /opt/obs-studio-32/squashfs-root /opt/obs-studio-32/AppDir
        ln -sf ./AppDir /opt/obs-studio-32/squashfs-root
      fi
      rm -f /tmp/obs32.AppImage
    fi

    if [ -x /opt/obs-studio-32/AppDir/AppRun ]; then
      ln -sf /opt/obs-studio-32/AppDir/AppRun /usr/bin/obs
      if [ ! -L /opt/obs-studio-32/AppDir/lib/dri ] && [ -d /usr/lib/x86_64-linux-gnu/dri ]; then
        mkdir -p /opt/obs-studio-32/AppDir/lib/.mesa-softpipe-bak
        mv /opt/obs-studio-32/AppDir/lib/dri /opt/obs-studio-32/AppDir/lib/.mesa-softpipe-bak/ 2>/dev/null || true
        mv /opt/obs-studio-32/AppDir/lib/libEGL_mesa* /opt/obs-studio-32/AppDir/lib/libGLX_mesa* /opt/obs-studio-32/AppDir/lib/libgbm* /opt/obs-studio-32/AppDir/lib/libglapi* /opt/obs-studio-32/AppDir/lib/libgallium* /opt/obs-studio-32/AppDir/lib/.mesa-softpipe-bak/ 2>/dev/null || true
        ln -sf /usr/lib/x86_64-linux-gnu/dri /opt/obs-studio-32/AppDir/lib/dri
        cp -P /usr/lib/x86_64-linux-gnu/libEGL_mesa* /usr/lib/x86_64-linux-gnu/libGLX_mesa* /usr/lib/x86_64-linux-gnu/libgbm* /usr/lib/x86_64-linux-gnu/libglapi* /usr/lib/x86_64-linux-gnu/libLLVM* /opt/obs-studio-32/AppDir/lib/ 2>/dev/null || true
      fi
    fi

    # 3. Check, compile & install obs-gstreamer plugin with Qt6 Dock
    cd /app/applet
    if [ ! -f /app/applet/build/obs-gstreamer.so ]; then
      echo "[Setup] Building obs-gstreamer plugin with Qt6 Dock..."
      meson setup build --wipe -Ddock=enabled
      ninja -C build
    fi

    mkdir -p /root/.config/obs-studio/plugins/obs-gstreamer/bin/64bit
    cp -f /app/applet/build/obs-gstreamer.so /root/.config/obs-studio/plugins/obs-gstreamer/bin/64bit/obs-gstreamer.so 2>/dev/null || true
    rm -f /opt/obs-studio-32/AppDir/lib/obs-plugins/obs-gstreamer.so 2>/dev/null || true

    # 4. Configure Desktop shortcuts & VNC startup/passwords
    mkdir -p /root/Desktop
    cp -f /usr/share/applications/thunar.desktop /root/Desktop/ 2>/dev/null || true
    cp -f /usr/share/applications/xarchiver.desktop /root/Desktop/ 2>/dev/null || true
    cp -f /usr/share/applications/firefox-esr.desktop /root/Desktop/ 2>/dev/null || true
    cat << 'EOF' > /root/Desktop/obs.desktop
[Desktop Entry]
Version=1.0
Type=Application
Name=OBS Studio 32
Comment=Open Broadcaster Software 32.2.2
Exec=obs
Icon=com.obsproject.Studio
Terminal=false
Categories=AudioVideo;Recorder;
EOF
    chmod +x /root/Desktop/*.desktop 2>/dev/null || true

    mkdir -p /root/.vnc /root/.config/tigervnc
    echo '#!/bin/sh
unset SESSION_MANAGER
unset DBUS_SESSION_BUS_ADDRESS
exec startxfce4' > /root/.vnc/xstartup
    chmod +x /root/.vnc/xstartup

    echo "vncpassword" | vncpasswd -f > /root/.vnc/passwd 2>/dev/null || true
    chmod 600 /root/.vnc/passwd 2>/dev/null || true
    cp -f /root/.vnc/passwd /root/.config/tigervnc/passwd 2>/dev/null || true
    chmod 600 /root/.config/tigervnc/passwd 2>/dev/null || true

    # 5. Restart all desktop processes cleanly
    pkill -9 -x obs 2>/dev/null || true
    pkill -9 -x thunar 2>/dev/null || true
    pkill -9 -x xarchiver 2>/dev/null || true
    pkill -9 -x firefox-esr 2>/dev/null || true
    pkill -9 -x websockify 2>/dev/null || true
    pkill -9 -x xfce4-session 2>/dev/null || true
    pkill -9 -x xfwm4 2>/dev/null || true
    pkill -9 -x xfce4-panel 2>/dev/null || true
    pkill -9 -x xfdesktop 2>/dev/null || true
    pkill -9 -x Xtigervnc 2>/dev/null || true
    vncserver -kill :1 >/dev/null 2>&1 || true
    rm -rf /tmp/.X1-lock /tmp/.X11-unix/X1 /root/.config/obs-studio/.sentinel 2>/dev/null || true
    sleep 1

    vncserver :1 -geometry 1280x800 -depth 24 -localhost no -PasswordFile /root/.vnc/passwd -SecurityTypes VncAuth -UseBlacklist=0 -AlwaysShared=1 >/tmp/vnc.log 2>&1 < /dev/null || \
    vncserver :1 -geometry 1280x800 -depth 24 -localhost no -UseBlacklist=0 -AlwaysShared=1 >/tmp/vnc.log 2>&1 < /dev/null || true

    setsid websockify --web=/usr/share/novnc/ 6080 localhost:5901 >/tmp/websockify.log 2>&1 < /dev/null &
    sleep 1.5
    DISPLAY=:1 setsid obs >/tmp/obs.log 2>&1 < /dev/null &
    sleep 2.5
    DISPLAY=:1 wmctrl -r "OBS" -b add,maximized_vert,maximized_horz 2>/dev/null || true
  `;

  exec(fullProvisionAndRestartCmd, { maxBuffer: 1024 * 1024 * 10 }, (err) => {
    isRestarting = false;
    isSettingUp = false;
    if (err) addLog('Full PC check/setup/restart error: ' + err.message);
    else addLog('All tools verified, configured & Full PC Desktop restarted');

    if (tcpTunnelInfo.status !== 'online') {
      startTCPTunnel();
    }

    ensureCustomModes(() => {
      if (currentResolutionMode && currentResolutionMode !== 'auto') {
        setVNCResolution(currentResolutionMode, () => {
          if (callback) callback(err, 'All tools verified and PC restarted');
        });
      } else {
        if (callback) callback(err, 'All tools verified and PC restarted');
      }
    });
  });
}

function startVNCAndOBS(callback) {
  restartOBSOnly(callback);
}

function ensureOBS() {
  if (isRestarting || isSettingUp) return;
  exec('pgrep -x obs >/dev/null && echo "1" || echo "0"', (err, stdout) => {
    if (stdout && stdout.trim() !== '1') {
      exec('rm -rf /root/.config/obs-studio/.sentinel 2>/dev/null || true; DISPLAY=:1 setsid obs >/tmp/obs.log 2>&1 < /dev/null &', () => {});
    }
  });
}

let currentResolutionMode = 'auto';

function ensureCustomModes(callback) {
  const setupModes = `
    DISPLAY=:1 xrandr --newmode "720x1280_60.00" 74.50 720 760 832 944 1280 1283 1293 1316 -hsync +vsync 2>/dev/null || true
    DISPLAY=:1 xrandr --addmode VNC-0 "720x1280_60.00" 2>/dev/null || true
    DISPLAY=:1 xrandr --newmode "1080x1920_60.00" 173.00 1080 1152 1264 1448 1920 1923 1933 1968 -hsync +vsync 2>/dev/null || true
    DISPLAY=:1 xrandr --addmode VNC-0 "1080x1920_60.00" 2>/dev/null || true
  `;
  exec(setupModes, () => {
    if (callback) callback();
  });
}

function setVNCResolution(resolution, callback) {
  currentResolutionMode = resolution;
  ensureCustomModes(() => {
    let modeArg = '';
    if (resolution === '720x1280') {
      modeArg = '"720x1280_60.00"';
    } else if (resolution === '1080x1920') {
      modeArg = '"1080x1920_60.00"';
    } else if (resolution === '1280x720') {
      modeArg = '1280x720';
    } else if (resolution === '1920x1080') {
      modeArg = '1920x1080';
    } else if (resolution === '1280x800') {
      modeArg = '1280x800';
    }

    if (modeArg) {
      addLog(`Setting desktop resolution to fixed ${resolution}`);
      exec(`DISPLAY=:1 xrandr -s ${modeArg}; DISPLAY=:1 wmctrl -r "OBS" -b add,maximized_vert,maximized_horz 2>/dev/null || true`, (err) => {
        if (callback) callback(err, resolution);
      });
    } else {
      addLog('Desktop resolution mode set to auto (dynamic)');
      if (callback) callback(null, 'auto');
    }
  });
}

// Ensure custom portrait modes on server boot; auto-provision if environment was reset
exec('which Xtigervnc websockify >/dev/null 2>&1 && [ -x /opt/obs-studio-32/AppDir/AppRun ] && echo "1" || echo "0"', (err, stdout) => {
  if (stdout && stdout.trim() !== '1') {
    addLog('Detected fresh/reset container on boot — running automatic provisioning...');
    restartFullPC();
  } else {
    ensureCustomModes();
  }
});

// Background supervisor to auto-heal desktop services & keep OBS open
setInterval(() => {
  if (isRestarting || isSettingUp) return;
  ensureVNC(() => {
    ensureOBS();
  });
}, 5000);

function runFullSetup(callback) {
  restartFullPC(callback);
}

const server = http.createServer((req, res) => {
  // Permissive CORS headers
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, DELETE, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization');

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  // API Status endpoint for OBS GStreamer WebRTC stream (port 8888)
  if (req.url === '/api/status') {
    checkPort(TARGET_PORT, TARGET_HOST, (err, isOnline, message) => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        online: isOnline,
        targetHost: TARGET_HOST,
        targetPort: TARGET_PORT,
        message: isOnline ? 'GStreamer WebRTC server is listening' : 'GStreamer WebRTC stream offline (not broadcasting on port 8888)',
        timestamp: new Date().toISOString()
      }));
    });
    return;
  }

  // API Master Hub Dock status endpoint
  if (req.url === '/api/dock/status') {
    const statusFile = '/tmp/obs_gstreamer_hubs.json';
    fs.readFile(statusFile, 'utf8', (err, data) => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      if (err || !data) {
        res.end(JSON.stringify({
          active: false,
          master_hubs: [],
          message: 'Master Hub Dock state not initialized yet or OBS inactive.'
        }));
      } else {
        try {
          const parsed = JSON.parse(data);
          res.end(JSON.stringify({
            active: true,
            ...parsed
          }));
        } catch (e) {
          res.end(JSON.stringify({ active: false, master_hubs: [], error: e.message }));
        }
      }
    });
    return;
  }

  // API VNC Status & Health Check
  if (req.url === '/api/vnc/status') {
    exec('pgrep -x Xtigervnc >/dev/null && echo "1" || echo "0"', (err, vncOut) => {
      const vncOnline = vncOut && vncOut.trim() === '1';
      exec('pgrep -x websockify >/dev/null && echo "1" || echo "0"', (err2, wsOut) => {
        const novncOnline = wsOut && wsOut.trim() === '1';
        exec('pgrep -x obs >/dev/null && echo "1" || echo "0"', (err3, stdout) => {
          const obsRunning = stdout && stdout.trim() === '1';
          exec('which vncserver websockify obs >/dev/null && echo "1" || echo "0"', (err4, outInstalled) => {
            const isInstalled = outInstalled && outInstalled.trim() === '1';
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({
              vncOnline,
              novncOnline,
              obsRunning,
              isInstalled,
              isSettingUp,
              vncPort: VNC_PORT,
              novncPort: NOVNC_PORT,
              vncPassword: 'vncpassword',
              tcpTunnel: tcpTunnelInfo,
              currentResolution: currentResolutionMode,
              logs: setupLogs.slice(-15),
              timestamp: new Date().toISOString()
            }));
          });
        });
      });
    });
    return;
  }

  // API VNC Resolution Change
  if (req.url === '/api/vnc/resolution' && req.method === 'POST') {
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      try {
        const parsed = JSON.parse(body || '{}');
        const targetRes = parsed.resolution || 'auto';
        setVNCResolution(targetRes, (err, actual) => {
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ success: !err, actual, resolution: targetRes }));
        });
      } catch (e) {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ success: false, error: e.message }));
      }
    });
    return;
  }

  // API VNC Start/Restart trigger (Restart OBS Only)
  if (req.url === '/api/vnc/start' && req.method === 'POST') {
    startVNCAndOBS((err) => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        success: !err,
        message: err ? err.message : 'OBS startup initiated'
      }));
    });
    return;
  }

  // API Full PC Restart trigger (VNC + Desktop + OBS)
  if (req.url === '/api/vnc/restart-pc' && req.method === 'POST') {
    restartFullPC((err) => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        success: !err,
        message: err ? err.message : 'Full PC restart completed'
      }));
    });
    return;
  }

  // API Full Setup trigger
  if (req.url === '/api/vnc/setup' && req.method === 'POST') {
    runFullSetup((err, msg) => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        success: !err,
        message: msg || (err ? err.message : 'Setup initiated')
      }));
    });
    return;
  }

  // Proxy noVNC web assets & pages
  const isNoVncPath = req.url === '/novnc' ||
                      req.url.startsWith('/novnc/') ||
                      req.url.startsWith('/app/') ||
                      req.url.startsWith('/core/') ||
                      req.url.startsWith('/vendor/') ||
                      req.url === '/vnc.html' ||
                      req.url.startsWith('/vnc.html?');

  if (isNoVncPath) {
    let proxyPath = req.url;
    if (proxyPath === '/novnc' || proxyPath === '/novnc/') {
      proxyPath = '/vnc.html?autoconnect=true&resize=remote&password=vncpassword';
    } else if (proxyPath.startsWith('/novnc/')) {
      proxyPath = proxyPath.replace(/^\/novnc/, '');
    }

    const proxyReq = http.request({
      host: '127.0.0.1',
      port: NOVNC_PORT,
      path: proxyPath,
      method: req.method,
      headers: {
        ...req.headers,
        host: `127.0.0.1:${NOVNC_PORT}`
      }
    }, (proxyRes) => {
      res.writeHead(proxyRes.statusCode, proxyRes.headers);
      proxyRes.pipe(res);
    });

    proxyReq.on('error', (err) => {
      // Gracefully handle offline noVNC service without logging error alarms
      res.writeHead(503, {
        'Content-Type': 'text/html; charset=utf-8',
        'Retry-After': '2'
      });
      res.end(`
        <!DOCTYPE html>
        <html style="background:#020617;color:#94a3b8;font-family:sans-serif;display:flex;align-items:center;justify-content:center;height:100%;margin:0;">
          <div style="text-align:center;padding:2rem;max-width:480px;background:#0f172a;border-radius:12px;border:1px solid #1e293b;box-shadow:0 20px 25px -5px rgba(0,0,0,0.5)">
            <h3 style="color:#f8fafc;margin-top:0;">OBS Desktop Initializing...</h3>
            <p style="font-size:14px;line-height:1.5;">The remote VNC desktop is starting up. This page will connect automatically in a few seconds.</p>
            <div style="margin:1.5rem auto;width:32px;height:32px;border:3px solid #6366f1;border-top-color:transparent;border-radius:50%;animation:spin 1s linear infinite;"></div>
            <style>@keyframes spin { to { transform: rotate(360deg); } }</style>
            <script>setTimeout(function() { window.location.reload(); }, 3000);</script>
          </div>
        </html>
      `);
    });

    req.pipe(proxyReq);
    return;
  }

  // Proxy WHEP / signaling requests to GStreamer WebRTC server on port 8888
  if (req.url === '/whep' || req.url.startsWith('/whep?')) {
    const proxyReq = http.request({
      host: TARGET_HOST,
      port: TARGET_PORT,
      path: req.url,
      method: req.method,
      headers: {
        ...req.headers,
        host: `${TARGET_HOST}:${TARGET_PORT}`
      }
    }, (proxyRes) => {
      res.writeHead(proxyRes.statusCode, proxyRes.headers);
      proxyRes.pipe(res);
    });

    proxyReq.on('error', (err) => {
      // Graceful offline stream response (no uncaught error logging)
      res.writeHead(500, {
        'Content-Type': 'application/json',
        'X-Stream-Status': 'offline'
      });
      res.end(JSON.stringify({
        offline: true,
        message: 'GStreamer WebRTC pipeline is not actively streaming on port 8888 yet.',
        code: 'STREAM_OFFLINE',
        targetHost: TARGET_HOST,
        targetPort: TARGET_PORT
      }));
    });

    req.pipe(proxyReq);
    return;
  }

  // Serve static files from the WebRTC webroot
  let filePath = req.url === '/' ? '/index.html' : req.url;
  filePath = filePath.split('?')[0];
  const fullPath = path.join(WEB_ROOT, filePath);

  if (!fullPath.startsWith(WEB_ROOT)) {
    res.writeHead(403, { 'Content-Type': 'text/plain' });
    res.end('Forbidden');
    return;
  }

  fs.stat(fullPath, (err, stats) => {
    if (err || !stats.isFile()) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end('Not Found');
      return;
    }

    let contentType = 'text/plain';
    if (fullPath.endsWith('.html')) contentType = 'text/html';
    else if (fullPath.endsWith('.js')) contentType = 'application/javascript';
    else if (fullPath.endsWith('.css')) contentType = 'text/css';
    else if (fullPath.endsWith('.json')) contentType = 'application/json';
    else if (fullPath.endsWith('.svg')) contentType = 'image/svg+xml';
    else if (fullPath.endsWith('.png')) contentType = 'image/png';

    res.writeHead(200, { 'Content-Type': contentType });
    fs.createReadStream(fullPath).pipe(res);
  });
});

// Proxy WebSocket connections for noVNC / websockify
server.on('upgrade', (req, clientSocket, head) => {
  const upstream = net.connect(NOVNC_PORT, '127.0.0.1', () => {
    let rawReq = `${req.method} ${req.url} HTTP/${req.httpVersion}\r\n`;
    for (let i = 0; i < req.rawHeaders.length; i += 2) {
      rawReq += `${req.rawHeaders[i]}: ${req.rawHeaders[i + 1]}\r\n`;
    }
    rawReq += '\r\n';

    upstream.write(rawReq);
    if (head && head.length > 0) upstream.write(head);
    upstream.pipe(clientSocket);
    clientSocket.pipe(upstream);
  });

  upstream.on('error', () => {
    clientSocket.destroy();
  });

  clientSocket.on('error', () => {
    upstream.destroy();
  });
});

server.listen(PORT, '0.0.0.0', () => {
  console.log(`Web/WHEP & noVNC proxy server listening on port ${PORT}`);
});
