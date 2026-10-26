#include "gstreamer-rtspserver.h"
#include "gstreamer-render-callback.h"
#include <obs-module.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <string.h>

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
	gst_master_pipeline_t *pipeline;
	gst_pipeline_subscriber_t *sub;
	gst_hub_branch_t *next;
};

struct rtsp_branch_extra {
	GstRTSPServer *server;
	GstRTSPMountPoints *mounts;
	GstRTSPMediaFactory *factory;
	GstRTSPMedia *media;
	guint source_id;
};

static gst_master_pipeline_t *g_master_pipe = NULL;
static gst_render_callback_t *g_source_callback = NULL;
static GMainContext *g_rtsp_context = NULL;
static GMainLoop *g_rtsp_loop = NULL;
static GThread *g_rtsp_thread = NULL;
static GMutex g_hub_mutex;

static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data);
static void media_unprepared_cb(GstRTSPMedia *media, gpointer user_data);

static struct {
	gst_hub_branch_t *branch;
	GstRTSPServer *server;
	GstRTSPMountPoints *mounts;
	GstRTSPMediaFactory *factory;
	guint source_id;
} g_rtsp_servers[32];
static int g_rtsp_server_count = 0;
static GMutex g_rtsp_lock;

static void register_rtsp_extra(gst_hub_branch_t *branch, GstRTSPServer *server, GstRTSPMountPoints *mounts, GstRTSPMediaFactory *factory, guint source_id)
{
	g_mutex_lock(&g_rtsp_lock);
	if (g_rtsp_server_count < 32) {
		g_rtsp_servers[g_rtsp_server_count++] = (typeof(g_rtsp_servers[0])){branch, server, mounts, factory, source_id};
	}
	g_mutex_unlock(&g_rtsp_lock);
}

static void unregister_rtsp_extra(gst_hub_branch_t *branch)
{
	g_mutex_lock(&g_rtsp_lock);
	for (int i = 0; i < g_rtsp_server_count; i++) {
		if (g_rtsp_servers[i].branch == branch) {
			if (g_rtsp_servers[i].source_id > 0) {
				if (g_rtsp_context) {
					GSource *source = g_main_context_find_source_by_id(g_rtsp_context, g_rtsp_servers[i].source_id);
					if (source) g_source_destroy(source);
				} else {
					g_source_remove(g_rtsp_servers[i].source_id);
				}
			}
			if (g_rtsp_servers[i].mounts && branch->mount_point) {
				gst_rtsp_mount_points_remove_factory(g_rtsp_servers[i].mounts, branch->mount_point);
			}
			if (g_rtsp_servers[i].factory) {
				g_signal_handlers_disconnect_by_data(g_rtsp_servers[i].factory, branch);
				gst_object_unref(g_rtsp_servers[i].factory);
			}
			if (g_rtsp_servers[i].mounts) {
				gst_object_unref(g_rtsp_servers[i].mounts);
			}
			if (g_rtsp_servers[i].server) {
				g_signal_handlers_disconnect_by_data(g_rtsp_servers[i].server, branch);
				gst_object_unref(g_rtsp_servers[i].server);
			}
			for (int j = i; j < g_rtsp_server_count - 1; j++) {
				g_rtsp_servers[j] = g_rtsp_servers[j + 1];
			}
			g_rtsp_server_count--;
			break;
		}
	}
	g_mutex_unlock(&g_rtsp_lock);
}

/* Pub-Sub Frame Callback for RTSP branches */
static void rtsp_frame_cb(GstBuffer *buf, bool is_keyframe, gpointer user_data)
{
	gst_hub_branch_t *branch = (gst_hub_branch_t *)user_data;
	g_mutex_lock(&branch->lock);
	if (branch->active && branch->appsrc)
	{
		if (branch->waiting_for_keyframe)
		{
			if (!is_keyframe)
			{
				g_mutex_unlock(&branch->lock);
				return;
			}
			branch->waiting_for_keyframe = false;
			blog(LOG_INFO, "[obs-gstreamer-rtspserver] Keyframe delivered to '%s', client playback starting", branch->name ? branch->name : "branch");
		}

		GstBuffer *out_buf = gst_buffer_copy(buf);
		GST_BUFFER_PTS(out_buf) = GST_CLOCK_TIME_NONE;
		GST_BUFFER_DTS(out_buf) = GST_CLOCK_TIME_NONE;
		gst_app_src_push_buffer(GST_APP_SRC(branch->appsrc), out_buf);
	}
	g_mutex_unlock(&branch->lock);
}

gst_hub_branch_t *gst_render_hub_start_pipeline_branch(const char *name, const char *source_type, const char *source_name, const char *pipeline_str)
{
	g_mutex_lock(&g_hub_mutex);

	if (!g_master_pipe) {
		struct obs_video_info ovi = {};
		if (!obs_get_video_info(&ovi)) {
			ovi.base_width = 1920;
			ovi.base_height = 1080;
			ovi.fps_num = 60;
			ovi.fps_den = 1;
		}
		uint32_t width = ovi.base_width ? ovi.base_width : 1920;
		uint32_t height = ovi.base_height ? ovi.base_height : 1080;
		uint32_t fps_num = ovi.fps_num ? ovi.fps_num : 60;
		uint32_t fps_den = ovi.fps_den ? ovi.fps_den : 1;

		g_master_pipe = gst_master_pipeline_create(NULL, "x264enc", 4000, 1, width, height, fps_num, fps_den);
		if (!g_master_pipe) {
			g_mutex_unlock(&g_hub_mutex);
			return NULL;
		}

		gst_source_type_t stype = GST_SOURCE_MAIN_OUTPUT;
		if (source_type && strcmp(source_type, "Scene") == 0) stype = GST_SOURCE_SCENE;
		else if (source_type && strcmp(source_type, "SceneItem") == 0) stype = GST_SOURCE_SCENE_ITEM;
		if (!g_source_callback) {
			g_source_callback = gst_render_callback_create(g_master_pipe, stype, source_name, NULL, width, height);
		}
	}

	gchar *pipe_desc = NULL;
	if (pipeline_str && strstr(pipeline_str, "appsrc")) {
		pipe_desc = g_strdup(pipeline_str);
	} else {
		pipe_desc = g_strdup_printf(
			"appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true "
			"caps=\"video/x-raw, format=BGRA, width=%u, height=%u, framerate=%u/%u\" ! "
			"%s",
			g_master_pipe->width, g_master_pipe->height, g_master_pipe->fps_num, g_master_pipe->fps_den,
			(pipeline_str && pipeline_str[0]) ? pipeline_str : "autovideosink sync=false");
	}

	GError *err = NULL;
	GstElement *pipe = gst_parse_launch(pipe_desc, &err);
	g_free(pipe_desc);

	if (err || !pipe) {
		if (err) g_error_free(err);
		g_mutex_unlock(&g_hub_mutex);
		return NULL;
	}

	GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipe), "appsrc_video");
	if (!appsrc) {
		gst_element_set_state(pipe, GST_STATE_NULL);
		gst_object_unref(pipe);
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
	branch->pipeline = g_master_pipe;
	branch->active = true;
	g_mutex_init(&branch->lock);

	gst_element_set_state(branch->pipe, GST_STATE_PLAYING);

	// Subscribe to master pipeline
	branch->sub = gst_master_pipeline_subscribe(g_master_pipe, rtsp_frame_cb, branch, branch->name);
	gst_master_pipeline_update_client_count(g_master_pipe, 1); // Pipeline branches always consume/push

	blog(LOG_INFO, "[obs-gstreamer-rtspserver] Direct GPU pipeline '%s' started", branch->name);

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

	if (!g_master_pipe) {
		struct obs_video_info ovi = {};
		if (!obs_get_video_info(&ovi)) {
			ovi.base_width = 1920;
			ovi.base_height = 1080;
			ovi.fps_num = 60;
			ovi.fps_den = 1;
		}
		uint32_t width = ovi.base_width ? ovi.base_width : 1920;
		uint32_t height = ovi.base_height ? ovi.base_height : 1080;
		uint32_t fps_num = ovi.fps_num ? ovi.fps_num : 60;
		uint32_t fps_den = ovi.fps_den ? ovi.fps_den : 1;

		g_master_pipe = gst_master_pipeline_create(params->master_pipeline, params->encoder_type,
		                                          params->bitrate_kbps, params->keyint_sec,
		                                          width, height, fps_num, fps_den);
		if (!g_master_pipe) {
			g_mutex_unlock(&g_hub_mutex);
			return NULL;
		}

		gst_source_type_t stype = GST_SOURCE_MAIN_OUTPUT;
		if (params->source_type && strcmp(params->source_type, "Scene") == 0) stype = GST_SOURCE_SCENE;
		else if (params->source_type && strcmp(params->source_type, "SceneItem") == 0) stype = GST_SOURCE_SCENE_ITEM;
		if (!g_source_callback) {
			g_source_callback = gst_render_callback_create(g_master_pipe, stype, params->source_name, NULL, width, height);
		}
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

	gst_hub_branch_t *branch = bzalloc(sizeof(gst_hub_branch_t));
	branch->name = bstrdup(params->name ? params->name : "Output");
	branch->service = bstrdup(requested_service);
	branch->mount_point = bstrdup(requested_mount);
	branch->pipeline = g_master_pipe;
	g_mutex_init(&branch->lock);

	GstRTSPServer *server = gst_rtsp_server_new();
	gst_rtsp_server_set_address(server, "0.0.0.0");
	gst_rtsp_server_set_service(server, branch->service);

	GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
	GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
	gst_rtsp_media_factory_set_shared(factory, TRUE);
	gst_rtsp_media_factory_set_protocols(factory, GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_UDP_MCAST | GST_RTSP_LOWER_TRANS_TCP);

	const char *default_launch_str =
		"( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true "
		"caps=\"video/x-h264, stream-format=byte-stream, alignment=au\" ! "
		"h264parse config-interval=1 ! rtph264pay name=pay0 pt=96 config-interval=1 )";

	const char *launch_str = (params->rtsp_pipeline && params->rtsp_pipeline[0]) ?
		params->rtsp_pipeline : default_launch_str;

	gst_rtsp_media_factory_set_launch(factory, launch_str);
	g_signal_connect(factory, "media-configure", G_CALLBACK(media_configure_cb), branch);
	gst_rtsp_mount_points_add_factory(mounts, branch->mount_point, factory);

	guint source_id = gst_rtsp_server_attach(server, g_rtsp_context);
	if (source_id == 0) {
		blog(LOG_ERROR, "[obs-gstreamer-rtspserver] Failed to attach RTSP server on port %s", branch->service);
		gst_object_unref(server);
		gst_object_unref(factory);
		gst_object_unref(mounts);
		bfree(branch->name);
		bfree(branch->service);
		bfree(branch->mount_point);
		g_mutex_clear(&branch->lock);
		bfree(branch);
		g_mutex_unlock(&g_hub_mutex);
		return NULL;
	}

	register_rtsp_extra(branch, server, mounts, factory, source_id);

	branch->active = true;

	// Subscribe to master pipeline for frame distribution
	branch->sub = gst_master_pipeline_subscribe(g_master_pipe, rtsp_frame_cb, branch, branch->name);

	blog(LOG_INFO, "[obs-gstreamer-rtspserver] RTSP branch started: rtsp://0.0.0.0:%s%s", branch->service, branch->mount_point);

	g_mutex_unlock(&g_hub_mutex);
	return branch;
}

static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data)
{
	UNUSED_PARAMETER(factory);
	gst_hub_branch_t *branch = (gst_hub_branch_t *)user_data;
	GstElement *element = gst_rtsp_media_get_element(media);
	GstElement *appsrc = gst_bin_get_by_name(GST_BIN(element), "appsrc_video");

	if (!appsrc) {
		blog(LOG_WARNING, "[obs-gstreamer-rtspserver] RTSP media has no appsrc_video");
		gst_object_unref(element);
		return;
	}

	gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);
	gst_app_src_set_latency(GST_APP_SRC(appsrc), 0, 0);

	g_mutex_lock(&branch->lock);
	if (branch->appsrc) {
		gst_object_unref(branch->appsrc);
	}
	branch->appsrc = appsrc;
	branch->client_count++;
	branch->waiting_for_keyframe = true;
	g_mutex_unlock(&branch->lock);

	g_signal_connect(media, "unprepared", G_CALLBACK(media_unprepared_cb), branch);

	// Notify master pipeline of client connection (+1 client)
	if (branch->pipeline) {
		gst_master_pipeline_update_client_count(branch->pipeline, 1);
	}

	gst_object_unref(element);
	blog(LOG_INFO, "[obs-gstreamer-rtspserver] RTSP client connected on %s%s", branch->service, branch->mount_point);
}

static void media_unprepared_cb(GstRTSPMedia *media, gpointer user_data)
{
	UNUSED_PARAMETER(media);
	gst_hub_branch_t *branch = (gst_hub_branch_t *)user_data;
	g_mutex_lock(&branch->lock);
	if (branch->client_count > 0) branch->client_count--;
	if (branch->client_count == 0) {
		if (branch->appsrc) {
			gst_object_unref(branch->appsrc);
			branch->appsrc = NULL;
		}
	}
	g_mutex_unlock(&branch->lock);

	// Notify master pipeline of client disconnection (-1 client)
	if (branch->pipeline) {
		gst_master_pipeline_update_client_count(branch->pipeline, -1);
	}
}

static void stop_branch_internal(gst_hub_branch_t *branch)
{
	if (!branch) return;

	gst_master_pipeline_t *hub = branch->pipeline;

	g_mutex_lock(&branch->lock);
	branch->active = false;

	if (branch->sub) {
		gst_master_pipeline_unsubscribe(hub, branch->sub);
		branch->sub = NULL;
	}

	if (branch->is_pipeline) {
		gst_master_pipeline_update_client_count(hub, -1);
		if (branch->pipe) {
			gst_element_set_state(branch->pipe, GST_STATE_NULL);
			gst_object_unref(branch->pipe);
			branch->pipe = NULL;
		}
		if (branch->appsrc) {
			gst_object_unref(branch->appsrc);
			branch->appsrc = NULL;
		}
	} else {
		unregister_rtsp_extra(branch);
	}

	g_mutex_unlock(&branch->lock);

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
	if (g_source_callback) {
		gst_render_callback_destroy(g_source_callback);
		g_source_callback = NULL;
	}
	if (g_master_pipe) {
		gst_master_pipeline_destroy(g_master_pipe);
		g_master_pipe = NULL;
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
