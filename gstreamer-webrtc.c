/*
 * obs-gstreamer. OBS Studio plugin.
 * Copyright (C) 2018-2021 Florian Zwoch <fzwoch@gmail.com>
 *
 * This file is part of obs-gstreamer.
 *
 * obs-gstreamer is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * obs-gstreamer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with obs-gstreamer. If not, see <http://www.gnu.org/licenses/>.
 */

#include "gstreamer-webrtc.h"

#include <gst/app/app.h>
#include <gst/sdp/sdp.h>
#include <gst/video/video.h>
#include <gst/webrtc/webrtc.h>
#include <libsoup/soup.h>
#include <plugin-support.h>

#define MAX_SESSIONS 16
#define ANSWER_TIMEOUT_SECONDS 5

typedef struct {
	const char *label;
	uint64_t count;
} packet_counter_t;

static GstPadProbeReturn rtp_packet_probe(GstPad *pad, GstPadProbeInfo *info,
	gpointer user_data)
{
	packet_counter_t *counter = user_data;
	if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
		return GST_PAD_PROBE_OK;
	counter->count++;
	if (counter->count == 1 || counter->count % 300 == 0)
		blog(LOG_INFO, "[obs-gstreamer] WebRTC RTP %s packet %llu",
			counter->label, (unsigned long long)counter->count);
	return GST_PAD_PROBE_OK;
}

typedef struct {
	char *id;
	GstElement *queue;
	GstElement *webrtcbin;
	GstPad *tee_pad;
	GstPad *sinkpad;
	packet_counter_t rtp_counter;
	SoupServerMessage *pending_msg;
	guint timeout_id;
	bool replied;
} whep_session_t;

struct gstreamer_webrtc {
	obs_output_t *output;
	obs_data_t *settings;
	struct obs_video_info ovi;

	GstElement *pipe;
	GstElement *appsrc;
	GstElement *tee;
	GstElement *pay0;

	char *web_root;
	SoupServer *server;

	GMutex sessions_lock;
	GHashTable *sessions;
	guint session_counter;
	guint bus_watch_id;
	packet_counter_t pay_counter;
};

// ---------------------------------------------------------------------------
// Embedded default viewer assets (seeded into the user-editable web root)
// ---------------------------------------------------------------------------

static const char *default_index_html =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"  <meta charset=\"utf-8\">\n"
"  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"  <title>OBS Stream</title>\n"
"  <link rel=\"stylesheet\" href=\"style.css\">\n"
"</head>\n"
"<body>\n"
"  <div id=\"player-wrap\">\n"
"    <video id=\"video\" autoplay playsinline muted></video>\n"
"    <pre id=\"status\">connecting&hellip;</pre>\n"
"  </div>\n"
"  <script src=\"app.js\"></script>\n"
"</body>\n"
"</html>\n";

static const char *default_style_css =
"html, body {\n"
"  margin: 0;\n"
"  height: 100%;\n"
"  background: #111;\n"
"  color: #eee;\n"
"  font-family: sans-serif;\n"
"}\n"
"#player-wrap {\n"
"  position: relative;\n"
"  height: 100%;\n"
"  display: flex;\n"
"  align-items: center;\n"
"  justify-content: center;\n"
"}\n"
"#video {\n"
"  max-width: 100%;\n"
"  max-height: 100%;\n"
"}\n"
"#status {\n"
"  position: absolute;\n"
"  top: 10px;\n"
"  left: 10px;\n"
"  margin: 0;\n"
"  white-space: pre-wrap;\n"
"  padding: 4px 10px;\n"
"  border-radius: 6px;\n"
"  background: rgba(0, 0, 0, 0.6);\n"
"  font-size: 13px;\n"
"}\n";

static const char *default_app_js =
"(function () {\n"
"  var video = document.getElementById('video');\n"
"  var status = document.getElementById('status');\n"
"  var pc = null;\n"
"  var attempts = 0;\n"
"\n"
"  function setStatus(text) {\n"
"    status.textContent = text;\n"
"    console.log('[obs-webrtc]', text);\n"
"  }\n"
"\n"
"  function connect() {\n"
"    setStatus('connecting\\u2026');\n"
"    pc = new RTCPeerConnection();\n"
"    window.__obsWebRTCPeerConnection = pc;\n"
"    pc.oniceconnectionstatechange = function () {\n"
"      setStatus('connection: ' + pc.connectionState + '\\nice: ' + pc.iceConnectionState +\n"
"        '\\nsignaling: ' + pc.signalingState + '\\ntrack: ' + (video.srcObject ? 'yes' : 'no') +\n"
"        '\\nvideo: ' + video.videoWidth + 'x' + video.videoHeight);\n"
"    };\n"
"    pc.onsignalingstatechange = function () {\n"
"      console.log('[obs-webrtc] signaling state:', pc.signalingState);\n"
"    };\n"
"\n"
"    pc.addTransceiver('video', { direction: 'recvonly' });\n"
"\n"
"    pc.ontrack = function (event) {\n"
"      console.log('[obs-webrtc] ontrack', event.track.kind, event.track.readyState, event.streams);\n"
"      video.srcObject = event.streams[0] || new MediaStream([event.track]);\n"
"      video.play().catch(function (err) {\n"
"        setStatus('playback error: ' + err.message);\n"
"      });\n"
"      setStatus('video track received');\n"
"    };\n"
"    video.onloadedmetadata = function () {\n"
"      setStatus('metadata: ' + video.videoWidth + 'x' + video.videoHeight);\n"
"    };\n"
"    video.onplaying = function () { setStatus('playing: ' + video.videoWidth + 'x' + video.videoHeight); };\n"
"    video.onerror = function () { console.error('[obs-webrtc] video error', video.error); };\n"
"\n"
"    pc.onconnectionstatechange = function () {\n"
"      setStatus('state: ' + pc.connectionState);\n"
"      if (pc.connectionState === 'failed' ||\n"
"          pc.connectionState === 'disconnected' ||\n"
"          pc.connectionState === 'closed') {\n"
"        reconnect();\n"
"      }\n"
"    };\n"
"\n"
"    pc.createOffer().then(function (offer) {\n"
"      return pc.setLocalDescription(offer);\n"
"    }).then(function () {\n"
"      return fetch('/whep', {\n"
"        method: 'POST',\n"
"        headers: { 'Content-Type': 'application/sdp' },\n"
"        body: pc.localDescription.sdp\n"
"      });\n"
"    }).then(function (response) {\n"
"      console.log('[obs-webrtc] WHEP response:', response.status);\n"
"      if (!response.ok) {\n"
"        throw new Error('WHEP POST failed: ' + response.status);\n"
"      }\n"
"      return response.text();\n"
"    }).then(function (answerSdp) {\n"
"      console.log('[obs-webrtc] answer SDP bytes:', answerSdp.length);\n"
"      attempts = 0;\n"
"      return pc.setRemoteDescription({ type: 'answer', sdp: answerSdp });\n"
"    }).catch(function (err) {\n"
"      setStatus('error: ' + err.message);\n"
"      reconnect();\n"
"    });\n"
"  }\n"
"\n"
"  function reconnect() {\n"
"    if (pc) {\n"
"      try { pc.close(); } catch (e) {}\n"
"      pc = null;\n"
"    }\n"
"    var delay = Math.min(1000 * Math.pow(2, attempts++), 10000);\n"
"    setTimeout(connect, delay);\n"
"  }\n"
"\n"
"  window.addEventListener('beforeunload', function () {\n"
"    if (pc) { try { pc.close(); } catch (e) {} }\n"
"  });\n"
"\n"
"  connect();\n"
"})();\n";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void session_free(gpointer data)
{
	whep_session_t *session = data;

	if (session->timeout_id)
		g_source_remove(session->timeout_id);
	if (session->sinkpad) {
		gst_object_unref(session->sinkpad);
		session->sinkpad = NULL;
	}
	if (session->webrtcbin && session->pending_msg) {
		soup_server_message_set_status(session->pending_msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		soup_server_message_unpause(session->pending_msg);
		session->pending_msg = NULL;
	}
	g_free(session->id);
	g_free(session);
}

typedef struct {
	struct gstreamer_webrtc *webrtc;
	char *id;
} idle_teardown_t;

static void remove_session_locked(struct gstreamer_webrtc *webrtc, whep_session_t *session);

static gboolean idle_teardown(gpointer user_data)
{
	idle_teardown_t *ctx = user_data;
	g_mutex_lock(&ctx->webrtc->sessions_lock);
	whep_session_t *session = g_hash_table_lookup(ctx->webrtc->sessions, ctx->id);
	if (session)
		remove_session_locked(ctx->webrtc, session);
	g_mutex_unlock(&ctx->webrtc->sessions_lock);

	g_free(ctx->id);
	g_free(ctx);
	return G_SOURCE_REMOVE;
}

static void session_teardown_async(struct gstreamer_webrtc *webrtc, const char *id)
{
	idle_teardown_t *ctx = g_new0(idle_teardown_t, 1);
	ctx->webrtc = webrtc;
	ctx->id = g_strdup(id);
	g_idle_add(idle_teardown, ctx);
}

static void webrtcbin_connection_state_changed(GstElement *webrtcbin, GParamSpec *pspec, gpointer user_data)
{
	struct gstreamer_webrtc *webrtc = user_data;
	GstWebRTCPeerConnectionState state;

	g_object_get(webrtcbin, "connection-state", &state, NULL);
	blog(LOG_DEBUG, "[obs-gstreamer] WHEP session %s: connection-state %d",
		GST_ELEMENT_NAME(webrtcbin), (int)state);
	if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED ||
	    state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED ||
	    state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED) {
		session_teardown_async(webrtc, GST_ELEMENT_NAME(webrtcbin));
	}
}

static void remove_session_locked(struct gstreamer_webrtc *webrtc, whep_session_t *session)
{
	// Detach from pipeline while holding lock so concurrent lookups are safe.
	if (session->webrtcbin) {
		g_signal_handlers_disconnect_by_data(session->webrtcbin, webrtc);
	}
	if (session->queue)
		gst_element_set_state(session->queue, GST_STATE_NULL);
	if (session->webrtcbin)
		gst_element_set_state(session->webrtcbin, GST_STATE_NULL);
	if (session->sinkpad) {
		gst_element_release_request_pad(session->webrtcbin, session->sinkpad);
		gst_object_unref(session->sinkpad);
		session->sinkpad = NULL;
	}
	if (session->tee_pad) {
		gst_element_release_request_pad(webrtc->tee, session->tee_pad);
		gst_object_unref(session->tee_pad);
		session->tee_pad = NULL;
	}

	GstPad *queue_src = session->queue ? gst_element_get_static_pad(session->queue, "src") : NULL;
	GstPad *peer = queue_src ? gst_pad_get_peer(queue_src) : NULL;
	if (peer) {
		gst_pad_unlink(queue_src, peer);
		gst_object_unref(peer);
	}
	if (queue_src)
		gst_object_unref(queue_src);

	if (session->queue)
		gst_bin_remove(GST_BIN(webrtc->pipe), session->queue);
	if (session->webrtcbin)
		gst_bin_remove(GST_BIN(webrtc->pipe), session->webrtcbin);

	blog(LOG_INFO, "[obs-gstreamer] WebRTC session %s removed", session->id);
	g_hash_table_steal(webrtc->sessions, session->id);
	session_free(session);
}

typedef struct {
	struct gstreamer_webrtc *webrtc;
	SoupServerMessage *msg;
	char *id;
	char *sdp;
	int status;
} reply_ctx_t;

static gboolean idle_reply(gpointer user_data)
{
	reply_ctx_t *ctx = user_data;

	g_mutex_lock(&ctx->webrtc->sessions_lock);
	whep_session_t *session = g_hash_table_lookup(ctx->webrtc->sessions, ctx->id);
	if (session) {
		session->replied = true;
		session->pending_msg = NULL;
	}
	g_mutex_unlock(&ctx->webrtc->sessions_lock);

	char *location = g_strdup_printf("/whep/%s", ctx->id);
	SoupMessageHeaders *headers = soup_server_message_get_response_headers(ctx->msg);
	soup_message_headers_append(headers, "Location", location);
	soup_server_message_set_status(ctx->msg, ctx->status, NULL);
	soup_server_message_set_response(ctx->msg, "application/sdp", SOUP_MEMORY_COPY,
		ctx->sdp ? ctx->sdp : "", ctx->sdp ? strlen(ctx->sdp) : 0);
	soup_server_message_unpause(ctx->msg);
	g_object_unref(ctx->msg);
	if (ctx->status != SOUP_STATUS_CREATED)
		session_teardown_async(ctx->webrtc, ctx->id);
	g_free(location);

	g_free(ctx->id);
	g_free(ctx->sdp);
	g_free(ctx);
	return G_SOURCE_REMOVE;
}

typedef struct {
	struct gstreamer_webrtc *webrtc;
	char *id;
} timeout_ctx_t;

static gboolean answer_timeout(gpointer user_data)
{
	timeout_ctx_t *ctx = user_data;

	g_mutex_lock(&ctx->webrtc->sessions_lock);
	whep_session_t *session = g_hash_table_lookup(ctx->webrtc->sessions, ctx->id);
	if (session && !session->replied && session->pending_msg) {
		blog(LOG_WARNING, "[obs-gstreamer] WHEP session %s: ICE gathering timed out -> 500",
			ctx->id);
		soup_server_message_set_status(session->pending_msg,
			SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		soup_server_message_unpause(session->pending_msg);
		session->pending_msg = NULL;
	}
	if (session)
		session->timeout_id = 0;
	g_mutex_unlock(&ctx->webrtc->sessions_lock);

	g_free(ctx->id);
	g_free(ctx);
	return G_SOURCE_REMOVE;
}

static void on_answer_created(GstPromise *promise, gpointer user_data);
static void on_local_description_set(GstPromise *promise, gpointer user_data);

struct answer_ctx {
	struct gstreamer_webrtc *webrtc;
	whep_session_t *session;
};

static void on_gathering_complete(GstElement *webrtcbin, GParamSpec *pspec, gpointer user_data)
{
	struct answer_ctx *ctx = user_data;
	GstWebRTCICEGatheringState state;

	g_object_get(webrtcbin, "ice-gathering-state", &state, NULL);
	if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE)
		return;

	blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: ICE gathering complete, sending 201 answer",
		ctx->session->id);
	g_signal_handlers_disconnect_by_func(webrtcbin, on_gathering_complete, user_data);

	GstWebRTCSessionDescription *local = NULL;
	g_object_get(webrtcbin, "local-description", &local, NULL);
	gchar *sdp_text = gst_sdp_message_as_text(local->sdp);
	gst_webrtc_session_description_free(local);

	reply_ctx_t *reply = g_new0(reply_ctx_t, 1);
	reply->webrtc = ctx->webrtc;
	reply->msg = g_object_ref(ctx->session->pending_msg);
	reply->id = g_strdup(ctx->session->id);
	reply->sdp = sdp_text;
	reply->status = SOUP_STATUS_CREATED;
	g_idle_add(idle_reply, reply);
}

static void on_local_description_set(GstPromise *promise, gpointer user_data)
{
	struct answer_ctx *ctx = user_data;
	gst_promise_unref(promise);

	blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: local description set, waiting for ICE",
		ctx->session->id);
	g_signal_connect(ctx->session->webrtcbin, "notify::ice-gathering-state",
		G_CALLBACK(on_gathering_complete), ctx);
}

static void on_remote_description_set(GstPromise *promise, gpointer user_data)
{
	struct answer_ctx *ctx = user_data;
	const GstStructure *remote_reply = gst_promise_get_reply(promise);
	if (remote_reply) {
		gchar *reply_text = gst_structure_to_string(remote_reply);
		blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: set-remote-description reply: %s",
			ctx->session->id, reply_text);
		g_free(reply_text);
	}
	gst_promise_unref(promise);

	whep_session_t *session = ctx->session;
	gst_element_sync_state_with_parent(session->queue);
	gst_element_sync_state_with_parent(session->webrtcbin);
	blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: branch synced to PLAYING",
		session->id);
	GArray *transceivers = NULL;
	g_signal_emit_by_name(session->webrtcbin, "get-transceivers", &transceivers);
	blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: transceivers after offer: %u",
		session->id, transceivers ? transceivers->len : 0);
	if (transceivers)
		g_array_unref(transceivers);
	GstWebRTCSignalingState signaling_state;
	g_object_get(session->webrtcbin, "signaling-state", &signaling_state, NULL);
	blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: signaling state before answer: %d",
		session->id, (int)signaling_state);

	GstPromise *p = gst_promise_new_with_change_func(on_answer_created, ctx, NULL);
	g_signal_emit_by_name(session->webrtcbin, "create-answer", NULL, p);
}

static void on_answer_created(GstPromise *promise, gpointer user_data)
{
	struct answer_ctx *ctx = user_data;

	const GstStructure *reply = gst_promise_get_reply(promise);
	GstWebRTCSessionDescription *answer = NULL;
	if (reply) {
		gchar *reply_text = gst_structure_to_string(reply);
		blog(LOG_ERROR, "[obs-gstreamer] WHEP session %s: create-answer reply: %s",
			ctx->session->id, reply_text);
		g_free(reply_text);
		gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
	}
	gst_promise_unref(promise);

	if (!answer) {
		blog(LOG_ERROR, "[obs-gstreamer] WHEP session %s: create-answer failed",
			ctx->session->id);
		reply_ctx_t *reply_ctx = g_new0(reply_ctx_t, 1);
		reply_ctx->webrtc = ctx->webrtc;
		reply_ctx->msg = g_object_ref(ctx->session->pending_msg);
		reply_ctx->id = g_strdup(ctx->session->id);
		reply_ctx->sdp = NULL;
		reply_ctx->status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
		g_idle_add(idle_reply, reply_ctx);
		return;
	}

	GstPromise *p = gst_promise_new_with_change_func(on_local_description_set, ctx, NULL);
	g_signal_emit_by_name(ctx->session->webrtcbin, "set-local-description", answer, p);
	gst_webrtc_session_description_free(answer);
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

static void whep_post(struct gstreamer_webrtc *webrtc, SoupServerMessage *msg)
{
	blog(LOG_INFO, "[obs-gstreamer] WHEP POST /whep: started");
	SoupMessageBody *body = soup_server_message_get_request_body(msg);
	gsize size = body ? body->length : 0;

	if (size == 0) {
		blog(LOG_WARNING, "[obs-gstreamer] WHEP POST: empty body -> 400");
		soup_server_message_set_status(msg, SOUP_STATUS_BAD_REQUEST, NULL);
		return;
	}

	g_mutex_lock(&webrtc->sessions_lock);
	guint count = g_hash_table_size(webrtc->sessions);
	g_mutex_unlock(&webrtc->sessions_lock);
	if (count >= MAX_SESSIONS) {
		blog(LOG_WARNING, "[obs-gstreamer] WHEP POST: session limit reached (%d) -> 503", MAX_SESSIONS);
		soup_server_message_set_status(msg, SOUP_STATUS_SERVICE_UNAVAILABLE, NULL);
		return;
	}

	whep_session_t *session = g_new0(whep_session_t, 1);
	session->id = g_strdup_printf("viewer%u", ++webrtc->session_counter);

	session->queue = gst_element_factory_make("queue", NULL);
	session->webrtcbin = gst_element_factory_make("webrtcbin", session->id);
	if (!session->queue || !session->webrtcbin) {
		blog(LOG_ERROR, "[obs-gstreamer] WHEP POST: failed to create queue/webrtcbin -> 500");
		g_free(session->id);
		g_free(session);
		soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		return;
	}

	g_object_set(session->queue, "leaky", 2 /* downstream */, "max-size-buffers", 30,
		"max-size-time", (guint64)0, "max-size-bytes", (guint)0, NULL);
	g_object_set(session->webrtcbin, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);

	const char *stun = obs_data_get_string(webrtc->settings, "webrtc_stun_server");
	if (stun && stun[0])
		g_object_set(session->webrtcbin, "stun-server", stun, NULL);

	// Build the RTP branch before applying the browser offer. Keep the
	// payloader's negotiated RTP caps intact all the way into webrtcbin.
	gst_bin_add_many(GST_BIN(webrtc->pipe), session->queue,
		session->webrtcbin, NULL);

	session->sinkpad = gst_element_request_pad_simple(session->webrtcbin, "sink_%u");
	GstWebRTCRTPTransceiver *transceiver = NULL;
	g_object_get(session->sinkpad, "transceiver", &transceiver, NULL);
	if (transceiver) {
		GstCaps *codec_caps = gst_caps_from_string(
			"application/x-rtp,media=video,encoding-name=H264,clock-rate=90000");
		g_object_set(transceiver, "direction",
			GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
			"codec-preferences", codec_caps, NULL);
		gst_caps_unref(codec_caps);
		gst_object_unref(transceiver);
	}
	GstPad *queue_src = gst_element_get_static_pad(session->queue, "src");
	if (gst_pad_link(queue_src, session->sinkpad) != GST_PAD_LINK_OK) {
		gst_object_unref(queue_src);
		blog(LOG_ERROR, "[obs-gstreamer] WHEP POST: failed to link viewer RTP branch -> 500");
		g_free(session->id);
		g_free(session);
		soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		return;
	}
	gst_object_unref(queue_src);
	session->tee_pad = gst_element_request_pad_simple(webrtc->tee, "src_%u");
	GstPad *queue_sink = gst_element_get_static_pad(session->queue, "sink");
	if (gst_pad_link(session->tee_pad, queue_sink) != GST_PAD_LINK_OK) {
		gst_object_unref(queue_sink);
		blog(LOG_ERROR, "[obs-gstreamer] WHEP POST: failed to link tee -> viewer queue -> 500");
		g_free(session->id);
		g_free(session);
		soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		return;
	}
	gst_object_unref(queue_sink);
	session->rtp_counter.label = session->id;
	GstPad *branch_src = gst_element_get_static_pad(session->queue, "src");
	gst_pad_add_probe(branch_src, GST_PAD_PROBE_TYPE_BUFFER, rtp_packet_probe,
		&session->rtp_counter, NULL);
	gst_object_unref(branch_src);
	blog(LOG_INFO, "[obs-gstreamer] WHEP POST %s: RTP branch linked before offer", session->id);
	gst_element_sync_state_with_parent(session->queue);
	gst_element_sync_state_with_parent(session->webrtcbin);
	blog(LOG_INFO, "[obs-gstreamer] WHEP POST %s: branch synced before remote offer", session->id);

	g_signal_connect(session->webrtcbin, "notify::connection-state",
		G_CALLBACK(webrtcbin_connection_state_changed), webrtc);

	g_mutex_lock(&webrtc->sessions_lock);
	g_hash_table_insert(webrtc->sessions, g_strdup(session->id), session);
	g_mutex_unlock(&webrtc->sessions_lock);

	blog(LOG_INFO, "[obs-gstreamer] WHEP POST %s: branch added (unlinked), negotiating", session->id);

	// Ask the encoder for a keyframe so late joiners get picture quickly.
	GstEvent *event = gst_video_event_new_upstream_force_key_unit(
		GST_CLOCK_TIME_NONE, TRUE, 0);
	gst_pad_push_event(gst_element_get_static_pad(webrtc->pay0, "src"), event);

	// Negotiate: offer -> answer. Canonical WHEP answerer flow:
	// set-remote-description -> request+link sink pad (elements in NULL)
	// -> create-answer -> set-local-description -> wait ICE complete.
	gchar *offer_text = g_strndup(body->data, size);
	GstSDPMessage *offer_sdp = NULL;
	gst_sdp_message_new_from_text(offer_text, &offer_sdp);
	g_free(offer_text);
	blog(LOG_INFO, "[obs-gstreamer] WHEP POST: offer parsed (%u bytes), starting negotiation", (guint)size);

	GstWebRTCSessionDescription *offer =
		gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, offer_sdp);

	struct answer_ctx *ctx = g_new0(struct answer_ctx, 1);
	ctx->webrtc = webrtc;
	ctx->session = session;

	session->pending_msg = msg;
	timeout_ctx_t *tctx = g_new0(timeout_ctx_t, 1);
	tctx->webrtc = webrtc;
	tctx->id = g_strdup(session->id);
	session->timeout_id = g_timeout_add_seconds(ANSWER_TIMEOUT_SECONDS, answer_timeout, tctx);
	soup_server_message_pause(msg);

	GstPromise *promise = gst_promise_new_with_change_func(on_remote_description_set, ctx, NULL);
	g_signal_emit_by_name(session->webrtcbin, "set-remote-description", offer, promise);
	gst_webrtc_session_description_free(offer);

	blog(LOG_INFO, "[obs-gstreamer] WebRTC session %s negotiating (%u active)",
		session->id, count + 1);
}

static void whep_delete(struct gstreamer_webrtc *webrtc, SoupServerMessage *msg, const char *path)
{
	blog(LOG_INFO, "[obs-gstreamer] WHEP DELETE %s: started", path);
	const char *id = path + strlen("/whep/");
	while (*id == '/')
		id++;

	g_mutex_lock(&webrtc->sessions_lock);
	whep_session_t *session = g_hash_table_lookup(webrtc->sessions, id);
	g_mutex_unlock(&webrtc->sessions_lock);

	if (!session) {
		soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
		return;
	}

	session_teardown_async(webrtc, id);
	soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
}

static void whep_handler(SoupServer *server, SoupServerMessage *msg, const char *path,
	GHashTable *query, gpointer user_data)
{
	struct gstreamer_webrtc *webrtc = user_data;

	if (g_strcmp0(soup_server_message_get_method(msg), "POST") == 0) {
		whep_post(webrtc, msg);
	} else if (g_strcmp0(soup_server_message_get_method(msg), "DELETE") == 0) {
		whep_delete(webrtc, msg, path);
	} else {
		soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
	}
}

static void static_file_handler(SoupServer *server, SoupServerMessage *msg, const char *path,
	GHashTable *query, gpointer user_data)
{
	struct gstreamer_webrtc *webrtc = user_data;
	blog(LOG_DEBUG, "[obs-gstreamer] HTTP %s %s", soup_server_message_get_method(msg), path);

	const char *method = soup_server_message_get_method(msg);
	if (g_strcmp0(method, "GET") != 0 && g_strcmp0(method, "HEAD") != 0) {
		soup_server_message_set_status(msg, SOUP_STATUS_METHOD_NOT_ALLOWED, NULL);
		return;
	}

	const char *rel = path;
	while (*rel == '/')
		rel++;
	if (!rel[0])
		rel = "index.html";

	char *file = g_build_filename(webrtc->web_root, rel, NULL);
	char *canonical = g_canonicalize_filename(file, webrtc->web_root);
	g_free(file);

	gboolean ok = FALSE;
	char *contents = NULL;
	gsize length = 0;
	GError *err = NULL;

	if (g_str_has_prefix(canonical, webrtc->web_root) &&
	    g_file_test(canonical, G_FILE_TEST_IS_REGULAR)) {
		ok = g_file_get_contents(canonical, &contents, &length, &err);
	}

	if (!ok) {
		soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
		if (err)
			g_error_free(err);
		g_free(canonical);
		return;
	}

	char *basename = g_path_get_basename(canonical);
	char *content_type = g_content_type_guess(basename, NULL, 0, NULL);
	char *mime = content_type ? g_content_type_get_mime_type(content_type) : NULL;
	soup_server_message_set_response(msg, mime ? mime : "application/octet-stream",
		SOUP_MEMORY_TAKE, contents, length);
	soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);

	g_free(mime);
	g_free(content_type);
	g_free(basename);
	g_free(canonical);
}

// ---------------------------------------------------------------------------
// Web root seeding
// ---------------------------------------------------------------------------

static void seed_web_root(const char *root)
{
	if (g_mkdir_with_parents(root, 0755) != 0) {
		blog(LOG_WARNING, "[obs-gstreamer] failed to create web root %s", root);
		return;
	}

	const char *names[] = { "index.html", "style.css", "app.js" };
	const char *bodies[] = { default_index_html, default_style_css, default_app_js };

	for (int i = 0; i < (int)G_N_ELEMENTS(names); i++) {
		char *file = g_build_filename(root, names[i], NULL);
		if (!g_file_test(file, G_FILE_TEST_EXISTS)) {
			GError *e = NULL;
			g_file_set_contents(file, bodies[i], -1, &e);
			if (e) {
				blog(LOG_WARNING, "[obs-gstreamer] failed to seed %s: %s",
					file, e->message);
				g_error_free(e);
			}
		}
		g_free(file);
	}
}

// ---------------------------------------------------------------------------
// Bus callback
// ---------------------------------------------------------------------------

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer user_data)
{
	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ERROR: {
		GError *err = NULL;
		gst_message_parse_error(message, &err, NULL);
		blog(LOG_ERROR, "[obs-gstreamer] WebRTC pipeline error: %s", err->message);
		g_error_free(err);
		break;
	}
	case GST_MESSAGE_WARNING: {
		GError *err = NULL;
		gst_message_parse_warning(message, &err, NULL);
		blog(LOG_WARNING, "[obs-gstreamer] WebRTC pipeline warning: %s", err->message);
		g_error_free(err);
		break;
	}
	default:
		break;
	}
	return TRUE;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

struct gstreamer_webrtc *gstreamer_webrtc_create(obs_output_t *output, obs_data_t *settings,
						 struct obs_video_info *ovi, char **error)
{
	const char *gst_format = NULL;
	switch (ovi->output_format) {
	case VIDEO_FORMAT_I420: gst_format = "I420"; break;
	case VIDEO_FORMAT_NV12: gst_format = "NV12"; break;
	case VIDEO_FORMAT_YVYU: gst_format = "YVYU"; break;
	case VIDEO_FORMAT_YUY2: gst_format = "YUY2"; break;
	case VIDEO_FORMAT_UYVY: gst_format = "UYVY"; break;
	case VIDEO_FORMAT_I422: gst_format = "Y42B"; break;
	case VIDEO_FORMAT_RGBA: gst_format = "RGBA"; break;
	case VIDEO_FORMAT_BGRA: gst_format = "BGRA"; break;
	case VIDEO_FORMAT_BGRX: gst_format = "BGRX"; break;
	case VIDEO_FORMAT_I444: gst_format = "Y444"; break;
	default: break;
	}

	if (!gst_format) {
		if (error)
			*error = g_strdup("unhandled output pixel format");
		return NULL;
	}

	struct gstreamer_webrtc *webrtc = g_new0(struct gstreamer_webrtc, 1);
	webrtc->output = output;
	webrtc->settings = settings;
	webrtc->ovi = *ovi;
	webrtc->sessions = g_hash_table_new(g_str_hash, g_str_equal);
	g_mutex_init(&webrtc->sessions_lock);

	const char *port = obs_data_get_string(settings, "webrtc_http_port");
	if (!port || !port[0])
		port = "8888";

	const char *root = obs_data_get_string(settings, "webrtc_web_root");
	if (!root || !root[0]) {
		webrtc->web_root = g_build_filename(g_get_user_data_dir(), "obs-gstreamer",
			"webrtc", NULL);
	} else {
		webrtc->web_root = g_strdup(root);
	}
	seed_web_root(webrtc->web_root);
	blog(LOG_INFO, "[obs-gstreamer] WebRTC web root: %s", webrtc->web_root);

	int fps = ovi->fps_den > 0 ? ovi->fps_num / ovi->fps_den : 30;
	if (fps <= 0)
		fps = 30;

	char *launch = g_strdup_printf(
		"appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true block=true "
		"! queue ! video/x-raw, format=%s, width=%d, height=%d, framerate=%d/%d "
		"! videoconvert ! x264enc tune=zerolatency speed-preset=veryfast bitrate=3000 "
		"bframes=0 cabac=false key-int-max=%d "
		"! video/x-h264, profile=constrained-baseline, stream-format=byte-stream, alignment=au "
		"! h264parse config-interval=-1 "
		"! rtph264pay name=pay0 pt=102 config-interval=-1 "
		"! tee name=t "
		// keep-alive branch: guarantees flow OK even with zero viewers
		"t. ! queue leaky=downstream max-size-buffers=30 ! fakesink sync=false",
		gst_format, ovi->output_width, ovi->output_height, ovi->fps_num, ovi->fps_den, fps);

	GError *err = NULL;
	webrtc->pipe = gst_parse_launch(launch, &err);
	g_free(launch);

	if (err || !webrtc->pipe) {
		if (error)
			*error = g_strdup(err ? err->message : "failed to create WebRTC pipeline");
		if (err)
			g_error_free(err);
		gstreamer_webrtc_destroy(webrtc);
		return NULL;
	}

	webrtc->appsrc = gst_bin_get_by_name(GST_BIN(webrtc->pipe), "appsrc_video");
	webrtc->tee = gst_bin_get_by_name(GST_BIN(webrtc->pipe), "t");
	webrtc->pay0 = gst_bin_get_by_name(GST_BIN(webrtc->pipe), "pay0");
	webrtc->pay_counter.label = "payloader";
	GstPad *pay_src = gst_element_get_static_pad(webrtc->pay0, "src");
	gst_pad_add_probe(pay_src, GST_PAD_PROBE_TYPE_BUFFER, rtp_packet_probe,
		&webrtc->pay_counter, NULL);
	gst_object_unref(pay_src);

	GstBus *bus = gst_element_get_bus(webrtc->pipe);
	webrtc->bus_watch_id = gst_bus_add_watch(bus, bus_callback, webrtc);
	gst_object_unref(bus);

	webrtc->server = soup_server_new(NULL, NULL);
	GError *listen_err = NULL;
	int port_num = port ? atoi(port) : 8888;
	if (port_num <= 0 || port_num > 65535)
		port_num = 8888;
	if (!soup_server_listen_local(webrtc->server, port_num, 0, &listen_err)) {
		if (error)
			*error = g_strdup_printf("failed to listen on HTTP port %s: %s", port,
				listen_err ? listen_err->message : "unknown error");
		if (listen_err)
			g_error_free(listen_err);
		gstreamer_webrtc_destroy(webrtc);
		return NULL;
	}

	soup_server_add_handler(webrtc->server, "/whep", whep_handler, webrtc, NULL);
	soup_server_add_handler(webrtc->server, NULL, static_file_handler, webrtc, NULL);

	if (gst_element_set_state(webrtc->pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		if (error)
			*error = g_strdup("failed to start WebRTC pipeline");
		gstreamer_webrtc_destroy(webrtc);
		return NULL;
	}

	blog(LOG_INFO, "[obs-gstreamer] WebRTC output started, HTTP port %s, viewers: http://<host>:%s/",
		port, port);
	return webrtc;
}

void gstreamer_webrtc_destroy(struct gstreamer_webrtc *webrtc)
{
	if (!webrtc)
		return;

	if (webrtc->bus_watch_id) {
		g_source_remove(webrtc->bus_watch_id);
		webrtc->bus_watch_id = 0;
	}

	if (webrtc->server) {
		soup_server_disconnect(webrtc->server);
		g_clear_object(&webrtc->server);
	}

	if (webrtc->pipe) {
		g_mutex_lock(&webrtc->sessions_lock);
		while (g_hash_table_size(webrtc->sessions) > 0) {
			GHashTableIter iter;
			gpointer key, value;
			g_hash_table_iter_init(&iter, webrtc->sessions);
			if (!g_hash_table_iter_next(&iter, &key, &value))
				break;
			remove_session_locked(webrtc, value);
		}
		g_mutex_unlock(&webrtc->sessions_lock);

		gst_element_set_state(webrtc->pipe, GST_STATE_NULL);
		g_clear_object(&webrtc->pipe);
	}

	g_clear_object(&webrtc->appsrc);
	g_clear_object(&webrtc->tee);
	g_clear_object(&webrtc->pay0);

	if (webrtc->sessions)
		g_hash_table_destroy(webrtc->sessions);
	g_mutex_clear(&webrtc->sessions_lock);

	g_free(webrtc->web_root);
	g_free(webrtc);
}

GstElement *gstreamer_webrtc_appsrc(struct gstreamer_webrtc *webrtc)
{
	return webrtc ? webrtc->appsrc : NULL;
}
