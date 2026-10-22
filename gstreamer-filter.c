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

struct gstreamer_filter_data
{
	obs_source_t *source;
	obs_data_t *settings;
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

	bool in_error; // Pipeline error flag to prevent spamming logs
};

static void stop_pipeline(struct gstreamer_filter_data *data);

/* --------------------------------------------------------------------------
 * GStreamer Synchronous Bus Handler
 * Handles bus messages immediately on the posting thread.
 * -------------------------------------------------------------------------- */
static GstBusSyncReply bus_sync_handler(GstBus *bus, GstMessage *message, gpointer user_data)
{
	UNUSED_PARAMETER(bus);
	struct gstreamer_filter_data *data = (struct gstreamer_filter_data *)user_data;
	if (!data || !data->source) return GST_BUS_PASS;

	const char *src_name = GST_MESSAGE_SRC_NAME(message);
	const char *source_name = obs_source_get_name(data->source);

	switch (GST_MESSAGE_TYPE(message))
	{
		case GST_MESSAGE_ERROR:
		{
			GError *err = NULL;
			gchar *debug = NULL;
			gst_message_parse_error(message, &err, &debug);

			blog(LOG_ERROR, "[obs-gstreamer-filter] %s [%s] ERROR: %s (debug: %s)",
			     source_name ? source_name : "filter",
			     src_name ? src_name : "pipeline",
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
			     source_name ? source_name : "filter",
			     src_name ? src_name : "pipeline",
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

				blog(LOG_INFO, "[obs-gstreamer-filter] %s: Pipeline state changed from %s to %s (pending: %s)",
				     source_name ? source_name : "filter",
				     gst_element_state_get_name(old_s),
				     gst_element_state_get_name(new_s),
				     gst_element_state_get_name(pending_s));
			}
			break;
		}

		case GST_MESSAGE_EOS:
		{
			blog(LOG_INFO, "[obs-gstreamer-filter] %s: End-of-stream reached", source_name ? source_name : "filter");
			break;
		}

		default: break;
	}

	return GST_BUS_PASS;
}

static void stop_pipeline(struct gstreamer_filter_data *data)
{
	if (!data || !data->pipe) return;

	const char *source_name = data->source ? obs_source_get_name(data->source) : "filter";
	blog(LOG_INFO, "[obs-gstreamer-filter] %s: Stopping filter pipeline", source_name ? source_name : "filter");

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

static GstVideoFormat obs_to_gst_video_format(enum video_format format)
{
	switch (format)
	{
		case VIDEO_FORMAT_I420: return GST_VIDEO_FORMAT_I420; break;
		case VIDEO_FORMAT_NV12: return GST_VIDEO_FORMAT_NV12; break;
		case VIDEO_FORMAT_I422: return GST_VIDEO_FORMAT_Y42B; break;
		case VIDEO_FORMAT_YVYU: return GST_VIDEO_FORMAT_YVYU; break;
		case VIDEO_FORMAT_YUY2: return GST_VIDEO_FORMAT_YUY2; break;
		case VIDEO_FORMAT_UYVY: return GST_VIDEO_FORMAT_UYVY; break;
		case VIDEO_FORMAT_RGBA: return GST_VIDEO_FORMAT_RGBA; break;
		case VIDEO_FORMAT_BGRA: return GST_VIDEO_FORMAT_BGRA; break;
		case VIDEO_FORMAT_BGRX: return GST_VIDEO_FORMAT_BGRx; break;
		default: return GST_VIDEO_FORMAT_UNKNOWN; break;
	}
}

static inline guint get_plane_height(const GstVideoInfo *info, guint plane)
{
	if (plane == 0 || GST_VIDEO_INFO_N_PLANES(info) == 1) return GST_VIDEO_INFO_HEIGHT(info);

	switch (GST_VIDEO_INFO_FORMAT(info))
	{
		case GST_VIDEO_FORMAT_I420: return (GST_VIDEO_INFO_HEIGHT(info) + 1) / 2; break;
		case GST_VIDEO_FORMAT_NV12: return (GST_VIDEO_INFO_HEIGHT(info) + 1) / 2; break;
		case GST_VIDEO_FORMAT_Y42B: return GST_VIDEO_INFO_HEIGHT(info); break;
		default: return GST_VIDEO_INFO_HEIGHT(info); break;
	}
}

const char *gstreamer_filter_get_name_video(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Video Filter";
}

const char *gstreamer_filter_get_name_audio(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Audio Filter";
}

void *gstreamer_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct gstreamer_filter_data *data = g_new0(struct gstreamer_filter_data, 1);
	data->source = source;
	data->settings = settings;

	const char *source_name = source ? obs_source_get_name(source) : "filter";
	blog(LOG_INFO, "[obs-gstreamer-filter] %s: Filter instance created", source_name ? source_name : "filter");

	return data;
}

void gstreamer_filter_destroy(void *p)
{
	struct gstreamer_filter_data *data = (struct gstreamer_filter_data *)p;
	if (!data) return;

	const char *source_name = data->source ? obs_source_get_name(data->source) : "filter";
	blog(LOG_INFO, "[obs-gstreamer-filter] %s: Filter instance destroyed", source_name ? source_name : "filter");

	stop_pipeline(data);
	g_free(data);
}

void gstreamer_filter_get_defaults_video(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline", "videoflip video-direction=horiz");
}

void gstreamer_filter_get_defaults_audio(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline", "audioecho delay=200000000 intensity=0.3");
}

void gstreamer_filter_update(void *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	struct gstreamer_filter_data *data = (struct gstreamer_filter_data *)p;
	if (!data) return;

	const char *source_name = data->source ? obs_source_get_name(data->source) : "filter";
	blog(LOG_INFO, "[obs-gstreamer-filter] %s: Filter settings updated, resetting pipeline", source_name ? source_name : "filter");

	stop_pipeline(data);
	data->in_error = false;
	data->width = 0;
	data->height = 0;
	data->format = VIDEO_FORMAT_NONE;
	data->sample_rate = 0;
	data->channels = 0;
}

static bool on_apply_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct gstreamer_filter_data *filter_data = (struct gstreamer_filter_data *)data;
	if (filter_data) gstreamer_filter_update(filter_data, filter_data->settings);
	return false;
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

struct obs_source_frame *
gstreamer_filter_filter_video(void *p, struct obs_source_frame *frame)
{
	struct gstreamer_filter_data *data = (struct gstreamer_filter_data *)p;
	if (!data || !frame) return frame;

	if (data->pipe == NULL || data->width != frame->width || data->height != frame->height || data->format != frame->format)
	{
		if (data->pipe != NULL)
		{
			const char *source_name = data->source ? obs_source_get_name(data->source) : "filter";

			blog(LOG_INFO, "[obs-gstreamer-filter] %s: Video format or resolution changed: %dx%d (fmt %d) -> %dx%d (fmt %d), recreating pipeline",
			     source_name ? source_name : "filter",
			     data->width, data->height, data->format,
			     frame->width, frame->height, frame->format);

			stop_pipeline(data);
			data->in_error = false;
		}

		if (data->in_error) return frame;
		

		GstVideoFormat gst_format = obs_to_gst_video_format(frame->format);
		if (gst_format == GST_VIDEO_FORMAT_UNKNOWN)
		{
			const char *source_name = data->source ? obs_source_get_name(data->source) : "filter";
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Unsupported video format: %d", source_name ? source_name : "filter", frame->format);
			data->in_error = true;
			return frame;
		}

		const char *user_pipeline = obs_data_get_string(data->settings, "pipeline");
		if (!user_pipeline || !*user_pipeline) return frame;
		

		gst_video_info_set_format(&data->video_info, gst_format, frame->width, frame->height);
		data->width = frame->width;
		data->height = frame->height;
		data->format = frame->format;

		const gchar *format_str = gst_video_format_to_string(gst_format);
		const char *source_name = data->source ? obs_source_get_name(data->source) : "filter";

		gchar *str = g_strdup_printf(
			"appsrc name=appsrc format=time ! video/x-raw, width=%d, height=%d, format=%s, framerate=0/1 ! videoconvert ! "
			"%s ! videoconvert ! video/x-raw, width=%d, height=%d, format=%s, framerate=0/1 ! appsink name=appsink sync=false max-buffers=1 drop=true",
			frame->width, frame->height, format_str,
			user_pipeline,
			frame->width, frame->height, format_str);

		blog(LOG_INFO, "[obs-gstreamer-filter] %s: Creating video filter pipeline: %s", source_name ? source_name : "filter", str);

		GError *err = NULL;
		data->pipe = gst_parse_launch(str, &err);
		g_free(str);

		if (err != NULL)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Pipeline parse error: %s", source_name ? source_name : "filter", err->message);
			g_error_free(err);
			data->in_error = true;

			if (data->pipe)
			{
				gst_object_unref(data->pipe);
				data->pipe = NULL;
			}
			return frame;
		}

		data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
		data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

		if (!data->appsrc || !data->appsink)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to locate appsrc or appsink in pipeline", source_name ? source_name : "filter");
			data->in_error = true;
			stop_pipeline(data);
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
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to set pipeline to PLAYING", source_name ? source_name : "filter");
			data->in_error = true;
			stop_pipeline(data);
			return frame;
		}

		blog(LOG_INFO, "[obs-gstreamer-filter] %s: Video filter pipeline started successfully (%dx%d %s)", source_name ? source_name : "filter", data->width, data->height, format_str);
	}

	if (data->in_error || !data->appsrc || !data->appsink) return frame;

	GstBuffer *in_buffer = gst_buffer_new_allocate(NULL, data->video_info.size, NULL);
	if (!in_buffer) return frame;

	GstVideoFrame in_vframe;
	if (gst_video_frame_map(&in_vframe, &data->video_info, in_buffer, GST_MAP_WRITE))
	{
		for (guint p = 0; p < GST_VIDEO_FRAME_N_PLANES(&in_vframe); p++)
		{
			const guint8 *src = frame->data[p];
			guint8 *dst = GST_VIDEO_FRAME_PLANE_DATA(&in_vframe, p);
			guint src_stride = frame->linesize[p];
			guint dst_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&in_vframe, p);
			guint plane_height = get_plane_height(&data->video_info, p);

			if (src_stride == dst_stride)
			{
				memcpy(dst, src, dst_stride * plane_height);
			}
			else
			{
				guint copy_stride = (src_stride < dst_stride) ? src_stride : dst_stride;
				for (guint h = 0; h < plane_height; h++)
				{
					memcpy(dst + h * dst_stride, src + h * src_stride, copy_stride);
				}
			}
		}
		gst_video_frame_unmap(&in_vframe);
	}

	GST_BUFFER_PTS(in_buffer) = frame->timestamp;
	gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), in_buffer);

	GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), 100 * GST_MSECOND);
	if (sample == NULL) return frame;

	GstBuffer *out_buffer = gst_sample_get_buffer(sample);
	if (out_buffer)
	{
		GstVideoFrame out_vframe;
		if (gst_video_frame_map(&out_vframe, &data->video_info, out_buffer, GST_MAP_READ))
		{
			for (guint p = 0; p < GST_VIDEO_FRAME_N_PLANES(&out_vframe); p++)
			{
				const guint8 *src = GST_VIDEO_FRAME_PLANE_DATA(&out_vframe, p);
				guint8 *dst = frame->data[p];
				guint src_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&out_vframe, p);
				guint dst_stride = frame->linesize[p];
				guint plane_height = get_plane_height(&data->video_info, p);

				if (src_stride == dst_stride)
				{
					memcpy(dst, src, dst_stride * plane_height);
				}
				else
				{
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

struct obs_audio_data *
gstreamer_filter_filter_audio(void *p, struct obs_audio_data *audio_data)
{
	struct gstreamer_filter_data *data = (struct gstreamer_filter_data *)p;
	if (!data || !audio_data) return audio_data;

	struct obs_audio_info audio_info;
	obs_get_audio_info(&audio_info);
	size_t channels = get_audio_channels(audio_info.speakers);
	if (channels == 0) channels = 2;

	if (data->pipe == NULL || data->sample_rate != audio_info.samples_per_sec || data->channels != channels)
	{
		if (data->pipe != NULL)
		{
			const char *source_name = data->source ? obs_source_get_name(data->source) : "filter";

			blog(LOG_INFO, "[obs-gstreamer-filter] %s: Audio format changed: %u Hz %zu ch -> %u Hz %zu ch, recreating pipeline",
			     source_name ? source_name : "filter",
			     data->sample_rate, data->channels,
			     audio_info.samples_per_sec, channels);

			stop_pipeline(data);
			data->in_error = false;
		}

		if (data->in_error) return audio_data;
		
		const char *user_pipeline = obs_data_get_string(data->settings, "pipeline");
		if (!user_pipeline || !*user_pipeline) return audio_data;
		

		gst_audio_info_init(&data->audio_info);
		gst_audio_info_set_format(&data->audio_info, GST_AUDIO_FORMAT_F32LE, audio_info.samples_per_sec, (gint)channels, NULL);
		data->audio_info.layout = GST_AUDIO_LAYOUT_NON_INTERLEAVED;

		data->sample_rate = audio_info.samples_per_sec;
		data->channels = channels;

		const char *source_name = data->source ? obs_source_get_name(data->source) : "filter";

		gchar *str = g_strdup_printf(
			"appsrc name=appsrc format=time ! audio/x-raw, rate=%d, channels=%d, format=F32LE, layout=non-interleaved ! audioconvert ! "
			"%s ! audioconvert ! audio/x-raw, rate=%d, channels=%d, format=F32LE, layout=non-interleaved ! appsink name=appsink sync=false max-buffers=1 drop=true",
			data->audio_info.rate, data->audio_info.channels,
			user_pipeline,
			data->audio_info.rate, data->audio_info.channels);

		blog(LOG_INFO, "[obs-gstreamer-filter] %s: Creating audio filter pipeline: %s", source_name ? source_name : "filter", str);

		GError *err = NULL;
		data->pipe = gst_parse_launch(str, &err);
		g_free(str);

		if (err != NULL)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Audio pipeline parse error: %s", source_name ? source_name : "filter", err->message);
			g_error_free(err);
			data->in_error = true;

			if (data->pipe)
			{
				gst_object_unref(data->pipe);
				data->pipe = NULL;
			}
			return audio_data;
		}

		data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
		data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

		if (!data->appsrc || !data->appsink)
		{
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to locate appsrc or appsink in audio pipeline", source_name ? source_name : "filter");
			data->in_error = true;
			stop_pipeline(data);
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
			blog(LOG_ERROR, "[obs-gstreamer-filter] %s: Failed to set audio pipeline to PLAYING", source_name ? source_name : "filter");
			data->in_error = true;
			stop_pipeline(data);
			return audio_data;
		}

		blog(LOG_INFO, "[obs-gstreamer-filter] %s: Audio filter pipeline started successfully (%d Hz, %d channels)", source_name ? source_name : "filter", data->audio_info.rate, data->audio_info.channels);
	}

	if (data->in_error || !data->appsrc || !data->appsink) return audio_data;

	gint channel_size = (gint)(sizeof(float) * audio_data->frames);
	gint total_bytes = channel_size * (gint)data->channels;

	GstBuffer *buffer = gst_buffer_new_allocate(NULL, total_bytes, NULL);
	if (!buffer) return audio_data;

	GstMapInfo info;
	if (gst_buffer_map(buffer, &info, GST_MAP_WRITE))
	{
		for (size_t i = 0; i < data->channels; i++)
		{
			if (audio_data->data[i])
			{
				memcpy(info.data + i * channel_size, audio_data->data[i], channel_size);
			}
		}
		gst_buffer_unmap(buffer, &info);
	}

	gst_buffer_add_audio_meta(buffer, &data->audio_info, audio_data->frames, NULL);
	GST_BUFFER_PTS(buffer) = audio_data->timestamp;

	gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), buffer);

	GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), 100 * GST_MSECOND);
	if (sample == NULL) return audio_data;

	buffer = gst_sample_get_buffer(sample);
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
	.filter_video = gstreamer_filter_filter_video,
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
	.filter_audio = gstreamer_filter_filter_audio,
};
