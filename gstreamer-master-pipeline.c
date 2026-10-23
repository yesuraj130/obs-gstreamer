/*
 * obs-gstreamer: Master Hardware Encoder Pipeline & Pub-Sub Subscriber Hub (Optimized Pure GPU Zero-Copy Mode)
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

#ifdef _WIN32
	const char *gpu_mem_caps = "video/x-raw(memory:D3D11Memory)";
	const char *convert_elem = "d3d11convert";
#else
	const char *gpu_mem_caps = "video/x-raw(memory:GLMemory)";
	const char *convert_elem = "glcolorconvert";
#endif

	if (custom_master_pipe && custom_master_pipe[0])
	{
		pipe_desc = g_strdup(custom_master_pipe);
	}
	else
	{
		int bitrate = bitrate_kbps > 0 ? bitrate_kbps : 4000;
		int keyint = keyint_sec > 0 ? keyint_sec : 1;
		int gop_size = keyint * (int)hub->fps_num / (int)hub->fps_den;
		if (gop_size < 15) gop_size = 30;

		gchar *encoder_elem = NULL;
		if (encoder_type && g_strcmp0(encoder_type, "nvh264enc") == 0) 
			encoder_elem = g_strdup_printf("%s ! nvh264enc tune=zerolatency bframes=0 rc-lookahead=0 gop-size=%d bitrate=%d", convert_elem, gop_size, bitrate);
		else if (encoder_type && g_strcmp0(encoder_type, "qsvh264enc") == 0) 
			encoder_elem = g_strdup_printf("%s ! qsvh264enc gop-size=%d bitrate=%d", convert_elem, gop_size, bitrate);
		else if (encoder_type && g_strcmp0(encoder_type, "vaapih264enc") == 0) 
			encoder_elem = g_strdup_printf("%s ! vaapih264enc keyframe-period=%d bitrate=%d", convert_elem, gop_size, bitrate);
		 else 
			encoder_elem = g_strdup_printf("%s ! x264enc tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=%d bitrate=%d", convert_elem, gop_size, bitrate);

		pipe_desc = g_strdup_printf(
			"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true "
			"caps=\"%s, width=%u, height=%u, format=RGBA, framerate=%u/%u\" ! "
			"%s ! h264parse config-interval=1 ! "
			"appsink name=hub_appsink sync=false drop=true max-buffers=1",
			gpu_mem_caps, hub->width, hub->height, hub->fps_num, hub->fps_den,
			encoder_elem);

		g_free(encoder_elem);
	}

	blog(LOG_INFO, "[obs-gstreamer-master] Launching Optimized Pure GPU Zero-Copy master pipeline: %s", pipe_desc);
	GError *error = NULL;
	GstElement *pipe = gst_parse_launch(pipe_desc, &error);
	g_free(pipe_desc);

	if (error)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to create pure GPU master pipeline: %s", error->message);
		g_clear_error(&error);
		return NULL;
	}

	return pipe;
}

/* Native GStreamer Appsink Callback */
static GstFlowReturn video_new_sample_cb(GstAppSink *sink, gpointer user_data)
{
	gst_master_pipeline_t *hub = (gst_master_pipeline_t *)user_data;
	if (!hub) return GST_FLOW_OK;

	GstSample *sample = gst_app_sink_pull_sample(sink);
	if (!sample) return GST_FLOW_OK;

	GstBuffer *buf = gst_sample_get_buffer(sample);
	GstCaps *caps = gst_sample_get_caps(sample);
	if (!buf || !caps)
	{
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	const bool is_keyframe = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);

	GPtrArray *subs = (GPtrArray *)g_atomic_pointer_get(&hub->subscribers_atomic);
	if (subs)
	{
		for (guint i = 0; i < subs->len; i++)
		{
			gst_pipeline_subscriber_t *sub = g_ptr_array_index(subs, i);
			if (sub && sub->cb)
			{
				sub->cb(buf, is_keyframe, sub->user_data);
			}
		}
	}

	gst_sample_unref(sample);
	return GST_FLOW_OK;
}

/* Zero-Copy GPU Frame Push using native GPU texture handles */
void gst_master_pipeline_push_raw_frame(gst_master_pipeline_t *hub, const uint8_t *data, uint32_t linesize)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(linesize);
	if (!hub || !hub->texrender[hub->pingpong_idx]) return;

	gs_texture_t *source_tex = gs_texrender_get_texture(hub->texrender[hub->pingpong_idx]);
	if (!source_tex) return;

	void *gpu_handle = gs_texture_get_obj(source_tex);
	if (!gpu_handle) return;

	GstBuffer *in_buf = gst_buffer_new();
	if (!in_buf) return;

	GstMemory *mem = gst_memory_new_wrapped(GST_MEMORY_FLAG_READONLY, gpu_handle, sizeof(void *), 0, sizeof(void *), NULL, NULL);
	gst_buffer_append_memory(in_buf, mem);

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(in_buf) = hub->frame_index * duration;
	GST_BUFFER_DTS(in_buf) = GST_BUFFER_PTS(in_buf);
	GST_BUFFER_DURATION(in_buf) = duration;
	hub->frame_index++;

	if (hub->appsrc && g_atomic_int_get(&hub->connected_client_count) > 0)
	{
		gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), gst_buffer_ref(in_buf));
	}

	gst_buffer_unref(in_buf);
}

gst_pipeline_subscriber_t *gst_master_pipeline_subscribe(gst_master_pipeline_t *hub, gst_master_frame_cb cb, gpointer user_data, const char *name)
{
	if (!hub) return NULL;

	gst_pipeline_subscriber_t *sub = bzalloc(sizeof(gst_pipeline_subscriber_t));
	sub->cb = cb;
	sub->user_data = user_data;
	sub->name = bstrdup(name ? name : "Subscriber");
	sub->waiting_for_keyframe = true;

	g_mutex_lock(&hub->subscriber_mutex);

	GPtrArray *old_subs = (GPtrArray *)hub->subscribers_atomic;
	GPtrArray *new_subs = g_ptr_array_new_full(old_subs ? old_subs->len + 1 : 1, g_free);

	if (old_subs)
	{
		for (guint i = 0; i < old_subs->len; i++)
		{
			g_ptr_array_add(new_subs, g_ptr_array_index(old_subs, i));
		}
	}
	g_ptr_array_add(new_subs, sub);

	g_atomic_pointer_set(&hub->subscribers_atomic, new_subs);

	g_mutex_unlock(&hub->subscriber_mutex);

	if (old_subs)
	{
		g_ptr_array_free(old_subs, TRUE);
	}

	blog(LOG_INFO, "[obs-gstreamer-master] Subscriber '%s' added", sub->name);
	return sub;
}

void gst_master_pipeline_unsubscribe(gst_master_pipeline_t *hub, gst_pipeline_subscriber_t *sub)
{
	if (!hub || !sub) return;

	g_mutex_lock(&hub->subscriber_mutex);

	GPtrArray *old_subs = (GPtrArray *)hub->subscribers_atomic;
	GPtrArray *new_subs = g_ptr_array_new_full(old_subs ? old_subs->len : 1, g_free);

	if (old_subs)
	{
		for (guint i = 0; i < old_subs->len; i++)
		{
			gst_pipeline_subscriber_t *existing = g_ptr_array_index(old_subs, i);
			if (existing != sub)
			{
				g_ptr_array_add(new_subs, existing);
			}
		}
	}

	g_atomic_pointer_set(&hub->subscribers_atomic, new_subs);

	g_mutex_unlock(&hub->subscriber_mutex);

	if (old_subs)
	{
		g_ptr_array_free(old_subs, TRUE);
	}

	blog(LOG_INFO, "[obs-gstreamer-master] Subscriber '%s' removed", sub->name ? sub->name : "Subscriber");
	bfree(sub->name);
	bfree(sub);
}

void gst_master_pipeline_update_client_count(gst_master_pipeline_t *hub, int delta)
{
	if (!hub) return;

	int prev = g_atomic_int_add(&hub->connected_client_count, delta);
	int updated = prev + delta;
	if (updated < 0)
	{
		g_atomic_int_set(&hub->connected_client_count, 0);
		updated = 0;
	}

	if (prev == 0 && updated > 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] First client connected! Activating master encoder pipeline.");
		if (hub->pipe)
		{
			gst_element_set_state(hub->pipe, GST_STATE_PLAYING);
		}
		if (hub->appsink)
		{
			GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
			gst_element_send_event(hub->appsink, event);
		}
	}
	else if (prev > 0 && updated == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] Last client disconnected. Pausing encoder pipeline (0%% CPU/GPU usage).");
		hub->first_frame_primed = false;
		if (hub->pipe)
		{
			gst_element_set_state(hub->pipe, GST_STATE_PAUSED);
		}
	}
}

gst_master_pipeline_t *gst_master_pipeline_get_or_create(const char *source_type, const char *source_name,
                                                         const char *custom_master_pipe, const char *encoder_type,
                                                         int bitrate_kbps, int keyint_sec, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den)
{
	gst_master_pipeline_t *hub = bzalloc(sizeof(gst_master_pipeline_t));
	g_mutex_init(&hub->subscriber_mutex);

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
		g_mutex_clear(&hub->subscriber_mutex);
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
	obs_leave_graphics();

	if (!hub->texrender[0] || !hub->texrender[1]) {
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to allocate GPU texrender");
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

	GstAppSinkCallbacks sink_cbs = {NULL, NULL, video_new_sample_cb};
	gst_app_sink_set_callbacks(GST_APP_SINK(hub->appsink), &sink_cbs, hub, NULL);

	hub->rtsp_context = g_main_context_new();
	hub->rtsp_loop = g_main_loop_new(hub->rtsp_context, FALSE);
	hub->rtsp_thread = g_thread_new("gst_hub_rtsp_loop", (GThreadFunc)g_main_loop_run, hub->rtsp_loop);

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

	GPtrArray *subs = (GPtrArray *)hub->subscribers_atomic;
	if (subs)
	{
		for (guint i = 0; i < subs->len; i++)
		{
			gst_pipeline_subscriber_t *sub = g_ptr_array_index(subs, i);
			if (sub)
			{
				bfree(sub->name);
				bfree(sub);
			}
		}
		g_ptr_array_free(subs, TRUE);
	}

	bfree(hub->source_type);
	bfree(hub->source_name);
	g_mutex_clear(&hub->subscriber_mutex);
	bfree(hub);
}
