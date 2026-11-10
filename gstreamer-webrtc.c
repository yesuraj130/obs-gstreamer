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
#include "gstreamer-helper.h"
#include <plugin-support.h>

#ifndef ENABLE_WEBRTC
#define ENABLE_WEBRTC 1
#endif

#if ENABLE_WEBRTC

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif

#include <gst/app/app.h>
#include <gst/sdp/sdp.h>
#include <gst/video/video.h>
#include <gst/webrtc/webrtc.h>
#include <libsoup/soup.h>

#define MAX_SESSIONS 16
#define ANSWER_TIMEOUT_SECONDS 10

struct packet_counter {
	const char *label;
	uint64_t count;
};

static GstPadProbeReturn rtp_packet_probe(GstPad *pad, GstPadProbeInfo *info,
	gpointer user_data)
{
	struct packet_counter *counter = user_data;
	if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
		return GST_PAD_PROBE_OK;
	counter->count++;
	if (counter->count == 1 || counter->count % 300 == 0)
		blog(LOG_INFO, "[obs-gstreamer] WebRTC RTP %s packet %llu",
			counter->label, (unsigned long long)counter->count);
	return GST_PAD_PROBE_OK;
}

struct gstreamer_webrtc;
struct answer_ctx;

struct whep_session {
	char *id;
	struct gstreamer_webrtc *webrtc;
	GstElement *queue;
	GstElement *webrtcbin;
	GstPad *tee_pad;
	GstPad *sinkpad;
	struct packet_counter rtp_counter;
	gulong rtp_probe_id;
	struct answer_ctx *answer_ctx;
	SoupServerMessage *pending_msg;
	guint timeout_id;
	bool replied;
};

struct answer_ctx {
	gatomicrefcount ref_count;
	struct gstreamer_webrtc *webrtc;
	char *session_id;
};

struct gstreamer_webrtc {
	obs_output_t *output;
	obs_data_t *settings;
	struct obs_video_info ovi;

	char *name;
	char *stun_server;
	char *turn_server;
	gst_master_pipeline_t *master_pipe;
	gst_pipeline_subscriber_t *sub;
	bool active;
	bool is_h265;

	GstElement *pipe;
	GstElement *appsrc;
	GstElement *tee;
	GstElement *pay0;

	char *web_root;
	SoupServer *server;
	GMainContext *loop_context;
	GMainLoop *loop;
	GThread *loop_thread;

	GMutex sessions_lock;
	GHashTable *sessions;
	guint session_counter;
	guint bus_watch_id;
	struct packet_counter pay_counter;
};

static void webrtc_frame_cb(GstBuffer *buf, bool is_keyframe, gpointer user_data)
{
	struct gstreamer_webrtc *webrtc = (struct gstreamer_webrtc *)user_data;
	if (!webrtc || !buf)
		return;

	g_mutex_lock(&webrtc->sessions_lock);
	if (!webrtc->active || !webrtc->appsrc || g_hash_table_size(webrtc->sessions) == 0) {
		g_mutex_unlock(&webrtc->sessions_lock);
		return;
	}

	GstBuffer *out_buf = gst_buffer_copy(buf);
	GST_BUFFER_PTS(out_buf) = GST_CLOCK_TIME_NONE;
	GST_BUFFER_DTS(out_buf) = GST_CLOCK_TIME_NONE;
	GstElement *appsrc = gst_object_ref(webrtc->appsrc);
	g_mutex_unlock(&webrtc->sessions_lock);

	GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), out_buf);
	if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING) {
		// Non-fatal flow status
	}
	gst_object_unref(appsrc);
}

#ifndef SOUP_CHECK_VERSION
#define SOUP_CHECK_VERSION(major, minor, micro) \
    (SOUP_MAJOR_VERSION > (major) || \
     (SOUP_MAJOR_VERSION == (major) && SOUP_MINOR_VERSION > (minor)) || \
     (SOUP_MAJOR_VERSION == (major) && SOUP_MINOR_VERSION == (minor) && \
      SOUP_MICRO_VERSION >= (micro)))
#endif

static inline void compat_soup_server_message_pause(SoupServer *server, SoupServerMessage *msg)
{
#if SOUP_CHECK_VERSION(3, 2, 0)
	(void)server;
	soup_server_message_pause(msg);
#else
	soup_server_pause_message(server, msg);
#endif
}

static inline void compat_soup_server_message_unpause(SoupServer *server, SoupServerMessage *msg)
{
#if SOUP_CHECK_VERSION(3, 2, 0)
	(void)server;
	soup_server_message_unpause(msg);
#else
	soup_server_unpause_message(server, msg);
#endif
}

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
"    var tc = pc.addTransceiver('video', { direction: 'recvonly' });\n"
"    if (typeof RTCRtpReceiver !== 'undefined' && RTCRtpReceiver.getCapabilities) {\n"
"      var caps = RTCRtpReceiver.getCapabilities('video');\n"
"      if (caps && caps.codecs) {\n"
"        var h265Codecs = caps.codecs.filter(function (c) {\n"
"          return c.mimeType && (c.mimeType.toLowerCase() === 'video/h265' || c.mimeType.toLowerCase() === 'video/hevc');\n"
"        });\n"
"        if (h265Codecs.length > 0) {\n"
"          var otherCodecs = caps.codecs.filter(function (c) {\n"
"            return !c.mimeType || (c.mimeType.toLowerCase() !== 'video/h265' && c.mimeType.toLowerCase() !== 'video/hevc');\n"
"          });\n"
"          try {\n"
"            tc.setCodecPreferences(h265Codecs.concat(otherCodecs));\n"
"            console.log('[obs-webrtc] Preferred H265/HEVC in browser WebRTC capabilities');\n"
"          } catch (e) {}\n"
"        }\n"
"      }\n"
"    }\n"
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
"      var validSdp = (answerSdp || '').trim().replace(/\\r?\\n/g, '\\r\\n') + '\\r\\n';\n"
"      console.log('[obs-webrtc] answer SDP bytes:', validSdp.length);\n"
"      attempts = 0;\n"
"      return pc.setRemoteDescription({ type: 'answer', sdp: validSdp });\n"
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

static gpointer webrtc_loop_thread_func(gpointer user_data)
{
	struct gstreamer_webrtc *webrtc = user_data;
	g_main_context_push_thread_default(webrtc->loop_context);
	g_main_loop_run(webrtc->loop);
	g_main_context_pop_thread_default(webrtc->loop_context);
	return NULL;
}

static guint webrtc_idle_add(struct gstreamer_webrtc *webrtc, GSourceFunc function, gpointer data)
{
	GSource *source = g_idle_source_new();
	g_source_set_callback(source, function, data, NULL);
	guint id = g_source_attach(source, (webrtc && webrtc->loop_context) ? webrtc->loop_context : NULL);
	g_source_unref(source);
	return id;
}

static guint webrtc_timeout_add_seconds(struct gstreamer_webrtc *webrtc, guint interval, GSourceFunc function, gpointer data)
{
	GSource *source = g_timeout_source_new_seconds(interval);
	g_source_set_callback(source, function, data, NULL);
	guint id = g_source_attach(source, (webrtc && webrtc->loop_context) ? webrtc->loop_context : NULL);
	g_source_unref(source);
	return id;
}

static void webrtc_source_remove(struct gstreamer_webrtc *webrtc, guint tag)
{
	if (!tag)
		return;
	if (webrtc && webrtc->loop_context) {
		GSource *source = g_main_context_find_source_by_id(webrtc->loop_context, tag);
		if (source)
			g_source_destroy(source);
	} else {
		g_source_remove(tag);
	}
}

static struct answer_ctx *answer_ctx_ref(struct answer_ctx *ctx)
{
	g_atomic_ref_count_inc(&ctx->ref_count);
	return ctx;
}

static void answer_ctx_unref(gpointer data)
{
	struct answer_ctx *ctx = data;
	if (ctx && g_atomic_ref_count_dec(&ctx->ref_count)) {
		g_free(ctx->session_id);
		g_free(ctx);
	}
}

static void answer_ctx_closure_unref(gpointer data, GClosure *closure)
{
	UNUSED_PARAMETER(closure);
	answer_ctx_unref(data);
}

static struct whep_session *lookup_answer_session_locked(struct answer_ctx *ctx)
{
	return g_hash_table_lookup(ctx->webrtc->sessions, ctx->session_id);
}

static GstElement *answer_ctx_get_webrtcbin(struct answer_ctx *ctx)
{
	g_mutex_lock(&ctx->webrtc->sessions_lock);
	struct whep_session *session = lookup_answer_session_locked(ctx);
	GstElement *webrtcbin = session && session->webrtcbin
		? gst_object_ref(session->webrtcbin) : NULL;
	g_mutex_unlock(&ctx->webrtc->sessions_lock);
	return webrtcbin;
}

static void session_free(gpointer data)
{
	struct whep_session *session = data;

	if (session->timeout_id) {
		webrtc_source_remove(session->webrtc, session->timeout_id);
		session->timeout_id = 0;
	}
	if (session->sinkpad) {
		gst_object_unref(session->sinkpad);
		session->sinkpad = NULL;
	}
	answer_ctx_unref(session->answer_ctx);
	session->answer_ctx = NULL;
	if (session->webrtcbin && session->pending_msg) {
		soup_server_message_set_status(session->pending_msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		compat_soup_server_message_unpause(session->webrtc ? session->webrtc->server : NULL, session->pending_msg);
		session->pending_msg = NULL;
	}
	g_free(session->id);
	g_free(session);
}

struct idle_teardown {
	struct gstreamer_webrtc *webrtc;
	char *id;
};

static void remove_session_locked(struct gstreamer_webrtc *webrtc, struct whep_session *session);

static gboolean idle_teardown(gpointer user_data)
{
	struct idle_teardown *ctx = user_data;
	gst_pipeline_subscriber_t *sub_to_unsub = NULL;
	gst_master_pipeline_t *master_pipe = NULL;
	guint remaining = 0;

	g_mutex_lock(&ctx->webrtc->sessions_lock);
	struct whep_session *session = g_hash_table_lookup(ctx->webrtc->sessions, ctx->id);
	if (session) {
		remove_session_locked(ctx->webrtc, session);
		remaining = g_hash_table_size(ctx->webrtc->sessions);
		blog(LOG_INFO, "[obs-gstreamer-webrtc] WebRTC viewer '%s' disconnected (remaining=%u)",
			ctx->id, remaining);
		if (remaining == 0 && ctx->webrtc->sub && ctx->webrtc->master_pipe) {
			sub_to_unsub = ctx->webrtc->sub;
			ctx->webrtc->sub = NULL;
			master_pipe = ctx->webrtc->master_pipe;
		}
	}
	g_mutex_unlock(&ctx->webrtc->sessions_lock);

	if (sub_to_unsub && master_pipe) {
		blog(LOG_INFO, "[obs-gstreamer-webrtc] All WebRTC viewers disconnected: unsubscribing '%s' from master pipeline",
			ctx->webrtc->name ? ctx->webrtc->name : "WebRTC");
		gst_master_pipeline_unsubscribe(master_pipe, sub_to_unsub);
	}

	g_free(ctx->id);
	g_free(ctx);
	return G_SOURCE_REMOVE;
}

static void session_teardown_async(struct gstreamer_webrtc *webrtc, const char *id)
{
	struct idle_teardown *ctx = g_new0(struct idle_teardown, 1);
	ctx->webrtc = webrtc;
	ctx->id = g_strdup(id);
	webrtc_idle_add(webrtc, idle_teardown, ctx);
}

static void webrtcbin_connection_state_changed(GstElement *webrtcbin, GParamSpec *pspec, gpointer user_data)
{
	UNUSED_PARAMETER(pspec);
	struct gstreamer_webrtc *webrtc = user_data;
	GstWebRTCPeerConnectionState state;

	g_object_get(webrtcbin, "connection-state", &state, NULL);
	blog(LOG_DEBUG, "[obs-gstreamer] WHEP session %s: connection-state %d",
		GST_ELEMENT_NAME(webrtcbin), (int)state);
	if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED) {
		if (webrtc && webrtc->master_pipe) {
			gst_master_pipeline_request_keyframe(webrtc->master_pipe);
		}
	} else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED ||
	           state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED ||
	           state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED) {
		session_teardown_async(webrtc, GST_ELEMENT_NAME(webrtcbin));
	}
}

struct cleanup_elements_ctx {
	GstElement *queue;
	GstElement *webrtcbin;
};

static gpointer cleanup_elements_thread(gpointer data)
{
	struct cleanup_elements_ctx *c = data;
	if (c->webrtcbin) {
		gst_element_set_state(c->webrtcbin, GST_STATE_NULL);
		gst_object_unref(c->webrtcbin);
	}
	if (c->queue) {
		gst_element_set_state(c->queue, GST_STATE_NULL);
		gst_object_unref(c->queue);
	}
	g_free(c);
	return NULL;
}

static void remove_session_locked(struct gstreamer_webrtc *webrtc, struct whep_session *session)
{
	// 1. Disconnect any signal callbacks on webrtcbin
	if (session->webrtcbin) {
		g_signal_handlers_disconnect_by_data(session->webrtcbin, webrtc);
		if (session->answer_ctx)
			g_signal_handlers_disconnect_by_data(session->webrtcbin, session->answer_ctx);
	}

	// 2. Remove the queue probe before releasing its pad.
	if (session->rtp_probe_id && session->queue) {
		GstPad *queue_src = gst_element_get_static_pad(session->queue, "src");
		if (queue_src) {
			gst_pad_remove_probe(queue_src, session->rtp_probe_id);
			gst_object_unref(queue_src);
		}
		session->rtp_probe_id = 0;
	}

	// 3. Unlink and release the tee request pad first so live data stops flowing into the queue
	if (session->tee_pad) {
		GstPad *queue_sink = session->queue ? gst_element_get_static_pad(session->queue, "sink") : NULL;
		if (queue_sink) {
			gst_pad_unlink(session->tee_pad, queue_sink);
			gst_object_unref(queue_sink);
		}
		gst_element_release_request_pad(webrtc->tee, session->tee_pad);
		gst_object_unref(session->tee_pad);
		session->tee_pad = NULL;
	}

	// 4. Unlink queue from webrtcbin and release webrtcbin sink pad
	if (session->sinkpad) {
		GstPad *queue_src = session->queue ? gst_element_get_static_pad(session->queue, "src") : NULL;
		if (queue_src) {
			gst_pad_unlink(queue_src, session->sinkpad);
			gst_object_unref(queue_src);
		}
		gst_element_release_request_pad(session->webrtcbin, session->sinkpad);
		gst_object_unref(session->sinkpad);
		session->sinkpad = NULL;
	}

	// 5. Remove from bin and transition to NULL on a helper thread so the GLib main loop / HTTP server never blocks
	struct cleanup_elements_ctx *c = g_new0(struct cleanup_elements_ctx, 1);
	if (session->queue) {
		c->queue = gst_object_ref(session->queue);
		gst_bin_remove(GST_BIN(webrtc->pipe), session->queue);
		session->queue = NULL;
	}
	if (session->webrtcbin) {
		c->webrtcbin = gst_object_ref(session->webrtcbin);
		gst_bin_remove(GST_BIN(webrtc->pipe), session->webrtcbin);
		session->webrtcbin = NULL;
	}
	GThread *t = g_thread_new("webrtc-teardown", cleanup_elements_thread, c);
	if (t)
		g_thread_unref(t);

	blog(LOG_INFO, "[obs-gstreamer] WebRTC session %s removed", session->id);
	g_hash_table_steal(webrtc->sessions, session->id);
	session_free(session);
}

static void discard_unregistered_session(struct gstreamer_webrtc *webrtc,
	struct whep_session *session)
{
	if (session->rtp_probe_id && session->queue) {
		GstPad *queue_src = gst_element_get_static_pad(session->queue, "src");
		if (queue_src) {
			gst_pad_remove_probe(queue_src, session->rtp_probe_id);
			gst_object_unref(queue_src);
		}
		session->rtp_probe_id = 0;
	}

	if (session->tee_pad) {
		GstPad *queue_sink = session->queue
			? gst_element_get_static_pad(session->queue, "sink") : NULL;
		if (queue_sink) {
			gst_pad_unlink(session->tee_pad, queue_sink);
			gst_object_unref(queue_sink);
		}
		if (webrtc->tee)
			gst_element_release_request_pad(webrtc->tee, session->tee_pad);
		gst_object_unref(session->tee_pad);
		session->tee_pad = NULL;
	}

	if (session->sinkpad) {
		GstPad *queue_src = session->queue
			? gst_element_get_static_pad(session->queue, "src") : NULL;
		if (queue_src) {
			gst_pad_unlink(queue_src, session->sinkpad);
			gst_object_unref(queue_src);
		}
		if (session->webrtcbin)
			gst_element_release_request_pad(session->webrtcbin, session->sinkpad);
		gst_object_unref(session->sinkpad);
		session->sinkpad = NULL;
	}

	GstElement **elements[] = { &session->queue, &session->webrtcbin };
	for (guint i = 0; i < G_N_ELEMENTS(elements); i++) {
		GstElement *element = *elements[i];
		if (!element)
			continue;
		gst_element_set_state(element, GST_STATE_NULL);
		if (webrtc->pipe && GST_OBJECT_PARENT(element) == GST_OBJECT(webrtc->pipe))
			gst_bin_remove(GST_BIN(webrtc->pipe), element);
		else
			gst_object_unref(element);
		*elements[i] = NULL;
	}

	session_free(session);
}

struct reply_ctx {
	struct gstreamer_webrtc *webrtc;
	SoupServerMessage *msg;
	char *id;
	char *sdp;
	int status;
};

static gboolean idle_reply(gpointer user_data)
{
	struct reply_ctx *ctx = user_data;

	g_mutex_lock(&ctx->webrtc->sessions_lock);
	struct whep_session *session = g_hash_table_lookup(ctx->webrtc->sessions, ctx->id);
	if (session) {
		session->replied = true;
		session->pending_msg = NULL;
	}
	g_mutex_unlock(&ctx->webrtc->sessions_lock);

	char *location = g_strdup_printf("/whep/%s", ctx->id);
	SoupMessageHeaders *headers = soup_server_message_get_response_headers(ctx->msg);
	soup_message_headers_append(headers, "Location", location);
	soup_message_headers_append(headers, "Access-Control-Allow-Origin", "*");
	soup_message_headers_append(headers, "Access-Control-Expose-Headers", "Location, Content-Type");
	soup_server_message_set_status(ctx->msg, ctx->status, NULL);
	soup_server_message_set_response(ctx->msg, "application/sdp", SOUP_MEMORY_COPY,
		ctx->sdp ? ctx->sdp : "", ctx->sdp ? strlen(ctx->sdp) : 0);
	compat_soup_server_message_unpause(ctx->webrtc ? ctx->webrtc->server : NULL, ctx->msg);
	g_object_unref(ctx->msg);
	if (ctx->status == SOUP_STATUS_CREATED) {
		if (ctx->webrtc && ctx->webrtc->master_pipe) {
			gst_master_pipeline_request_keyframe(ctx->webrtc->master_pipe);
		} else if (ctx->webrtc && ctx->webrtc->pay0) {
			GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
			GstPad *pay_src_pad = gst_element_get_static_pad(ctx->webrtc->pay0, "src");
			if (pay_src_pad) {
				gst_pad_push_event(pay_src_pad, event);
				gst_object_unref(pay_src_pad);
			}
		}
	} else {
		session_teardown_async(ctx->webrtc, ctx->id);
	}
	g_free(location);

	g_free(ctx->id);
	g_free(ctx->sdp);
	g_free(ctx);
	return G_SOURCE_REMOVE;
}

struct timeout_ctx {
	struct gstreamer_webrtc *webrtc;
	char *id;
};

static gboolean answer_timeout(gpointer user_data)
{
	struct timeout_ctx *ctx = user_data;

	g_mutex_lock(&ctx->webrtc->sessions_lock);
	struct whep_session *session = g_hash_table_lookup(ctx->webrtc->sessions, ctx->id);
	if (session && !session->replied && session->pending_msg) {
		blog(LOG_WARNING, "[obs-gstreamer] WHEP session %s: ICE gathering timed out -> 500",
			ctx->id);
		soup_server_message_set_status(session->pending_msg,
			SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		compat_soup_server_message_unpause(ctx->webrtc ? ctx->webrtc->server : NULL, session->pending_msg);
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

static void on_gathering_complete(GstElement *webrtcbin, GParamSpec *pspec, gpointer user_data)
{
	struct answer_ctx *ctx = user_data;
	answer_ctx_ref(ctx);
	GstWebRTCICEGatheringState state;

	g_object_get(webrtcbin, "ice-gathering-state", &state, NULL);
	if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) {
		answer_ctx_unref(ctx);
		return;
	}

	g_signal_handlers_disconnect_by_func(webrtcbin, on_gathering_complete, user_data);

	GstWebRTCSessionDescription *local = NULL;
	g_object_get(webrtcbin, "local-description", &local, NULL);
	gchar *sdp_text = local ? gst_sdp_message_as_text(local->sdp) : NULL;
	if (local)
		gst_webrtc_session_description_free(local);

	g_mutex_lock(&ctx->webrtc->sessions_lock);
	struct whep_session *session = lookup_answer_session_locked(ctx);
	struct answer_ctx *session_ctx = NULL;
	if (!session || session->answer_ctx != ctx || session->replied || !session->pending_msg) {
		g_mutex_unlock(&ctx->webrtc->sessions_lock);
		blog(LOG_WARNING, "[obs-gstreamer] WHEP session %s: completed after timeout/cleanup",
			ctx->session_id);
		g_free(sdp_text);
		answer_ctx_unref(ctx);
		return;
	}

	blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: ICE gathering complete, sending 201 answer",
		session->id);
	session_ctx = session->answer_ctx;
	session->answer_ctx = NULL;

	struct reply_ctx *reply = g_new0(struct reply_ctx, 1);
	reply->webrtc = ctx->webrtc;
	reply->msg = g_object_ref(session->pending_msg);
	reply->id = g_strdup(session->id);
	reply->sdp = sdp_text;
	reply->status = SOUP_STATUS_CREATED;
	webrtc_idle_add(ctx->webrtc, idle_reply, reply);
	g_mutex_unlock(&ctx->webrtc->sessions_lock);

	answer_ctx_unref(session_ctx);
	answer_ctx_unref(ctx);
}

static void on_local_description_set(GstPromise *promise, gpointer user_data)
{
	struct answer_ctx *ctx = user_data;
	answer_ctx_ref(ctx);
	gst_promise_unref(promise);

	blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: local description set, waiting for ICE",
		ctx->session_id);
	g_mutex_lock(&ctx->webrtc->sessions_lock);
	struct whep_session *session = lookup_answer_session_locked(ctx);
	GstElement *webrtcbin = session && session->answer_ctx == ctx && session->webrtcbin
		? gst_object_ref(session->webrtcbin) : NULL;
	if (webrtcbin) {
		g_signal_connect_data(webrtcbin, "notify::ice-gathering-state",
			G_CALLBACK(on_gathering_complete), answer_ctx_ref(ctx),
			answer_ctx_closure_unref, 0);
	}
	g_mutex_unlock(&ctx->webrtc->sessions_lock);

	if (webrtcbin) {
		GstWebRTCICEGatheringState state;
		g_object_get(webrtcbin, "ice-gathering-state", &state, NULL);
		if (state == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE)
			on_gathering_complete(webrtcbin, NULL, ctx);
		gst_object_unref(webrtcbin);
	}
	answer_ctx_unref(ctx);
}

static void on_remote_description_set(GstPromise *promise, gpointer user_data)
{
	struct answer_ctx *ctx = user_data;
	answer_ctx_ref(ctx);
	const GstStructure *remote_reply = gst_promise_get_reply(promise);
	if (remote_reply) {
		gchar *reply_text = gst_structure_to_string(remote_reply);
		blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: set-remote-description reply: %s",
			ctx->session_id, reply_text);
		g_free(reply_text);
	}
	gst_promise_unref(promise);

	g_mutex_lock(&ctx->webrtc->sessions_lock);
	struct whep_session *session = lookup_answer_session_locked(ctx);
	GstElement *queue = session && session->queue ? gst_object_ref(session->queue) : NULL;
	GstElement *webrtcbin = session && session->webrtcbin ? gst_object_ref(session->webrtcbin) : NULL;
	g_mutex_unlock(&ctx->webrtc->sessions_lock);
	if (!queue || !webrtcbin) {
		if (queue) gst_object_unref(queue);
		if (webrtcbin) gst_object_unref(webrtcbin);
		answer_ctx_unref(ctx);
		return;
	}

	gst_element_sync_state_with_parent(queue);
	gst_element_sync_state_with_parent(webrtcbin);
	blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: branch synced to PLAYING",
		ctx->session_id);
	GArray *transceivers = NULL;
	g_signal_emit_by_name(webrtcbin, "get-transceivers", &transceivers);
	blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: transceivers after offer: %u",
		ctx->session_id, transceivers ? transceivers->len : 0);
	if (transceivers)
		g_array_unref(transceivers);
	GstWebRTCSignalingState signaling_state;
	g_object_get(webrtcbin, "signaling-state", &signaling_state, NULL);
	blog(LOG_INFO, "[obs-gstreamer] WHEP session %s: signaling state before answer: %d",
		ctx->session_id, (int)signaling_state);

	GstPromise *p = gst_promise_new_with_change_func(on_answer_created,
		answer_ctx_ref(ctx), answer_ctx_unref);
	g_signal_emit_by_name(webrtcbin, "create-answer", NULL, p);
	gst_object_unref(queue);
	gst_object_unref(webrtcbin);
	answer_ctx_unref(ctx);
}

static void on_answer_created(GstPromise *promise, gpointer user_data)
{
	struct answer_ctx *ctx = user_data;
	answer_ctx_ref(ctx);

	const GstStructure *reply = gst_promise_get_reply(promise);
	GstWebRTCSessionDescription *answer = NULL;
	if (reply) {
		gchar *reply_text = gst_structure_to_string(reply);
		blog(LOG_ERROR, "[obs-gstreamer] WHEP session %s: create-answer reply: %s",
			ctx->session_id, reply_text);
		g_free(reply_text);
		gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
	}
	gst_promise_unref(promise);

	if (!answer) {
		blog(LOG_ERROR, "[obs-gstreamer] WHEP session %s: create-answer failed",
			ctx->session_id);
		g_mutex_lock(&ctx->webrtc->sessions_lock);
		struct whep_session *session = lookup_answer_session_locked(ctx);
		if (session && !session->replied && session->pending_msg) {
			struct reply_ctx *reply_ctx = g_new0(struct reply_ctx, 1);
			reply_ctx->webrtc = ctx->webrtc;
			reply_ctx->msg = g_object_ref(session->pending_msg);
			reply_ctx->id = g_strdup(session->id);
			reply_ctx->sdp = NULL;
			reply_ctx->status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
			webrtc_idle_add(ctx->webrtc, idle_reply, reply_ctx);
		}
		g_mutex_unlock(&ctx->webrtc->sessions_lock);
		answer_ctx_unref(ctx);
		return;
	}

	GstElement *webrtcbin = answer_ctx_get_webrtcbin(ctx);
	if (!webrtcbin) {
		gst_webrtc_session_description_free(answer);
		answer_ctx_unref(ctx);
		return;
	}
	GstPromise *p = gst_promise_new_with_change_func(on_local_description_set,
		answer_ctx_ref(ctx), answer_ctx_unref);
	g_signal_emit_by_name(webrtcbin, "set-local-description", answer, p);
	gst_webrtc_session_description_free(answer);
	gst_object_unref(webrtcbin);
	answer_ctx_unref(ctx);
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
	if (count >= MAX_SESSIONS) {
		GHashTableIter iter;
		gpointer key, value;
		struct whep_session *evict = NULL;
		g_hash_table_iter_init(&iter, webrtc->sessions);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			struct whep_session *s = value;
			GstWebRTCPeerConnectionState st = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
			if (s->webrtcbin)
				g_object_get(s->webrtcbin, "connection-state", &st, NULL);
			if (st != GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED) {
				evict = s;
				break;
			}
		}
		if (!evict) {
			g_hash_table_iter_init(&iter, webrtc->sessions);
			if (g_hash_table_iter_next(&iter, &key, &value))
				evict = value;
		}
		if (evict) {
			blog(LOG_INFO, "[obs-gstreamer] WHEP POST: evicting stale session %s to free slot", evict->id);
			remove_session_locked(webrtc, evict);
			count = g_hash_table_size(webrtc->sessions);
		}
	}
	g_mutex_unlock(&webrtc->sessions_lock);
	if (count >= MAX_SESSIONS) {
		blog(LOG_WARNING, "[obs-gstreamer] WHEP POST: session limit reached (%d) -> 503", MAX_SESSIONS);
		soup_server_message_set_status(msg, SOUP_STATUS_SERVICE_UNAVAILABLE, NULL);
		return;
	}

	struct whep_session *session = g_new0(struct whep_session, 1);
	session->webrtc = webrtc;
	session->id = g_strdup_printf("viewer%u", ++webrtc->session_counter);

	session->queue = gst_element_factory_make("queue", NULL);
	session->webrtcbin = gst_element_factory_make("webrtcbin", session->id);
	if (!session->queue || !session->webrtcbin) {
		blog(LOG_ERROR, "[obs-gstreamer] WHEP POST: failed to create queue/webrtcbin -> 500");
		discard_unregistered_session(webrtc, session);
		soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		return;
	}

	g_object_set(session->queue, "leaky", 2 /* downstream */, "max-size-buffers", 2,
		"max-size-time", (guint64)0, "max-size-bytes", (guint)0, NULL);
	g_object_set(session->webrtcbin, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
		"latency", (guint)0, NULL);

	const char *stun = webrtc->stun_server
		? webrtc->stun_server
		: (webrtc->settings ? obs_data_get_string(webrtc->settings, "webrtc_stun_server") : NULL);
	if (stun && stun[0])
		g_object_set(session->webrtcbin, "stun-server", stun, NULL);

	const char *turn = webrtc->turn_server
		? webrtc->turn_server
		: (webrtc->settings ? obs_data_get_string(webrtc->settings, "webrtc_turn_server") : NULL);
	if (turn && turn[0] && g_strcmp0(turn, "none") != 0) {
		g_object_set(session->webrtcbin, "turn-server", turn, NULL);
		blog(LOG_INFO, "[obs-gstreamer-webrtc] WHEP session %s: configured TURN relay: %s",
			session->id, turn);
	}

	// Build the RTP branch before applying the browser offer. Keep the
	// payloader's negotiated RTP caps intact all the way into webrtcbin.
	gst_bin_add_many(GST_BIN(webrtc->pipe), session->queue,
		session->webrtcbin, NULL);

	gchar *offer_text = g_strndup(body->data, size);
	GstSDPMessage *offer_sdp = NULL;
	GstSDPResult sdp_result = gst_sdp_message_new_from_text(offer_text, &offer_sdp);
	g_free(offer_text);
	if (sdp_result != GST_SDP_OK || !offer_sdp) {
		blog(LOG_WARNING, "[obs-gstreamer] WHEP POST: invalid SDP offer -> 400");
		discard_unregistered_session(webrtc, session);
		soup_server_message_set_status(msg, SOUP_STATUS_BAD_REQUEST, NULL);
		return;
	}
	GstWebRTCSessionDescription *offer =
		gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, offer_sdp);
	if (!offer) {
		gst_sdp_message_free(offer_sdp);
		discard_unregistered_session(webrtc, session);
		soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		return;
	}

	session->sinkpad = gst_element_request_pad_simple(session->webrtcbin, "sink_%u");
	if (!session->sinkpad) {
		blog(LOG_ERROR, "[obs-gstreamer] WHEP POST: failed to request webrtcbin sink pad -> 500");
		gst_webrtc_session_description_free(offer);
		discard_unregistered_session(webrtc, session);
		soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		return;
	}
	GstWebRTCRTPTransceiver *transceiver = NULL;
	g_object_get(session->sinkpad, "transceiver", &transceiver, NULL);
	if (transceiver) {
		const char *rtp_caps_str = webrtc->is_h265
			? "application/x-rtp,media=video,encoding-name=H265,clock-rate=90000"
			: "application/x-rtp,media=video,encoding-name=H264,clock-rate=90000";
		GstCaps *codec_caps = gst_caps_from_string(rtp_caps_str);
		g_object_set(transceiver, "direction",
			GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
			"codec-preferences", codec_caps, NULL);
		gst_caps_unref(codec_caps);
		gst_object_unref(transceiver);
	}
	GstPad *queue_src = gst_element_get_static_pad(session->queue, "src");
	if (!queue_src || gst_pad_link(queue_src, session->sinkpad) != GST_PAD_LINK_OK) {
		if (queue_src) gst_object_unref(queue_src);
		blog(LOG_ERROR, "[obs-gstreamer] WHEP POST: failed to link viewer RTP branch -> 500");
		gst_webrtc_session_description_free(offer);
		discard_unregistered_session(webrtc, session);
		soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		return;
	}
	gst_object_unref(queue_src);
	session->tee_pad = gst_element_request_pad_simple(webrtc->tee, "src_%u");
	GstPad *queue_sink = gst_element_get_static_pad(session->queue, "sink");
	if (!session->tee_pad || !queue_sink ||
	    gst_pad_link(session->tee_pad, queue_sink) != GST_PAD_LINK_OK) {
		if (queue_sink) gst_object_unref(queue_sink);
		blog(LOG_ERROR, "[obs-gstreamer] WHEP POST: failed to link tee -> viewer queue -> 500");
		gst_webrtc_session_description_free(offer);
		discard_unregistered_session(webrtc, session);
		soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		return;
	}
	gst_object_unref(queue_sink);
	session->rtp_counter.label = session->id;
	GstPad *branch_src = gst_element_get_static_pad(session->queue, "src");
	if (branch_src) {
		session->rtp_probe_id = gst_pad_add_probe(branch_src, GST_PAD_PROBE_TYPE_BUFFER,
			rtp_packet_probe, &session->rtp_counter, NULL);
		gst_object_unref(branch_src);
	}
	blog(LOG_INFO, "[obs-gstreamer] WHEP POST %s: RTP branch linked before offer", session->id);
	if (!gst_element_sync_state_with_parent(session->queue) ||
	    !gst_element_sync_state_with_parent(session->webrtcbin)) {
		blog(LOG_ERROR, "[obs-gstreamer] WHEP POST: failed to sync viewer branch -> 500");
		gst_webrtc_session_description_free(offer);
		discard_unregistered_session(webrtc, session);
		soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		return;
	}
	blog(LOG_INFO, "[obs-gstreamer] WHEP POST %s: branch synced before remote offer", session->id);

	g_mutex_lock(&webrtc->sessions_lock);
	g_hash_table_insert(webrtc->sessions, g_strdup(session->id), session);
	guint new_count = g_hash_table_size(webrtc->sessions);
	bool need_subscribe = (webrtc->master_pipe != NULL && webrtc->sub == NULL && webrtc->active);
	gst_master_pipeline_t *master_pipe = webrtc->master_pipe;
	g_mutex_unlock(&webrtc->sessions_lock);

	g_signal_connect(session->webrtcbin, "notify::connection-state",
		G_CALLBACK(webrtcbin_connection_state_changed), webrtc);

	if (need_subscribe && master_pipe) {
		blog(LOG_INFO, "[obs-gstreamer-webrtc] WebRTC viewer '%s' connected (viewers=%u): subscribing '%s' to master pipeline",
			session->id, new_count, webrtc->name ? webrtc->name : "WebRTC");
		gst_pipeline_subscriber_t *new_sub = gst_master_pipeline_subscribe(
			master_pipe, webrtc_frame_cb, webrtc, webrtc->name ? webrtc->name : "WebRTC");
		g_mutex_lock(&webrtc->sessions_lock);
		if (!webrtc->active || g_hash_table_size(webrtc->sessions) == 0 || webrtc->sub != NULL) {
			g_mutex_unlock(&webrtc->sessions_lock);
			if (new_sub) {
				gst_master_pipeline_unsubscribe(master_pipe, new_sub);
			}
		} else {
			webrtc->sub = new_sub;
			g_mutex_unlock(&webrtc->sessions_lock);
		}
	} else if (master_pipe) {
		blog(LOG_INFO, "[obs-gstreamer-webrtc] WebRTC viewer '%s' connected (total viewers=%u)",
			session->id, new_count);
		gst_master_pipeline_request_keyframe(master_pipe);
	} else if (webrtc->pay0) {
		// Ask the encoder for a keyframe so late joiners get picture quickly.
		GstEvent *event = gst_video_event_new_upstream_force_key_unit(
			GST_CLOCK_TIME_NONE, TRUE, 0);
		GstPad *pay_src_pad = gst_element_get_static_pad(webrtc->pay0, "src");
		if (pay_src_pad) {
			gst_pad_push_event(pay_src_pad, event);
			gst_object_unref(pay_src_pad);
		}
	}

	blog(LOG_INFO, "[obs-gstreamer] WHEP POST %s: branch added (unlinked), negotiating", session->id);

	// Negotiate: offer -> answer. Canonical WHEP answerer flow:
	// set-remote-description -> request+link sink pad (elements in NULL)
	// -> create-answer -> set-local-description -> wait ICE complete.
	blog(LOG_INFO, "[obs-gstreamer] WHEP POST: offer parsed (%u bytes), starting negotiation", (guint)size);

	struct answer_ctx *ctx = g_new0(struct answer_ctx, 1);
	g_atomic_ref_count_init(&ctx->ref_count);
	ctx->webrtc = webrtc;
	ctx->session_id = g_strdup(session->id);

	struct timeout_ctx *tctx = g_new0(struct timeout_ctx, 1);
	tctx->webrtc = webrtc;
	tctx->id = g_strdup(ctx->session_id);
	g_mutex_lock(&webrtc->sessions_lock);
	session = g_hash_table_lookup(webrtc->sessions, ctx->session_id);
	GstElement *session_webrtcbin = session && session->webrtcbin
		? gst_object_ref(session->webrtcbin) : NULL;
	if (session_webrtcbin) {
		session->answer_ctx = answer_ctx_ref(ctx);
		session->pending_msg = msg;
		session->timeout_id = webrtc_timeout_add_seconds(
			webrtc, ANSWER_TIMEOUT_SECONDS, answer_timeout, tctx);
		compat_soup_server_message_pause(webrtc->server, msg);
	}
	g_mutex_unlock(&webrtc->sessions_lock);
	if (!session_webrtcbin) {
		g_free(tctx->id);
		g_free(tctx);
		answer_ctx_unref(ctx);
		gst_webrtc_session_description_free(offer);
		soup_server_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
		return;
	}

	GstPromise *promise = gst_promise_new_with_change_func(on_remote_description_set,
		answer_ctx_ref(ctx), answer_ctx_unref);
	g_signal_emit_by_name(session_webrtcbin, "set-remote-description", offer, promise);
	gst_webrtc_session_description_free(offer);
	blog(LOG_INFO, "[obs-gstreamer] WebRTC session %s negotiating (%u active)",
		ctx->session_id, count + 1);
	gst_object_unref(session_webrtcbin);
	answer_ctx_unref(ctx);
}

static void whep_delete(struct gstreamer_webrtc *webrtc, SoupServerMessage *msg, const char *path)
{
	blog(LOG_INFO, "[obs-gstreamer] WHEP DELETE %s: started", path);
	const char *id = path + strlen("/whep/");
	while (*id == '/')
		id++;

	gst_pipeline_subscriber_t *sub_to_unsub = NULL;
	gst_master_pipeline_t *master_pipe = NULL;

	g_mutex_lock(&webrtc->sessions_lock);
	struct whep_session *session = g_hash_table_lookup(webrtc->sessions, id);
	if (!session) {
		g_mutex_unlock(&webrtc->sessions_lock);
		soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
		return;
	}

	remove_session_locked(webrtc, session);
	guint remaining = g_hash_table_size(webrtc->sessions);
	blog(LOG_INFO, "[obs-gstreamer-webrtc] WebRTC viewer '%s' disconnected (remaining=%u)",
		id, remaining);
	if (remaining == 0 && webrtc->sub && webrtc->master_pipe) {
		sub_to_unsub = webrtc->sub;
		webrtc->sub = NULL;
		master_pipe = webrtc->master_pipe;
	}
	g_mutex_unlock(&webrtc->sessions_lock);

	if (sub_to_unsub && master_pipe) {
		blog(LOG_INFO, "[obs-gstreamer-webrtc] All WebRTC viewers disconnected: unsubscribing '%s' from master pipeline",
			webrtc->name ? webrtc->name : "WebRTC");
		gst_master_pipeline_unsubscribe(master_pipe, sub_to_unsub);
	}

	SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);
	soup_message_headers_append(headers, "Access-Control-Allow-Origin", "*");
	soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
}

static void whep_handler(SoupServer *server, SoupServerMessage *msg, const char *path,
	GHashTable *query, gpointer user_data)
{
	struct gstreamer_webrtc *webrtc = user_data;

	if (g_strcmp0(soup_server_message_get_method(msg), "OPTIONS") == 0) {
		SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);
		soup_message_headers_append(headers, "Access-Control-Allow-Origin", "*");
		soup_message_headers_append(headers, "Access-Control-Allow-Methods", "POST, DELETE, OPTIONS");
		soup_message_headers_append(headers, "Access-Control-Allow-Headers", "Content-Type, Accept");
		soup_message_headers_append(headers, "Access-Control-Expose-Headers", "Location, Content-Type");
		soup_server_message_set_status(msg, SOUP_STATUS_NO_CONTENT, NULL);
	} else if (g_strcmp0(soup_server_message_get_method(msg), "POST") == 0) {
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
	const char *src_name = GST_MESSAGE_SRC_NAME(message);

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ERROR: {
		GError *err = NULL;
		gchar *debug = NULL;
		gst_message_parse_error(message, &err, &debug);
		blog(LOG_ERROR, "[obs-gstreamer-webrtc] [%s] ERROR: %s (debug: %s)",
			src_name ? src_name : "pipeline",
			err ? err->message : "unknown error",
			debug ? debug : "none");
		if (err) g_error_free(err);
		g_free(debug);
		break;
	}
	case GST_MESSAGE_WARNING: {
		GError *err = NULL;
		gchar *debug = NULL;
		gst_message_parse_warning(message, &err, &debug);
		blog(LOG_WARNING, "[obs-gstreamer-webrtc] [%s] WARNING: %s (debug: %s)",
			src_name ? src_name : "pipeline",
			err ? err->message : "unknown warning",
			debug ? debug : "none");
		if (err) g_error_free(err);
		g_free(debug);
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

struct gstreamer_webrtc *gst_webrtc_branch_start(gst_master_pipeline_t *master_pipe,
						 const char *name,
						 const char *http_port,
						 const char *stun_server,
						 const char *turn_server,
						 const char *web_root,
						 const char *encoder_type)
{
	UNUSED_PARAMETER(encoder_type);
	if (!master_pipe) {
		blog(LOG_ERROR, "[obs-gstreamer-webrtc] Cannot start WebRTC branch '%s': master_pipe is NULL",
			name ? name : "unnamed");
		return NULL;
	}

	struct gstreamer_webrtc *webrtc = g_new0(struct gstreamer_webrtc, 1);
	webrtc->name = g_strdup(name && name[0] ? name : "WebRTC");
	webrtc->stun_server = (stun_server && stun_server[0]) ? g_strdup(stun_server) : NULL;
	webrtc->turn_server = (turn_server && turn_server[0]) ? g_strdup(turn_server) : NULL;
	webrtc->master_pipe = master_pipe;
	webrtc->sub = NULL;
	webrtc->active = true;
	webrtc->sessions = g_hash_table_new(g_str_hash, g_str_equal);
	g_mutex_init(&webrtc->sessions_lock);

	const char *port = (http_port && http_port[0]) ? http_port : "8888";

	if (!web_root || !web_root[0]) {
		webrtc->web_root = g_build_filename(g_get_user_data_dir(), "obs-gstreamer",
			"webrtc", NULL);
	} else {
		webrtc->web_root = g_strdup(web_root);
	}
	seed_web_root(webrtc->web_root);
	blog(LOG_INFO, "[obs-gstreamer-webrtc] WebRTC web root: %s", webrtc->web_root);

	const char *pipe_codec = gst_master_pipeline_get_codec(master_pipe);
	webrtc->is_h265 = (pipe_codec && g_strcmp0(pipe_codec, "h265") == 0) ||
	                  (encoder_type && (strstr(encoder_type, "h265") || strstr(encoder_type, "hevc")));

	gchar *launch = webrtc->is_h265
		? g_strdup(
			"appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true "
			"caps=\"video/x-h265, stream-format=byte-stream, alignment=au\" "
			"! h265parse "
			"! rtph265pay name=pay0 config-interval=-1 aggregate-mode=zero-latency "
			"! tee name=t "
			"t. ! queue leaky=downstream max-size-buffers=2 ! fakesink sync=false")
		: g_strdup(
			"appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true "
			"caps=\"video/x-h264, stream-format=byte-stream, alignment=au\" "
			"! h264parse "
			"! rtph264pay name=pay0 config-interval=-1 aggregate-mode=zero-latency "
			"! tee name=t "
			"t. ! queue leaky=downstream max-size-buffers=2 ! fakesink sync=false");

	GError *err = NULL;
	webrtc->pipe = gst_parse_launch(launch, &err);
	g_free(launch);
	if (err || !webrtc->pipe) {
		blog(LOG_ERROR, "[obs-gstreamer-webrtc] Failed to create WebRTC pipeline for '%s': %s",
			webrtc->name, err ? err->message : "unknown error");
		if (err)
			g_error_free(err);
		gstreamer_webrtc_destroy(webrtc);
		return NULL;
	}

	webrtc->appsrc = gst_bin_get_by_name(GST_BIN(webrtc->pipe), "appsrc_video");
	if (webrtc->appsrc) {
		gst_app_src_set_stream_type(GST_APP_SRC(webrtc->appsrc), GST_APP_STREAM_TYPE_STREAM);
		gst_app_src_set_latency(GST_APP_SRC(webrtc->appsrc), 0, 0);
	}
	webrtc->tee = gst_bin_get_by_name(GST_BIN(webrtc->pipe), "t");
	webrtc->pay0 = gst_bin_get_by_name(GST_BIN(webrtc->pipe), "pay0");
	webrtc->pay_counter.label = "payloader";
	if (webrtc->pay0) {
		GstPad *pay_src = gst_element_get_static_pad(webrtc->pay0, "src");
		if (pay_src) {
			gst_pad_add_probe(pay_src, GST_PAD_PROBE_TYPE_BUFFER, rtp_packet_probe,
				&webrtc->pay_counter, NULL);
			gst_object_unref(pay_src);
		}
	}

	webrtc->loop_context = g_main_context_new();
	webrtc->loop = g_main_loop_new(webrtc->loop_context, FALSE);

	GstBus *bus = gst_element_get_bus(webrtc->pipe);
	GSource *bus_source = gst_bus_create_watch(bus);
	g_source_set_callback(bus_source, (GSourceFunc)bus_callback, webrtc, NULL);
	webrtc->bus_watch_id = g_source_attach(bus_source, webrtc->loop_context);
	g_source_unref(bus_source);
	gst_object_unref(bus);

	g_main_context_push_thread_default(webrtc->loop_context);
#if !SOUP_CHECK_VERSION(3, 0, 0)
	webrtc->server = soup_server_new(SOUP_SERVER_ASYNC_CONTEXT, webrtc->loop_context, NULL);
#else
	webrtc->server = soup_server_new(NULL, NULL);
#endif
	GError *listen_err = NULL;
	int port_num = port ? atoi(port) : 8888;
	if (port_num <= 0 || port_num > 65535)
		port_num = 8888;
	if (!soup_server_listen_all(webrtc->server, port_num, 0, &listen_err)) {
		g_clear_error(&listen_err);
		if (!soup_server_listen_local(webrtc->server, port_num, 0, &listen_err)) {
			g_main_context_pop_thread_default(webrtc->loop_context);
			blog(LOG_ERROR, "[obs-gstreamer-webrtc] Failed to listen on HTTP port %s: %s",
				port, listen_err ? listen_err->message : "unknown error");
			if (listen_err)
				g_error_free(listen_err);
			gstreamer_webrtc_destroy(webrtc);
			return NULL;
		}
	}

	soup_server_add_handler(webrtc->server, "/whep", whep_handler, webrtc, NULL);
	soup_server_add_handler(webrtc->server, NULL, static_file_handler, webrtc, NULL);
	g_main_context_pop_thread_default(webrtc->loop_context);

	webrtc->loop_thread = g_thread_new("obs_gst_webrtc_loop", webrtc_loop_thread_func, webrtc);

	if (gst_element_set_state(webrtc->pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		blog(LOG_ERROR, "[obs-gstreamer-webrtc] Failed to start WebRTC pipeline for '%s'", webrtc->name);
		gstreamer_webrtc_destroy(webrtc);
		return NULL;
	}

	blog(LOG_INFO, "[obs-gstreamer-webrtc] WebRTC branch started: http://0.0.0.0:%s/ (name='%s', waiting for viewer connection)",
		port, webrtc->name);
	return webrtc;
}

void gst_webrtc_branch_stop(struct gstreamer_webrtc *webrtc)
{
	gstreamer_webrtc_destroy(webrtc);
}

struct gstreamer_webrtc *gstreamer_webrtc_create(obs_output_t *output, obs_data_t *settings,
						 struct obs_video_info *ovi, char **error)
{
	const char *gst_format = obs_to_gst_video_format_str(ovi->output_format);

	if (!gst_format) {
		if (error)
			*error = g_strdup("unhandled output pixel format");
		return NULL;
	}

	struct gstreamer_webrtc *webrtc = g_new0(struct gstreamer_webrtc, 1);
	webrtc->output = output;
	webrtc->settings = settings;
	webrtc->ovi = *ovi;
	webrtc->name = g_strdup(output ? obs_output_get_name(output) : "WebRTC");
	const char *stun = settings ? obs_data_get_string(settings, "webrtc_stun_server") : NULL;
	webrtc->stun_server = (stun && stun[0]) ? g_strdup(stun) : NULL;
	const char *turn = settings ? obs_data_get_string(settings, "webrtc_turn_server") : NULL;
	webrtc->turn_server = (turn && turn[0]) ? g_strdup(turn) : NULL;
	webrtc->active = true;
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
		"! h264parse "
		"! rtph264pay name=pay0 config-interval=-1 "
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

	webrtc->loop_context = g_main_context_new();
	webrtc->loop = g_main_loop_new(webrtc->loop_context, FALSE);

	GstBus *bus = gst_element_get_bus(webrtc->pipe);
	GSource *bus_source = gst_bus_create_watch(bus);
	g_source_set_callback(bus_source, (GSourceFunc)bus_callback, webrtc, NULL);
	webrtc->bus_watch_id = g_source_attach(bus_source, webrtc->loop_context);
	g_source_unref(bus_source);
	gst_object_unref(bus);

	g_main_context_push_thread_default(webrtc->loop_context);
#if !SOUP_CHECK_VERSION(3, 0, 0)
	webrtc->server = soup_server_new(SOUP_SERVER_ASYNC_CONTEXT, webrtc->loop_context, NULL);
#else
	webrtc->server = soup_server_new(NULL, NULL);
#endif
	GError *listen_err = NULL;
	int port_num = port ? atoi(port) : 8888;
	if (port_num <= 0 || port_num > 65535)
		port_num = 8888;
	if (!soup_server_listen_all(webrtc->server, port_num, 0, &listen_err)) {
		g_clear_error(&listen_err);
		if (!soup_server_listen_local(webrtc->server, port_num, 0, &listen_err)) {
			g_main_context_pop_thread_default(webrtc->loop_context);
			if (error)
				*error = g_strdup_printf("failed to listen on HTTP port %s: %s", port,
					listen_err ? listen_err->message : "unknown error");
			if (listen_err)
				g_error_free(listen_err);
			gstreamer_webrtc_destroy(webrtc);
			return NULL;
		}
	}

	soup_server_add_handler(webrtc->server, "/whep", whep_handler, webrtc, NULL);
	soup_server_add_handler(webrtc->server, NULL, static_file_handler, webrtc, NULL);
	g_main_context_pop_thread_default(webrtc->loop_context);

	webrtc->loop_thread = g_thread_new("obs_gst_webrtc_loop", webrtc_loop_thread_func, webrtc);

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

	g_mutex_lock(&webrtc->sessions_lock);
	webrtc->active = false;
	gst_pipeline_subscriber_t *sub_to_unsub = webrtc->sub;
	webrtc->sub = NULL;
	gst_master_pipeline_t *master_pipe = webrtc->master_pipe;
	webrtc->master_pipe = NULL;
	g_mutex_unlock(&webrtc->sessions_lock);

	if (sub_to_unsub && master_pipe) {
		gst_master_pipeline_unsubscribe(master_pipe, sub_to_unsub);
	}

	if (webrtc->bus_watch_id) {
		webrtc_source_remove(webrtc, webrtc->bus_watch_id);
		webrtc->bus_watch_id = 0;
	}

	if (webrtc->server) {
		soup_server_disconnect(webrtc->server);
		g_clear_object(&webrtc->server);
	}

	if (webrtc->loop) {
		g_main_loop_quit(webrtc->loop);
	}
	if (webrtc->loop_thread) {
		g_thread_join(webrtc->loop_thread);
		webrtc->loop_thread = NULL;
	}
	if (webrtc->loop) {
		g_main_loop_unref(webrtc->loop);
		webrtc->loop = NULL;
	}
	if (webrtc->loop_context) {
		g_main_context_unref(webrtc->loop_context);
		webrtc->loop_context = NULL;
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

	g_free(webrtc->name);
	g_free(webrtc->stun_server);
	g_free(webrtc->turn_server);
	g_free(webrtc->web_root);
	g_free(webrtc);
}

GstElement *gstreamer_webrtc_appsrc(struct gstreamer_webrtc *webrtc)
{
	return webrtc ? webrtc->appsrc : NULL;
}

#else /* !ENABLE_WEBRTC */

#include <glib.h>

struct gstreamer_webrtc *gst_webrtc_branch_start(gst_master_pipeline_t *master_pipe,
						 const char *name,
						 const char *http_port,
						 const char *stun_server,
						 const char *turn_server,
						 const char *web_root,
						 const char *encoder_type)
{
	(void)master_pipe;
	(void)name;
	(void)http_port;
	(void)stun_server;
	(void)turn_server;
	(void)web_root;
	(void)encoder_type;
	blog(LOG_WARNING, "[obs-gstreamer] WebRTC WHEP branch is disabled in this build");
	return NULL;
}

void gst_webrtc_branch_stop(struct gstreamer_webrtc *webrtc)
{
	(void)webrtc;
}

struct gstreamer_webrtc *gstreamer_webrtc_create(obs_output_t *output, obs_data_t *settings,
						 struct obs_video_info *ovi, char **error)
{
	(void)output;
	(void)settings;
	(void)ovi;
	blog(LOG_WARNING, "[obs-gstreamer] WebRTC WHEP output is disabled in this build");
	if (error)
		*error = g_strdup("WebRTC output is not supported in this build (compiled without libsoup-3.0 / WebRTC)");
	return NULL;
}

void gstreamer_webrtc_destroy(struct gstreamer_webrtc *webrtc)
{
	(void)webrtc;
}

GstElement *gstreamer_webrtc_appsrc(struct gstreamer_webrtc *webrtc)
{
	(void)webrtc;
	return NULL;
}

#endif /* ENABLE_WEBRTC */
