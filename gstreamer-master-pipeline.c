/*
 * obs-gstreamer: Master Hardware Encoder Pipeline & Pub-Sub Subscriber Hub
 */

#include "gstreamer-master-pipeline.h"
#include <obs-module.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <stdlib.h>
#include <string.h>

static GstElement *create_master_encoder_pipeline(gst_master_pipeline_t *hub, const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec)
{
	gchar *pipe_desc = NULL;

#if defined(_WIN32) && defined(HAVE_GST_D3D11) && HAVE_GST_D3D11
	const char *gpu_mem_caps = "video/x-raw(memory:D3D11Memory)";
	const char *convert_elem = "d3d11convert";
	const char *download_elem = "d3d11convert ! d3d11download ! videoconvert";
#elif defined(HAVE_GST_GL) && HAVE_GST_GL
	const char *gpu_mem_caps = "video/x-raw(memory:GLMemory)";
	const char *convert_elem = "glcolorconvert";
	const char *download_elem = "glcolorconvert ! gldownload ! videoconvert";
#else
	const char *gpu_mem_caps = "video/x-raw";
	const char *convert_elem = "videoconvert";
	const char *download_elem = "videoconvert";
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
			encoder_elem = g_strdup_printf("%s ! x264enc tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=%d bitrate=%d", download_elem, gop_size, bitrate);

		pipe_desc = g_strdup_printf(
			"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true "
			"caps=\"%s, width=%u, height=%u, format=RGBA, framerate=%u/%u\" ! "
			"%s ! h264parse config-interval=1 ! "
			"appsink name=hub_appsink sync=false drop=true max-buffers=1",
			gpu_mem_caps, hub->width, hub->height, hub->fps_num, hub->fps_den,
			encoder_elem);

		g_free(encoder_elem);
	}

	blog(LOG_INFO, "[obs-gstreamer-master] Launching Master Hardware Encoder Pipeline: %s", pipe_desc);
	GError *error = NULL;
	GstElement *pipe = gst_parse_launch(pipe_desc, &error);
	g_free(pipe_desc);

	if (error)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to create master pipeline: %s", error->message);
		g_clear_error(&error);
		return NULL;
	}

	return pipe;
}

static GstFlowReturn video_new_sample_cb(GstAppSink *sink, gpointer user_data)
{
	gst_master_pipeline_t *hub = (gst_master_pipeline_t *)user_data;
	if (!hub) return GST_FLOW_OK;

	GstSample *sample = gst_app_sink_pull_sample(sink);
	if (!sample) return GST_FLOW_OK;

	GstBuffer *buf = gst_sample_get_buffer(sample);
	if (!buf)
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
			if (sub && sub->cb) sub->cb(buf, is_keyframe, sub->user_data);
		}
	}

	gst_sample_unref(sample);
	return GST_FLOW_OK;
}

void gst_master_pipeline_push_texture(gst_master_pipeline_t *hub, void *gpu_handle, uint64_t pts)
{
	if (!hub || !gpu_handle || g_atomic_int_get(&hub->active_destinations) <= 0) return;

	GstBuffer *in_buf = gst_buffer_new();
	if (!in_buf) return;

	GstMemory *mem = NULL;

#if defined(_WIN32) && defined(HAVE_GST_D3D11) && HAVE_GST_D3D11
	if (hub->d3d11_allocator && hub->d3d11_device)
	{
		mem = gst_d3d11_allocator_alloc_wrapped(GST_D3D11_ALLOCATOR(hub->d3d11_allocator), hub->d3d11_device, (ID3D11Texture2D *)gpu_handle, sizeof(void *), NULL, NULL);
	}
#elif defined(HAVE_GST_GL) && HAVE_GST_GL
	if (hub->gl_context && hub->gl_allocator)
	{
		GLuint tex_id = (GLuint)(uintptr_t)gpu_handle;
		GstVideoInfo vinfo;
		gst_video_info_set_format(&vinfo, GST_VIDEO_FORMAT_RGBA, hub->width, hub->height);

		GstGLVideoAllocationParams *params = gst_gl_video_allocation_params_new_wrapped_texture(hub->gl_context, NULL, &vinfo, 0, NULL, GST_GL_TEXTURE_TARGET_2D, GST_GL_RGBA, tex_id, NULL, NULL);

		if (params)
		{
			mem = gst_gl_memory_allocator_alloc(GST_GL_MEMORY_ALLOCATOR(hub->gl_allocator), params);
			gst_gl_allocation_params_free((GstGLAllocationParams *)params);
		}
	}
#else
	UNUSED_PARAMETER(gpu_handle);
#endif

	if (!mem)
	{
		gst_buffer_unref(in_buf);
		return;
	}

	gst_buffer_append_memory(in_buf, mem);

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(in_buf) = pts > 0 ? pts : (hub->frame_index * duration);
	GST_BUFFER_DTS(in_buf) = GST_BUFFER_PTS(in_buf);
	GST_BUFFER_DURATION(in_buf) = duration;
	hub->frame_index++;

	if (hub->appsrc)
	{
		gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), in_buf);
	}
	else
	{
		gst_buffer_unref(in_buf);
	}
}

void gst_master_pipeline_push_raw_video(gst_master_pipeline_t *hub, struct video_data *frame)
{
	if (!hub || !frame || g_atomic_int_get(&hub->active_destinations) <= 0 || !hub->appsrc) return;

	size_t plane0_size = (size_t)frame->linesize[0] * hub->height;
	size_t plane1_size = frame->linesize[1] > 0 ? (size_t)frame->linesize[1] * (hub->height / 2) : 0;
	size_t plane2_size = frame->linesize[2] > 0 ? (size_t)frame->linesize[2] * (hub->height / 2) : 0;
	size_t total_size = plane0_size + plane1_size + plane2_size;
	if (total_size == 0) return;

	GstBuffer *buf = gst_buffer_new_allocate(NULL, total_size, NULL);
	if (!buf) return;

	GstMapInfo map;
	if (gst_buffer_map(buf, &map, GST_MAP_WRITE))
	{
		uint8_t *dst = map.data;
		if (frame->data[0] && plane0_size > 0)
		{
			memcpy(dst, frame->data[0], plane0_size);
			dst += plane0_size;
		}
		if (frame->data[1] && plane1_size > 0)
		{
			memcpy(dst, frame->data[1], plane1_size);
			dst += plane1_size;
		}
		if (frame->data[2] && plane2_size > 0)
		{
			memcpy(dst, frame->data[2], plane2_size);
		}
		gst_buffer_unmap(buf, &map);
	}

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(buf) = frame->timestamp;
	GST_BUFFER_DTS(buf) = frame->timestamp;
	GST_BUFFER_DURATION(buf) = duration;
	hub->frame_index++;

	gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), buf);
}

static void update_active_destinations(gst_master_pipeline_t *hub, int delta)
{
	if (!hub) return;

	int prev = g_atomic_int_add(&hub->active_destinations, delta);
	int updated = prev + delta;
	if (updated < 0)
	{
		g_atomic_int_set(&hub->active_destinations, 0);
		updated = 0;
	}

	if (prev == 0 && updated > 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] First destination active! Starting master encoder.");
		if (hub->pipe) gst_element_set_state(hub->pipe, GST_STATE_PLAYING);

		if (hub->appsink)
		{
			GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
			gst_element_send_event(hub->appsink, event);
		}

		g_mutex_lock(&hub->state_mutex);
		gst_master_state_cb cb = hub->state_cb;
		gpointer ud = hub->state_user_data;
		g_mutex_unlock(&hub->state_mutex);
		if (cb) cb(true, ud);
	}
	else if (prev > 0 && updated == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] All destinations inactive. Pausing encoder pipeline (0%% CPU/GPU).");
		if (hub->pipe) gst_element_set_state(hub->pipe, GST_STATE_PAUSED);

		g_mutex_lock(&hub->state_mutex);
		gst_master_state_cb cb = hub->state_cb;
		gpointer ud = hub->state_user_data;
		g_mutex_unlock(&hub->state_mutex);
		if (cb) cb(false, ud);
	}
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
	GPtrArray *new_subs = g_ptr_array_new();

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

	if (old_subs) g_ptr_array_free(old_subs, TRUE);

	blog(LOG_INFO, "[obs-gstreamer-master] Subscriber '%s' connected", sub->name);
	update_active_destinations(hub, 1);

	/* Request immediate IDR keyframe so the new subscriber receives video immediately */
	if (hub->appsink)
	{
		GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
		gst_element_send_event(hub->appsink, event);
	}

	return sub;
}

void gst_master_pipeline_unsubscribe(gst_master_pipeline_t *hub, gst_pipeline_subscriber_t *sub)
{
	if (!hub || !sub) return;

	g_mutex_lock(&hub->subscriber_mutex);

	GPtrArray *old_subs = (GPtrArray *)hub->subscribers_atomic;
	GPtrArray *new_subs = g_ptr_array_new();

	if (old_subs)
	{
		for (guint i = 0; i < old_subs->len; i++)
		{
			gst_pipeline_subscriber_t *existing = g_ptr_array_index(old_subs, i);
			if (existing != sub) g_ptr_array_add(new_subs, existing);
		}
	}
	g_atomic_pointer_set(&hub->subscribers_atomic, new_subs);

	g_mutex_unlock(&hub->subscriber_mutex);

	if (old_subs) g_ptr_array_free(old_subs, TRUE);

	blog(LOG_INFO, "[obs-gstreamer-master] Subscriber '%s' disconnected", sub->name ? sub->name : "Subscriber");
	bfree(sub->name);
	bfree(sub);
	update_active_destinations(hub, -1);
}

gst_master_pipeline_t *gst_master_pipeline_create(const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, gst_master_state_cb state_cb, gpointer user_data)
{
	gst_master_pipeline_t *hub = bzalloc(sizeof(gst_master_pipeline_t));
	g_mutex_init(&hub->subscriber_mutex);
	g_mutex_init(&hub->state_mutex);

	hub->width = width;
	hub->height = height;
	hub->fps_num = fps_num;
	hub->fps_den = fps_den;
	hub->state_cb = state_cb;
	hub->state_user_data = user_data;

#if defined(_WIN32) && defined(HAVE_GST_D3D11) && HAVE_GST_D3D11
	GError *err = NULL;
	hub->d3d11_device = gst_d3d11_device_new(0, &err);
	if (hub->d3d11_device)
	{
		hub->d3d11_allocator = GST_ALLOCATOR(gst_d3d11_allocator_new(hub->d3d11_device));
	}
#elif defined(HAVE_GST_GL) && HAVE_GST_GL
	hub->gl_display = gst_gl_display_new();
	if (hub->gl_display)
	{
		hub->gl_context = gst_gl_context_new(hub->gl_display);
		if (hub->gl_context)
		{
			hub->gl_allocator = GST_ALLOCATOR(gst_gl_memory_allocator_get_default(hub->gl_context));
		}
	}
#endif

	hub->pipe = create_master_encoder_pipeline(hub, custom_master_pipe, encoder_type, bitrate_kbps, keyint_sec);
	if (!hub->pipe)
	{
		gst_master_pipeline_destroy(hub);
		return NULL;
	}

	hub->appsrc = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsrc");
	hub->appsink = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsink");
	if (!hub->appsrc || !hub->appsink)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Pipeline missing hub_appsrc or hub_appsink");
		gst_master_pipeline_destroy(hub);
		return NULL;
	}

	hub->active_destinations = 0;
	gst_element_set_state(hub->pipe, GST_STATE_PAUSED);

	GstAppSinkCallbacks sink_cbs = {NULL, NULL, video_new_sample_cb};
	gst_app_sink_set_callbacks(GST_APP_SINK(hub->appsink), &sink_cbs, hub, NULL);

	if (state_cb) state_cb(false, user_data);

	return hub;
}

void gst_master_pipeline_destroy(gst_master_pipeline_t *hub)
{
	if (!hub) return;

#if defined(_WIN32) && defined(HAVE_GST_D3D11) && HAVE_GST_D3D11
	if (hub->d3d11_allocator)
	{
		gst_object_unref(hub->d3d11_allocator);
		hub->d3d11_allocator = NULL;
	}
	if (hub->d3d11_device)
	{
		gst_object_unref(hub->d3d11_device);
		hub->d3d11_device = NULL;
	}
#elif defined(HAVE_GST_GL) && HAVE_GST_GL
	if (hub->gl_allocator)
	{
		gst_object_unref(hub->gl_allocator);
		hub->gl_allocator = NULL;
	}
	if (hub->gl_context)
	{
		gst_object_unref(hub->gl_context);
		hub->gl_context = NULL;
	}
	if (hub->gl_display)
	{
		gst_object_unref(hub->gl_display);
		hub->gl_display = NULL;
	}
#endif

	if (hub->pipe)
	{
		gst_element_set_state(hub->pipe, GST_STATE_NULL);
		if (hub->appsrc) gst_object_unref(hub->appsrc);
		if (hub->appsink) gst_object_unref(hub->appsink);
		gst_object_unref(hub->pipe);
		hub->pipe = NULL;
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

	g_mutex_clear(&hub->subscriber_mutex);
	g_mutex_clear(&hub->state_mutex);
	bfree(hub);
}
