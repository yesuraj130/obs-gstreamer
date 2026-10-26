/*
 * obs-gstreamer: RTSP Server and Branch Management
 */

#include "gstreamer-rtspserver.h"
#include "gstreamer-render-callback.h"
#include "gstreamer-filter-callback.h"
#include "gstreamer-output-callback.h"
#include "gstreamer-encoder-callback.h"
#include "gstreamer-helper.h"
#include <obs-module.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <string.h>

typedef enum {
	GST_HUB_CB_RENDER = 0,
	GST_HUB_CB_FILTER,
	GST_HUB_CB_OUTPUT,
	GST_HUB_CB_ENCODER,
} gst_hub_cb_type_t;

typedef struct gst_hub_source_pipe {
	gst_hub_cb_type_t cb_type;
	gst_source_type_t source_type;
	char *source_name;
	char *encoder_type;
	char *custom_pipeline;
	int bitrate_kbps;
	int keyint_sec;
	void *cb_handle;
	gst_master_pipeline_t *pipe;
	int ref_count;
	struct gst_hub_source_pipe *next;
} gst_hub_source_pipe_t;

typedef struct gst_hub_server_entry {
	char *service;
	GstRTSPServer *server;
	GstRTSPMountPoints *mounts;
	guint source_id;
	int ref_count;
	struct gst_hub_server_entry *next;
} gst_hub_server_entry_t;

struct gst_hub_branch {
	char *name;
	char *service;
	char *mount_point;
	bool active;
	bool is_pipeline;
	bool waiting_for_keyframe;
	int client_count;
	GstElement *appsrc;
	GstElement *pipe;
	GMutex lock;

	gst_hub_source_pipe_t *source_pipe;
	gst_master_pipeline_t *pipeline;
	gst_pipeline_subscriber_t *sub;

	// RTSP branch fields
	gst_hub_server_entry_t *server_entry;
	GstRTSPMediaFactory *factory;
	GstRTSPMedia *media;

	gst_hub_branch_t *next;
};

static gst_hub_source_pipe_t *g_source_pipes = NULL;
static gst_hub_server_entry_t *g_server_entries = NULL;
static gst_hub_branch_t *g_branches = NULL;
static GMainContext *g_rtsp_context = NULL;
static GMainLoop *g_rtsp_loop = NULL;
static GThread *g_rtsp_thread = NULL;
static GMutex g_hub_mutex;

static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data);
static void media_unprepared_cb(GstRTSPMedia *media, gpointer user_data);
static void stop_branch_internal(gst_hub_branch_t *branch);

static gst_hub_cb_type_t parse_cb_type(const char *render_mode, gst_source_type_t source_type)
{
	if (!render_mode || !*render_mode) {
		return GST_HUB_CB_RENDER;
	}
	if (strstr(render_mode, "Filter")) {
		if (source_type == GST_SOURCE_MAIN_OUTPUT) {
			blog(LOG_INFO, "[obs-gstreamer-rtspserver] Filter callback requested on Main Output, falling back to Render Callback");
			return GST_HUB_CB_RENDER;
		}
		return GST_HUB_CB_FILTER;
	}
	if (strstr(render_mode, "Encoder") || strstr(render_mode, "GPU Texture")) {
		return GST_HUB_CB_ENCODER;
	}
	if (strstr(render_mode, "Output") || strstr(render_mode, "CPU RAM")) {
		return GST_HUB_CB_OUTPUT;
	}
	return GST_HUB_CB_RENDER;
}

static gst_source_type_t parse_source_type(const char *source_type)
{
	if (source_type && strcmp(source_type, "Scene") == 0)
		return GST_SOURCE_SCENE;
	if (source_type && strcmp(source_type, "Source") == 0)
		return GST_SOURCE_SOURCE;
	return GST_SOURCE_MAIN_OUTPUT;
}

static gst_hub_source_pipe_t *get_or_create_source_pipe(
	gst_hub_cb_type_t cb_type,
	gst_source_type_t source_type,
	const char *source_name,
	const char *encoder_type,
	const char *custom_pipeline,
	int bitrate_kbps,
	int keyint_sec)
{
	const char *src_name_safe = source_name ? source_name : "";
	const char *enc_safe = (encoder_type && *encoder_type) ? encoder_type : "x264enc";
	int bitrate = bitrate_kbps > 0 ? bitrate_kbps : 4000;
	int keyint = keyint_sec > 0 ? keyint_sec : 1;

	for (gst_hub_source_pipe_t *p = g_source_pipes; p; p = p->next) {
		if (p->cb_type == cb_type &&
		    p->source_type == source_type &&
		    g_strcmp0(p->source_name, src_name_safe) == 0 &&
		    g_strcmp0(p->encoder_type, enc_safe) == 0 &&
		    p->bitrate_kbps == bitrate &&
		    p->keyint_sec == keyint) {
			p->ref_count++;
			blog(LOG_INFO, "[obs-gstreamer-rtspserver] Reusing active master pipeline for '%s' (refcount=%d)",
				src_name_safe[0] ? src_name_safe : "Main Output", p->ref_count);
			return p;
		}
	}

	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		ovi.base_width = 1920;
		ovi.base_height = 1080;
		ovi.fps_num = 60;
		ovi.fps_den = 1;
	}
	uint32_t fps_num = ovi.fps_num ? ovi.fps_num : 60;
	uint32_t fps_den = ovi.fps_den ? ovi.fps_den : 1;

	void *handle = NULL;
	gst_master_pipeline_t *pipe = NULL;

	switch (cb_type) {
	case GST_HUB_CB_FILTER:
		handle = gst_filter_callback_create(source_type, src_name_safe, custom_pipeline, enc_safe, bitrate, keyint, fps_num, fps_den);
		if (handle) pipe = gst_filter_callback_get_pipeline((gst_filter_callback_t *)handle);
		break;
	case GST_HUB_CB_OUTPUT:
		handle = gst_output_callback_create(source_type, src_name_safe, custom_pipeline, enc_safe, bitrate, keyint, fps_num, fps_den);
		if (handle) pipe = gst_output_callback_get_pipeline((gst_output_callback_t *)handle);
		break;
	case GST_HUB_CB_ENCODER:
		handle = gst_encoder_callback_create(source_type, src_name_safe, custom_pipeline, enc_safe, bitrate, keyint, fps_num, fps_den);
		if (handle) pipe = gst_encoder_callback_get_pipeline((gst_encoder_callback_t *)handle);
		break;
	case GST_HUB_CB_RENDER:
	default:
		handle = gst_render_callback_create(source_type, src_name_safe, NULL, custom_pipeline, enc_safe, bitrate, keyint, fps_num, fps_den);
		if (handle) pipe = gst_render_callback_get_pipeline((gst_render_callback_t *)handle);
		break;
	}

	if (!handle || !pipe) {
		blog(LOG_ERROR, "[obs-gstreamer-rtspserver] Failed to create source callback for '%s'",
			src_name_safe[0] ? src_name_safe : "Main Output");
		if (handle) {
			if (cb_type == GST_HUB_CB_FILTER) gst_filter_callback_destroy((gst_filter_callback_t *)handle);
			else if (cb_type == GST_HUB_CB_OUTPUT) gst_output_callback_destroy((gst_output_callback_t *)handle);
			else if (cb_type == GST_HUB_CB_ENCODER) gst_encoder_callback_destroy((gst_encoder_callback_t *)handle);
			else gst_render_callback_destroy((gst_render_callback_t *)handle);
		}
		return NULL;
	}

	gst_hub_source_pipe_t *entry = bzalloc(sizeof(gst_hub_source_pipe_t));
	entry->cb_type = cb_type;
	entry->source_type = source_type;
	entry->source_name = bstrdup(src_name_safe);
	entry->encoder_type = bstrdup(enc_safe);
	entry->custom_pipeline = bstrdup(custom_pipeline ? custom_pipeline : "");
	entry->bitrate_kbps = bitrate;
	entry->keyint_sec = keyint;
	entry->cb_handle = handle;
	entry->pipe = pipe;
	entry->ref_count = 1;
	entry->next = g_source_pipes;
	g_source_pipes = entry;

	blog(LOG_INFO, "[obs-gstreamer-rtspserver] Created new master pipeline for '%s' (cb_type=%d)",
		src_name_safe[0] ? src_name_safe : "Main Output", (int)cb_type);
	return entry;
}

static void release_source_pipe(gst_hub_source_pipe_t *entry)
{
	if (!entry) return;
	entry->ref_count--;
	if (entry->ref_count > 0) {
		blog(LOG_INFO, "[obs-gstreamer-rtspserver] Master pipeline for '%s' dereferenced (refcount=%d)",
			entry->source_name ? entry->source_name : "Main Output", entry->ref_count);
		return;
	}

	if (g_source_pipes == entry) {
		g_source_pipes = entry->next;
	} else {
		gst_hub_source_pipe_t *prev = g_source_pipes;
		while (prev && prev->next != entry) prev = prev->next;
		if (prev) prev->next = entry->next;
	}

	blog(LOG_INFO, "[obs-gstreamer-rtspserver] Destroying master pipeline for '%s'",
		entry->source_name ? entry->source_name : "Main Output");

	switch (entry->cb_type) {
	case GST_HUB_CB_FILTER:
		gst_filter_callback_destroy((gst_filter_callback_t *)entry->cb_handle);
		break;
	case GST_HUB_CB_OUTPUT:
		gst_output_callback_destroy((gst_output_callback_t *)entry->cb_handle);
		break;
	case GST_HUB_CB_ENCODER:
		gst_encoder_callback_destroy((gst_encoder_callback_t *)entry->cb_handle);
		break;
	case GST_HUB_CB_RENDER:
	default:
		gst_render_callback_destroy((gst_render_callback_t *)entry->cb_handle);
		break;
	}

	bfree(entry->source_name);
	bfree(entry->encoder_type);
	bfree(entry->custom_pipeline);
	bfree(entry);
}

static gst_hub_server_entry_t *get_or_create_rtsp_server(const char *service)
{
	const char *srv = (service && *service) ? service : "8554";
	for (gst_hub_server_entry_t *s = g_server_entries; s; s = s->next) {
		if (g_strcmp0(s->service, srv) == 0) {
			s->ref_count++;
			blog(LOG_INFO, "[obs-gstreamer-rtspserver] Reusing RTSP server on port %s (refcount=%d)",
				srv, s->ref_count);
			return s;
		}
	}

	GstRTSPServer *server = gst_rtsp_server_new();
	if (!server) return NULL;

	gst_rtsp_server_set_address(server, "0.0.0.0");
	gst_rtsp_server_set_service(server, srv);

	GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
	guint source_id = gst_rtsp_server_attach(server, g_rtsp_context);
	if (source_id == 0) {
		blog(LOG_ERROR, "[obs-gstreamer-rtspserver] Failed to attach RTSP server on port %s", srv);
		gst_object_unref(mounts);
		gst_object_unref(server);
		return NULL;
	}

	gst_hub_server_entry_t *entry = bzalloc(sizeof(gst_hub_server_entry_t));
	entry->service = bstrdup(srv);
	entry->server = server;
	entry->mounts = mounts;
	entry->source_id = source_id;
	entry->ref_count = 1;
	entry->next = g_server_entries;
	g_server_entries = entry;

	blog(LOG_INFO, "[obs-gstreamer-rtspserver] Bound new RTSP server listening on port %s", srv);
	return entry;
}

static void release_rtsp_server(gst_hub_server_entry_t *entry)
{
	if (!entry) return;
	entry->ref_count--;
	if (entry->ref_count > 0) {
		blog(LOG_INFO, "[obs-gstreamer-rtspserver] RTSP server on port %s dereferenced (refcount=%d)",
			entry->service, entry->ref_count);
		return;
	}

	if (g_server_entries == entry) {
		g_server_entries = entry->next;
	} else {
		gst_hub_server_entry_t *prev = g_server_entries;
		while (prev && prev->next != entry) prev = prev->next;
		if (prev) prev->next = entry->next;
	}

	blog(LOG_INFO, "[obs-gstreamer-rtspserver] Stopping RTSP server on port %s", entry->service);

	if (entry->source_id > 0) {
		if (g_rtsp_context) {
			GSource *source = g_main_context_find_source_by_id(g_rtsp_context, entry->source_id);
			if (source) g_source_destroy(source);
		} else {
			g_source_remove(entry->source_id);
		}
		entry->source_id = 0;
	}

	if (entry->mounts) {
		gst_object_unref(entry->mounts);
		entry->mounts = NULL;
	}
	if (entry->server) {
		gst_object_unref(entry->server);
		entry->server = NULL;
	}

	bfree(entry->service);
	bfree(entry);
}

static void rtsp_frame_cb(GstBuffer *buf, bool is_keyframe, gpointer user_data)
{
	gst_hub_branch_t *branch = (gst_hub_branch_t *)user_data;
	if (!branch || !buf) return;

	g_mutex_lock(&branch->lock);
	if (!branch->active || !branch->appsrc) {
		g_mutex_unlock(&branch->lock);
		return;
	}

	if (branch->waiting_for_keyframe) {
		if (!is_keyframe) {
			g_mutex_unlock(&branch->lock);
			return;
		}
		branch->waiting_for_keyframe = false;
		blog(LOG_INFO, "[obs-gstreamer-rtspserver] Keyframe delivered to '%s', client playback starting",
			branch->name ? branch->name : "branch");
	}

	GstBuffer *out_buf = gst_buffer_copy(buf);
	GST_BUFFER_PTS(out_buf) = GST_CLOCK_TIME_NONE;
	GST_BUFFER_DTS(out_buf) = GST_CLOCK_TIME_NONE;
	GstElement *appsrc = gst_object_ref(branch->appsrc);
	g_mutex_unlock(&branch->lock);

	GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), out_buf);
	if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING) {
		// Non-fatal flow status
	}
	gst_object_unref(appsrc);
}

gst_hub_branch_t *gst_render_hub_start_pipeline_branch(const char *name, const char *source_type, const char *source_name, const char *pipeline_str)
{
	g_mutex_lock(&g_hub_mutex);

	gst_source_type_t stype = parse_source_type(source_type);
	gst_hub_cb_type_t cb_type = GST_HUB_CB_RENDER;
	gst_hub_source_pipe_t *sp = get_or_create_source_pipe(cb_type, stype, source_name, "x264enc", NULL, 4000, 1);
	if (!sp) {
		g_mutex_unlock(&g_hub_mutex);
		return NULL;
	}

	gchar *pipe_desc = NULL;
	if (pipeline_str && strstr(pipeline_str, "appsrc")) {
		pipe_desc = g_strdup(pipeline_str);
	} else {
		pipe_desc = g_strdup_printf(
			"appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true "
			"caps=\"video/x-h264, stream-format=byte-stream, alignment=au\" ! "
			"h264parse ! decodebin ! %s",
			(pipeline_str && pipeline_str[0]) ? pipeline_str : "autovideosink sync=false");
	}

	GError *err = NULL;
	GstElement *pipe = gst_parse_launch(pipe_desc, &err);
	g_free(pipe_desc);

	if (err || !pipe) {
		if (err) {
			blog(LOG_ERROR, "[obs-gstreamer-rtspserver] Failed to parse pipeline branch: %s", err->message);
			g_error_free(err);
		}
		release_source_pipe(sp);
		g_mutex_unlock(&g_hub_mutex);
		return NULL;
	}

	GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipe), "appsrc_video");
	if (!appsrc) {
		gst_element_set_state(pipe, GST_STATE_NULL);
		gst_object_unref(pipe);
		release_source_pipe(sp);
		g_mutex_unlock(&g_hub_mutex);
		return NULL;
	}

	gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);
	gst_app_src_set_latency(GST_APP_SRC(appsrc), 0, 0);

	gst_hub_branch_t *branch = bzalloc(sizeof(gst_hub_branch_t));
	branch->name = bstrdup(name ? name : "Pipeline");
	branch->is_pipeline = true;
	branch->pipe = pipe;
	branch->appsrc = appsrc;
	branch->source_pipe = sp;
	branch->pipeline = sp->pipe;
	branch->active = true;
	g_mutex_init(&branch->lock);

	gst_element_set_state(branch->pipe, GST_STATE_PLAYING);

	branch->sub = gst_master_pipeline_subscribe(branch->pipeline, rtsp_frame_cb, branch, branch->name);

	branch->next = g_branches;
	g_branches = branch;

	blog(LOG_INFO, "[obs-gstreamer-rtspserver] Direct pipeline branch '%s' started", branch->name);

	g_mutex_unlock(&g_hub_mutex);
	return branch;
}

gst_hub_branch_t *gst_render_hub_start_branch_data(obs_data_t *settings)
{
	if (!settings) return NULL;

	const char *mode = obs_data_get_string(settings, "mode");
	if (g_strcmp0(mode, "Pipeline") == 0) {
		const char *name = obs_data_get_string(settings, "name");
		const char *source_type = obs_data_get_string(settings, "source_type");
		const char *source_name = obs_data_get_string(settings, "source_name");
		const char *pipeline_str = obs_data_get_string(settings, "pipeline");
		return gst_render_hub_start_pipeline_branch(name, source_type, source_name, pipeline_str);
	}

	gst_hub_output_params_t params = {0};
	params.name = obs_data_get_string(settings, "name");
	params.source_type = obs_data_get_string(settings, "source_type");
	params.source_name = obs_data_get_string(settings, "source_name");
	params.render_mode = obs_data_get_string(settings, "render_mode");

	int port = (int)obs_data_get_int(settings, "rtsp_port");
	char port_buf[16] = {0};
	if (port > 0) {
		snprintf(port_buf, sizeof(port_buf), "%d", port);
		params.rtsp_service = port_buf;
	} else {
		params.rtsp_service = obs_data_get_string(settings, "rtsp_service");
	}

	params.rtsp_mount = obs_data_get_string(settings, "rtsp_mount");
	params.encoder_type = obs_data_get_string(settings, "encoder_type");
	params.bitrate_kbps = (int)obs_data_get_int(settings, "bitrate_kbps");
	params.keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	if (params.keyint_sec <= 0) params.keyint_sec = 1;
	params.master_pipeline = obs_data_get_string(settings, "master_pipeline");
	params.rtsp_pipeline = obs_data_get_string(settings, "rtsp_pipeline");

	return gst_render_hub_start_branch(&params);
}

gst_hub_branch_t *gst_render_hub_start_branch(const gst_hub_output_params_t *params)
{
	if (!params) return NULL;

	g_mutex_lock(&g_hub_mutex);

	if (!g_rtsp_context) {
		g_rtsp_context = g_main_context_new();
		g_rtsp_loop = g_main_loop_new(g_rtsp_context, FALSE);
		g_rtsp_thread = g_thread_new("gst_hub_rtsp_loop", (GThreadFunc)g_main_loop_run, g_rtsp_loop);
	}

	gst_source_type_t stype = parse_source_type(params->source_type);
	gst_hub_cb_type_t cb_type = parse_cb_type(params->render_mode, stype);

	gst_hub_source_pipe_t *sp = get_or_create_source_pipe(
		cb_type,
		stype,
		params->source_name,
		params->encoder_type,
		params->master_pipeline,
		params->bitrate_kbps,
		params->keyint_sec);

	if (!sp) {
		g_mutex_unlock(&g_hub_mutex);
		return NULL;
	}

	const char *requested_service = "8554";
	if (params->rtsp_service) {
		const char *s = params->rtsp_service;
		while (*s == ' ' || *s == '\t') s++;
		if (*s != '\0') requested_service = s;
	}

	const char *requested_mount = "/live";
	if (params->rtsp_mount) {
		const char *m = params->rtsp_mount;
		while (*m == ' ' || *m == '\t') m++;
		if (*m != '\0') requested_mount = m;
	}

	gst_hub_server_entry_t *server_entry = get_or_create_rtsp_server(requested_service);
	if (!server_entry) {
		release_source_pipe(sp);
		g_mutex_unlock(&g_hub_mutex);
		return NULL;
	}

	gst_hub_branch_t *branch = bzalloc(sizeof(gst_hub_branch_t));
	branch->name = bstrdup(params->name ? params->name : "Output");
	branch->service = bstrdup(requested_service);
	branch->mount_point = bstrdup(requested_mount);
	branch->source_pipe = sp;
	branch->pipeline = sp->pipe;
	branch->server_entry = server_entry;
	g_mutex_init(&branch->lock);

	GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
	gst_rtsp_media_factory_set_shared(factory, TRUE);
	gst_rtsp_media_factory_set_protocols(factory, GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_UDP_MCAST | GST_RTSP_LOWER_TRANS_TCP);

	const char *launch_str = NULL;
	if (params->rtsp_pipeline && params->rtsp_pipeline[0]) {
		launch_str = params->rtsp_pipeline;
	} else if (params->encoder_type && (strstr(params->encoder_type, "h265") || strstr(params->encoder_type, "hevc"))) {
		launch_str = "( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true "
		             "caps=\"video/x-h265, stream-format=byte-stream, alignment=au\" ! "
		             "h265parse config-interval=1 ! rtph265pay name=pay0 pt=96 config-interval=1 )";
	} else {
		launch_str = "( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true "
		             "caps=\"video/x-h264, stream-format=byte-stream, alignment=au\" ! "
		             "h264parse config-interval=1 ! rtph264pay name=pay0 pt=96 config-interval=1 )";
	}

	gst_rtsp_media_factory_set_launch(factory, launch_str);
	g_signal_connect(factory, "media-configure", G_CALLBACK(media_configure_cb), branch);
	gst_rtsp_mount_points_add_factory(server_entry->mounts, branch->mount_point, factory);

	branch->factory = factory;
	branch->active = true;

	branch->sub = gst_master_pipeline_subscribe(branch->pipeline, rtsp_frame_cb, branch, branch->name);

	branch->next = g_branches;
	g_branches = branch;

	blog(LOG_INFO, "[obs-gstreamer-rtspserver] RTSP branch started: rtsp://0.0.0.0:%s%s (name='%s')",
		branch->service, branch->mount_point, branch->name);

	g_mutex_unlock(&g_hub_mutex);
	return branch;
}

static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data)
{
	UNUSED_PARAMETER(factory);
	gst_hub_branch_t *branch = (gst_hub_branch_t *)user_data;
	if (!branch) return;

	GstElement *element = gst_rtsp_media_get_element(media);
	if (!element) return;

	GstElement *appsrc = gst_bin_get_by_name(GST_BIN(element), "appsrc_video");
	if (!appsrc) {
		blog(LOG_WARNING, "[obs-gstreamer-rtspserver] RTSP media has no appsrc_video");
		gst_object_unref(element);
		return;
	}

	gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);
	gst_app_src_set_latency(GST_APP_SRC(appsrc), 0, 0);

	g_mutex_lock(&branch->lock);
	if (!branch->active) {
		g_mutex_unlock(&branch->lock);
		gst_object_unref(appsrc);
		gst_object_unref(element);
		return;
	}

	if (branch->appsrc) {
		gst_object_unref(branch->appsrc);
	}
	branch->appsrc = appsrc;
	branch->client_count++;

	if (branch->media != media) {
		if (branch->media) {
			g_signal_handlers_disconnect_by_data(branch->media, branch);
			gst_object_unref(branch->media);
		}
		branch->media = (GstRTSPMedia *)gst_object_ref(media);
		g_signal_connect(media, "unprepared", G_CALLBACK(media_unprepared_cb), branch);
	}

	if (branch->client_count == 1) {
		branch->waiting_for_keyframe = true;
	}
	g_mutex_unlock(&branch->lock);

	// Request immediate upstream keyframe so newly connected clients start instantly
	if (branch->pipeline) {
		gst_master_pipeline_request_keyframe(branch->pipeline);
	}

	gst_object_unref(element);
	blog(LOG_INFO, "[obs-gstreamer-rtspserver] RTSP client connected on %s%s (total clients=%d)",
		branch->service ? branch->service : "", branch->mount_point ? branch->mount_point : "",
		branch->client_count);
}

static void media_unprepared_cb(GstRTSPMedia *media, gpointer user_data)
{
	UNUSED_PARAMETER(media);
	gst_hub_branch_t *branch = (gst_hub_branch_t *)user_data;
	if (!branch) return;

	g_mutex_lock(&branch->lock);
	if (!branch->active) {
		g_mutex_unlock(&branch->lock);
		return;
	}

	if (branch->client_count > 0) branch->client_count--;
	blog(LOG_INFO, "[obs-gstreamer-rtspserver] RTSP client disconnected on %s%s (remaining=%d)",
		branch->service ? branch->service : "", branch->mount_point ? branch->mount_point : "",
		branch->client_count);

	if (branch->client_count == 0) {
		if (branch->appsrc) {
			gst_object_unref(branch->appsrc);
			branch->appsrc = NULL;
		}
		if (branch->media) {
			g_signal_handlers_disconnect_by_data(branch->media, branch);
			gst_object_unref(branch->media);
			branch->media = NULL;
		}
	}
	g_mutex_unlock(&branch->lock);
}

static void stop_branch_internal(gst_hub_branch_t *branch)
{
	if (!branch) return;

	if (g_branches == branch) {
		g_branches = branch->next;
	} else {
		gst_hub_branch_t *prev = g_branches;
		while (prev && prev->next != branch) prev = prev->next;
		if (prev) prev->next = branch->next;
	}

	g_mutex_lock(&branch->lock);
	branch->active = false;

	if (branch->sub && branch->pipeline) {
		gst_master_pipeline_unsubscribe(branch->pipeline, branch->sub);
		branch->sub = NULL;
	}

	if (branch->appsrc) {
		gst_object_unref(branch->appsrc);
		branch->appsrc = NULL;
	}

	if (branch->media) {
		g_signal_handlers_disconnect_by_data(branch->media, branch);
		gst_object_unref(branch->media);
		branch->media = NULL;
	}

	if (branch->is_pipeline) {
		if (branch->pipe) {
			gst_element_set_state(branch->pipe, GST_STATE_NULL);
			gst_object_unref(branch->pipe);
			branch->pipe = NULL;
		}
	} else {
		if (branch->factory) {
			g_signal_handlers_disconnect_by_data(branch->factory, branch);
			if (branch->server_entry && branch->server_entry->mounts && branch->mount_point) {
				gst_rtsp_mount_points_remove_factory(branch->server_entry->mounts, branch->mount_point);
			}
			gst_object_unref(branch->factory);
			branch->factory = NULL;
		}
		if (branch->server_entry) {
			release_rtsp_server(branch->server_entry);
			branch->server_entry = NULL;
		}
	}

	if (branch->source_pipe) {
		release_source_pipe(branch->source_pipe);
		branch->source_pipe = NULL;
		branch->pipeline = NULL;
	}

	g_mutex_unlock(&branch->lock);

	blog(LOG_INFO, "[obs-gstreamer-rtspserver] Stopped and destroyed branch '%s'",
		branch->name ? branch->name : "unnamed");

	bfree(branch->name);
	bfree(branch->service);
	bfree(branch->mount_point);
	g_mutex_clear(&branch->lock);
	bfree(branch);
}

void gst_render_hub_stop_branch(gst_hub_branch_t *branch)
{
	if (!branch) return;
	g_mutex_lock(&g_hub_mutex);
	stop_branch_internal(branch);
	g_mutex_unlock(&g_hub_mutex);
}

bool gst_render_hub_is_branch_active(const gst_hub_branch_t *branch)
{
	return branch != NULL && branch->active;
}

void gst_render_hub_shutdown(void)
{
	g_mutex_lock(&g_hub_mutex);
	while (g_branches) {
		stop_branch_internal(g_branches);
	}
	if (g_rtsp_loop) {
		g_main_loop_quit(g_rtsp_loop);
	}
	if (g_rtsp_thread) {
		g_thread_join(g_rtsp_thread);
		g_rtsp_thread = NULL;
	}
	if (g_rtsp_loop) {
		g_main_loop_unref(g_rtsp_loop);
		g_rtsp_loop = NULL;
	}
	if (g_rtsp_context) {
		g_main_context_unref(g_rtsp_context);
		g_rtsp_context = NULL;
	}
	g_mutex_unlock(&g_hub_mutex);
}
