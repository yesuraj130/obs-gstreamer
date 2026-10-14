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

function startVNCAndOBS(callback) {
  addLog('Starting TigerVNC, Websockify and OBS Studio...');
  const startScript = `
    mkdir -p /root/.vnc
    echo '#!/bin/sh
unset SESSION_MANAGER
unset DBUS_SESSION_BUS_ADDRESS
exec startxfce4' > /root/.vnc/xstartup
    chmod +x /root/.vnc/xstartup

    # Configure VNC Password
    mkdir -p /root/.vnc
    echo "vncpassword" | vncpasswd -f > /root/.vnc/passwd 2>/dev/null || true
    chmod 600 /root/.vnc/passwd 2>/dev/null || true

    # Kill any stale VNC display
    vncserver -kill :1 >/dev/null 2>&1 || true
    rm -rf /tmp/.X1-lock /tmp/.X11-unix/X1 2>/dev/null || true

    # Start TigerVNC on :1 (port 5901)
    vncserver :1 -geometry 1280x800 -depth 24 -localhost no -SecurityTypes None >/tmp/vnc.log 2>&1 || \
    vncserver :1 -geometry 1280x800 -depth 24 -localhost no >/tmp/vnc.log 2>&1 || true

    # Start websockify for noVNC on 6080 -> 5901
    pkill -f websockify 2>/dev/null || true
    sleep 1
    nohup websockify --web=/usr/share/novnc/ 6080 localhost:5901 >/tmp/websockify.log 2>&1 &

    # Launch OBS Studio with display :1
    if ! pgrep -x obs >/dev/null; then
      DISPLAY=:1 nohup obs >/root/.obs.log 2>&1 &
    fi
  `;

  exec(startScript, (err, stdout, stderr) => {
    if (err) {
      addLog(`Start script error: ${err.message}`);
    } else {
      addLog('VNC services launch commands dispatched successfully');
    }
    if (callback) callback(err);
  });
}

function runFullSetup(callback) {
  if (isSettingUp) {
    if (callback) callback(null, 'Setup already in progress');
    return;
  }
  isSettingUp = true;
  addLog('Starting environment setup & package installation...');

  const setupCmd = `
    export DEBIAN_FRONTEND=noninteractive
    dpkg --remove --force-all lightdm 2>/dev/null || true
    dpkg --configure -a || true
    apt-get update
    apt-get install -y --no-install-recommends xfce4-session xfwm4 xfce4-panel xfdesktop4 xfce4-terminal tigervnc-standalone-server tigervnc-tools novnc websockify obs-studio libobs-dev meson ninja-build build-essential gstreamer1.0-tools libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad libsoup-3.0-dev qtbase5-dev libqt5svg5-dev

    # Build obs-gstreamer plugin
    cd /app/applet
    rm -rf build
    meson setup build || true
    ninja -C build || true

    # Copy plugin to OBS plugin locations
    mkdir -p /root/.config/obs-studio/plugins/obs-gstreamer/bin/64bit
    cp build/obs-gstreamer.so /root/.config/obs-studio/plugins/obs-gstreamer/bin/64bit/ 2>/dev/null || true
    cp build/obs-gstreamer.so /usr/lib/x86_64-linux-gnu/obs-plugins/ 2>/dev/null || true
  `;

  exec(setupCmd, (err, stdout, stderr) => {
    isSettingUp = false;
    if (err) {
      addLog(`Setup failed: ${err.message}`);
      if (callback) callback(err);
      return;
    }
    addLog('Setup & build completed! Starting desktop environment...');
    startVNCAndOBS(() => {
      if (callback) callback(null, 'Setup and startup complete');
    });
  });
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

  // API VNC Status & Health Check
  if (req.url === '/api/vnc/status') {
    checkPort(VNC_PORT, '127.0.0.1', (err, vncOnline) => {
      checkPort(NOVNC_PORT, '127.0.0.1', (err2, novncOnline) => {
        exec('pgrep -x obs >/dev/null && echo "1" || echo "0"', (err3, stdout) => {
          const obsRunning = stdout.trim() === '1';
          exec('which vncserver websockify obs >/dev/null && echo "1" || echo "0"', (err4, outInstalled) => {
            const isInstalled = outInstalled.trim() === '1';
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
              logs: setupLogs.slice(-15),
              timestamp: new Date().toISOString()
            }));
          });
        });
      });
    });
    return;
  }

  // API VNC Start/Restart trigger
  if (req.url === '/api/vnc/start' && req.method === 'POST') {
    startVNCAndOBS((err) => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        success: !err,
        message: err ? err.message : 'VNC and OBS startup initiated'
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
