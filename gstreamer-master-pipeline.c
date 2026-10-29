/*
 * obs-gstreamer: Master Hardware Encoder Pipeline & Pub-Sub Subscriber Hub
 */

#include "gstreamer-master-pipeline.h"
#include <obs-module.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#if HAVE_GST_D3D11
#include <d3d11.h>
#include <gst/d3d11/gstd3d11memory.h>
#endif

#if HAVE_GST_D3D11
static bool element_factory_exists(const char *name)
{
	GstElementFactory *factory = gst_element_factory_find(name);
	if (!factory) return false;
	gst_object_unref(factory);
	return true;
}
#endif

static GstElement *create_master_encoder_pipeline(gst_master_pipeline_t *hub, const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec)
{
	gchar *pipe_desc = NULL;
	const char *convert_elem = "videoconvert ! video/x-raw, format=I420";
	const char *download_elem = "videoconvert ! video/x-raw, format=I420";

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
#if HAVE_GST_D3D11
		if (hub->gpu_input)
		{
			encoder_elem = g_strdup_printf(
				"d3d11convert ! video/x-raw(memory:D3D11Memory), format=NV12 ! "
				"qsvh264enc gop-size=%d bitrate=%d",
				gop_size, bitrate);
			pipe_desc = g_strdup_printf(
				"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true "
				"caps=\"video/x-raw(memory:D3D11Memory), width=%u, height=%u, "
				"format={RGBA,BGRA,BGRx}, framerate=%u/%u\" ! "
				"%s ! h264parse config-interval=-1 ! "
				"video/x-h264, stream-format=byte-stream, alignment=au ! "
				"appsink name=hub_appsink sync=false drop=true max-buffers=1",
				hub->width, hub->height, hub->fps_num, hub->fps_den, encoder_elem);
			g_free(encoder_elem);
			goto launch_pipeline;
		}
#endif
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
			"caps=\"video/x-raw, width=%u, height=%u, format=RGBA, framerate=%u/%u\" ! "
			"%s ! h264parse config-interval=-1 ! video/x-h264, stream-format=byte-stream, alignment=au ! "
			"appsink name=hub_appsink sync=false drop=true max-buffers=1",
			hub->width, hub->height, hub->fps_num, hub->fps_den,
			encoder_elem);

		g_free(encoder_elem);
	}

#if HAVE_GST_D3D11
launch_pipeline:
#endif
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
	uint64_t sample_number = ++hub->encoded_sample_count;
	if (sample_number == 1 || sample_number % 300 == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] Encoded sample #%" PRIu64 ": %" G_GSIZE_FORMAT
			" bytes, keyframe=%s",
			sample_number, gst_buffer_get_size(buf), is_keyframe ? "yes" : "no");
	}

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

#if HAVE_GST_D3D11
static void release_d3d11_texture(gpointer user_data)
{
	ID3D11Texture2D_Release((ID3D11Texture2D *)user_data);
}

static bool push_d3d11_texture(gst_master_pipeline_t *hub, gs_texture_t *texture, uint64_t pts)
{
	if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] GPU zero-copy requires the OBS Direct3D 11 graphics backend");
		return false;
	}

	uint32_t width = gs_texture_get_width(texture);
	uint32_t height = gs_texture_get_height(texture);
	if (width != hub->width || height != hub->height)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] D3D11 texture size %ux%u does not match pipeline size %ux%u",
			width, height, hub->width, hub->height);
		return false;
	}

	enum gs_color_format color_format = gs_texture_get_color_format(texture);
	GstVideoFormat format;

	switch (color_format)
	{
	case GS_RGBA: format = GST_VIDEO_FORMAT_RGBA; break;
	case GS_BGRA:
	case GS_BGRA_UNORM: format = GST_VIDEO_FORMAT_BGRA; break;
	case GS_BGRX: format = GST_VIDEO_FORMAT_BGRx; break;
	default:
		blog(LOG_ERROR, "[obs-gstreamer-master] Unsupported D3D11 texture format for zero-copy input: %d",
			(int)color_format);
		return false;
	}

	if (!hub->d3d11_device)
	{
		ID3D11Device *device = (ID3D11Device *)gs_get_device_obj();
		if (!device)
		{
			blog(LOG_ERROR, "[obs-gstreamer-master] OBS did not provide a D3D11 device for texture sharing");
			return false;
		}
		hub->d3d11_device = gst_d3d11_device_new_wrapped(device);
		if (!hub->d3d11_device)
		{
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to wrap OBS D3D11 device for GStreamer");
			return false;
		}
	}

	ID3D11Texture2D *d3d_texture = (ID3D11Texture2D *)gs_texture_get_obj(texture);
	if (!d3d_texture)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] OBS texture has no native D3D11 resource");
		return false;
	}

	GstVideoInfo info;
	if (!gst_video_info_set_format(&info, format, width, height))
		return false;

	gst_d3d11_memory_init_once();
	GstAllocator *allocator = gst_allocator_find(GST_D3D11_MEMORY_NAME);
	if (!allocator)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] GStreamer D3D11 allocator is unavailable");
		return false;
	}

	ID3D11Texture2D_AddRef(d3d_texture);
	GstMemory *memory = gst_d3d11_allocator_alloc_wrapped(
		GST_D3D11_ALLOCATOR(allocator), hub->d3d11_device, d3d_texture,
		GST_VIDEO_INFO_SIZE(&info), d3d_texture, release_d3d11_texture);
	gst_object_unref(allocator);
	if (!memory)
	{
		ID3D11Texture2D_Release(d3d_texture);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to wrap OBS texture as GstD3D11Memory");
		return false;
	}

	GstBuffer *buffer = gst_buffer_new();
	if (!buffer)
	{
		gst_memory_unref(memory);
		return false;
	}
	gst_buffer_append_memory(buffer, memory);

	gsize offsets[GST_VIDEO_MAX_PLANES] = {0};
	gint strides[GST_VIDEO_MAX_PLANES] = {0};
	for (guint plane = 0; plane < GST_VIDEO_INFO_N_PLANES(&info); plane++)
	{
		offsets[plane] = GST_VIDEO_INFO_PLANE_OFFSET(&info, plane);
		strides[plane] = GST_VIDEO_INFO_PLANE_STRIDE(&info, plane);
	}
	if (!gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, format, width, height,
		GST_VIDEO_INFO_N_PLANES(&info), offsets, strides))
	{
		gst_buffer_unref(buffer);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to attach video metadata to D3D11 texture frame");
		return false;
	}

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(buffer) = pts;
	GST_BUFFER_DTS(buffer) = pts;
	GST_BUFFER_DURATION(buffer) = duration;
	hub->frame_index++;

	GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), buffer);
	if (flow != GST_FLOW_OK)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] D3D11 texture push failed: flow=%s",
			gst_flow_get_name(flow));
		return false;
	}

	uint64_t frame_number = ++hub->texture_push_count;
	if (frame_number == 1 || frame_number % 300 == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] D3D11 texture frame #%" PRIu64
			" wrapped and pushed (%ux%u, format=%d)",
			frame_number, width, height, (int)color_format);
	}
	return true;
}
#endif

void gst_master_pipeline_push_texture(gst_master_pipeline_t *hub, void *gpu_handle, uint64_t pts)
{
	if (!hub || !gpu_handle || g_atomic_int_get(&hub->active_destinations) <= 0 || !hub->appsrc) return;

	gs_texture_t *tex = (gs_texture_t *)gpu_handle;
#if HAVE_GST_D3D11
	if (hub->gpu_input)
	{
		push_d3d11_texture(hub, tex, pts);
		return;
	}
#endif
	uint64_t frame_number = ++hub->texture_push_count;
	uint32_t tex_w = gs_texture_get_width(tex);
	uint32_t tex_h = gs_texture_get_height(tex);
	enum gs_color_format fmt = gs_texture_get_color_format(tex);

	if (tex_w == 0 || tex_h == 0) return;

	if (!hub->stagesurf ||
	    gs_stagesurface_get_width(hub->stagesurf) != tex_w ||
	    gs_stagesurface_get_height(hub->stagesurf) != tex_h ||
	    gs_stagesurface_get_color_format(hub->stagesurf) != fmt)
	{
		if (hub->stagesurf)
		{
			gs_stagesurface_destroy(hub->stagesurf);
			hub->stagesurf = NULL;
		}
		hub->stagesurf = gs_stagesurface_create(tex_w, tex_h, fmt);
	}

	if (!hub->stagesurf)
	{
		if (frame_number == 1)
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to create %ux%u staging surface for format=%d",
				tex_w, tex_h, (int)fmt);
		return;
	}

	gs_stage_texture(hub->stagesurf, tex);

	uint8_t *src_data = NULL;
	uint32_t src_linesize = 0;
	if (!gs_stagesurface_map(hub->stagesurf, &src_data, &src_linesize))
	{
		if (frame_number == 1)
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to map %ux%u staging surface",
				tex_w, tex_h);
		return;
	}

	uint32_t copy_w = tex_w < hub->width ? tex_w : hub->width;
	uint32_t copy_h = tex_h < hub->height ? tex_h : hub->height;
	size_t dst_stride = (size_t)hub->width * 4;
	size_t total_size = dst_stride * hub->height;

	GstBuffer *in_buf = gst_buffer_new_allocate(NULL, total_size, NULL);
	if (!in_buf)
	{
		if (frame_number == 1)
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to allocate input buffer for texture frame");
		gs_stagesurface_unmap(hub->stagesurf);
		return;
	}

	GstMapInfo map;
	if (!gst_buffer_map(in_buf, &map, GST_MAP_WRITE))
	{
		if (frame_number == 1)
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to map input buffer for texture frame");
		gst_buffer_unref(in_buf);
		gs_stagesurface_unmap(hub->stagesurf);
		return;
	}

	bool is_bgra = (fmt == GS_BGRA || fmt == GS_BGRA_UNORM || fmt == GS_BGRX);
	if ((frame_number == 1 || frame_number % 300 == 0) && copy_w > 0 && copy_h > 0)
	{
		uint32_t sample_x = copy_w / 2;
		uint32_t sample_y = copy_h / 2;
		const uint8_t *sample = src_data + (size_t)sample_y * src_linesize + (size_t)sample_x * 4;
		blog(LOG_INFO, "[obs-gstreamer-master] Texture frame #%" PRIu64
			" readback center pixel (raw)=%u,%u,%u,%u",
			frame_number, sample[0], sample[1], sample[2], sample[3]);
	}

	for (uint32_t y = 0; y < copy_h; y++)
	{
		const uint8_t *src_row = src_data + (size_t)y * src_linesize;
		uint8_t *dst_row = map.data + (size_t)y * dst_stride;
		if (!is_bgra)
		{
			memcpy(dst_row, src_row, (size_t)copy_w * 4);
		}
		else
		{
			for (uint32_t x = 0; x < copy_w; x++)
			{
				dst_row[x * 4 + 0] = src_row[x * 4 + 2];
				dst_row[x * 4 + 1] = src_row[x * 4 + 1];
				dst_row[x * 4 + 2] = src_row[x * 4 + 0];
				dst_row[x * 4 + 3] = src_row[x * 4 + 3];
			}
		}
	}
	gst_buffer_unmap(in_buf, &map);

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(in_buf) = pts > 0 ? pts : (hub->frame_index * duration);
	GST_BUFFER_DTS(in_buf) = GST_BUFFER_PTS(in_buf);
	GST_BUFFER_DURATION(in_buf) = duration;
	hub->frame_index++;

	GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), in_buf);
	if (flow != GST_FLOW_OK)
	{
		if (frame_number == 1 || frame_number % 300 == 0)
			blog(LOG_ERROR, "[obs-gstreamer-master] Texture frame #%" PRIu64
				" push to encoder failed: flow=%s",
				frame_number, gst_flow_get_name(flow));
	}
	else if (frame_number == 1 || frame_number % 300 == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] Texture frame #%" PRIu64
			" pushed to encoder (%ux%u texture, %ux%u submitted)",
			frame_number, tex_w, tex_h, copy_w, copy_h);
	}

	gs_stagesurface_unmap(hub->stagesurf);
}

void gst_master_pipeline_push_raw_video(gst_master_pipeline_t *hub, struct video_data *frame)
{
	if (!hub || !frame || !frame->data[0] || g_atomic_int_get(&hub->active_destinations) <= 0 || !hub->appsrc) return;

	size_t row_size = (size_t)hub->width * 4;
	if (hub->gpu_input || hub->height == 0 || row_size / 4 != hub->width ||
	    (size_t)frame->linesize[0] < row_size)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Invalid CPU frame layout for wrapped input");
		return;
	}

	size_t frame_size = (size_t)frame->linesize[0] * hub->height;
	if (frame_size / hub->height != (size_t)frame->linesize[0])
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] CPU frame size overflow while wrapping input");
		return;
	}

	GstBuffer *buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, frame->data[0],
		frame_size, 0, frame_size, NULL, NULL);
	if (!buf) return;

	gsize offsets[GST_VIDEO_MAX_PLANES] = {0};
	gint strides[GST_VIDEO_MAX_PLANES] = {(gint)frame->linesize[0]};
	if (!gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_RGBA,
		hub->width, hub->height, 1, offsets, strides))
	{
		gst_buffer_unref(buf);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to attach video metadata to wrapped CPU frame");
		return;
	}

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(buf) = frame->timestamp;
	GST_BUFFER_DTS(buf) = frame->timestamp;
	GST_BUFFER_DURATION(buf) = duration;
	hub->frame_index++;

	GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), buf);
	if (flow != GST_FLOW_OK)
		blog(LOG_ERROR, "[obs-gstreamer-master] Wrapped CPU frame push failed: flow=%s",
			gst_flow_get_name(flow));
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

void gst_master_pipeline_request_keyframe(gst_master_pipeline_t *hub)
{
	if (!hub || !hub->appsink) return;
	GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
	gst_element_send_event(hub->appsink, event);
}

gst_master_pipeline_t *gst_master_pipeline_create(const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, bool gpu_input, gst_master_state_cb state_cb, gpointer user_data)
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
#if HAVE_GST_D3D11
	hub->gpu_input = gpu_input && (!custom_master_pipe || !custom_master_pipe[0]) &&
		(encoder_type == NULL || !encoder_type[0] || g_strcmp0(encoder_type, "auto") == 0 ||
		 g_strcmp0(encoder_type, "qsvh264enc") == 0) &&
		element_factory_exists("d3d11convert") && element_factory_exists("qsvh264enc");
	if (gpu_input && !hub->gpu_input)
	{
		blog(LOG_WARNING, "[obs-gstreamer-master] D3D11 zero-copy unavailable for this pipeline/encoder; using texture readback");
	}
#else
	UNUSED_PARAMETER(gpu_input);
	hub->gpu_input = false;
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
	GstStateChangeReturn state_ret = gst_element_set_state(hub->pipe, GST_STATE_PAUSED);
#if HAVE_GST_D3D11
	if (state_ret == GST_STATE_CHANGE_FAILURE && hub->gpu_input)
	{
		blog(LOG_WARNING, "[obs-gstreamer-master] D3D11/QSV pipeline failed to initialize; retrying with CPU texture readback");
		gst_object_unref(hub->appsrc);
		gst_object_unref(hub->appsink);
		hub->appsrc = NULL;
		hub->appsink = NULL;
		gst_object_unref(hub->pipe);
		hub->gpu_input = false;

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
			blog(LOG_ERROR, "[obs-gstreamer-master] Fallback pipeline missing hub_appsrc or hub_appsink");
			gst_master_pipeline_destroy(hub);
			return NULL;
		}
		state_ret = gst_element_set_state(hub->pipe, GST_STATE_PAUSED);
	}
#endif
	if (state_ret == GST_STATE_CHANGE_FAILURE)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to initialize master pipeline");
		gst_master_pipeline_destroy(hub);
		return NULL;
	}

	GstAppSinkCallbacks sink_cbs = {NULL, NULL, video_new_sample_cb};
	gst_app_sink_set_callbacks(GST_APP_SINK(hub->appsink), &sink_cbs, hub, NULL);

	if (state_cb) state_cb(false, user_data);

	return hub;
}

void gst_master_pipeline_destroy(gst_master_pipeline_t *hub)
{
	if (!hub) return;

	if (hub->pipe)
	{
		gst_element_set_state(hub->pipe, GST_STATE_NULL);
		if (hub->appsrc) gst_object_unref(hub->appsrc);
		if (hub->appsink) gst_object_unref(hub->appsink);
		gst_object_unref(hub->pipe);
		hub->pipe = NULL;
	}

	if (hub->stagesurf)
	{
		obs_enter_graphics();
		gs_stagesurface_destroy(hub->stagesurf);
		hub->stagesurf = NULL;
		obs_leave_graphics();
	}

#if HAVE_GST_D3D11
	if (hub->d3d11_device)
	{
		gst_object_unref(hub->d3d11_device);
		hub->d3d11_device = NULL;
	}
#endif

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
