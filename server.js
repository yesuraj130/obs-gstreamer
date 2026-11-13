const http = require('http');
const fs = require('fs');
const path = require('path');
const net = require('net');
const { exec, spawn } = require('child_process');

process.on('uncaughtException', (err) => {
  console.error('[Process Error] uncaughtException:', err);
  if (typeof addLog === 'function') addLog(`[Error] ${err.message}`);
});

process.on('unhandledRejection', (reason) => {
  console.error('[Process Error] unhandledRejection:', reason);
});

const PORT = 3000;
const WEB_ROOT = path.join(__dirname, '.webrtc-webroot');
const TARGET_HOST = '127.0.0.1';
const TARGET_PORT = 8888;
const NOVNC_PORT = 6080;
const VNC_PORT = 5901;

let setupProcess = null;
let setupLogs = [];
let isSettingUp = false;
let currentRunningCmd = '';

function addLog(msg) {
  if (!msg) return;
  const lines = String(msg).split(/\r?\n/);
  for (const raw of lines) {
    const trimmed = raw.trimEnd();
    if (!trimmed) continue;
    const line = `[${new Date().toLocaleTimeString()}] ${trimmed}`;
    setupLogs.push(line);
    if (setupLogs.length > 250) setupLogs.shift();
    console.log(line);
  }
}

function runCommandWithLiveLogs(label, scriptContent, callback) {
  addLog(`==> ${label}`);
  currentRunningCmd = label;
  const wrappedScript = `
    PS4='+ $ '
    set -x
    ${scriptContent}
  `;
  const proc = spawn('bash', ['-c', wrappedScript], {
    stdio: ['ignore', 'pipe', 'pipe']
  });
  setupProcess = proc;

  const handleChunk = (chunk) => {
    const text = chunk.toString();
    const lines = text.split(/\r?\n/);
    for (const l of lines) {
      const clean = l.trim();
      if (!clean) continue;
      if (clean.startsWith('+ $ ')) {
        currentRunningCmd = clean.slice(4);
        addLog(`$ ${currentRunningCmd}`);
      } else {
        addLog(`  ${clean}`);
      }
    }
  };

  proc.stdout.on('data', handleChunk);
  proc.stderr.on('data', handleChunk);

  proc.on('close', (code) => {
    if (setupProcess === proc) setupProcess = null;
    currentRunningCmd = '';
    const err = code !== 0 ? new Error(`${label} exited with code ${code}`) : null;
    if (callback) callback(err);
  });

  proc.on('error', (err) => {
    if (setupProcess === proc) setupProcess = null;
    currentRunningCmd = '';
    if (callback) callback(err);
  });
}

const PLAYIT_CONFIG_PATH = '/app/applet/playit.toml';
const PLAYIT_BIN = '/usr/local/bin/playit';
const PLAYIT_CLI_BIN = '/usr/local/bin/playit-cli';

const TUNNEL_CACHE_FILE = '/app/applet/.vnc_tunnel_cache.json';

let tcpTunnelInfo = {
  host: '',
  port: '',
  full: '',
  status: 'connecting',
  provider: 'pinggy',
  claimUrl: '',
  tailscaleIp: '',
  isReused: false,
  lastUpdated: null
};

let pinggyProcess = null;
let playitDaemon = null;
let playitSetupProc = null;
let playitPollTimer = null;

const TURN_PORT = 3478;
const TURN_USER = 'obs';
const TURN_PASS = 'turnpassword';

let turnTunnelInfo = {
  host: '',
  port: '',
  full: '',
  url: '',
  status: 'connecting',
  isReused: false,
  lastUpdated: null
};
let turnTunnelProc = null;

function getTurnConfigPayload() {
  const hostPort = turnTunnelInfo.full || `127.0.0.1:${TURN_PORT}`;
  return {
    iceServers: [
      {
        urls: `turn:${hostPort}?transport=tcp`,
        username: TURN_USER,
        credential: TURN_PASS
      }
    ],
    iceTransportPolicy: 'relay',
    publicTurn: turnTunnelInfo.full ? `turn:${turnTunnelInfo.full}?transport=tcp` : '',
    localTurn: `turn:127.0.0.1:${TURN_PORT}?transport=tcp`
  };
}

function writeTurnConfigFiles() {
  try {
    const json = JSON.stringify(getTurnConfigPayload(), null, 2);
    fs.writeFileSync(path.join(WEB_ROOT, 'turn-config.json'), json);
    const obsWebRoot = '/root/.local/share/obs-gstreamer/webrtc';
    if (fs.existsSync(obsWebRoot)) {
      fs.writeFileSync(path.join(obsWebRoot, 'turn-config.json'), json);
    }
  } catch (e) {}
}

function loadTunnelCache() {
  try {
    if (fs.existsSync(TUNNEL_CACHE_FILE)) {
      const data = JSON.parse(fs.readFileSync(TUNNEL_CACHE_FILE, 'utf8'));
      if (data.tcpTunnelInfo && data.tcpTunnelInfo.full) {
        tcpTunnelInfo = { ...tcpTunnelInfo, ...data.tcpTunnelInfo, isReused: true };
      }
      if (data.webrtcHttpTunnel && data.webrtcHttpTunnel.url) {
        webrtcHttpTunnel = { ...webrtcHttpTunnel, ...data.webrtcHttpTunnel, isReused: true };
      }
      if (data.turnTunnelInfo && data.turnTunnelInfo.full) {
        turnTunnelInfo = { ...turnTunnelInfo, ...data.turnTunnelInfo, isReused: true };
      }
    }
  } catch (e) {
    console.error('Failed to load tunnel cache:', e.message);
  }
  writeTurnConfigFiles();
}

function saveTunnelCache() {
  try {
    fs.writeFileSync(TUNNEL_CACHE_FILE, JSON.stringify({
      tcpTunnelInfo,
      webrtcHttpTunnel,
      turnTunnelInfo,
      savedAt: new Date().toISOString()
    }, null, 2));
  } catch (e) {
    console.error('Failed to save tunnel cache:', e.message);
  }
  writeTurnConfigFiles();
}

function updateTailscaleIp() {
  exec('tailscale ip -4 2>/dev/null', (err, stdout) => {
    const ip = (stdout || '').trim();
    if (ip) {
      tcpTunnelInfo.tailscaleIp = `${ip}:5901`;
      saveTunnelCache();
    }
  });
}

function startPinggyTunnel(forceReset = false) {
  loadTunnelCache();
  updateTailscaleIp();

  // If Playit is active and not forcing reset, stay on Playit
  if (tcpTunnelInfo.provider === 'playit' && tcpTunnelInfo.status === 'online' && !forceReset) {
    return;
  }

  // Check if an SSH Pinggy tunnel is already running in background
  if (!forceReset) {
    exec('pgrep -f "ssh.*5901.*tcp@a\\.pinggy\\.io" >/dev/null && echo "1" || echo "0"', (err, stdout) => {
      const isRunning = stdout && stdout.trim() === '1';
      if (isRunning && fs.existsSync('/tmp/pinggy_vnc.log')) {
        const logContent = fs.readFileSync('/tmp/pinggy_vnc.log', 'utf8');
        const match = logContent.match(/tcp:\/\/([^:\s]+):(\d+)/);
        if (match) {
          tcpTunnelInfo.host = match[1];
          tcpTunnelInfo.port = match[2];
          tcpTunnelInfo.full = `${match[1]}:${match[2]}`;
          tcpTunnelInfo.status = 'online';
          tcpTunnelInfo.provider = 'pinggy';
          tcpTunnelInfo.isReused = true;
          tcpTunnelInfo.lastUpdated = new Date().toISOString();
          saveTunnelCache();
          addLog(`Reusing active persistent VNC TCP Tunnel (Pinggy): ${tcpTunnelInfo.full}`);
          return;
        }
      }
      if (isRunning && tcpTunnelInfo.full && tcpTunnelInfo.status === 'online') {
        tcpTunnelInfo.isReused = true;
        addLog(`Reusing cached persistent VNC TCP Tunnel (Pinggy): ${tcpTunnelInfo.full}`);
        return;
      }

      spawnNewPinggyTunnel();
    });
  } else {
    exec('pkill -9 -f "ssh.*5901.*tcp@a\\.pinggy\\.io" 2>/dev/null || true', () => {
      if (pinggyProcess) {
        try { pinggyProcess.kill(); } catch (e) {}
        pinggyProcess = null;
      }
      spawnNewPinggyTunnel();
    });
  }
}

function spawnNewPinggyTunnel() {
  exec('which ssh >/dev/null 2>&1 && echo "1" || echo "0"', (err, stdout) => {
    if (!stdout || stdout.trim() !== '1') {
      tcpTunnelInfo.status = 'offline';
      return;
    }

    addLog('Establishing persistent Pinggy TCP tunnel for VNC access...');
    try {
      fs.writeFileSync('/tmp/pinggy_vnc.log', '');

      const proc = spawn('setsid', [
        'ssh',
        '-p', '443',
        '-o', 'StrictHostKeyChecking=no',
        '-o', 'ServerAliveInterval=30',
        '-o', 'ServerAliveCountMax=3',
        '-R', '0:localhost:5901',
        'tcp@a.pinggy.io'
      ], {
        stdio: ['ignore', 'pipe', 'pipe'],
        detached: true
      });
      pinggyProcess = proc;

      const parseOutput = (chunk) => {
        const text = chunk.toString();
        fs.appendFileSync('/tmp/pinggy_vnc.log', text);
        const match = text.match(/tcp:\/\/([^:\s]+):(\d+)/);
        if (match && tcpTunnelInfo.provider !== 'playit') {
          tcpTunnelInfo.host = match[1];
          tcpTunnelInfo.port = match[2];
          tcpTunnelInfo.full = `${match[1]}:${match[2]}`;
          tcpTunnelInfo.status = 'online';
          tcpTunnelInfo.provider = 'pinggy';
          tcpTunnelInfo.isReused = false;
          tcpTunnelInfo.lastUpdated = new Date().toISOString();
          saveTunnelCache();
          addLog(`Public VNC TCP Tunnel (Pinggy) established: ${tcpTunnelInfo.full}`);
        }
      };

      proc.stdout.on('data', parseOutput);
      proc.stderr.on('data', parseOutput);

      proc.on('close', () => {
        pinggyProcess = null;
        if (tcpTunnelInfo.provider !== 'playit') {
          exec('pgrep -f "ssh.*5901.*tcp@a\\.pinggy\\.io" >/dev/null && echo "1" || echo "0"', (err2, out2) => {
            if (!out2 || out2.trim() !== '1') {
              tcpTunnelInfo.status = 'offline';
              setTimeout(() => startPinggyTunnel(false), 10000);
            }
          });
        }
      });

      proc.on('error', () => {
        pinggyProcess = null;
        if (tcpTunnelInfo.provider !== 'playit') {
          tcpTunnelInfo.status = 'offline';
        }
      });

      proc.unref();
    } catch (e) {
      addLog(`Pinggy spawn error: ${e.message}`);
    }
  });
}

function startPlayit() {
  try {
    exec('mkdir -p /run/playit', () => {});
    const hasConfig = fs.existsSync(PLAYIT_CONFIG_PATH) && fs.statSync(PLAYIT_CONFIG_PATH).size > 10;

    // Start playit daemon if installed and not already running
    exec('pgrep -x playit >/dev/null && echo "1" || echo "0"', (err, stdout) => {
      const isRunning = stdout && stdout.trim() === '1';
      if (!isRunning && fs.existsSync(PLAYIT_BIN)) {
        addLog('Launching Playit.gg daemon for permanent static tunnel...');
        try {
          playitDaemon = spawn(PLAYIT_BIN, ['--secret-path', PLAYIT_CONFIG_PATH, '-l', '/app/applet/playit.log'], {
            detached: true,
            stdio: 'ignore'
          });
          playitDaemon.on('error', (err) => {
            addLog(`Playit daemon error: ${err.message}`);
          });
          playitDaemon.unref();
        } catch (e) {
          addLog(`Playit daemon spawn error: ${e.message}`);
        }
      }
    });

    if (hasConfig) {
      pollPlayitTunnel();
    } else {
      if (!playitSetupProc && fs.existsSync(PLAYIT_CLI_BIN)) {
        addLog('Starting Playit setup to generate permanent claim URL...');
        try {
          playitSetupProc = spawn(PLAYIT_CLI_BIN, ['setup']);

          const handleSetupOut = (chunk) => {
            const text = chunk.toString();
            const match = text.match(/https:\/\/playit\.gg\/claim\/([a-zA-Z0-9]+)/);
            if (match) {
              tcpTunnelInfo.claimUrl = `https://playit.gg/claim/${match[1]}`;
              addLog(`Playit permanent static tunnel claim URL: ${tcpTunnelInfo.claimUrl}`);
            }
          };

          playitSetupProc.stdout.on('data', handleSetupOut);
          playitSetupProc.stderr.on('data', handleSetupOut);

          playitSetupProc.on('error', (err) => {
            playitSetupProc = null;
            addLog(`Playit setup error: ${err.message}`);
          });

          playitSetupProc.on('close', () => {
            playitSetupProc = null;
            if (fs.existsSync(PLAYIT_CONFIG_PATH) && fs.statSync(PLAYIT_CONFIG_PATH).size > 10) {
              addLog('Playit agent successfully claimed! Loading static tunnel...');
              tcpTunnelInfo.claimUrl = '';
              pollPlayitTunnel();
            } else {
              setTimeout(startPlayit, 10000);
            }
          });
        } catch (e) {
          playitSetupProc = null;
          addLog(`Playit setup spawn error: ${e.message}`);
        }
      }
    }
  } catch (e) {
    addLog(`Playit setup error: ${e.message}`);
  }
}

function pollPlayitTunnel() {
  if (playitPollTimer) clearInterval(playitPollTimer);
  playitPollTimer = setInterval(() => {
    if (!fs.existsSync(PLAYIT_CONFIG_PATH)) return;

    exec('timeout 3 /usr/local/bin/playit-cli attach --stdout 2>/dev/null || cat /app/applet/playit.log 2>/dev/null', (err, stdout) => {
      if (!stdout) return;
      const match = stdout.match(/127\.0\.0\.1:5901\s*=>\s*([a-zA-Z0-9_\-\.]+\.[a-zA-Z]{2,}):(\d+)/i) ||
                    stdout.match(/([a-zA-Z0-9_\-]+\.(?:gl\.joinmc\.link|auto\.playit\.gg|craft\.ply\.gg|at\.ply\.gg|playit\.gg)):(\d+)/i);
      if (match) {
        const host = match[1];
        const port = match[2];
        tcpTunnelInfo = {
          host: host,
          port: port,
          full: `${host}:${port}`,
          status: 'online',
          provider: 'playit',
          claimUrl: '',
          isReused: true,
          lastUpdated: new Date().toISOString()
        };
        saveTunnelCache();
        if (pinggyProcess) {
          try { pinggyProcess.kill(); } catch (e) {}
          pinggyProcess = null;
        }
      }
    });
  }, 3000);
}

const STANDALONE_WEBRTC_PORT = 8889;
let standaloneServerStarted = false;

function ensureStandaloneWebrtcProxy() {
  if (standaloneServerStarted) return;
  standaloneServerStarted = true;
  const standaloneSrv = http.createServer((req, res) => {
    if (req.url === '/turn-config.json' || req.url.startsWith('/turn-config.json?')) {
      res.writeHead(200, {
        'Content-Type': 'application/json',
        'Access-Control-Allow-Origin': '*',
        'Cache-Control': 'no-cache, no-store, must-revalidate'
      });
      res.end(JSON.stringify(getTurnConfigPayload()));
      return;
    }
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
      res.writeHead(proxyRes.statusCode, {
        ...proxyRes.headers,
        'cache-control': 'no-cache, no-store, must-revalidate'
      });
      proxyRes.pipe(res);
    });
    proxyReq.on('error', () => {
      res.writeHead(503, { 'Content-Type': 'text/plain' });
      res.end('OBS GStreamer WebRTC server (port 8888) is offline.');
    });
    req.pipe(proxyReq);
  });
  standaloneSrv.listen(STANDALONE_WEBRTC_PORT, '127.0.0.1', () => {});
}

let webrtcHttpTunnel = {
  url: '',
  status: 'connecting',
  isReused: false
};
let webrtcHttpProc = null;

function startWebrtcHttpTunnel(forceReset = false) {
  ensureStandaloneWebrtcProxy();

  if (!forceReset) {
    exec(`pgrep -f "ssh.*${STANDALONE_WEBRTC_PORT}.*a\\.pinggy\\.io" >/dev/null && echo "1" || echo "0"`, (err, stdout) => {
      const isRunning = stdout && stdout.trim() === '1';
      if (isRunning && fs.existsSync('/tmp/pinggy_webrtc.log')) {
        const logContent = fs.readFileSync('/tmp/pinggy_webrtc.log', 'utf8');
        const match = logContent.match(/https:\/\/([a-zA-Z0-9_\-]+\.(?:free\.pinggy\.link|run\.pinggy-free\.link|a\.free\.pinggy\.link|pinggy\.link))/i);
        if (match) {
          webrtcHttpTunnel.url = `https://${match[1]}`;
          webrtcHttpTunnel.status = 'online';
          webrtcHttpTunnel.isReused = true;
          saveTunnelCache();
          addLog(`Reusing active persistent Public Standalone WebRTC Player URL: ${webrtcHttpTunnel.url}`);
          return;
        }
      }
      if (isRunning && webrtcHttpTunnel.url && webrtcHttpTunnel.status === 'online') {
        webrtcHttpTunnel.isReused = true;
        addLog(`Reusing cached Public Standalone WebRTC Player URL: ${webrtcHttpTunnel.url}`);
        return;
      }

      spawnNewWebrtcHttpTunnel();
    });
  } else {
    exec(`pkill -9 -f "ssh.*${STANDALONE_WEBRTC_PORT}.*a\\.pinggy\\.io" 2>/dev/null || true`, () => {
      if (webrtcHttpProc) {
        try { webrtcHttpProc.kill(); } catch (e) {}
        webrtcHttpProc = null;
      }
      spawnNewWebrtcHttpTunnel();
    });
  }
}

function spawnNewWebrtcHttpTunnel() {
  exec('which ssh >/dev/null 2>&1 && echo "1" || echo "0"', (err, stdout) => {
    if (!stdout || stdout.trim() !== '1') return;
    try {
      fs.writeFileSync('/tmp/pinggy_webrtc.log', '');

      const proc = spawn('setsid', [
        'ssh',
        '-p', '443',
        '-o', 'StrictHostKeyChecking=no',
        '-o', 'ServerAliveInterval=30',
        '-o', 'ServerAliveCountMax=3',
        '-R', `0:localhost:${STANDALONE_WEBRTC_PORT}`,
        'a.pinggy.io'
      ], {
        stdio: ['ignore', 'pipe', 'pipe'],
        detached: true
      });
      webrtcHttpProc = proc;

      const parseOut = (chunk) => {
        const text = chunk.toString();
        fs.appendFileSync('/tmp/pinggy_webrtc.log', text);
        const match = text.match(/https:\/\/([a-zA-Z0-9_\-]+\.(?:free\.pinggy\.link|run\.pinggy-free\.link|a\.free\.pinggy\.link|pinggy\.link))/i);
        if (match && !webrtcHttpTunnel.url) {
          webrtcHttpTunnel.url = `https://${match[1]}`;
          webrtcHttpTunnel.status = 'online';
          webrtcHttpTunnel.isReused = false;
          saveTunnelCache();
          addLog(`Public Standalone WebRTC Player URL online: ${webrtcHttpTunnel.url}`);
        }
      };

      proc.stdout.on('data', parseOut);
      proc.stderr.on('data', parseOut);

      proc.on('close', () => {
        webrtcHttpProc = null;
        exec(`pgrep -f "ssh.*${STANDALONE_WEBRTC_PORT}.*a\\.pinggy\\.io" >/dev/null && echo "1" || echo "0"`, (err2, out2) => {
          if (!out2 || out2.trim() !== '1') {
            webrtcHttpTunnel.url = '';
            webrtcHttpTunnel.status = 'offline';
            setTimeout(() => startWebrtcHttpTunnel(false), 10000);
          }
        });
      });

      proc.on('error', () => {});
      proc.unref();
    } catch (e) {}
  });
}

function ensureCoturn(callback) {
  exec('ip addr add 10.200.0.1/24 dev eth1 2>/dev/null || true; pgrep -x turnserver >/dev/null && echo "1" || echo "0"', (err, stdout) => {
    const running = stdout && stdout.trim() === '1';
    if (running) {
      writeTurnConfigFiles();
      if (callback) callback();
      return;
    }
    const startCoturnCmd = `
      ip addr add 10.200.0.1/24 dev eth1 2>/dev/null || true
      if ! which turnserver >/dev/null 2>&1; then
        export DEBIAN_FRONTEND=noninteractive
        apt-get update && apt-get install -y --no-install-recommends -o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" coturn || true
      fi
      if which turnserver >/dev/null 2>&1; then
        cat << 'EOF' > /etc/turnserver.conf
listening-port=${TURN_PORT}
listening-ip=127.0.0.1
listening-ip=10.200.0.1
relay-ip=127.0.0.1
external-ip=8.8.8.8/127.0.0.1
min-port=49152
max-port=49252
fingerprint
lt-cred-mech
user=${TURN_USER}:${TURN_PASS}
realm=obs-webrtc
allow-loopback-peers
no-multicast-peers
no-cli
no-tls
no-dtls
log-file=/tmp/coturn.log
simple-log
EOF
        turnserver -c /etc/turnserver.conf -o >/dev/null 2>&1 || true
      fi
    `;
    exec(startCoturnCmd, () => {
      writeTurnConfigFiles();
      if (callback) callback();
    });
  });
}

function startTurnTunnel(forceReset = false) {
  ensureCoturn(() => {
    if (!forceReset) {
      exec(`pgrep -f "ssh.*${TURN_PORT}.*tcp@a\\.pinggy\\.io" >/dev/null && echo "1" || echo "0"`, (err, stdout) => {
        const isRunning = stdout && stdout.trim() === '1';
        if (isRunning && fs.existsSync('/tmp/pinggy_turn.log')) {
          const logContent = fs.readFileSync('/tmp/pinggy_turn.log', 'utf8');
          const match = logContent.match(/tcp:\/\/([^:\s]+):(\d+)/);
          if (match) {
            turnTunnelInfo.host = match[1];
            turnTunnelInfo.port = match[2];
            turnTunnelInfo.full = `${match[1]}:${match[2]}`;
            turnTunnelInfo.url = `turn:${match[1]}:${match[2]}?transport=tcp`;
            turnTunnelInfo.status = 'online';
            turnTunnelInfo.isReused = true;
            turnTunnelInfo.lastUpdated = new Date().toISOString();
            saveTunnelCache();
            addLog(`Reusing active persistent Coturn TCP Tunnel (Pinggy): ${turnTunnelInfo.url}`);
            return;
          }
        }
        if (isRunning && turnTunnelInfo.full && turnTunnelInfo.status === 'online') {
          turnTunnelInfo.isReused = true;
          writeTurnConfigFiles();
          addLog(`Reusing cached persistent Coturn TCP Tunnel (Pinggy): ${turnTunnelInfo.url}`);
          return;
        }
        spawnNewTurnTunnel();
      });
    } else {
      exec(`pkill -9 -f "ssh.*${TURN_PORT}.*tcp@a\\.pinggy\\.io" 2>/dev/null || true`, () => {
        if (turnTunnelProc) {
          try { turnTunnelProc.kill(); } catch (e) {}
          turnTunnelProc = null;
        }
        spawnNewTurnTunnel();
      });
    }
  });
}

function spawnNewTurnTunnel() {
  exec('which ssh >/dev/null 2>&1 && echo "1" || echo "0"', (err, stdout) => {
    if (!stdout || stdout.trim() !== '1') {
      turnTunnelInfo.status = 'offline';
      return;
    }
    addLog('Establishing persistent Pinggy TCP tunnel for self-hosted Coturn TURN server (port 3478)...');
    try {
      fs.writeFileSync('/tmp/pinggy_turn.log', '');

      const proc = spawn('setsid', [
        'ssh',
        '-p', '443',
        '-o', 'StrictHostKeyChecking=no',
        '-o', 'ServerAliveInterval=30',
        '-o', 'ServerAliveCountMax=3',
        '-R', `0:localhost:${TURN_PORT}`,
        'tcp@a.pinggy.io'
      ], {
        stdio: ['ignore', 'pipe', 'pipe'],
        detached: true
      });
      turnTunnelProc = proc;

      const parseOutput = (chunk) => {
        const text = chunk.toString();
        fs.appendFileSync('/tmp/pinggy_turn.log', text);
        const match = text.match(/tcp:\/\/([^:\s]+):(\d+)/);
        if (match) {
          turnTunnelInfo.host = match[1];
          turnTunnelInfo.port = match[2];
          turnTunnelInfo.full = `${match[1]}:${match[2]}`;
          turnTunnelInfo.url = `turn:${match[1]}:${match[2]}?transport=tcp`;
          turnTunnelInfo.status = 'online';
          turnTunnelInfo.isReused = false;
          turnTunnelInfo.lastUpdated = new Date().toISOString();
          saveTunnelCache();
          addLog(`Public Coturn TURN TCP Tunnel (Pinggy) established: ${turnTunnelInfo.url}`);
        }
      };

      proc.stdout.on('data', parseOutput);
      proc.stderr.on('data', parseOutput);

      proc.on('close', () => {
        turnTunnelProc = null;
        exec(`pgrep -f "ssh.*${TURN_PORT}.*tcp@a\\.pinggy\\.io" >/dev/null && echo "1" || echo "0"`, (err2, out2) => {
          if (!out2 || out2.trim() !== '1') {
            turnTunnelInfo.status = 'offline';
            setTimeout(() => startTurnTunnel(false), 10000);
          }
        });
      });

      proc.on('error', () => {
        turnTunnelProc = null;
        turnTunnelInfo.status = 'offline';
      });

      proc.unref();
    } catch (e) {
      addLog(`Coturn tunnel spawn error: ${e.message}`);
    }
  });
}

function startTCPTunnel(forceReset = false) {
  loadTunnelCache();
  startPinggyTunnel(forceReset);
  startPlayit();
  startWebrtcHttpTunnel(forceReset);
  startTurnTunnel(forceReset);
}

// Start public tunnels on server init
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
  if (isRestarting || isSettingUp) {
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
    pkill -9 -x ld-linux-x86-64 2>/dev/null || true
    rm -rf /root/.config/obs-studio/.sentinel 2>/dev/null || true
    sleep 0.5
    DISPLAY=:1 setsid obs --disable-shutdown-check >/tmp/obs.log 2>&1 < /dev/null &
    sleep 2.5
    DISPLAY=:1 wmctrl -r "OBS" -b add,maximized_vert,maximized_horz 2>/dev/null || true
  `;
  runCommandWithLiveLogs('Restarting OBS Studio', restartCmd, (err) => {
    isRestarting = false;
    if (err) addLog('OBS restart error: ' + err.message);
    else addLog('OBS Studio restarted and active on desktop');
    if (callback) callback(err);
  });
}

function rebuildPluginAndRestartOBS(callback) {
  if (isSettingUp) {
    if (callback) callback(new Error('Setup in progress, please wait...'));
    return;
  }
  addLog('Rebuilding obs-gstreamer plugin, installing in OBS, and restarting OBS...');
  isRestarting = true;
  const cmd = `
    pkill -9 -x obs 2>/dev/null || true
    pkill -9 -x ld-linux-x86-64 2>/dev/null || true
    rm -rf /root/.config/obs-studio/.sentinel 2>/dev/null || true
    cd /app/applet
    export PKG_CONFIG_PATH="/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH"
    if [ -f /usr/include/obs/media-io/video-io.h ] && ! grep -q "VIDEO_FORMAT_V210" /usr/include/obs/media-io/video-io.h; then
      sed -i 's#VIDEO_FORMAT_YA2L, // Little Endian#VIDEO_FORMAT_YA2L, // Little Endian\\n\\tVIDEO_FORMAT_V210,#' /usr/include/obs/media-io/video-io.h 2>/dev/null || true
    fi
    if [ -d build ]; then
      ninja -C build || (rm -rf build && meson setup build -Ddock=enabled && ninja -C build)
    else
      meson setup build -Ddock=enabled && ninja -C build
    fi
    mkdir -p /root/.config/obs-studio/plugins/obs-gstreamer/bin/64bit
    cp -f /app/applet/build/obs-gstreamer.so /root/.config/obs-studio/plugins/obs-gstreamer/bin/64bit/obs-gstreamer.so 2>/dev/null || true
    rm -f /opt/obs-studio-32/AppDir/lib/obs-plugins/obs-gstreamer.so 2>/dev/null || true
    rm -f /opt/obs-studio-32/AppDir/.junest/usr/lib/obs-plugins/obs-gstreamer.so 2>/dev/null || true
    sleep 0.5
    DISPLAY=:1 setsid obs --disable-shutdown-check >/tmp/obs.log 2>&1 < /dev/null &
    sleep 2.5
    DISPLAY=:1 wmctrl -r "OBS" -b add,maximized_vert,maximized_horz 2>/dev/null || true
  `;
  runCommandWithLiveLogs('Rebuilding Plugin & Restarting OBS', cmd, (err) => {
    isRestarting = false;
    if (err) addLog('Rebuild plugin error: ' + err.message);
    else addLog('Plugin rebuilt, installed, and OBS Studio restarted successfully');
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
    dpkg --remove --force-all lightdm 2>/dev/null || true
    dpkg --configure -a --force-confdef --force-confold </dev/null || true
    [ -x /usr/bin/tigervncserver ] && [ ! -e /usr/bin/vncserver ] && ln -sf /usr/bin/tigervncserver /usr/bin/vncserver || true
    [ -x /usr/bin/tigervncpasswd ] && [ ! -e /usr/bin/vncpasswd ] && ln -sf /usr/bin/tigervncpasswd /usr/bin/vncpasswd || true

    if ! which vncserver Xtigervnc websockify startxfce4 wmctrl xdotool xrandr thunar xarchiver google-chrome meson ninja gst-launch-1.0 ssh dbus-launch git >/dev/null 2>&1 || \
       ! gst-inspect-1.0 nicesrc >/dev/null 2>&1 || \
       ! gst-inspect-1.0 glupload >/dev/null 2>&1 || \
       [ ! -f /usr/include/obs/obs-module.h ] || \
       [ ! -f /usr/lib/x86_64-linux-gnu/libQt6Widgets.so ]; then
      echo "[Setup] Installing missing system packages..."
      dpkg --remove --force-all lightdm 2>/dev/null || true
      dpkg --configure -a --force-confdef --force-confold </dev/null || true
      apt-get update
      apt-get install -y --no-install-recommends -o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" \
        xfce4-session xfwm4 xfce4-panel xfdesktop4 xfce4-terminal dbus-x11 adwaita-icon-theme \
        thunar thunar-archive-plugin xarchiver zip unzip p7zip-full \
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
      if ! which google-chrome >/dev/null 2>&1; then
        bash /app/applet/.devcontainer/setup/browser.sh 2>/dev/null || true
      fi
    fi

    # Ensure GStreamer 1.28.9 is installed and compiled
    export PKG_CONFIG_PATH="/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH"
    if ! pkg-config --atleast-version=1.28.0 gstreamer-1.0 2>/dev/null; then
      echo "[Setup] Installing GStreamer 1.28.9 from source..."
      bash /app/applet/.devcontainer/setup/gstreamer.sh || true
    fi

    # 2. Check & install OBS Studio 32.2.2 AppImage
    if [ ! -x /opt/obs-studio-32/AppDir/AppRun ]; then
      echo "[Setup] Downloading and extracting OBS Studio 32.2.2..."
      mkdir -p /opt/obs-studio-32
      APPIMAGE_URL="https://github.com/ivan-hc/OBS-Studio-appimage/releases/download/20261024-102748/OBS-Studio_32.2.2-1-archimage5.0-lite-x86_64.AppImage"
      curl -L -s --retry 3 -o /tmp/obs32.AppImage "$APPIMAGE_URL"
      chmod +x /tmp/obs32.AppImage
      cd /opt/obs-studio-32
      /tmp/obs32.AppImage --appimage-extract >/dev/null 2>&1 || true
      if [ -d /opt/obs-studio-32/squashfs-root ] && [ ! -d /opt/obs-studio-32/AppDir ]; then
        mv /opt/obs-studio-32/squashfs-root /opt/obs-studio-32/AppDir
        ln -sf ./AppDir /opt/obs-studio-32/squashfs-root
      fi
      rm -f /tmp/obs32.AppImage

      # Apply saved OBS settings, docks, scenes & sources only on fresh OBS install
      if [ -d /app/applet/.devcontainer/obs-studio-config ]; then
        echo "[Setup] Applying saved OBS settings, docks, scenes and sources for fresh OBS install..."
        mkdir -p /root/.config/obs-studio
        cp -rf /app/applet/.devcontainer/obs-studio-config/* /root/.config/obs-studio/ 2>/dev/null || true
      fi
    fi

    if [ -x /opt/obs-studio-32/AppDir/AppRun ]; then
      for MESA_DIR in /opt/obs-studio-32/AppDir/lib /opt/obs-studio-32/AppDir/.junest/usr/lib; do
        if [ -d "$MESA_DIR" ] && [ ! -L "$MESA_DIR/dri" ] && [ -d /usr/lib/x86_64-linux-gnu/dri ]; then
          mkdir -p "$MESA_DIR/.mesa-softpipe-bak"
          mv "$MESA_DIR/dri" "$MESA_DIR/.mesa-softpipe-bak/" 2>/dev/null || true
          mv "$MESA_DIR"/libEGL_mesa* "$MESA_DIR"/libGLX_mesa* "$MESA_DIR"/libgbm* "$MESA_DIR"/libglapi* "$MESA_DIR"/libgallium* "$MESA_DIR/.mesa-softpipe-bak/" 2>/dev/null || true
          ln -sf /usr/lib/x86_64-linux-gnu/dri "$MESA_DIR/dri"
          cp -P /usr/lib/x86_64-linux-gnu/libEGL_mesa* /usr/lib/x86_64-linux-gnu/libGLX_mesa* /usr/lib/x86_64-linux-gnu/libgbm* /usr/lib/x86_64-linux-gnu/libglapi* /usr/lib/x86_64-linux-gnu/libLLVM* "$MESA_DIR/" 2>/dev/null || true
        fi
      done

      if [ -d /opt/obs-studio-32/AppDir/.junest/usr/share/obs ]; then
        rm -rf /usr/share/obs /usr/lib/obs-plugins 2>/dev/null || true
        ln -sf /opt/obs-studio-32/AppDir/.junest/usr/share/obs /usr/share/obs
        ln -sf /opt/obs-studio-32/AppDir/.junest/usr/lib/obs-plugins /usr/lib/obs-plugins
      fi

      cat << 'WEOF' > /usr/local/bin/obs32-wrapper
#!/bin/sh
J="/opt/obs-studio-32/AppDir/.junest"
export GST_PLUGIN_PATH="/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0:/usr/lib/x86_64-linux-gnu/gstreamer-1.0:$J/usr/lib/gstreamer-1.0"
export GST_PLUGIN_SYSTEM_PATH="/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0:/usr/lib/x86_64-linux-gnu/gstreamer-1.0:$J/usr/lib/gstreamer-1.0"
export OBS_WEBRTC_TURN_SERVER="turn://obs:turnpassword@127.0.0.1:3478?transport=tcp"
export OBS_WEBRTC_ICE_POLICY="relay"
if [ -x "$J/usr/bin/obs" ]; then
  export PYTHONHOME="$J/usr"
  export QT_PLUGIN_PATH="$J/usr/lib/qt6/plugins:$J/usr/lib/qt/plugins"
  export OBS_PLUGINS_PATH="/root/.config/obs-studio/plugins:$J/usr/lib/obs-plugins"
  export OBS_PLUGINS_DATA_PATH="$J/usr/share/obs/obs-plugins"
  export LIBGL_DRIVERS_PATH="/usr/lib/x86_64-linux-gnu/dri"
  exec "$J/usr/lib/ld-linux-x86-64.so.2" \
    --library-path "/usr/local/lib/x86_64-linux-gnu:/usr/local/lib:$J/usr/lib:$J/usr/lib/gstreamer-1.0:$J/usr/lib/libproxy:$J/usr/lib/pulseaudio:$J/usr/lib/obs-scripting:/usr/lib/x86_64-linux-gnu" \
    "$J/usr/bin/obs" "$@"
else
  exec /opt/obs-studio-32/AppDir/AppRun "$@"
fi
WEOF
      chmod +x /usr/local/bin/obs32-wrapper
      ln -sf /usr/local/bin/obs32-wrapper /usr/bin/obs
    fi

    # Ensure OBS 32 headers (VIDEO_FORMAT_V210) & system libobs symlinks are active
    if [ -d /opt/obs-studio-32/AppDir/.junest/usr/lib ]; then
      cp -f /opt/obs-studio-32/AppDir/.junest/usr/lib/libobs.so.30 /usr/lib/x86_64-linux-gnu/ 2>/dev/null || true
      cp -f /opt/obs-studio-32/AppDir/.junest/usr/lib/libobs-frontend-api.so.30 /usr/lib/x86_64-linux-gnu/ 2>/dev/null || true
      cp -f /opt/obs-studio-32/AppDir/.junest/usr/lib/libobs-opengl.so.30 /usr/lib/x86_64-linux-gnu/ 2>/dev/null || true
      cp -f /opt/obs-studio-32/AppDir/.junest/usr/lib/libobs-scripting.so.30 /usr/lib/x86_64-linux-gnu/ 2>/dev/null || true
      ln -sf libobs.so.30 /usr/lib/x86_64-linux-gnu/libobs.so
      ln -sf libobs.so.30 /usr/lib/x86_64-linux-gnu/libobs.so.0
      ln -sf libobs-frontend-api.so.30 /usr/lib/x86_64-linux-gnu/libobs-frontend-api.so
      ln -sf libobs-frontend-api.so.30 /usr/lib/x86_64-linux-gnu/libobs-frontend-api.so.0
      ldconfig
    fi
    if [ -f /usr/include/obs/media-io/video-io.h ] && ! grep -q "VIDEO_FORMAT_V210" /usr/include/obs/media-io/video-io.h; then
      sed -i 's#VIDEO_FORMAT_YA2L, // Little Endian#VIDEO_FORMAT_YA2L, // Little Endian\\n\\tVIDEO_FORMAT_V210,#' /usr/include/obs/media-io/video-io.h 2>/dev/null || true
    fi

    # 3. Check, compile & install obs-gstreamer plugin with Qt6 Dock
    cd /app/applet
    export PKG_CONFIG_PATH="/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH"
    if [ ! -f /app/applet/build/obs-gstreamer.so ]; then
      echo "[Setup] Building obs-gstreamer plugin with Qt6 Dock..."
      rm -rf build
      meson setup build -Ddock=enabled
      ninja -C build
    fi

    mkdir -p /root/.config/obs-studio/plugins/obs-gstreamer/bin/64bit
    cp -f /app/applet/build/obs-gstreamer.so /root/.config/obs-studio/plugins/obs-gstreamer/bin/64bit/obs-gstreamer.so
    rm -f /opt/obs-studio-32/AppDir/lib/obs-plugins/obs-gstreamer.so 2>/dev/null || true
    rm -f /opt/obs-studio-32/AppDir/.junest/usr/lib/obs-plugins/obs-gstreamer.so 2>/dev/null || true

    # 4. Configure Desktop shortcuts & VNC startup/passwords
    mkdir -p /root/Desktop
    cp -f /usr/share/applications/thunar.desktop /root/Desktop/ 2>/dev/null || true
    cp -f /usr/share/applications/xarchiver.desktop /root/Desktop/ 2>/dev/null || true
    rm -f /root/Desktop/firefox-esr.desktop 2>/dev/null || true
    cp -f /usr/share/applications/google-chrome.desktop /root/Desktop/ 2>/dev/null || true
    cp -f ~/.local/share/applications/web-browser.desktop /root/Desktop/ 2>/dev/null || true
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
    pkill -9 -x ld-linux-x86-64 2>/dev/null || true
    pkill -9 -x thunar 2>/dev/null || true
    pkill -9 -x xarchiver 2>/dev/null || true
    pkill -9 -x firefox-esr 2>/dev/null || true
    pkill -9 -x chrome 2>/dev/null || true
    pkill -9 -x google-chrome 2>/dev/null || true
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
    DISPLAY=:1 setsid obs --disable-shutdown-check >/tmp/obs.log 2>&1 < /dev/null &
    sleep 2.5
    DISPLAY=:1 wmctrl -r "OBS" -b add,maximized_vert,maximized_horz 2>/dev/null || true
  `;

  runCommandWithLiveLogs('Full PC Provisioning & Desktop Restart', fullProvisionAndRestartCmd, (err) => {
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
  exec('(pgrep -x obs >/dev/null || pgrep -x ld-linux-x86-64 >/dev/null) && echo "1" || echo "0"', (err, stdout) => {
    if (stdout && stdout.trim() !== '1') {
      exec('rm -rf /root/.config/obs-studio/.sentinel 2>/dev/null || true; DISPLAY=:1 setsid obs --disable-shutdown-check >/tmp/obs.log 2>&1 < /dev/null &', () => {});
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

// Ensure custom portrait modes on server boot; auto-provision if environment was reset or incomplete (including GStreamer 1.28 & installed plugin)
exec('which Xtigervnc vncserver websockify startxfce4 >/dev/null 2>&1 && [ -x /opt/obs-studio-32/AppDir/AppRun ] && [ -f /app/applet/build/obs-gstreamer.so ] && [ -f /root/.config/obs-studio/plugins/obs-gstreamer/bin/64bit/obs-gstreamer.so ] && (PKG_CONFIG_PATH="/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH" pkg-config --atleast-version=1.28.0 gstreamer-1.0 2>/dev/null) && echo "1" || echo "0"', (err, stdout) => {
  if (stdout && stdout.trim() !== '1') {
    addLog('Detected fresh/reset or incomplete container on boot (GStreamer 1.28+ or plugin missing) — running automatic provisioning...');
    restartFullPC();
  } else {
    ensureCustomModes();
  }
});

// Background supervisor to auto-heal desktop services, coturn & keep OBS open
setInterval(() => {
  if (isRestarting || isSettingUp) return;
  ensureCoturn();
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

  // Dynamic TURN configuration endpoint for WebRTC WHEP viewers
  if (req.url === '/turn-config.json' || req.url.startsWith('/turn-config.json?')) {
    res.writeHead(200, {
      'Content-Type': 'application/json',
      'Cache-Control': 'no-cache, no-store, must-revalidate'
    });
    res.end(JSON.stringify(getTurnConfigPayload()));
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
        publicUrl: webrtcHttpTunnel.url || '',
        turnTunnel: turnTunnelInfo,
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
        exec('(pgrep -x obs >/dev/null || pgrep -x ld-linux-x86-64 >/dev/null) && echo "1" || echo "0"', (err3, stdout) => {
          const obsRunning = stdout && stdout.trim() === '1';
          exec('which vncserver websockify obs >/dev/null && echo "1" || echo "0"', (err4, outInstalled) => {
            const isInstalled = outInstalled && outInstalled.trim() === '1';
            exec('ps -eo pid,etimes,args --no-headers 2>/dev/null | grep -E "apt-get|dpkg|curl|AppImage|meson|ninja|cc |c\\+\\+|vncserver|Xtigervnc|websockify|startxfce4|obs" | grep -v -E "grep -E|/api/vnc/status|ps -eo|PS4="', (err5, psOut) => {
              const runningProcesses = (psOut || '')
                .split(/\r?\n/)
                .map(s => s.trim())
                .filter(Boolean)
                .slice(-15);

              res.writeHead(200, { 'Content-Type': 'application/json' });
              res.end(JSON.stringify({
                vncOnline,
                novncOnline,
                obsRunning,
                isInstalled,
                isSettingUp,
                isRestarting,
                currentCommand: currentRunningCmd || (runningProcesses.length > 0 ? runningProcesses[runningProcesses.length - 1] : ''),
                runningProcesses,
                vncPort: VNC_PORT,
                novncPort: NOVNC_PORT,
                vncPassword: 'vncpassword',
                tcpTunnel: tcpTunnelInfo,
                webrtcPublicUrl: webrtcHttpTunnel.url || '',
                turnTunnel: turnTunnelInfo,
                currentResolution: currentResolutionMode,
                logs: setupLogs.slice(-120),
                timestamp: new Date().toISOString()
              }));
            });
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

  // API Rebuild Plugin & Restart OBS trigger
  if (req.url === '/api/vnc/rebuild-plugin' && req.method === 'POST') {
    rebuildPluginAndRestartOBS((err) => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        success: !err,
        message: err ? err.message : 'Plugin rebuilt, installed, and OBS restarted'
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

  // API Reset VNC IP & Tunnels trigger (only when explicitly requested)
  if (req.url === '/api/vnc/reset-ip' && req.method === 'POST') {
    addLog('Resetting VNC IP & generating fresh tunnels on explicit user request...');
    try {
      if (fs.existsSync(TUNNEL_CACHE_FILE)) {
        fs.unlinkSync(TUNNEL_CACHE_FILE);
      }
    } catch (e) {}
    tcpTunnelInfo.full = '';
    tcpTunnelInfo.status = 'connecting';
    tcpTunnelInfo.isReused = false;
    webrtcHttpTunnel.url = '';
    webrtcHttpTunnel.status = 'connecting';
    webrtcHttpTunnel.isReused = false;
    turnTunnelInfo.full = '';
    turnTunnelInfo.url = '';
    turnTunnelInfo.status = 'connecting';
    turnTunnelInfo.isReused = false;

    startTCPTunnel(true);

    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      success: true,
      message: 'VNC IP reset initiated. Fresh tunnel being established.'
    }));
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
      // Gracefully handle offline noVNC service with live running commands console
      res.writeHead(503, {
        'Content-Type': 'text/html; charset=utf-8',
        'Retry-After': '2'
      });
      res.end(`
        <!DOCTYPE html>
        <html style="background:#020617;color:#e2e8f0;font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;height:100%;margin:0;">
          <body style="margin:0;height:100%;display:flex;flex-direction:column;background:#020617;padding:16px;box-sizing:border-box;">
            <div style="display:flex;align-items:center;justify-content:space-between;background:#0f172a;border:1px solid #1e293b;padding:12px 16px;border-radius:8px 8px 0 0;">
              <div style="display:flex;align-items:center;gap:10px;">
                <div style="width:14px;height:14px;border:2px solid #6366f1;border-top-color:transparent;border-radius:50%;animation:spin 1s linear infinite;"></div>
                <strong style="color:#f8fafc;font-size:13px;">Desktop Starting — Live Command Console</strong>
              </div>
              <span id="cmd-badge" style="font-size:11px;color:#38bdf8;background:#020617;padding:3px 8px;border-radius:4px;border:1px solid #1e293b;">Initializing...</span>
            </div>
            <div style="background:#090d16;border-left:1px solid #1e293b;border-right:1px solid #1e293b;padding:8px 16px;font-size:11px;color:#94a3b8;border-bottom:1px solid #1e293b;">
              <div style="color:#64748b;font-size:10px;text-transform:uppercase;margin-bottom:4px;">Active Running Processes (PID / Elapsed / Command)</div>
              <pre id="procs" style="margin:0;color:#a5f3fc;white-space:pre-wrap;max-height:90px;overflow-y:auto;">Checking running commands...</pre>
            </div>
            <pre id="logs" style="flex:1;margin:0;background:#020617;border:1px solid #1e293b;border-top:none;border-radius:0 0 8px 8px;padding:14px 16px;font-size:12px;line-height:1.5;color:#cbd5e1;overflow-y:auto;white-space:pre-wrap;"></pre>
            <style>@keyframes spin { to { transform: rotate(360deg); } }</style>
            <script>
              function poll() {
                fetch('/api/vnc/status').then(r => r.json()).then(d => {
                  if (d.vncOnline && d.novncOnline && !d.isSettingUp && !d.isRestarting) {
                    window.location.reload();
                    return;
                  }
                  document.getElementById('cmd-badge').textContent = d.currentCommand || 'Provisioning & starting desktop...';
                  document.getElementById('procs').textContent = (d.runningProcesses && d.runningProcesses.length) ? d.runningProcesses.join('\\n') : 'Waiting for next command...';
                  var logsEl = document.getElementById('logs');
                  logsEl.textContent = (d.logs || []).join('\\n');
                  logsEl.scrollTop = logsEl.scrollHeight;
                }).catch(() => {});
              }
              setInterval(poll, 1200);
              poll();
            </script>
          </body>
        </html>
      `);
    });

    req.pipe(proxyReq);
    return;
  }

  // Proxy WHEP / signaling requests to GStreamer WebRTC server on port 8888
  if (req.url === '/whep' || req.url.startsWith('/whep?') || req.url.startsWith('/whep/')) {
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

    res.writeHead(200, {
      'Content-Type': contentType,
      'Cache-Control': 'no-cache, no-store, must-revalidate'
    });
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
