/*
 * obs-gstreamer: Master Hardware Encoder Pipeline & Subscriber Hub
 */

#include "gstreamer-master-pipeline.h"
#include <obs-module.h>
#include <util/darray.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <stdlib.h>
#include <string.h>

extern void hub_render_callback(void *param, uint32_t cx, uint32_t cy);

static GstElement *create_master_encoder_pipeline(gst_master_pipeline_t *hub, const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec)
{
	gchar *pipe_desc = NULL;

	if (custom_master_pipe && custom_master_pipe[0])
	{
		if (strstr(custom_master_pipe, "appsrc") && strstr(custom_master_pipe, "appsink"))
		{
			int u_count = 0;
			bool has_invalid = false;
			const char *p = custom_master_pipe;
			while ((p = strchr(p, '%')) != NULL)
			{
				if (*(p + 1) == 'u')
				{
					u_count++;
					p += 2;
				}
				else if (*(p + 1) == '%')
				{
					p += 2;
				}
				else
				{
					has_invalid = true;
					break;
				}
			}
			if (!has_invalid && u_count == 4) pipe_desc = g_strdup_printf(custom_master_pipe, hub->width, hub->height, hub->fps_num, hub->fps_den);
			else if (!has_invalid && u_count == 2) pipe_desc = g_strdup_printf(custom_master_pipe, hub->width, hub->height);
			else pipe_desc = g_strdup(custom_master_pipe);	
		}
		else
		{
			pipe_desc = g_strdup_printf(
				"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true "
				"caps=\"video/x-raw, format=BGRA, width=%u, height=%u, framerate=%u/%u\" ! "
				"%s ! "
				"appsink name=hub_appsink sync=false drop=true max-buffers=1",
				hub->width, hub->height, hub->fps_num, hub->fps_den,
				custom_master_pipe);
		}
	}
	else
	{
		int bitrate = bitrate_kbps > 0 ? bitrate_kbps : 4000;
		int keyint = keyint_sec > 0 ? keyint_sec : 1;
		int gop_size = keyint * (int)hub->fps_num / (int)hub->fps_den;
		if (gop_size < 15) gop_size = 30;

		gchar *encoder_elem = NULL;
		if (encoder_type && g_strcmp0(encoder_type, "nvh264enc") == 0) 
			encoder_elem = g_strdup_printf("videoconvert ! nvh264enc tune=zerolatency bframes=0 rc-lookahead=0 gop-size=%d bitrate=%d", gop_size, bitrate);
		else if (encoder_type && g_strcmp0(encoder_type, "qsvh264enc") == 0) 
			encoder_elem = g_strdup_printf("videoconvert ! qsvh264enc gop-size=%d bitrate=%d", gop_size, bitrate);
		else if (encoder_type && g_strcmp0(encoder_type, "vaapih264enc") == 0) 
			encoder_elem = g_strdup_printf("videoconvert ! vaapih264enc keyframe-period=%d bitrate=%d", gop_size, bitrate);
		 else 
			encoder_elem = g_strdup_printf("videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=%d bitrate=%d", gop_size, bitrate);
		

		pipe_desc = g_strdup_printf(
			"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true "
			"caps=\"video/x-raw, format=BGRA, width=%u, height=%u, framerate=%u/%u\" ! "
			"%s ! h264parse config-interval=1 ! "
			"appsink name=hub_appsink sync=false drop=true max-buffers=1",
			hub->width, hub->height, hub->fps_num, hub->fps_den,
			encoder_elem);

		g_free(encoder_elem);
	}

	blog(LOG_INFO, "[obs-gstreamer-master] Launching master pipeline: %s", pipe_desc);
	GError *error = NULL;
	GstElement *pipe = gst_parse_launch(pipe_desc, &error);
	g_free(pipe_desc);

	if (error)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to create master encoder pipeline: %s", error->message);
		g_clear_error(&error);
		return NULL;
	}

	return pipe;
}

/* Event-driven GStreamer Appsink New-Sample Callback */
static GstFlowReturn on_new_sample_cb(GstAppSink *sink, gpointer user_data)
{
	gst_master_pipeline_t *hub = (gst_master_pipeline_t *)user_data;
	GstSample *sample = gst_app_sink_pull_sample(sink);
	if (!sample) return GST_FLOW_OK;

	GstBuffer *buf = gst_sample_get_buffer(sample);
	if (buf)
	{
		const bool is_keyframe = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);

		g_mutex_lock(&hub->branch_lock);
		gst_subscriber_branch_t *curr = hub->branches;
		while (curr)
		{
			g_mutex_lock(&curr->lock);
			if (curr->active && curr->appsrc && !curr->is_pipeline)
			{
				if (curr->waiting_for_keyframe)
				{
					if (!is_keyframe)
					{
						g_mutex_unlock(&curr->lock);
						curr = curr->next;
						continue;
					}
					curr->waiting_for_keyframe = false;
					blog(LOG_INFO, "[obs-gstreamer-master] Keyframe delivered to '%s', client playback starting", curr->name ? curr->name : "branch");
				}

				GstBuffer *out_buf = gst_buffer_copy(buf);
				GST_BUFFER_PTS(out_buf) = GST_CLOCK_TIME_NONE;
				GST_BUFFER_DTS(out_buf) = GST_CLOCK_TIME_NONE;
				gst_app_src_push_buffer(GST_APP_SRC(curr->appsrc), out_buf);
			}
			g_mutex_unlock(&curr->lock);
			curr = curr->next;
		}
		g_mutex_unlock(&hub->branch_lock);
	}

	gst_sample_unref(sample);
	return GST_FLOW_OK;
}

void gst_master_pipeline_push_raw_frame(gst_master_pipeline_t *hub, const uint8_t *data, uint32_t linesize)
{
	if (!hub) return;

	size_t frame_bytes = (size_t)hub->width * (size_t)hub->height * 4;
	GstBuffer *buf = gst_buffer_new_allocate(NULL, frame_bytes, NULL);
	if (!buf) return;

	GstMapInfo map;
	if (gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
		if (linesize == hub->width * 4) {
			memcpy(map.data, data, frame_bytes);
		} else {
			for (uint32_t y = 0; y < hub->height; y++) {
				memcpy(map.data + y * hub->width * 4, data + y * linesize, hub->width * 4);
			}
		}
		gst_buffer_unmap(buf, &map);

		uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
		GST_BUFFER_PTS(buf) = hub->frame_index * duration;
		GST_BUFFER_DTS(buf) = GST_BUFFER_PTS(buf);
		GST_BUFFER_DURATION(buf) = duration;
		hub->frame_index++;

		// Push to master encoder if clients active
		if (hub->appsrc && g_atomic_int_get(&hub->connected_client_count) > 0) {
			gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), gst_buffer_ref(buf));
		}

		// Push raw frames directly to custom pipeline subscribers (Always Pushing)
		if (g_atomic_int_get(&hub->pipeline_branch_count) > 0) {
			g_mutex_lock(&hub->branch_lock);
			gst_subscriber_branch_t *curr = hub->branches;
			while (curr) {
				if (curr->active && curr->is_pipeline && curr->appsrc) {
					gst_app_src_push_buffer(GST_APP_SRC(curr->appsrc), gst_buffer_ref(buf));
				}
				curr = curr->next;
			}
			g_mutex_unlock(&hub->branch_lock);
		}

		gst_buffer_unref(buf);
	} else {
		gst_buffer_unref(buf);
	}
}

gst_master_pipeline_t *gst_master_pipeline_get_or_create(const char *source_type, const char *source_name,
                                                         const char *custom_master_pipe, const char *encoder_type,
                                                         int bitrate_kbps, int keyint_sec, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den)
{
	gst_master_pipeline_t *hub = bzalloc(sizeof(gst_master_pipeline_t));
	g_mutex_init(&hub->branch_lock);

	hub->source_type = bstrdup(source_type ? source_type : "Program Output");
	hub->source_name = bstrdup(source_name ? source_name : "");
	hub->width = width;
	hub->height = height;
	hub->fps_num = fps_num;
	hub->fps_den = fps_den;

	if (strcmp(hub->source_type, "Scene") == 0) {
		obs_scene_t *sc = obs_get_scene_by_name(hub->source_name);
		hub->source = sc ? obs_scene_get_source(sc) : NULL;
		if (hub->source) obs_source_get_ref(hub->source);
		if (sc) obs_scene_release(sc);
	} else if (strcmp(hub->source_type, "Source") == 0) {
		hub->source = obs_get_source_by_name(hub->source_name);
	} else {
		hub->source = obs_get_output_source(0);
	}

	if (!hub->source) {
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to resolve source '%s' of type '%s'",
			hub->source_name, hub->source_type);
		g_mutex_clear(&hub->branch_lock);
		bfree(hub->source_type);
		bfree(hub->source_name);
		bfree(hub);
		return NULL;
	}

	hub->view = obs_view_create();
	obs_view_set_source(hub->view, 0, hub->source);

	obs_enter_graphics();
	hub->texrender[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	hub->texrender[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	hub->stagesurf[0] = gs_stagesurface_create(hub->width, hub->height, GS_RGBA);
	hub->stagesurf[1] = gs_stagesurface_create(hub->width, hub->height, GS_RGBA);
	obs_leave_graphics();

	if (!hub->texrender[0] || !hub->texrender[1] || !hub->stagesurf[0] || !hub->stagesurf[1]) {
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to allocate GPU texrender or stagesurf");
		gst_master_pipeline_destroy(hub);
		return NULL;
	}

	hub->pingpong_idx = 0;
	hub->first_frame_primed = false;

	hub->pipe = create_master_encoder_pipeline(hub, custom_master_pipe, encoder_type, bitrate_kbps, keyint_sec);
	if (!hub->pipe) {
		gst_master_pipeline_destroy(hub);
		return NULL;
	}

	hub->appsrc = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsrc");
	hub->appsink = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsink");
	if (!hub->appsrc || !hub->appsink) {
		blog(LOG_ERROR, "[obs-gstreamer-master] Master pipeline missing hub_appsrc or hub_appsink");
		gst_master_pipeline_destroy(hub);
		return NULL;
	}

	hub->connected_client_count = 0;
	gst_element_set_state(hub->pipe, GST_STATE_PAUSED);

	// Enable event-driven signals on appsink
	g_object_set(G_OBJECT(hub->appsink), "emit-signals", TRUE, NULL);
	g_signal_connect(hub->appsink, "new-sample", G_CALLBACK(on_new_sample_cb), hub);

	hub->rtsp_context = g_main_context_new();
	hub->rtsp_loop = g_main_loop_new(hub->rtsp_context, FALSE);
	hub->rtsp_thread = g_thread_new("gst_hub_rtsp_loop", (GThreadFunc)g_main_loop_run, hub->rtsp_loop);

	// Register OBS main render callback
	obs_add_main_render_callback(hub_render_callback, hub);
	hub->render_hook_active = true;

	return hub;
}

void gst_master_pipeline_destroy(gst_master_pipeline_t *hub)
{
	if (!hub) return;

	if (hub->render_hook_active) {
		obs_remove_main_render_callback(hub_render_callback, hub);
		hub->render_hook_active = false;
	}

	if (hub->appsink) {
		g_signal_handlers_disconnect_by_data(hub->appsink, hub);
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
}
