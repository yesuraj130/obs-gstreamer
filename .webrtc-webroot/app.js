(function () {
  'use strict';

  // Navigation & Tabs
  var tabNavVnc = document.getElementById('tab-nav-vnc');
  var tabNavWebrtc = document.getElementById('tab-nav-webrtc');
  var viewVnc = document.getElementById('view-vnc');
  var viewWebrtc = document.getElementById('view-webrtc');
  var navVncDot = document.getElementById('nav-vnc-dot');
  var navWebrtcDot = document.getElementById('nav-webrtc-dot');
  var vncStatusText = document.getElementById('vnc-status-text');
  var btnRestartVnc = document.getElementById('btn-restart-vnc');
  var btnReloadIframe = document.getElementById('btn-reload-iframe');
  var vncIframe = document.getElementById('vnc-iframe');
  var btnGotoDesktop = document.getElementById('btn-goto-desktop');

  // Video Player Elements
  var video = document.getElementById('video');
  var statusEl = document.getElementById('status');
  var testCanvas = document.getElementById('test-canvas');
  var offlineOverlay = document.getElementById('offline-overlay');
  var overlayTitle = document.getElementById('overlay-title');
  var overlayDesc = document.getElementById('overlay-desc');
  var lblEndpoint = document.getElementById('lbl-endpoint');
  var lblPortStatus = document.getElementById('lbl-port-status');
  var lblRetryCountdown = document.getElementById('lbl-retry-countdown');
  var btnSwitchTestPattern = document.getElementById('btn-switch-test-pattern');

  var badgeSourceMode = document.getElementById('badge-source-mode');
  var badgeStreamState = document.getElementById('badge-stream-state');
  var streamPulse = document.getElementById('stream-pulse');
  var streamText = document.getElementById('stream-text');
  var badgeResolution = document.getElementById('badge-resolution');
  var badgeBitrate = document.getElementById('badge-bitrate');
  var audioVuMeter = document.getElementById('audio-vu-meter');
  var clockDisplay = document.getElementById('clock-display');

  // Stats Elements
  var statPcState = document.getElementById('stat-pc-state');
  var statIceState = document.getElementById('stat-ice-state');
  var statSignaling = document.getElementById('stat-signaling');
  var statTracks = document.getElementById('stat-tracks');
  var statDimensions = document.getElementById('stat-dimensions');
  var statFps = document.getElementById('stat-fps');
  var statBitrate = document.getElementById('stat-bitrate');
  var statPacketloss = document.getElementById('stat-packetloss');

  // Player Controls
  var btnPlayPause = document.getElementById('btn-play-pause');
  var iconPlay = document.getElementById('icon-play');
  var iconPause = document.getElementById('icon-pause');
  var btnMute = document.getElementById('btn-mute');
  var iconMuted = document.getElementById('icon-muted');
  var iconUnmuted = document.getElementById('icon-unmuted');
  var volumeSlider = document.getElementById('volume-slider');
  var btnSnapshot = document.getElementById('btn-snapshot');
  var btnPip = document.getElementById('btn-pip');
  var btnFullscreen = document.getElementById('btn-fullscreen');
  var btnToggleStats = document.getElementById('btn-toggle-stats');
  var sidePanel = document.getElementById('side-panel');
  var btnClearLogs = document.getElementById('btn-clear-logs');

  // Guide Modal
  var btnOpenGuide = document.getElementById('btn-open-guide');
  var btnCloseGuide = document.getElementById('btn-close-guide');
  var btnModalGotIt = document.getElementById('btn-modal-got-it');
  var modalGuide = document.getElementById('modal-guide');

  // State Variables
  var activeView = 'vnc'; // 'vnc' | 'webrtc'
  var isTestPattern = false;
  var pc = null;
  var loopbackSender = null;
  var reconnectTimer = null;
  var countdownTimer = null;
  var reconnectSeconds = 0;
  var reconnectAttempts = 0;
  var statsInterval = null;
  var prevBytesReceived = 0;
  var prevStatsTimestamp = 0;
  var isManualDisconnect = false;
  var testAnimationId = null;
  var testAudioCtx = null;

  function log(msg, type) {
    var now = new Date();
    var timeStr = now.toTimeString().split(' ')[0] + '.' + String(now.getMilliseconds()).padStart(3, '0');
    var line = '[' + timeStr + '] ' + msg;
    console.log('[obs-app]', line);
    if (statusEl) {
      statusEl.textContent = line + '\n' + statusEl.textContent.slice(0, 4000);
    }
  }

  // Tab Navigation Handling
  function switchTab(view) {
    activeView = view;
    if (view === 'vnc') {
      viewVnc.classList.remove('hidden');
      viewWebrtc.classList.add('hidden');
      tabNavVnc.className = 'px-3.5 py-1.5 rounded-md font-medium transition flex items-center gap-2 bg-indigo-600 text-white shadow-sm';
      tabNavWebrtc.className = 'px-3.5 py-1.5 rounded-md font-medium transition flex items-center gap-2 text-slate-400 hover:text-slate-200';
      badgeSourceMode.textContent = 'OBS Desktop Active';
    } else {
      viewVnc.classList.add('hidden');
      viewWebrtc.classList.remove('hidden');
      tabNavWebrtc.className = 'px-3.5 py-1.5 rounded-md font-medium transition flex items-center gap-2 bg-indigo-600 text-white shadow-sm';
      tabNavVnc.className = 'px-3.5 py-1.5 rounded-md font-medium transition flex items-center gap-2 text-slate-400 hover:text-slate-200';
      badgeSourceMode.textContent = isTestPattern ? 'Test Pattern' : 'WebRTC Live';
      if (!isTestPattern) {
        connectWhep();
      }
    }
  }

  tabNavVnc.addEventListener('click', function () { switchTab('vnc'); });
  tabNavWebrtc.addEventListener('click', function () { switchTab('webrtc'); });
  if (btnGotoDesktop) {
    btnGotoDesktop.addEventListener('click', function () { switchTab('vnc'); });
  }

  btnReloadIframe.addEventListener('click', function () {
    if (vncIframe) {
      var currentSrc = vncIframe.src;
      vncIframe.src = 'about:blank';
      setTimeout(function () {
        vncIframe.src = currentSrc;
      }, 100);
    }
  });

  // Restart VNC & OBS Studio
  btnRestartVnc.addEventListener('click', function () {
    vncStatusText.textContent = 'Restarting OBS...';
    log('Triggering restart of OBS Studio and VNC session...', 'info');

    fetch('/api/vnc/start', { method: 'POST' })
      .then(function (res) { return res.json(); })
      .then(function (data) {
        log('OBS restart command completed', 'info');
        setTimeout(function () {
          checkVncStatus();
          btnReloadIframe.click();
        }, 1500);
      })
      .catch(function (err) {
        log('Restart request error: ' + err.message, 'error');
      });
  });

  // VNC & OBS Health Check
  function checkVncStatus() {
    fetch('/api/vnc/status')
      .then(function (res) { return res.json(); })
      .then(function (data) {
        if (data && data.vncOnline && data.novncOnline) {
          navVncDot.className = 'w-2 h-2 rounded-full bg-emerald-400 animate-pulse';
          vncStatusText.textContent = data.obsRunning ? 'OBS & VNC Active' : 'VNC Desktop Ready';
        } else {
          navVncDot.className = 'w-2 h-2 rounded-full bg-amber-400';
          vncStatusText.textContent = 'Desktop Starting...';
        }
      })
      .catch(function () {});
  }

  setInterval(checkVncStatus, 3000);
  checkVncStatus();

  // WebRTC Stream Status
  function checkStreamStatus() {
    fetch('/api/status')
      .then(function (res) { return res.json(); })
      .then(function (data) {
        if (data && data.online) {
          navWebrtcDot.className = 'w-2 h-2 rounded-full bg-emerald-400 animate-pulse';
          lblPortStatus.className = 'text-emerald-400 font-semibold';
          lblPortStatus.textContent = '127.0.0.1:8888 (STREAMING)';

          // If stream just became available while in player view, connect immediately!
          if (activeView === 'webrtc' && !isTestPattern && (!pc || pc.connectionState === 'closed')) {
            connectWhep();
          }
        } else {
          navWebrtcDot.className = 'w-2 h-2 rounded-full bg-slate-500';
          lblPortStatus.className = 'text-amber-400 font-semibold';
          lblPortStatus.textContent = '127.0.0.1:8888 (Waiting for OBS)';
        }
      })
      .catch(function () {});
  }

  setInterval(checkStreamStatus, 3000);
  checkStreamStatus();

  function setStreamBadge(state, label) {
    if (!badgeStreamState || !streamPulse || !streamText) return;
    if (state === 'live') {
      streamPulse.className = 'w-2 h-2 rounded-full bg-red-500 animate-pulse';
      badgeStreamState.className = 'px-2.5 py-1 rounded-md text-xs font-semibold uppercase tracking-wider backdrop-blur-md bg-red-950/80 border border-red-700/80 text-red-200 flex items-center gap-2 shadow-lg';
      streamText.textContent = label || 'LIVE';
      offlineOverlay.classList.add('hidden');
    } else if (state === 'connecting') {
      streamPulse.className = 'w-2 h-2 rounded-full bg-amber-400 animate-ping';
      badgeStreamState.className = 'px-2.5 py-1 rounded-md text-xs font-semibold uppercase tracking-wider backdrop-blur-md bg-amber-950/80 border border-amber-700/80 text-amber-200 flex items-center gap-2 shadow-lg';
      streamText.textContent = label || 'CONNECTING';
    } else if (state === 'test') {
      streamPulse.className = 'w-2 h-2 rounded-full bg-emerald-400';
      badgeStreamState.className = 'px-2.5 py-1 rounded-md text-xs font-semibold uppercase tracking-wider backdrop-blur-md bg-emerald-950/80 border border-emerald-700/80 text-emerald-200 flex items-center gap-2 shadow-lg';
      streamText.textContent = label || 'TEST PATTERN';
      offlineOverlay.classList.add('hidden');
    } else {
      streamPulse.className = 'w-2 h-2 rounded-full bg-slate-500';
      badgeStreamState.className = 'px-2.5 py-1 rounded-md text-xs font-semibold uppercase tracking-wider backdrop-blur-md bg-slate-900/80 border border-slate-700 text-slate-300 flex items-center gap-2 shadow-lg';
      streamText.textContent = label || 'OFFLINE';
      if (!isTestPattern) {
        offlineOverlay.classList.remove('hidden');
      }
    }
  }

  // Clock in telemetry
  setInterval(function () {
    if (clockDisplay) clockDisplay.textContent = new Date().toTimeString().split(' ')[0];
  }, 1000);

  function cleanupWebRtc() {
    clearTimeout(reconnectTimer);
    clearInterval(countdownTimer);
    clearInterval(statsInterval);
    statsInterval = null;

    if (pc) {
      try {
        pc.ontrack = null;
        pc.close();
      } catch (e) {}
      pc = null;
    }
    if (loopbackSender) {
      try { loopbackSender.close(); } catch (e) {}
      loopbackSender = null;
    }
    if (testAnimationId) {
      cancelAnimationFrame(testAnimationId);
      testAnimationId = null;
    }
    if (testAudioCtx) {
      try { testAudioCtx.close(); } catch (e) {}
      testAudioCtx = null;
    }
    if (video.srcObject) {
      try {
        video.srcObject.getTracks().forEach(function (t) { t.stop(); });
      } catch (e) {}
      video.srcObject = null;
    }

    statPcState.textContent = 'closed';
    statIceState.textContent = 'closed';
    statSignaling.textContent = 'closed';
    statTracks.textContent = 'none';
    statDimensions.textContent = '-';
    statFps.textContent = '0 fps';
    statBitrate.textContent = '0 kbps';
    badgeResolution.classList.add('hidden');
    badgeBitrate.classList.add('hidden');
    audioVuMeter.classList.add('hidden');
  }

  function scheduleReconnect(delayMs) {
    clearTimeout(reconnectTimer);
    clearInterval(countdownTimer);
    if (isManualDisconnect || isTestPattern) return;

    var delay = delayMs || Math.min(1000 * Math.pow(1.5, reconnectAttempts++), 10000);
    reconnectSeconds = Math.ceil(delay / 1000);

    if (lblRetryCountdown) lblRetryCountdown.textContent = 'in ' + reconnectSeconds + 's';

    countdownTimer = setInterval(function () {
      reconnectSeconds--;
      if (reconnectSeconds <= 0) {
        clearInterval(countdownTimer);
        if (lblRetryCountdown) lblRetryCountdown.textContent = 'Connecting...';
      } else {
        if (lblRetryCountdown) lblRetryCountdown.textContent = 'in ' + reconnectSeconds + 's';
      }
    }, 1000);

    reconnectTimer = setTimeout(function () {
      connectWhep();
    }, delay);
  }

  // WHEP Connection
  function connectWhep() {
    isTestPattern = false;
    cleanupWebRtc();
    isManualDisconnect = false;

    setStreamBadge('connecting', 'Connecting...');
    log('Starting WebRTC handshake with /whep...', 'info');

    pc = new RTCPeerConnection({
      iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
    });
    window.__obsWebRTCPeerConnection = pc;

    statPcState.textContent = pc.connectionState;
    statIceState.textContent = pc.iceConnectionState;
    statSignaling.textContent = pc.signalingState;

    pc.onconnectionstatechange = function () {
      statPcState.textContent = pc.connectionState;
      if (pc.connectionState === 'connected') {
        reconnectAttempts = 0;
        setStreamBadge('live', 'LIVE');
        startStatsMonitor();
      } else if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
        setStreamBadge('offline', 'DISCONNECTED');
        scheduleReconnect();
      }
    };

    pc.oniceconnectionstatechange = function () {
      statIceState.textContent = pc.iceConnectionState;
    };
    pc.onsignalingstatechange = function () {
      statSignaling.textContent = pc.signalingState;
    };

    pc.addTransceiver('video', { direction: 'recvonly' });
    try { pc.addTransceiver('audio', { direction: 'recvonly' }); } catch (e) {}

    pc.ontrack = function (event) {
      log('WebRTC track received: ' + event.track.kind, 'info');
      statTracks.textContent = event.track.kind;
      if (!video.srcObject) {
        video.srcObject = event.streams[0] || new MediaStream([event.track]);
      } else {
        video.srcObject.addTrack(event.track);
      }
      video.play().catch(function () {});
      setStreamBadge('live', 'LIVE');
      audioVuMeter.classList.remove('hidden');
    };

    video.onloadedmetadata = function () {
      var res = video.videoWidth + 'x' + video.videoHeight;
      statDimensions.textContent = res;
      badgeResolution.textContent = res;
      badgeResolution.classList.remove('hidden');
    };

    pc.createOffer()
      .then(function (offer) {
        return pc.setLocalDescription(offer);
      })
      .then(function () {
        return fetch('/whep', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/sdp',
            'Accept': 'application/sdp'
          },
          body: pc.localDescription.sdp
        });
      })
      .then(function (response) {
        var contentType = response.headers.get('content-type') || '';
        if (!response.ok) {
          return response.text().then(function (body) {
            throw new Error('OBS GStreamer stream offline (status ' + response.status + ')');
          });
        }
        return response.text().then(function (answerSdp) {
          var trimmed = (answerSdp || '').trim();
          if (contentType.includes('text/html') || trimmed.startsWith('<') || !trimmed.startsWith('v=')) {
            throw new Error('Stream offline (gateway returned HTML instead of SDP answer)');
          }
          log('Received SDP answer (' + trimmed.length + ' bytes). Setting remote description...', 'info');
          return pc.setRemoteDescription({ type: 'answer', sdp: trimmed });
        });
      })
      .then(function () {
        log('Remote description set. Waiting for video stream...', 'info');
      })
      .catch(function (err) {
        log(err.message, 'warn');
        setStreamBadge('offline', 'OFFLINE');
        overlayTitle.textContent = 'OBS WebRTC Stream Offline';
        overlayDesc.textContent = 'OBS Studio is running on the Remote Desktop. Switch to the "OBS Desktop (VNC)" tab to configure and start the stream!';
        scheduleReconnect();
      });
  }

  // Built-in Test Pattern Generator (WebRTC Loopback)
  function startTestPattern() {
    isTestPattern = true;
    cleanupWebRtc();
    isManualDisconnect = false;

    setStreamBadge('test', 'TEST PATTERN');
    log('Running WebRTC Loopback Test Pattern generator...', 'info');

    var ctx = testCanvas.getContext('2d');
    var frame = 0;
    var colors = ['#ffffff', '#ffea00', '#00e5ff', '#00e676', '#e040fb', '#ff1744', '#2979ff', '#212121'];

    function draw() {
      var w = testCanvas.width;
      var h = testCanvas.height;
      var barW = w / colors.length;

      for (var i = 0; i < colors.length; i++) {
        ctx.fillStyle = colors[i];
        ctx.fillRect(i * barW, 0, barW, h * 0.7);
      }

      var grad = ctx.createLinearGradient(0, 0, w, 0);
      grad.addColorStop(0, '#000');
      grad.addColorStop(0.5, '#6366f1');
      grad.addColorStop(1, '#fff');
      ctx.fillStyle = grad;
      ctx.fillRect(0, h * 0.7, w, h * 0.1);

      ctx.fillStyle = '#090d16';
      ctx.fillRect(0, h * 0.8, w, h * 0.2);

      var boxX = (Math.sin(frame * 0.05) * 0.5 + 0.5) * (w - 200) + 20;
      ctx.fillStyle = '#4f46e5';
      ctx.fillRect(boxX, h * 0.72, 160, 24);
      ctx.fillStyle = '#fff';
      ctx.font = 'bold 12px monospace';
      ctx.fillText('WEBRTC TEST STREAM', boxX + 12, h * 0.72 + 16);

      ctx.fillStyle = '#fff';
      ctx.font = 'bold 22px monospace';
      ctx.fillText('OBS GSTREAMER PLUGIN - TEST PATTERN', 30, h * 0.88);
      ctx.fillStyle = '#38bdf8';
      ctx.font = '18px monospace';
      ctx.fillText(new Date().toISOString() + ' | Frame: ' + frame++, 30, h * 0.94);

      testAnimationId = requestAnimationFrame(draw);
    }
    draw();

    var stream = testCanvas.captureStream(30);

    var sender = new RTCPeerConnection();
    var receiver = new RTCPeerConnection();
    loopbackSender = sender;
    pc = receiver;

    sender.onicecandidate = function (e) { if (e.candidate) receiver.addIceCandidate(e.candidate); };
    receiver.onicecandidate = function (e) { if (e.candidate) sender.addIceCandidate(e.candidate); };

    receiver.ontrack = function (e) {
      statTracks.textContent = e.track.kind;
      if (!video.srcObject) video.srcObject = e.streams[0] || new MediaStream([e.track]);
      else video.srcObject.addTrack(e.track);
      video.play().catch(function () {});
      audioVuMeter.classList.remove('hidden');
    };

    stream.getTracks().forEach(function (t) { sender.addTrack(t, stream); });

    sender.createOffer()
      .then(function (o) { return sender.setLocalDescription(o); })
      .then(function () { return receiver.setRemoteDescription(sender.localDescription); })
      .then(function () { return receiver.createAnswer(); })
      .then(function (a) { return receiver.setLocalDescription(a); })
      .then(function () { return sender.setRemoteDescription(receiver.localDescription); })
      .then(function () {
        statDimensions.textContent = '1280x720';
        badgeResolution.textContent = '1280x720 @ 30fps';
        badgeResolution.classList.remove('hidden');
        log('WebRTC Loopback running successfully', 'info');
      });
  }

  btnSwitchTestPattern.addEventListener('click', function () {
    startTestPattern();
  });

  // Telemetry getStats
  function startStatsMonitor() {
    if (statsInterval) clearInterval(statsInterval);
    statsInterval = setInterval(function () {
      if (!pc) return;
      pc.getStats().then(function (stats) {
        var currentBytes = 0;
        var currentTs = 0;
        var fps = 0;
        var lost = 0;
        stats.forEach(function (r) {
          if (r.type === 'inbound-rtp' && (r.kind === 'video' || r.mediaType === 'video')) {
            currentBytes = r.bytesReceived || 0;
            currentTs = r.timestamp || Date.now();
            fps = r.framesPerSecond || (video.videoWidth ? 30 : 0);
            lost = r.packetsLost || 0;
          }
        });
        if (prevStatsTimestamp && currentTs > prevStatsTimestamp) {
          var dt = (currentTs - prevStatsTimestamp) / 1000;
          var db = currentBytes - prevBytesReceived;
          if (db > 0 && dt > 0) {
            var kbps = Math.round((db * 8) / (dt * 1000));
            statBitrate.textContent = kbps + ' kbps';
            badgeBitrate.textContent = kbps + ' kbps';
            badgeBitrate.classList.remove('hidden');
          }
        }
        prevBytesReceived = currentBytes;
        prevStatsTimestamp = currentTs;
        if (fps > 0) {
          statFps.textContent = Math.round(fps) + ' fps';
          if (video.videoWidth) {
            badgeResolution.textContent = video.videoWidth + 'x' + video.videoHeight + ' @ ' + Math.round(fps) + 'fps';
          }
        }
        statPacketloss.textContent = String(lost);
      }).catch(function () {});
    }, 1000);
  }

  // Player Controls
  btnPlayPause.addEventListener('click', function () {
    if (video.paused) {
      video.play().then(function () {
        iconPlay.classList.add('hidden');
        iconPause.classList.remove('hidden');
      });
    } else {
      video.pause();
      iconPlay.classList.remove('hidden');
      iconPause.classList.add('hidden');
    }
  });

  btnMute.addEventListener('click', function () {
    video.muted = !video.muted;
    if (video.muted) {
      iconMuted.classList.remove('hidden');
      iconUnmuted.classList.add('hidden');
      volumeSlider.value = 0;
    } else {
      iconMuted.classList.add('hidden');
      iconUnmuted.classList.remove('hidden');
      volumeSlider.value = video.volume || 1;
    }
  });

  volumeSlider.addEventListener('input', function (e) {
    var val = parseFloat(e.target.value);
    video.volume = val;
    video.muted = (val === 0);
    if (val === 0) {
      iconMuted.classList.remove('hidden');
      iconUnmuted.classList.add('hidden');
    } else {
      iconMuted.classList.add('hidden');
      iconUnmuted.classList.remove('hidden');
    }
  });

  btnSnapshot.addEventListener('click', function () {
    if (!video.videoWidth || !video.videoHeight) return;
    var c = document.createElement('canvas');
    c.width = video.videoWidth;
    c.height = video.videoHeight;
    c.getContext('2d').drawImage(video, 0, 0);
    var a = document.createElement('a');
    a.download = 'obs-stream-' + Date.now() + '.png';
    a.href = c.toDataURL('image/png');
    a.click();
  });

  btnPip.addEventListener('click', function () {
    if (document.pictureInPictureElement) document.exitPictureInPicture();
    else if (document.pictureInPictureEnabled && video) video.requestPictureInPicture();
  });

  btnFullscreen.addEventListener('click', function () {
    var wrap = document.getElementById('player-wrap');
    if (!document.fullscreenElement) {
      if (wrap.requestFullscreen) wrap.requestFullscreen();
      else if (video.requestFullscreen) video.requestFullscreen();
    } else {
      if (document.exitFullscreen) document.exitFullscreen();
    }
  });

  btnToggleStats.addEventListener('click', function () {
    sidePanel.classList.toggle('hidden');
  });

  btnClearLogs.addEventListener('click', function () {
    statusEl.textContent = '';
  });

  btnOpenGuide.addEventListener('click', function () { modalGuide.classList.remove('hidden'); });
  btnCloseGuide.addEventListener('click', function () { modalGuide.classList.add('hidden'); });
  btnModalGotIt.addEventListener('click', function () { modalGuide.classList.add('hidden'); });
  modalGuide.addEventListener('click', function (e) {
    if (e.target === modalGuide) modalGuide.classList.add('hidden');
  });

  window.addEventListener('beforeunload', function () {
    isManualDisconnect = true;
    cleanupWebRtc();
  });

  // Start with OBS Desktop (VNC) view by default
  switchTab('vnc');
})();
