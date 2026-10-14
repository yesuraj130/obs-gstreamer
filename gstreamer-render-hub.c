/*
 * obs-gstreamer: Direct GPU Render Hub for Ultra-Low Latency Multi-Port RTSP
 *
 * Implements direct GPU off-screen view rendering, single hardware encode pass,
 * and direct RTSP multi-port distribution with zero OBS output queue latency.
 */

#include "gstreamer-render-hub.h"

#include <obs-module.h>
#include <util/darray.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <stdlib.h>
#include <string.h>

struct gst_master_hub;

struct gst_hub_branch {
	char *name;
	char *service;
	char *mount_point;
	guint source_id;
	GstRTSPServer *server;
	GstRTSPMountPoints *mounts;
	GstRTSPMediaFactory *factory;
	GstElement *appsrc;
	GstRTSPMedia *media;
	GMutex lock;
	bool active;
	int client_count;
	struct gst_master_hub *hub;
	struct gst_hub_branch *next;
};

struct gst_master_hub {
	char *source_type;
	char *source_name;
	obs_view_t *view;
	obs_source_t *source;

	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;

	// Double-buffered GPU ping-pong render targets
	gs_texrender_t *texrender[2];
	gs_stagesurf_t *stagesurf[2];
	uint32_t pingpong_idx;
	bool first_frame_primed;

	// Master GStreamer Hardware Encoder
	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;
	GThread *worker_thread;
	bool worker_running;

	// Dedicated GLib RTSP Server Event Loop Thread
	GMainContext *rtsp_context;
	GMainLoop *rtsp_loop;
	GThread *rtsp_thread;

	gint connected_client_count; // Number of active remote RTSP clients across all branches
	GMutex branch_lock;
	struct gst_hub_branch *branches;
	size_t active_branch_count;
	bool render_hook_active;
};

// Global singleton hub for the active video canvas
static struct gst_master_hub *g_hub = NULL;
static GMutex g_hub_mutex;

// Forward declarations
static void hub_render_callback(void *param, uint32_t cx, uint32_t cy);
static gpointer hub_worker_loop(gpointer arg);
static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data);
static void media_unprepared_cb(GstRTSPMedia *media, gpointer user_data);

/* Helper to select and initialize the hardware encoder pipeline */
static GstElement *create_master_encoder_pipeline(struct gst_master_hub *hub,
                                                  const char *custom_master_pipe,
                                                  const char *encoder_type,
                                                  int bitrate_kbps,
                                                  int keyint_sec)
{
	gchar *pipe_desc = NULL;

	if (custom_master_pipe && custom_master_pipe[0]) {
		// User provided custom master pipeline
		if (strstr(custom_master_pipe, "appsrc") && strstr(custom_master_pipe, "appsink")) {
			// Safely count %u specifiers and check for invalid/unexpected specifiers
			int u_count = 0;
			bool has_invalid = false;
			const char *p = custom_master_pipe;
			while ((p = strchr(p, '%')) != NULL) {
				if (*(p + 1) == 'u') {
					u_count++;
					p += 2;
				} else if (*(p + 1) == '%') {
					p += 2;
				} else {
					has_invalid = true;
					break;
				}
			}
			if (!has_invalid && u_count == 4) {
				pipe_desc = g_strdup_printf(custom_master_pipe, hub->width, hub->height, hub->fps_num, hub->fps_den);
			} else if (!has_invalid && u_count == 2) {
				pipe_desc = g_strdup_printf(custom_master_pipe, hub->width, hub->height);
			} else {
				pipe_desc = g_strdup(custom_master_pipe);
			}
		} else {
			// User provided the encoder middle-chain (e.g. "videoconvert ! nvh264enc ... ! h264parse")
			pipe_desc = g_strdup_printf(
				"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true "
				"caps=\"video/x-raw, format=BGRA, width=%u, height=%u, framerate=%u/%u\" ! "
				"%s ! "
				"appsink name=hub_appsink sync=false drop=true max-buffers=1",
				hub->width, hub->height, hub->fps_num, hub->fps_den,
				custom_master_pipe);
		}
	} else {
		int bitrate = bitrate_kbps > 0 ? bitrate_kbps : 4000;
		int keyint = keyint_sec > 0 ? keyint_sec : 1;
		int gop_size = keyint * (int)hub->fps_num / (int)hub->fps_den;
		if (gop_size < 15) gop_size = 30;

		gchar *encoder_elem = NULL;
		if (encoder_type && g_strcmp0(encoder_type, "nvh264enc") == 0) {
			encoder_elem = g_strdup_printf(
				"videoconvert ! nvh264enc tune=zerolatency bframes=0 rc-lookahead=0 gop-size=%d bitrate=%d",
				gop_size, bitrate);
		} else if (encoder_type && g_strcmp0(encoder_type, "qsvh264enc") == 0) {
			encoder_elem = g_strdup_printf(
				"videoconvert ! qsvh264enc gop-size=%d bitrate=%d",
				gop_size, bitrate);
		} else if (encoder_type && g_strcmp0(encoder_type, "vaapih264enc") == 0) {
			encoder_elem = g_strdup_printf(
				"videoconvert ! vaapih264enc keyframe-period=%d bitrate=%d",
				gop_size, bitrate);
		} else {
			// Ultra-fast zero-latency x264 fallback (guaranteed to be present on any host)
			encoder_elem = g_strdup_printf(
				"videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=%d bitrate=%d",
				gop_size, bitrate);
		}

		pipe_desc = g_strdup_printf(
			"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true "
			"caps=\"video/x-raw, format=BGRA, width=%u, height=%u, framerate=%u/%u\" ! "
			"%s ! h264parse config-interval=-1 ! "
			"appsink name=hub_appsink sync=false drop=true max-buffers=1",
			hub->width, hub->height, hub->fps_num, hub->fps_den,
			encoder_elem);

		g_free(encoder_elem);
	}

	GError *error = NULL;
	GstElement *pipe = gst_parse_launch(pipe_desc, &error);
	blog(LOG_INFO, "[obs-gstreamer-hub] Launching master pipeline: %s", pipe_desc);
	g_free(pipe_desc);

	if (error) {
		blog(LOG_ERROR, "[obs-gstreamer-hub] Failed to create master encoder pipeline: %s", error->message);
		g_clear_error(&error);
		return NULL;
	}

	return pipe;
}

/* Master GPU Frame Capture Callback */
static void hub_render_callback(void *param, uint32_t cx, uint32_t cy)
{
	(void)cx; (void)cy;
	struct gst_master_hub *hub = (struct gst_master_hub *)param;
	if (!hub || !hub->view) return;

	// On-Demand Encoding: If no client is connected to any branch, do NOT render or encode!
	if (g_atomic_int_get(&hub->connected_client_count) <= 0) {
		hub->first_frame_primed = false;
		return;
	}

	uint32_t cur = hub->pingpong_idx;
	uint32_t prev = 1 - cur;

	// 1. Render the offscreen view into the current pingpong texture
	if (hub->texrender[cur]) {
		gs_texrender_begin(hub->texrender[cur], hub->width, hub->height);
		gs_enable_blending(false);
		gs_matrix_push();
		gs_ortho(0.0f, (float)hub->width, 0.0f, (float)hub->height, -100.0f, 100.0f);
		obs_view_render(hub->view);
		gs_matrix_pop();
		gs_texrender_end(hub->texrender[cur]);

		// Copy texture into staging surface for zero-stall DMA
		gs_texture_t *tex = gs_texrender_get_texture(hub->texrender[cur]);
		if (tex && hub->stagesurf[cur]) {
			gs_stage_texture(hub->stagesurf[cur], tex);
		}
	}

	// 2. Map the PREVIOUS frame's staging surface (GPU finished DMA, zero CPU wait)
	if (hub->first_frame_primed && hub->stagesurf[prev] && hub->appsrc) {
		uint8_t *data = NULL;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(hub->stagesurf[prev], &data, &linesize)) {
			size_t frame_bytes = (size_t)hub->width * (size_t)hub->height * 4;
			GstBuffer *buf = gst_buffer_new_allocate(NULL, frame_bytes, NULL);
			if (buf) {
				GstMapInfo map;
				if (gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
					// Copy row-by-row if linesize differs from width*4
					if (linesize == hub->width * 4) {
						memcpy(map.data, data, frame_bytes);
					} else {
						for (uint32_t y = 0; y < hub->height; y++) {
							memcpy(map.data + y * hub->width * 4, data + y * linesize, hub->width * 4);
						}
					}
					gst_buffer_unmap(buf, &map);

					GST_BUFFER_DURATION(buf) = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
					gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), buf);
				} else {
					gst_buffer_unref(buf);
				}
			}
			gs_stagesurface_unmap(hub->stagesurf[prev]);
		}
	} else {
		hub->first_frame_primed = true;
	}

	// Swap pingpong index
	hub->pingpong_idx = prev;
}

/* Worker loop pulling compressed H.264 packets from appsink and pushing to RTSP branches */
static gpointer hub_worker_loop(gpointer arg)
{
	struct gst_master_hub *hub = (struct gst_master_hub *)arg;

	while (hub->worker_running) {
		GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(hub->appsink), 20 * GST_MSECOND);
		if (!sample) continue;

		GstBuffer *buf = gst_sample_get_buffer(sample);
		if (buf) {
			g_mutex_lock(&hub->branch_lock);
			struct gst_hub_branch *curr = hub->branches;
			while (curr) {
				g_mutex_lock(&curr->lock);
				if (curr->active && curr->appsrc) {
					// Push a copy/ref to this RTSP client's pipeline
					gst_app_src_push_buffer(GST_APP_SRC(curr->appsrc), gst_buffer_ref(buf));
				}
				g_mutex_unlock(&curr->lock);
				curr = curr->next;
			}
			g_mutex_unlock(&hub->branch_lock);
		}

		gst_sample_unref(sample);
	}

	return NULL;
}

/* RTSP client connects and prepares media */
static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data)
{
	(void)factory;
	struct gst_hub_branch *branch = (struct gst_hub_branch *)user_data;
	GstElement *element = gst_rtsp_media_get_element(media);
	GstElement *appsrc = gst_bin_get_by_name(GST_BIN(element), "appsrc_video");

	if (!appsrc) {
		blog(LOG_WARNING, "[obs-gstreamer-hub] RTSP media has no appsrc_video");
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

	if (branch->media && branch->media != media) {
		g_signal_handlers_disconnect_by_data(branch->media, branch);
		gst_object_unref(branch->media);
		branch->media = NULL;
	}
	if (!branch->media) {
		branch->media = GST_RTSP_MEDIA(gst_object_ref(media));
		g_signal_connect(media, "unprepared", G_CALLBACK(media_unprepared_cb), branch);
	}
	g_mutex_unlock(&branch->lock);

	if (branch->hub) {
		int prev = g_atomic_int_add(&branch->hub->connected_client_count, 1);
		if (prev == 0) {
			blog(LOG_INFO, "[obs-gstreamer-hub] First client connected! Activating GPU render & encoder pipeline.");
			if (branch->hub->pipe) {
				gst_element_set_state(branch->hub->pipe, GST_STATE_PLAYING);
			}
		}
	}

	gst_object_unref(element);
	blog(LOG_INFO, "[obs-gstreamer-hub] RTSP client connected on port %s%s (Active clients: %d)",
		branch->service, branch->mount_point,
		branch->hub ? g_atomic_int_get(&branch->hub->connected_client_count) : 1);
}

static void media_unprepared_cb(GstRTSPMedia *media, gpointer user_data)
{
	(void)media;
	struct gst_hub_branch *branch = (struct gst_hub_branch *)user_data;
	g_mutex_lock(&branch->lock);
	if (branch->client_count > 0) {
		branch->client_count--;
	}
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

	if (branch->hub) {
		int remaining = g_atomic_int_add(&branch->hub->connected_client_count, -1) - 1;
		if (remaining <= 0) {
			g_atomic_int_set(&branch->hub->connected_client_count, 0);
			blog(LOG_INFO, "[obs-gstreamer-hub] Last client disconnected. Pausing GPU render & encoder (0%% GPU/CPU usage).");
			branch->hub->first_frame_primed = false;
			if (branch->hub->pipe) {
				gst_element_set_state(branch->hub->pipe, GST_STATE_PAUSED);
			}
		} else {
			blog(LOG_INFO, "[obs-gstreamer-hub] RTSP client disconnected from port %s%s (Remaining clients: %d)",
				branch->service, branch->mount_point, remaining);
		}
	}
}

/* Initialize master hub for a given source */
static bool hub_init(struct gst_master_hub *hub, const gst_hub_output_params_t *params)
{
	hub->source_type = bstrdup(params->source_type ? params->source_type : "Program Output");
	hub->source_name = bstrdup(params->source_name ? params->source_name : "");

	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		ovi.base_width = 1920;
		ovi.base_height = 1080;
		ovi.fps_num = 60;
		ovi.fps_den = 1;
	}
	// Canvas size matching OBS program output canvas (base resolution)
	hub->width = ovi.base_width ? ovi.base_width : (ovi.output_width ? ovi.output_width : 1920);
	hub->height = ovi.base_height ? ovi.base_height : (ovi.output_height ? ovi.output_height : 1080);
	hub->fps_num = ovi.fps_num ? ovi.fps_num : 60;
	hub->fps_den = ovi.fps_den ? ovi.fps_den : 1;

	// Resolve the OBS source
	if (strcmp(hub->source_type, "Scene") == 0) {
		obs_scene_t *sc = obs_get_scene_by_name(hub->source_name);
		hub->source = sc ? obs_scene_get_source(sc) : NULL;
		if (hub->source) obs_source_get_ref(hub->source);
		if (sc) obs_scene_release(sc);
	} else if (strcmp(hub->source_type, "Source") == 0) {
		hub->source = obs_get_source_by_name(hub->source_name);
	} else {
		// Program Output: grab channel 0
		hub->source = obs_get_output_source(0);
	}

	if (!hub->source) {
		blog(LOG_ERROR, "[obs-gstreamer-hub] Failed to resolve source '%s' of type '%s'",
			hub->source_name, hub->source_type);
		return false;
	}

	// Create dedicated off-screen view
	hub->view = obs_view_create();
	obs_view_set_source(hub->view, 0, hub->source);

	// Create double-buffered GPU render targets
	obs_enter_graphics();
	hub->texrender[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	hub->texrender[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	hub->stagesurf[0] = gs_stagesurface_create(hub->width, hub->height, GS_RGBA);
	hub->stagesurf[1] = gs_stagesurface_create(hub->width, hub->height, GS_RGBA);
	obs_leave_graphics();

	if (!hub->texrender[0] || !hub->texrender[1] || !hub->stagesurf[0] || !hub->stagesurf[1]) {
		blog(LOG_ERROR, "[obs-gstreamer-hub] Failed to allocate GPU texrender (%p, %p) or stagesurf (%p, %p)",
			(void *)hub->texrender[0], (void *)hub->texrender[1],
			(void *)hub->stagesurf[0], (void *)hub->stagesurf[1]);
		return false;
	}

	hub->pingpong_idx = 0;
	hub->first_frame_primed = false;

	// Launch Master GStreamer Hardware Encoder
	hub->pipe = create_master_encoder_pipeline(hub, params->master_pipeline, params->encoder_type, params->bitrate_kbps, params->keyint_sec);
	if (!hub->pipe) {
		blog(LOG_ERROR, "[obs-gstreamer-hub] Failed to create master encoder pipeline");
		return false;
	}

	hub->appsrc = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsrc");
	hub->appsink = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsink");
	if (!hub->appsrc || !hub->appsink) {
		blog(LOG_ERROR, "[obs-gstreamer-hub] Master pipeline missing hub_appsrc (%p) or hub_appsink (%p)",
			(void *)hub->appsrc, (void *)hub->appsink);
		return false;
	}

	hub->connected_client_count = 0;
	gst_element_set_state(hub->pipe, GST_STATE_PAUSED);

	// Start worker thread
	hub->worker_running = true;
	hub->worker_thread = g_thread_new("gst_hub_worker", hub_worker_loop, hub);

	// Start dedicated GLib RTSP main loop thread
	hub->rtsp_context = g_main_context_new();
	hub->rtsp_loop = g_main_loop_new(hub->rtsp_context, FALSE);
	hub->rtsp_thread = g_thread_new("gst_hub_rtsp_loop", (GThreadFunc)g_main_loop_run, hub->rtsp_loop);

	// Hook into OBS GPU render thread
	obs_add_main_render_callback(hub_render_callback, hub);
	hub->render_hook_active = true;

	blog(LOG_INFO, "[obs-gstreamer-hub] Master GPU Render Hub started (%ux%u @ %u/%u fps)",
		hub->width, hub->height, hub->fps_num, hub->fps_den);

	return true;
}

/* Teardown master hub */
static void hub_destroy(struct gst_master_hub *hub)
{
	if (!hub) return;

	if (hub->render_hook_active) {
		obs_remove_main_render_callback(hub_render_callback, hub);
		hub->render_hook_active = false;
	}

	hub->worker_running = false;
	if (hub->worker_thread) {
		g_thread_join(hub->worker_thread);
		hub->worker_thread = NULL;
	}

	if (hub->rtsp_loop) {
		g_main_loop_quit(hub->rtsp_loop);
	}
	if (hub->rtsp_thread) {
		g_thread_join(hub->rtsp_thread);
		hub->rtsp_thread = NULL;
	}
	if (hub->rtsp_loop) {
		g_main_loop_unref(hub->rtsp_loop);
		hub->rtsp_loop = NULL;
	}
	if (hub->rtsp_context) {
		g_main_context_unref(hub->rtsp_context);
		hub->rtsp_context = NULL;
	}

	if (hub->pipe) {
		gst_element_set_state(hub->pipe, GST_STATE_NULL);
		if (hub->appsrc) gst_object_unref(hub->appsrc);
		if (hub->appsink) gst_object_unref(hub->appsink);
		gst_object_unref(hub->pipe);
		hub->pipe = NULL;
	}

	obs_enter_graphics();
	for (int i = 0; i < 2; i++) {
		if (hub->texrender[i]) {
			gs_texrender_destroy(hub->texrender[i]);
			hub->texrender[i] = NULL;
		}
		if (hub->stagesurf[i]) {
			gs_stagesurface_destroy(hub->stagesurf[i]);
			hub->stagesurf[i] = NULL;
		}
	}
	obs_leave_graphics();

	if (hub->view) {
		obs_view_destroy(hub->view);
		hub->view = NULL;
	}
	if (hub->source) {
		obs_source_release(hub->source);
		hub->source = NULL;
	}

	bfree(hub->source_type);
	bfree(hub->source_name);
	g_mutex_clear(&hub->branch_lock);
	bfree(hub);

	blog(LOG_INFO, "[obs-gstreamer-hub] Master GPU Render Hub destroyed cleanly");
}

/* Public API: Start a direct RTSP output branch */
gst_hub_branch_t *gst_render_hub_start_branch(const gst_hub_output_params_t *params)
{
	if (!params) return NULL;

	g_mutex_lock(&g_hub_mutex);

	// Ensure master hub is running
	if (!g_hub) {
		g_hub = bzalloc(sizeof(struct gst_master_hub));
		g_mutex_init(&g_hub->branch_lock);
		if (!hub_init(g_hub, params)) {
			hub_destroy(g_hub);
			g_hub = NULL;
			g_mutex_unlock(&g_hub_mutex);
			return NULL;
		}
	}

	const char *requested_service = (params->rtsp_service && params->rtsp_service[0]) ? params->rtsp_service : "8554";

	// Port Conflict Detection: Check if any active branch is already using this RTSP port
	if (g_hub) {
		g_mutex_lock(&g_hub->branch_lock);
		struct gst_hub_branch *existing = g_hub->branches;
		while (existing) {
			if (existing->active && existing->service && strcmp(existing->service, requested_service) == 0) {
				blog(LOG_ERROR, "[obs-gstreamer-hub] Cannot start RTSP branch '%s': Port %s is already in use by active branch '%s'!",
					params->name ? params->name : "Output", requested_service, existing->name ? existing->name : "Unknown");
				g_mutex_unlock(&g_hub->branch_lock);
				g_mutex_unlock(&g_hub_mutex);
				return NULL;
			}
			existing = existing->next;
		}
		g_mutex_unlock(&g_hub->branch_lock);
	}

	// Create new RTSP branch
	struct gst_hub_branch *branch = bzalloc(sizeof(struct gst_hub_branch));
	branch->name = bstrdup(params->name ? params->name : "Output");
	branch->service = bstrdup(requested_service);
	branch->mount_point = bstrdup(params->rtsp_mount && params->rtsp_mount[0] ? params->rtsp_mount : "/live");
	branch->hub = g_hub;
	g_mutex_init(&branch->lock);

	// Setup GStreamer RTSP Server for this branch
	branch->server = gst_rtsp_server_new();
	gst_rtsp_server_set_address(branch->server, "0.0.0.0");
	gst_rtsp_server_set_service(branch->server, branch->service);

	branch->mounts = gst_rtsp_server_get_mount_points(branch->server);
	branch->factory = gst_rtsp_media_factory_new();
	gst_rtsp_media_factory_set_shared(branch->factory, TRUE);
	gst_rtsp_media_factory_set_protocols(branch->factory, GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_UDP_MCAST | GST_RTSP_LOWER_TRANS_TCP);

	// The branch pipeline packetizes already-compressed H.264 into RTP (zero encode overhead)
	const char *default_launch_str =
		"( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true "
		"caps=\"video/x-h264, stream-format=byte-stream, alignment=au\" ! "
		"h264parse config-interval=-1 ! rtph264pay name=pay0 pt=96 config-interval=1 )";

	const char *launch_str = (params->rtsp_pipeline && params->rtsp_pipeline[0]) ?
		params->rtsp_pipeline : default_launch_str;

	gst_rtsp_media_factory_set_launch(branch->factory, launch_str);
	g_signal_connect(branch->factory, "media-configure", G_CALLBACK(media_configure_cb), branch);
	gst_rtsp_mount_points_add_factory(branch->mounts, branch->mount_point, branch->factory);

	branch->source_id = gst_rtsp_server_attach(branch->server, g_hub->rtsp_context);
	if (branch->source_id == 0) {
		blog(LOG_ERROR, "[obs-gstreamer-hub] Failed to attach RTSP server on port %s: address already in use or bind failed", branch->service);
		gst_object_unref(branch->server);
		if (branch->factory) {
			gst_object_unref(branch->factory);
		}
		if (branch->mounts) {
			gst_object_unref(branch->mounts);
		}
		bfree(branch->name);
		bfree(branch->service);
		bfree(branch->mount_point);
		g_mutex_clear(&branch->lock);
		bfree(branch);
		g_mutex_unlock(&g_hub_mutex);
		return NULL;
	}

	branch->active = true;

	// Link branch into hub
	g_mutex_lock(&g_hub->branch_lock);
	branch->next = g_hub->branches;
	g_hub->branches = branch;
	g_hub->active_branch_count++;
	g_mutex_unlock(&g_hub->branch_lock);

	blog(LOG_INFO, "[obs-gstreamer-hub] Branch started: rtsp://0.0.0.0:%s%s", branch->service, branch->mount_point);

	g_mutex_unlock(&g_hub_mutex);
	return branch;
}

/* Internal helper to stop branch assuming g_hub_mutex is already locked */
static void gst_render_hub_stop_branch_internal(gst_hub_branch_t *branch)
{
	if (!branch) return;

	struct gst_master_hub *hub = branch->hub;
	if (hub) {
		g_mutex_lock(&hub->branch_lock);
		struct gst_hub_branch **curr = &hub->branches;
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
	int branch_clients = branch->client_count;
	branch->client_count = 0;
	if (branch->media) {
		g_signal_handlers_disconnect_by_data(branch->media, branch);
		gst_object_unref(branch->media);
		branch->media = NULL;
	}
	if (branch->source_id > 0) {
		if (hub && hub->rtsp_context) {
			GSource *source = g_main_context_find_source_by_id(hub->rtsp_context, branch->source_id);
			if (source)
				g_source_destroy(source);
		} else {
			g_source_remove(branch->source_id);
		}
		branch->source_id = 0;
	}
	if (branch->mounts && branch->mount_point) {
		gst_rtsp_mount_points_remove_factory(branch->mounts, branch->mount_point);
	}
	if (branch->factory) {
		g_signal_handlers_disconnect_by_data(branch->factory, branch);
		gst_object_unref(branch->factory);
		branch->factory = NULL;
	}
	if (branch->appsrc) {
		gst_object_unref(branch->appsrc);
		branch->appsrc = NULL;
	}
	if (branch->mounts) {
		gst_object_unref(branch->mounts);
		branch->mounts = NULL;
	}
	if (branch->server) {
		g_signal_handlers_disconnect_by_data(branch->server, branch);
		gst_object_unref(branch->server);
		branch->server = NULL;
	}
	g_mutex_unlock(&branch->lock);

	if (hub && branch_clients > 0) {
		int remaining = g_atomic_int_add(&hub->connected_client_count, -branch_clients) - branch_clients;
		if (remaining <= 0) {
			g_atomic_int_set(&hub->connected_client_count, 0);
			hub->first_frame_primed = false;
			if (hub->pipe) {
				gst_element_set_state(hub->pipe, GST_STATE_PAUSED);
			}
		}
	}

	bfree(branch->name);
	bfree(branch->service);
	bfree(branch->mount_point);
	g_mutex_clear(&branch->lock);
	bfree(branch);

	// If no more branches remain on the hub, cleanly destroy the master hub
	if (hub && hub->active_branch_count == 0) {
		hub_destroy(hub);
		g_hub = NULL;
	}
}

/* Public API: Stop an active RTSP output branch */
void gst_render_hub_stop_branch(gst_hub_branch_t *branch)
{
	if (!branch) return;

	g_mutex_lock(&g_hub_mutex);
	gst_render_hub_stop_branch_internal(branch);
	g_mutex_unlock(&g_hub_mutex);
}

/* Public API: Check if branch is active */
bool gst_render_hub_is_branch_active(const gst_hub_branch_t *branch)
{
	return branch != NULL && branch->active;
}

/* Public API: Global shutdown */
void gst_render_hub_shutdown(void)
{
	g_mutex_lock(&g_hub_mutex);
	if (g_hub) {
		while (g_hub->branches) {
			gst_render_hub_stop_branch_internal(g_hub->branches);
		}
	}
	g_mutex_unlock(&g_hub_mutex);
}
