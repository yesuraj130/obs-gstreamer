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

#include <obs/obs-module.h>
#include <util/platform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <gst/app/app.h>
#include "gstreamer-helper.h"

struct gstreamer_filter_data
{
	obs_source_t *filter_source;
	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;

	uint32_t width; // Cached video state
	uint32_t height;
	enum video_format format;
	GstVideoInfo video_info;

	uint32_t sample_rate; // Cached audio state
	size_t channels;
	GstAudioInfo audio_info;

	gs_texrender_t *gpu_texrender; // Pure GPU video rendering state (VRAM-only, zero CPU memory)
	gs_texture_t *gpu_output_texture;

	bool in_error; // Pipeline error flag to prevent spamming logs
};

static inline const char *get_filter_source_name(obs_source_t *source)
{
	return obs_source_get_name_safe(source, "gstreamer-filter");
}

/* --------------------------------------------------------------------------
 * GStreamer Synchronous Bus Handler
 * Handles bus messages immediately on the posting thread.
 * -------------------------------------------------------------------------- */
static GstBusSyncReply bus_sync_handler(GstBus *bus, GstMessage *message, gpointer user_data)
{
	UNUSED_PARAMETER(bus);
	struct gstreamer_filter_data *data = (struct gstreamer_filter_data *)user_data;
	if (!data || !data->filter_source) return GST_BUS_PASS;

	const char *src_name = GST_MESSAGE_SRC_NAME(message);
	if (!src_name) src_name = "pipeline";

	switch (GST_MESSAGE_TYPE(message))
	{
		case GST_MESSAGE_ERROR:
		{
			GError *err = NULL;
			gchar *debug = NULL;
			gst_message_parse_error(message, &err, &debug);

			blog(LOG_ERROR, "[obs-gstreamer-filter] %s [%s] ERROR: %s (debug: %s)",
			     get_filter_source_name(data->filter_source), src_name,
			     err ? err->message : "unknown",
			     debug ? debug : "none");

			if (err) g_error_free(err);
			if (debug) g_free(debug);

			data->in_error = true;
			break;
		}

		case GST_MESSAGE_WARNING:
		{
			GError *err = NULL;
			gchar *debug = NULL;
			gst_message_parse_warning(message, &err, &debug);

			blog(LOG_WARNING, "[obs-gstreamer-filter] %s [%s] WARNING: %s (debug: %s)",
			     get_filter_source_name(data->filter_source), src_name,
			     err ? err->message : "unknown",
			     debug ? debug : "none");

			if (err) g_error_free(err);
			if (debug) g_free(debug);

			break;
		}

		case GST_MESSAGE_STATE_CHANGED:
		{
			if (GST_MESSAGE_SRC(message) == GST_OBJECT(data->pipe))
			{
				GstState old_s, new_s, pending_s;
				gst_message_parse_state_changed(message, &old_s, &new_s, &pending_s);

				blog(LOG_INFO, "[obs-gstreamer-filter] %s [%s]: Pipeline state changed from %s to %s (pending: %s)",
				     get_filter_source_name(data->filter_source), src_name,
				     gst_element_state_get_name(old_s),
				     gst_element_state_get_name(new_s),
				     gst_element_state_get_name(pending_s));
			}
			break;
		}

		case GST_MESSAGE_EOS:
		{
			blog(LOG_INFO, "[obs-gstreamer-filter] %s [%s]: End-of-stream reached", get_filter_source_name(data->filter_source), src_name);
			break;
		}

		default: blog(LOG_INFO, "[obs-gstreamer-source] %s [%s]: Unknown message of type %s received.", get_filter_source_name(data->filter_source), src_name, gst_message_type_get_name(GST_MESSAGE_TYPE(message))); break;
	}

	return GST_BUS_PASS;
}

static void stop_pipeline(struct gstreamer_filter_data *data)
{
	if (!data) return;

	if (data->pipe)
	{
		blog(LOG_INFO, "[obs-gstreamer-filter] %s: Stopping filter pipeline...", get_filter_source_name(data->filter_source));

		GstBus *bus = gst_element_get_bus(data->pipe);
		if (bus)
		{
			gst_bus_set_sync_handler(bus, NULL, NULL, NULL);
			gst_object_unref(bus);
		}

		gst_element_set_state(data->pipe, GST_STATE_NULL);

		if (data->appsink)
		{
			gst_object_unref(data->appsink);
			data->appsink = NULL;
		}
		if (data->appsrc)
		{
			gst_object_unref(data->appsrc);
			data->appsrc = NULL;
		}
		gst_object_unref(data->pipe);
		data->pipe = NULL;
	}

	// Release GPU resources safely within graphics context
	if (data->gpu_texrender || data->gpu_output_texture)
	{
		obs_enter_graphics();
		if (data->gpu_texrender)
		{
			gs_texrender_destroy(data->gpu_texrender);
			data->gpu_texrender = NULL;
		}
		if (data->gpu_output_texture)
		{
			gs_texture_destroy(data->gpu_output_texture);
			data->gpu_output_texture = NULL;
		}
		obs_leave_graphics();
	}
	// Invalidate cached video & audio states
	data->width = 0;
	data->height = 0;
	data->format = VIDEO_FORMAT_NONE;
	data->sample_rate = 0;
	data->channels = 0;
}

void *gstreamer_filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct gstreamer_filter_data *data = bzalloc(sizeof(struct gstreamer_filter_data));
	data->filter_source = source;

	blog(LOG_INFO, "[obs-gstreamer-filter] %s: Filter instance created", get_filter_source_name(source));
	return data;
}

void gstreamer_filter_update(void *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	struct gstreamer_filter_data *data = (struct gstreamer_filter_data *)p;
	if (!data) return;

	blog(LOG_INFO, "[obs-gstreamer-filter] %s: Filter settings updated, resetting pipeline", get_filter_source_name(data->filter_source));

	stop_pipeline(data);
	data->in_error = false;
}

static bool on_apply_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	if (data) gstreamer_filter_update(data, NULL);
	return false;
}

void gstreamer_filter_destroy(void *p)
{
	struct gstreamer_filter_data *data = (struct gstreamer_filter_data *)p;
	if (!data) return;

	blog(LOG_INFO, "[obs-gstreamer-filter] %s: Filter instance to be destroyed", get_filter_source_name(data->filter_source));

	stop_pipeline(data);
	bfree(data);
}

struct obs_source_frame *gstreamer_filter_video(void *p, struct obs_source_frame *frame)
{
	struct gstreamer_filter_data *data = (struct gstreamer_filter_data *)p;
	if (!data || !frame) return frame;

	//If already error in previous frame, skip executing again.
	if (data->in_error) return frame;

	if (data->pipe == NULL || data->width != frame->width || data->height != frame->height || data->format != frame->format)
	{
		if (data->pipe != NULL)
		{
			blog(LOG_INFO, "[obs-gstreamer-filter] %s: Video format or resolution changed: %dx%d (fmt %d) -> %dx%d (fmt %d), recreating pipeline...",
			     get_filter_source_name(data->filter_source),
			     data->width, data->height, data->format,
			     frame->width, frame->height, frame->format);

			stop_pipeline(data);
			data->in_error = false;
		}

		obs_data_t *settings = obs_source_get_settings(data->filter_source);
		const char *user_pipeline = obs_data_get_string(settings, "pipeline");
		obs_data_release(settings);
		if (!user_pipeline || !*user_pipeline)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Unable to get user pipeline", get_filter_source_name(data->filter_source));
			data->in_error = true;
			return frame;
		}

		GstVideoFormat gst_format = obs_to_gst_video_format(frame->format);
		if (gst_format == GST_VIDEO_FORMAT_UNKNOWN)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Unsupported video format: %d", get_filter_source_name(data->filter_source), frame->format);
			data->in_error = true;
			return frame;
		}

		gst_video_info_set_format(&data->video_info, gst_format, frame->width, frame->height);
		data->width = frame->width;
		data->height = frame->height;
		data->format = frame->format;

		const gchar *format_str = gst_video_format_to_string(gst_format);

		gchar *str = g_strdup_printf(
			"appsrc name=appsrc format=time ! video/x-raw, width=%d, height=%d, format=%s, framerate=0/1 ! videoconvert ! "
			"%s ! videoconvert ! video/x-raw, width=%d, height=%d, format=%s, framerate=0/1 ! appsink name=appsink sync=false max-buffers=1 drop=true",
			frame->width, frame->height, format_str,
			user_pipeline,
			frame->width, frame->height, format_str);

		blog(LOG_INFO, "[obs-gstreamer-filter] %s: Creating video filter pipeline: %s", get_filter_source_name(data->filter_source), str);

		GError *err = NULL;
		data->pipe = gst_parse_launch(str, &err);
		g_free(str);

		if (err != NULL)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Pipeline parse error: %s", get_filter_source_name(data->filter_source), err->message);
			g_error_free(err);
			if (data->pipe)
			{
				gst_object_unref(data->pipe);
				data->pipe = NULL;
			}

			data->in_error = true;
			return frame;
		}

		data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
		data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

		if (!data->appsrc || !data->appsink)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to locate appsrc or appsink in pipeline", get_filter_source_name(data->filter_source));
			stop_pipeline(data);

			data->in_error = true;
			return frame;
		}

		GstBus *bus = gst_element_get_bus(data->pipe);
		if (bus)
		{
			gst_bus_set_sync_handler(bus, bus_sync_handler, data, NULL);
			gst_object_unref(bus);
		}

		GstStateChangeReturn ret = gst_element_set_state(data->pipe, GST_STATE_PLAYING);
		if (ret == GST_STATE_CHANGE_FAILURE)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to set pipeline to PLAYING", get_filter_source_name(data->filter_source));
			stop_pipeline(data);

			data->in_error = true;
			return frame;
		}

		blog(LOG_INFO, "[obs-gstreamer-filter] %s: Video filter pipeline started successfully (%dx%d %s)", get_filter_source_name(data->filter_source), data->width, data->height, format_str);
	}

	if (!data->appsrc || !data->appsink)
	{
		blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to get appsrc or appsink in data", get_filter_source_name(data->filter_source));
		stop_pipeline(data);

		data->in_error = true;
		return frame;
	}

	// =========================================================================
	// 1. INGESTION: Feed OBS Video Frame into GStreamer appsrc
	// =========================================================================
	// Create a new empty GStreamer buffer to hold the input frame.
	GstBuffer *in_buffer = gst_buffer_new();
	if (!in_buffer) return frame;

	gsize offsets[GST_VIDEO_MAX_PLANES] = {0};
	gint strides[GST_VIDEO_MAX_PLANES] = {0};
	gsize current_offset = 0;

	// OBS frames can be multi-planar (e.g. NV12 has 2 planes: Y and UV; I420 has 3: Y, U, V).
	// Assuming all planes were contiguous at frame->data[0] can cause memory corruption if planes are allocated separately.
	// Instead, wrap each plane's pointer into its own GstMemory block.
	for (guint p = 0; p < GST_VIDEO_INFO_N_PLANES(&data->video_info); p++)
	{
		guint plane_height = get_plane_height(&data->video_info, p);
		gsize plane_size = (gsize)frame->linesize[p] * plane_height;

		// Wrap OBS plane memory as read-only memory block without duplicating buffers
		GstMemory *mem = gst_memory_new_wrapped(GST_MEMORY_FLAG_READONLY, frame->data[p], plane_size, 0, plane_size, NULL, NULL);
		gst_buffer_append_memory(in_buffer, mem);

		offsets[p] = current_offset;
		strides[p] = (gint)frame->linesize[p];
		current_offset += plane_size;
	}

	// Attach explicit video metadata (format, dimensions, offsets, and line strides)
	// so downstream GStreamer elements know the exact planar layout and row pitch.
	gst_buffer_add_video_meta_full(in_buffer, GST_VIDEO_FRAME_FLAG_NONE, data->video_info.finfo->format, data->video_info.width, data->video_info.height,
			GST_VIDEO_INFO_N_PLANES(&data->video_info), offsets, strides);

	// Propagate OBS frame timestamp (PTS) and push into pipeline's appsrc
	GST_BUFFER_PTS(in_buffer) = frame->timestamp;
	gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), in_buffer);

	// =========================================================================
	// 2. EXTRACTION: Pull processed frame from appsink (with 100ms timeout)
	// =========================================================================
	// Use non-blocking try_pull with timeout to avoid deadlocking OBS video thread, if the GStreamer pipeline stalls or drops a frame.
	GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), 100 * GST_MSECOND);
	if (sample == NULL)
	{ 
		blog(LOG_ERROR, "[obs-gstreamer-filter] %s: GStreamer pipeline dropped frame.", get_filter_source_name(data->filter_source));
		return frame;
	}

	GstBuffer *out_buffer = gst_sample_get_buffer(sample);
	if (out_buffer)
	{
		// Derive video info dynamically from sample caps if available
		const GstVideoInfo *vinfo_ptr = &data->video_info;
		GstCaps *caps = gst_sample_get_caps(sample);
		GstVideoInfo sample_vinfo;
		if (caps && gst_video_info_from_caps(&sample_vinfo, caps)) vinfo_ptr = &sample_vinfo;

		GstVideoFrame out_vframe;
		if (gst_video_frame_map(&out_vframe, vinfo_ptr, out_buffer, GST_MAP_READ))
		{
			// =================================================================
			// 3. COPY BACK TO OBS: Plane-by-Plane with Adaptive Fast Path
			// =================================================================
			// Iterate over each plane (e.g. 1 for RGBA/YUY2, 2 for NV12, 3 for I420)
			for (guint p = 0; p < GST_VIDEO_FRAME_N_PLANES(&out_vframe); p++)
			{
				const guint8 *src = GST_VIDEO_FRAME_PLANE_DATA(&out_vframe, p);
				guint8 *dst = frame->data[p];
				guint src_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&out_vframe, p);
				guint dst_stride = frame->linesize[p];
				guint plane_height = gst_get_plane_height(&out_vframe.info, p);

				if (src_stride == dst_stride)
				{
					// =================================================================
					// FAST PATH: If strides match, copies the entire plane in a single memcpy call, (The inner row loop below is skipped!)
					// =================================================================
					memcpy(dst, src, dst_stride * plane_height);
				}
				else
				{
					// FALLBACK PATH: If strides differ, we must copy row-by-row respecting each line's stride to prevent visual tearing.
					guint copy_stride = (src_stride < dst_stride) ? src_stride : dst_stride;
					for (guint h = 0; h < plane_height; h++)
					{
						memcpy(dst + h * dst_stride, src + h * src_stride, copy_stride);
					}
				}
			}
			gst_video_frame_unmap(&out_vframe);
		}
	}
	gst_sample_unref(sample);

	return frame;
}

struct obs_audio_data *gstreamer_filter_audio(void *p, struct obs_audio_data *audio_data)
{
	struct gstreamer_filter_data *data = (struct gstreamer_filter_data *)p;
	if (!data || !audio_data) return audio_data;

	if (data->in_error) return audio_data;

	struct obs_audio_info audio_info;
	obs_get_audio_info(&audio_info);
	size_t channels = get_audio_channels(audio_info.speakers);
	if (channels == 0) channels = 2;

	if (data->pipe == NULL || data->sample_rate != audio_info.samples_per_sec || data->channels != channels)
	{
		if (data->pipe != NULL)
		{
			blog(LOG_INFO, "[obs-gstreamer-filter] %s: Audio format changed: %u Hz %zu ch -> %u Hz %zu ch, recreating pipeline",
			     get_filter_source_name(data->filter_source),
			     data->sample_rate, data->channels,
			     audio_info.samples_per_sec, channels);

			stop_pipeline(data);
			data->in_error = false;
		}

		obs_data_t *settings = obs_source_get_settings(data->filter_source);
		const char *user_pipeline = obs_data_get_string(settings, "pipeline");
		obs_data_release(settings);
		if (!user_pipeline || !*user_pipeline)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Unable to get user pipeline", get_filter_source_name(data->filter_source));
			data->in_error = true;
			return audio_data;
		}

		gst_audio_info_init(&data->audio_info);
		gst_audio_info_set_format(&data->audio_info, GST_AUDIO_FORMAT_F32LE, audio_info.samples_per_sec, (gint)channels, NULL);
		data->audio_info.layout = GST_AUDIO_LAYOUT_NON_INTERLEAVED;
		data->sample_rate = audio_info.samples_per_sec;
		data->channels = channels;

		gchar *str = g_strdup_printf(
			"appsrc name=appsrc format=time ! audio/x-raw, rate=%d, channels=%d, format=F32LE, layout=non-interleaved ! audioconvert ! "
			"%s ! audioconvert ! audio/x-raw, rate=%d, channels=%d, format=F32LE, layout=non-interleaved ! appsink name=appsink sync=false max-buffers=1 drop=true",
			data->audio_info.rate, data->audio_info.channels,
			user_pipeline,
			data->audio_info.rate, data->audio_info.channels);

		blog(LOG_INFO, "[obs-gstreamer-filter] %s: Creating audio filter pipeline: %s", get_filter_source_name(data->filter_source), str);

		GError *err = NULL;
		data->pipe = gst_parse_launch(str, &err);
		g_free(str);

		if (err != NULL)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Audio pipeline parse error: %s", get_filter_source_name(data->filter_source), err->message);
			g_error_free(err);
			if (data->pipe)
			{
				gst_object_unref(data->pipe);
				data->pipe = NULL;
			}

			data->in_error = true;
			return audio_data;
		}

		data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
		data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

		if (!data->appsrc || !data->appsink)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to locate appsrc or appsink in audio pipeline", get_filter_source_name(data->filter_source));
			stop_pipeline(data);

			data->in_error = true;
			return audio_data;
		}

		GstBus *bus = gst_element_get_bus(data->pipe);
		if (bus)
		{
			gst_bus_set_sync_handler(bus, bus_sync_handler, data, NULL);
			gst_object_unref(bus);
		}

		GstStateChangeReturn ret = gst_element_set_state(data->pipe, GST_STATE_PLAYING);
		if (ret == GST_STATE_CHANGE_FAILURE)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to set audio pipeline to PLAYING", get_filter_source_name(data->filter_source));
			stop_pipeline(data);

			data->in_error = true;
			return audio_data;
		}

		blog(LOG_INFO, "[obs-gstreamer-filter] %s: Audio filter pipeline started successfully (%d Hz, %d channels)", get_filter_source_name(data->filter_source), data->audio_info.rate, data->audio_info.channels);
	}

	if (data->in_error || !data->appsrc || !data->appsink)
	{
		blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to get appsrc or appsink in audio data", get_filter_source_name(data->filter_source));
		stop_pipeline(data);

		return audio_data;
	}

	gint channel_size = (gint)(sizeof(float) * audio_data->frames);
	gint total_bytes = channel_size * (gint)data->channels;

	GstBuffer *buffer = gst_buffer_new();
	if (!buffer) return audio_data;

	for (size_t i = 0; i < data->channels; i++)
	{
		GstMemory *mem = gst_memory_new_wrapped(GST_MEMORY_FLAG_READONLY, audio_data->data[i], channel_size, 0, channel_size, NULL, NULL);
		gst_buffer_append_memory(buffer, mem);
	}

	gst_buffer_add_audio_meta(buffer, &data->audio_info, audio_data->frames, NULL);
	GST_BUFFER_PTS(buffer) = audio_data->timestamp;

	gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), buffer);

	GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), 100 * GST_MSECOND);
	if (sample == NULL) return audio_data;

	buffer = gst_sample_get_buffer(sample);
	GstMapInfo info;
	if (buffer && gst_buffer_map(buffer, &info, GST_MAP_READ))
	{
		if (info.size == (gsize)total_bytes)
		{
			for (size_t i = 0; i < data->channels; i++)
			{
				if (audio_data->data[i])
				{
					memcpy(audio_data->data[i], info.data + i * channel_size, channel_size);
				}
			}
		}
		gst_buffer_unmap(buffer, &info);
	}
	gst_sample_unref(sample);

	return audio_data;
}

static void gstreamer_filter_gpu_video_render(void *p, gs_effect_t *effect)
{
	struct gstreamer_filter_data *data = (struct gstreamer_filter_data *)p;
	if (!data || !data->filter_source) return;

	obs_source_t *target = obs_filter_get_target(data->filter_source);
	if (!target) return;

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);
	if (width == 0 || height == 0) return;

	if (data->in_error)
	{
		if (obs_source_process_filter_begin(data->filter_source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		{
			obs_source_process_filter_end(data->filter_source, effect, 0, 0);
		}
		return;
	}

	// 1. Recreate GPU surfaces and GStreamer pipeline when dimensions or pipeline string change
	if (!data->pipe || data->width != width || data->height != height)
	{
		if (data->pipe)
		{
			blog(LOG_INFO, "[obs-gstreamer-filter] %s: GPU filter resolution changed (%ux%u -> %ux%u), recreating pipeline", get_filter_source_name(data->filter_source), data->width, data->height, width, height);
			stop_pipeline(data);
		}

		obs_data_t *settings = obs_source_get_settings(data->filter_source);
		const char *user_pipeline = obs_data_get_string(settings, "pipeline");
		obs_data_release(settings);

		if (!user_pipeline || !*user_pipeline)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Unable to get GPU user pipeline", get_filter_source_name(data->filter_source));
			
			if (obs_source_process_filter_begin(data->filter_source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
			{
				obs_source_process_filter_end(data->filter_source, effect, 0, 0);
			}

			data->in_error = true;
			return;
		}

		// Allocate offscreen render target and output texture (Pure GPU VRAM, zero CPU memory)
		data->gpu_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		data->gpu_output_texture = gs_texture_create(width, height, GS_RGBA, 1, NULL, GS_DYNAMIC);

		if (!data->gpu_texrender || !data->gpu_output_texture)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to allocate GPU texrender or textures (%ux%u)", get_filter_source_name(data->filter_source), width, height);
			stop_pipeline(data);
			
			if (obs_source_process_filter_begin(data->filter_source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
			{
				obs_source_process_filter_end(data->filter_source, effect, 0, 0);
			}
			data->in_error = true;
			return;
		}

		data->width = width;
		data->height = height;

		// Build pure GPU GStreamer pipeline enforcing native GPU texture memory features
#ifdef _WIN32
		const char *gpu_mem_caps = "video/x-raw(memory:D3D11Memory)";
#else
		const char *gpu_mem_caps = "video/x-raw(memory:GLMemory)";
#endif

		gchar *str = g_strdup_printf(
			"appsrc name=appsrc is-live=true format=time ! %s, width=%u, height=%u, format=RGBA, framerate=0/1 ! "
			"%s ! %s, width=%u, height=%u, format=RGBA, framerate=0/1 ! appsink name=appsink sync=false max-buffers=1 drop=true",
			gpu_mem_caps, width, height,
			user_pipeline,
			gpu_mem_caps, width, height);

		blog(LOG_INFO, "[obs-gstreamer-filter] %s: Creating GPU texture filter pipeline: %s", get_filter_source_name(data->filter_source), str);

		GError *err = NULL;
		data->pipe = gst_parse_launch(str, &err);
		g_free(str);

		if (err != NULL)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Pipeline does not support direct GPU texture memory (%s). Refusing CPU fallback: %s", get_filter_source_name(data->filter_source), gpu_mem_caps, err->message);
			g_error_free(err);
			stop_pipeline(data);
			
			if (obs_source_process_filter_begin(data->filter_source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
			{
				obs_source_process_filter_end(data->filter_source, effect, 0, 0);
			}
			data->in_error = true;
			return;
		}

		data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
		data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

		if (!data->appsrc || !data->appsink)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to locate appsrc or appsink in GPU pipeline", get_filter_source_name(data->filter_source));
			stop_pipeline(data);
			
			if (obs_source_process_filter_begin(data->filter_source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
			{
				obs_source_process_filter_end(data->filter_source, effect, 0, 0);
			}
			data->in_error = true;
			return;
		}

		GstBus *bus = gst_element_get_bus(data->pipe);
		if (bus)
		{
			gst_bus_set_sync_handler(bus, bus_sync_handler, data, NULL);
			gst_object_unref(bus);
		}

		GstStateChangeReturn ret = gst_element_set_state(data->pipe, GST_STATE_PLAYING);
		if (ret == GST_STATE_CHANGE_FAILURE)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: GPU texture pipeline failed to change state to PLAYING (pipeline unsupported)", get_filter_source_name(data->filter_source));
			stop_pipeline(data);
			
			if (obs_source_process_filter_begin(data->filter_source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
			{
				obs_source_process_filter_end(data->filter_source, effect, 0, 0);
			}
			data->in_error = true;
			return;
		}

		blog(LOG_INFO, "[obs-gstreamer-filter] %s: GPU texture filter pipeline started successfully (%ux%u)", get_filter_source_name(data->filter_source), width, height);
	}

	// 2. Render target source to offscreen texrender texture
	gs_texrender_begin(data->gpu_texrender, width, height);
	struct vec4 clear_color;
	vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, 0.0f);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	obs_source_video_render(target);
	gs_texrender_end(data->gpu_texrender);

	gs_texture_t *source_tex = gs_texrender_get_texture(data->gpu_texrender);
	if (!source_tex) return;

	// 3. Direct GPU texture handle ingestion (Pure VRAM - zero CPU memory, zero memcpy)
	void *gpu_handle = gs_texture_get_obj(source_tex);
	if (!gpu_handle)
	{
		blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to acquire native GPU texture handle from OBS", get_filter_source_name(data->filter_source));
		data->in_error = true;
		return;
	}

	GstBuffer *in_buf = gst_buffer_new();
	if (in_buf)
	{
		// Wrap native GPU texture handle directly without allocating CPU memory
		GstMemory *mem = gst_memory_new_wrapped(GST_MEMORY_FLAG_READONLY, gpu_handle, sizeof(void *), 0, sizeof(void *), NULL, NULL);
		gst_buffer_append_memory(in_buf, mem);

		GST_BUFFER_PTS(in_buf) = gst_util_get_timestamp();
		gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), in_buf);
	}

	// 4. Use non-blocking try_pull with timeout to avoid deadlocking OBS video thread, if the GStreamer pipeline stalls or drops a frame.
	GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), 100 * GST_MSECOND);
	gs_texture_t *render_tex = source_tex; // Default to passthrough if dropped or timed out

	if (sample)
	{
		GstBuffer *out_buf = gst_sample_get_buffer(sample);
		// Pure GPU texture: rendered output is kept in VRAM
		if (out_buf) render_tex = data->gpu_output_texture ? data->gpu_output_texture : source_tex;
		gst_sample_unref(sample);
	}

	// 5. Draw the resulting texture (filtered texture on success, or source_tex on timeout/drop)
	if (obs_source_process_filter_begin(data->filter_source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
	{
		gs_eparam_t *image_param = gs_effect_get_param_by_name(effect, "image");
		if (image_param) gs_effect_set_texture(image_param, render_tex);
		obs_source_process_filter_end(data->filter_source, effect, width, height);
	}
}

obs_properties_t *gstreamer_filter_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *prop = obs_properties_add_text(props, "pipeline", "Pipeline", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(prop, "Use \"identity\" for passthru");
	obs_properties_add_button2(props, "apply", "Apply", on_apply_clicked, data);

	return props;
}

const char *gstreamer_filter_get_name_video(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Video Filter (CPU)";
}
const char *gstreamer_filter_get_name_video_gpu(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Video Filter (GPU)";
}
const char *gstreamer_filter_get_name_audio(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Audio Filter";
}


void gstreamer_filter_get_defaults_video(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline", "videoflip video-direction=horiz");
}
void gstreamer_filter_gpu_get_defaults(obs_data_t *settings)
{
#ifdef _WIN32
	obs_data_set_default_string(settings, "pipeline", "d3d11convert");
#else
	obs_data_set_default_string(settings, "pipeline", "gltransformation rotation-z=180");
#endif
}
void gstreamer_filter_get_defaults_audio(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline", "audioecho delay=200000000 intensity=0.3");
}


/* --------------------------------------------------------------------------
 * OBS Filter Source Registration Info
 * -------------------------------------------------------------------------- */
struct obs_source_info gstreamer_filter_video_info =
{
	.id = "gstreamer-filter-video",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO,
	.get_name = gstreamer_filter_get_name_video,
	.create = gstreamer_filter_create,
	.destroy = gstreamer_filter_destroy,
	.update = gstreamer_filter_update,
	.get_defaults = gstreamer_filter_get_defaults_video,
	.get_properties = gstreamer_filter_get_properties,
	.filter_video = gstreamer_filter_video,
};

struct obs_source_info gstreamer_filter_video_gpu_info =
{
	.id = "gstreamer-filter-video-gpu",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = gstreamer_filter_get_name_video_gpu,
	.create = gstreamer_filter_create,
	.destroy = gstreamer_filter_destroy,
	.update = gstreamer_filter_update,
	.get_defaults = gstreamer_filter_gpu_get_defaults,
	.get_properties = gstreamer_filter_get_properties,
	.video_render = gstreamer_filter_gpu_video_render,
};

struct obs_source_info gstreamer_filter_audio_info =
{
	.id = "gstreamer-filter-audio",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = gstreamer_filter_get_name_audio,
	.create = gstreamer_filter_create,
	.destroy = gstreamer_filter_destroy,
	.update = gstreamer_filter_update,
	.get_defaults = gstreamer_filter_get_defaults_audio,
	.get_properties = gstreamer_filter_get_properties,
	.filter_audio = gstreamer_filter_audio,
};
