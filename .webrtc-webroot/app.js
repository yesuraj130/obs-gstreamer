(function () {
  var video = document.getElementById('video');
  var status = document.getElementById('status');
  var pc = null;
  var attempts = 0;

  function setStatus(text) {
    status.textContent = text;
    console.log('[obs-webrtc]', text);
  }

  function connect() {
    setStatus('connecting\u2026');
    pc = new RTCPeerConnection();
    window.__obsWebRTCPeerConnection = pc;
    pc.oniceconnectionstatechange = function () {
      setStatus('connection: ' + pc.connectionState + '\nice: ' + pc.iceConnectionState +
        '\nsignaling: ' + pc.signalingState + '\ntrack: ' + (video.srcObject ? 'yes' : 'no') +
        '\nvideo: ' + video.videoWidth + 'x' + video.videoHeight);
    };
    pc.onsignalingstatechange = function () {
      console.log('[obs-webrtc] signaling state:', pc.signalingState);
    };

    pc.addTransceiver('video', { direction: 'recvonly' });

    pc.ontrack = function (event) {
      console.log('[obs-webrtc] ontrack', event.track.kind, event.track.readyState, event.streams);
        video.srcObject = event.streams[0] || new MediaStream([event.track]);
        video.play().catch(function (err) {
          setStatus('playback error: ' + err.message);
        });
        setStatus('video track received');
    };
      video.onloadedmetadata = function () {
        setStatus('metadata: ' + video.videoWidth + 'x' + video.videoHeight);
      };
      video.onplaying = function () { setStatus('playing: ' + video.videoWidth + 'x' + video.videoHeight); };
      video.onerror = function () { console.error('[obs-webrtc] video error', video.error); };

    pc.onconnectionstatechange = function () {
      setStatus('state: ' + pc.connectionState);
      if (pc.connectionState === 'failed' ||
          pc.connectionState === 'disconnected' ||
          pc.connectionState === 'closed') {
        reconnect();
      }
    };

    pc.createOffer().then(function (offer) {
      return pc.setLocalDescription(offer);
    }).then(function () {
      return fetch('/whep', {
        method: 'POST',
        headers: { 'Content-Type': 'application/sdp' },
        body: pc.localDescription.sdp
      });
    }).then(function (response) {
      console.log('[obs-webrtc] WHEP response:', response.status);
      if (!response.ok) {
        throw new Error('WHEP POST failed: ' + response.status);
      }
      return response.text();
    }).then(function (answerSdp) {
      console.log('[obs-webrtc] answer SDP bytes:', answerSdp.length);
      attempts = 0;
      return pc.setRemoteDescription({ type: 'answer', sdp: answerSdp });
    }).catch(function (err) {
      setStatus('error: ' + err.message);
      reconnect();
    });
  }

  function reconnect() {
    if (pc) {
      try { pc.close(); } catch (e) {}
      pc = null;
    }
    var delay = Math.min(1000 * Math.pow(2, attempts++), 10000);
    setTimeout(connect, delay);
  }

  window.addEventListener('beforeunload', function () {
    if (pc) { try { pc.close(); } catch (e) {} }
  });

  connect();
})();
