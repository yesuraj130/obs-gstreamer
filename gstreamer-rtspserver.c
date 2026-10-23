/*
 * obs-gstreamer: RTSP Server and Branch Management
 */

#include "gstreamer-rtspserver.h"
#include <obs-module.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <string.h>

struct rtsp_branch_extra {
	GstRTSPServer *server;
	GstRTSPMountPoints *mounts;
	GstRTSPMediaFactory *factory;
	GstRTSPMedia *media;
	guint source_id;
};

// Global singleton master pipeline and branches lock
static gst_master_pipeline_t *g_master_pipe = NULL;
static GMutex g_hub_mutex;

// Forward declarations for RTSP callbacks
static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data);
static void media_unprepared_cb(GstRTSPMedia *media, gpointer user_data);

// Store extra per-branch RTSP server handles via private lookup or association
// For simplicity in refactoring, we can attach server structures or manage them.
static struct {
	gst_subscriber_branch_t *branch;
	GstRTSPServer *server;
	GstRTSPMountPoints *mounts;
	GstRTSPMediaFactory *factory;
	guint source_id;
} g_rtsp_servers[32];
static int g_rtsp_server_count = 0;
static GMutex g_rtsp_lock;

static void register_rtsp_extra(gst_subscriber_branch_t *branch, GstRTSPServer *server, GstRTSPMountPoints *mounts, GstRTSPMediaFactory *factory, guint source_id)
	{
	g_mutex_lock(&g_rtsp_lock);
	if (g_rtsp_server_count < 32) {
		g_rtsp_servers[g_rtsp_server_count++] = (typeof(g_rtsp_servers[0])){branch, server, mounts, factory, source_id};
	}
	g_mutex_unlock(&g_rtsp_lock);
}

static void unregister_rtsp_extra(gst_subscriber_branch_t *branch)
{
	g_mutex_lock(&g_rtsp_lock);
	for (int i = 0; i < g_rtsp_server_count; i++) {
		if (g_rtsp_servers[i].branch == branch) {
			if (g_rtsp_servers[i].source_id > 0) {
				if (g_master_pipe && g_master_pipe->rtsp_context) {
					GSource *source = g_main_context_find_source_by_id(g_master_pipe->rtsp_context, g_rtsp_servers[i].source_id);
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
			// Shift remaining
			for (int j = i; j < g_rtsp_server_count - 1; j++) {
				g_rtsp_servers[j] = g_rtsp_servers[j + 1];
			}
			g_rtsp_server_count--;
			break;
		}
	}
	g_mutex_unlock(&g_rtsp_lock);
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

		g_master_pipe = gst_master_pipeline_get_or_create(source_type, source_name, NULL, "x264enc", 4000, 1, width, height, fps_num, fps_den);
		if (!g_master_pipe) {
			g_mutex_unlock(&g_hub_mutex);
			return NULL;
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
	blog(LOG_INFO, "[obs-gstreamer-rtspserver] Launching direct GPU pipeline: %s", pipe_desc);
	g_free(pipe_desc);

	if (err || !pipe) {
		blog(LOG_ERROR, "[obs-gstreamer-rtspserver] Failed to create direct GPU pipeline: %s",
			err ? err->message : "unknown error");
		if (err) g_error_free(err);
		g_mutex_unlock(&g_hub_mutex);
		return NULL;
	}

	GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipe), "appsrc_video");
	if (!appsrc) {
		blog(LOG_ERROR, "[obs-gstreamer-rtspserver] 'appsrc_video' not found in direct GPU pipeline");
		gst_element_set_state(pipe, GST_STATE_NULL);
		gst_object_unref(pipe);
		g_mutex_unlock(&g_hub_mutex);
		return NULL;
	}

	gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);
	gst_app_src_set_latency(GST_APP_SRC(appsrc), 0, 0);

	gst_subscriber_branch_t *branch = bzalloc(sizeof(gst_subscriber_branch_t));
	branch->name = bstrdup(name ? name : "Pipeline");
	branch->is_pipeline = true;
	branch->pipe = pipe;
	branch->appsrc = appsrc;
	branch->pipeline = g_master_pipe;
	branch->active = true;
	g_mutex_init(&branch->lock);

	gst_element_set_state(branch->pipe, GST_STATE_PLAYING);

	g_mutex_lock(&g_master_pipe->branch_lock);
	branch->next = g_master_pipe->branches;
	g_master_pipe->branches = branch;
	g_master_pipe->active_branch_count++;
	g_atomic_int_inc(&g_master_pipe->pipeline_branch_count);
	g_mutex_unlock(&g_master_pipe->branch_lock);

	blog(LOG_INFO, "[obs-gstreamer-rtspserver] Direct GPU pipeline '%s' started (always-pushing)", branch->name);

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

		g_master_pipe = gst_master_pipeline_get_or_create(params->source_type, params->source_name,
		                                                  params->master_pipeline, params->encoder_type,
		                                                  params->bitrate_kbps, params->keyint_sec,
		                                                  width, height, fps_num, fps_den);
		if (!g_master_pipe) {
			g_mutex_unlock(&g_hub_mutex);
			return NULL;
		}
	}

	const char *requested_service = "8554";
	if (params->rtsp_service) {
		const char *s = params->rtsp_service;
		while (*s == ' ' || *s == '\t') s++;
		if (*s != '\0') requested_service = s;
	}

	if (g_master_pipe) {
		g_mutex_lock(&g_master_pipe->branch_lock);
		gst_subscriber_branch_t *existing = g_master_pipe->branches;
		while (existing) {
			if (existing->active && existing->service && strcmp(existing->service, requested_service) == 0) {
				blog(LOG_ERROR, "[obs-gstreamer-rtspserver] Port %s already in use by branch '%s'", requested_service, existing->name);
				g_mutex_unlock(&g_master_pipe->branch_lock);
				g_mutex_unlock(&g_hub_mutex);
				return NULL;
			}
			existing = existing->next;
		}
		g_mutex_unlock(&g_master_pipe->branch_lock);
	}

	const char *requested_mount = "/live";
	if (params->rtsp_mount) {
		const char *m = params->rtsp_mount;
		while (*m == ' ' || *m == '\t') m++;
		if (*m != '\0') requested_mount = m;
	}

	gst_subscriber_branch_t *branch = bzalloc(sizeof(gst_subscriber_branch_t));
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

	guint source_id = gst_rtsp_server_attach(server, g_master_pipe->rtsp_context);
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

	g_mutex_lock(&g_master_pipe->branch_lock);
	branch->next = g_master_pipe->branches;
	g_master_pipe->branches = branch;
	g_master_pipe->active_branch_count++;
	g_mutex_unlock(&g_master_pipe->branch_lock);

	blog(LOG_INFO, "[obs-gstreamer-rtspserver] RTSP branch started: rtsp://0.0.0.0:%s%s", branch->service, branch->mount_point);

	g_mutex_unlock(&g_hub_mutex);
	return branch;
}

static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data)
{
	(void)factory;
	gst_subscriber_branch_t *branch = (gst_subscriber_branch_t *)user_data;
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

	struct rtsp_branch_extra *extra = NULL; // handled via tracking or cleanup
	// We can connect unprepared callback to media
	g_signal_connect(media, "unprepared", G_CALLBACK(media_unprepared_cb), branch);

	if (branch->pipeline) {
		int prev = g_atomic_int_add(&branch->pipeline->connected_client_count, 1);
		if (prev == 0) {
			blog(LOG_INFO, "[obs-gstreamer-rtspserver] First client connected! Activating pipeline.");
			if (branch->pipeline->pipe) {
				gst_element_set_state(branch->pipeline->pipe, GST_STATE_PLAYING);
			}
		}
		if (branch->pipeline->appsink) {
			GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
			gst_element_send_event(branch->pipeline->appsink, event);
		}
	}

	gst_object_unref(element);
	blog(LOG_INFO, "[obs-gstreamer-rtspserver] RTSP client connected on %s%s", branch->service, branch->mount_point);
}

static void media_unprepared_cb(GstRTSPMedia *media, gpointer user_data)
{
	(void)media;
	gst_subscriber_branch_t *branch = (gst_subscriber_branch_t *)user_data;
	g_mutex_lock(&branch->lock);
	if (branch->client_count > 0) branch->client_count--;
	if (branch->client_count == 0) {
		if (branch->appsrc) {
			gst_object_unref(branch->appsrc);
			branch->appsrc = NULL;
		}
	}
	g_mutex_unlock(&branch->lock);

	if (branch->pipeline) {
		int remaining = g_atomic_int_add(&branch->pipeline->connected_client_count, -1) - 1;
		if (remaining <= 0) {
			g_atomic_int_set(&branch->pipeline->connected_client_count, 0);
			blog(LOG_INFO, "[obs-gstreamer-rtspserver] Last client disconnected. Pausing encoder.");
			branch->pipeline->first_frame_primed = false;
			if (branch->pipeline->pipe) {
				gst_element_set_state(branch->pipeline->pipe, GST_STATE_PAUSED);
			}
		}
	}
}

static void stop_branch_internal(gst_hub_branch_t *branch)
{
	if (!branch) return;

	gst_master_pipeline_t *hub = branch->pipeline;
	if (hub) {
		g_mutex_lock(&hub->branch_lock);
		gst_subscriber_branch_t **curr = &hub->branches;
		while (*curr) {
			if (*curr == branch) {
				*curr = branch->next;
				hub->active_branch_count--;
				break;
			}
			curr = &(*curr)->next;
		}
		g_mutex_unlock(&hub->branch_lock);
	}

	g_mutex_lock(&branch->lock);
	branch->active = false;

	if (branch->is_pipeline) {
		if (hub) {
			g_atomic_int_dec_and_test(&hub->pipeline_branch_count);
		}
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

	if (hub && hub->active_branch_count == 0) {
		gst_master_pipeline_destroy(hub);
		g_master_pipe = NULL;
	}
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
	if (g_master_pipe) {
		while (g_master_pipe->branches) {
			stop_branch_internal(g_master_pipe->branches);
		}
	}
	g_mutex_unlock(&g_hub_mutex);
}
