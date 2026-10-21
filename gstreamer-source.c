/*
 * obs-gstreamer. OBS Studio plugin.
 * Copyright (C) 2018-2021 Florian Zwoch <fzwoch@gmail.com>
 *
 * Streamlined Zero-Thread Architecture with Full User Pipeline Support.
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
#include <string.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <gst/app/app.h>

struct gstreamer_source_data
{
	obs_source_t *gstreamer_source;
	GstElement   *pipe;

	
	bool use_timestamps_video; //cached settings flags (since accessed in hot path)
	bool use_timestamps_audio;
	
	GstCaps *last_video_caps; // Cached Video Format State (Calculated ONLY on caps change)
	size_t video_offset[3];
	struct obs_source_frame cached_frame; // Pre-filled video frame template
	
	GstCaps *last_audio_caps; // Cached Audio Format State (Calculated ONLY on caps change)
	uint32_t audio_bpf;
	struct obs_source_audio cached_audio; // Pre-filled audio packet template

	enum obs_media_state obs_media_state;
};

static void start(struct gstreamer_source_data *data, obs_data_t *settings);
static void stop(struct gstreamer_source_data *data);

/* --------------------------------------------------------------------------
 * GStreamer Synchronous Bus Handler (Zero Additional Threading)
 * Handles bus messages immediately on the posting thread.
 * -------------------------------------------------------------------------- */
static GstBusSyncReply bus_sync_handler(GstBus *bus, GstMessage *message, gpointer user_data)
{
	UNUSED_PARAMETER(bus);
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data || !data->gstreamer_source) return GST_BUS_DROP;

	switch (GST_MESSAGE_TYPE(message))
	{
		case GST_MESSAGE_ERROR:
		{
			GError *err = NULL;
			gchar *debug = NULL;
			gst_message_parse_error(message, &err, &debug);
			const char *source_name = obs_source_get_name(data->gstreamer_source);
			const char *src_name = GST_MESSAGE_SRC_NAME(message);

			blog(LOG_ERROR, "[obs-gstreamer-source] %s [%s] ERROR: %s (debug: %s)",
					source_name ? source_name : "gstreamer-source",
					src_name ? src_name : "pipeline",
					err ? err->message : "unknown",
					debug ? debug : "none");

			if (err) g_error_free(err);
			if (debug) g_free(debug);

			data->obs_media_state = OBS_MEDIA_STATE_ERROR;

			// Read clear_on_end
			obs_data_t *settings = obs_source_get_settings(data->gstreamer_source);
			if (obs_data_get_bool(settings, "clear_on_end")) obs_source_output_video(data->gstreamer_source, NULL);
			obs_data_release(settings);

			return GST_BUS_DROP;
		}

		case GST_MESSAGE_WARNING:
		{
			GError *err = NULL;
			gchar *debug = NULL;
			gst_message_parse_warning(message, &err, &debug);
			const char *source_name = obs_source_get_name(data->gstreamer_source);
			const char *src_name = GST_MESSAGE_SRC_NAME(message);

			blog(LOG_WARNING, "[obs-gstreamer-source] %s [%s] WARNING: %s (debug: %s)",
					source_name ? source_name : "gstreamer-source",
					src_name ? src_name : "pipeline",
					err ? err->message : "unknown",
					debug ? debug : "none");

			if (err) g_error_free(err);
			if (debug) g_free(debug);

			return GST_BUS_DROP;
		}

		case GST_MESSAGE_EOS:
		{
			data->obs_media_state = OBS_MEDIA_STATE_ENDED;

			// Read clear_on_end
			obs_data_t *settings = obs_source_get_settings(data->gstreamer_source);
			if (obs_data_get_bool(settings, "clear_on_end")) obs_source_output_video(data->gstreamer_source, NULL);
			obs_data_release(settings);

			return GST_BUS_DROP;
		}

		case GST_MESSAGE_BUFFERING:
		{
			gint percent = 0;
			gst_message_parse_buffering(message, &percent);
			data->obs_media_state = (percent < 100) ? OBS_MEDIA_STATE_BUFFERING : OBS_MEDIA_STATE_PLAYING;
			return GST_BUS_DROP;
		}

		case GST_MESSAGE_STATE_CHANGED:
		{
			if (data->pipe && GST_MESSAGE_SRC(message) == GST_OBJECT(data->pipe))
			{
				GstState old_s, new_s;
				gst_message_parse_state_changed(message, &old_s, &new_s, NULL);

				if (new_s == GST_STATE_PLAYING) data->obs_media_state = OBS_MEDIA_STATE_PLAYING;
				else if (new_s == GST_STATE_PAUSED) data->obs_media_state = OBS_MEDIA_STATE_PAUSED;
				else if (new_s == GST_STATE_READY || new_s == GST_STATE_NULL) data->obs_media_state = OBS_MEDIA_STATE_STOPPED;
			}
			return GST_BUS_DROP;
		}

		default: break;
	}

	return GST_BUS_DROP;
}

/* --------------------------------------------------------------------------
 * Update Video Format Cache (Called ONLY on stream start or caps change)
 * -------------------------------------------------------------------------- */
static bool update_video_format_cache(struct gstreamer_source_data *data, GstCaps *caps)
{
	if (!data || !caps) return false;

	bool initial = (data->last_video_caps == NULL);
	const char *source_name = data->gstreamer_source ? obs_source_get_name(data->gstreamer_source) : "gstreamer-source";
	gchar *caps_str = gst_caps_to_string(caps);
	blog(LOG_INFO, "[obs-gstreamer-source] %s: Video caps %s: %s",
			source_name ? source_name : "gstreamer-source",
			initial ? "initial set" : "changed",
			caps_str ? caps_str : "unknown");
	if (caps_str) g_free(caps_str);

	if (data->last_video_caps) gst_caps_unref(data->last_video_caps);
	data->last_video_caps = gst_caps_ref(caps);

	GstVideoInfo v_info;
	if (!gst_video_info_from_caps(&v_info, caps))
	{
		data->cached_frame.format = VIDEO_FORMAT_NONE;
		return false;
	}

	// Pre-fill the persistent cached frame template (zero-initialized without memset)
	data->cached_frame = (struct obs_source_frame){ 0 };
	data->cached_frame.width = v_info.width;
	data->cached_frame.height = v_info.height;
	data->cached_frame.linesize[0] = v_info.stride[0];
	data->cached_frame.linesize[1] = v_info.stride[1];
	data->cached_frame.linesize[2] = v_info.stride[2];

	data->video_offset[0] = v_info.offset[0];
	data->video_offset[1] = v_info.offset[1];
	data->video_offset[2] = v_info.offset[2];

	enum video_range_type range = VIDEO_RANGE_DEFAULT;
	switch (v_info.colorimetry.range)
	{
		case GST_VIDEO_COLOR_RANGE_0_255: range = VIDEO_RANGE_FULL; data->cached_frame.full_range = true; break;
		case GST_VIDEO_COLOR_RANGE_16_235: range = VIDEO_RANGE_PARTIAL; data->cached_frame.full_range = false; break;
		default: data->cached_frame.full_range = false; break;
	}

	enum video_colorspace cs = VIDEO_CS_DEFAULT;
	switch (v_info.colorimetry.matrix)
	{
		case GST_VIDEO_COLOR_MATRIX_BT709: cs = VIDEO_CS_709; break;
		case GST_VIDEO_COLOR_MATRIX_BT601: cs = VIDEO_CS_601; break;
		default: break;
	}

	video_format_get_parameters(cs, range, data->cached_frame.color_matrix, data->cached_frame.color_range_min, data->cached_frame.color_range_max);

	switch (v_info.finfo->format)
	{
		case GST_VIDEO_FORMAT_I420: data->cached_frame.format = VIDEO_FORMAT_I420; break;
		case GST_VIDEO_FORMAT_NV12: data->cached_frame.format = VIDEO_FORMAT_NV12; break;
		case GST_VIDEO_FORMAT_BGRA: data->cached_frame.format = VIDEO_FORMAT_BGRA; break;
		case GST_VIDEO_FORMAT_BGRx: data->cached_frame.format = VIDEO_FORMAT_BGRX; break;
		case GST_VIDEO_FORMAT_RGBx: data->cached_frame.format = VIDEO_FORMAT_RGBA; break;
		case GST_VIDEO_FORMAT_RGBA: data->cached_frame.format = VIDEO_FORMAT_RGBA; break;
		case GST_VIDEO_FORMAT_UYVY: data->cached_frame.format = VIDEO_FORMAT_UYVY; break;
		case GST_VIDEO_FORMAT_YUY2: data->cached_frame.format = VIDEO_FORMAT_YUY2; break;
		case GST_VIDEO_FORMAT_YVYU: data->cached_frame.format = VIDEO_FORMAT_YVYU; break;

#ifdef GST_VIDEO_FORMAT_I420_10LE
		case GST_VIDEO_FORMAT_I420_10LE: data->cached_frame.format = VIDEO_FORMAT_I010; break;
		case GST_VIDEO_FORMAT_P010_10LE: data->cached_frame.format = VIDEO_FORMAT_P010; break;
		case GST_VIDEO_FORMAT_I422_10LE: data->cached_frame.format = VIDEO_FORMAT_I210; break;
		case GST_VIDEO_FORMAT_Y444_12LE: data->cached_frame.format = VIDEO_FORMAT_I412; break;
#endif

		default:
			data->cached_frame.format = VIDEO_FORMAT_NONE;
			blog(LOG_ERROR, "[obs-gstreamer-source] Unknown video format: %s", v_info.finfo ? v_info.finfo->name : "unspecified");
			break;
	}

	return (data->cached_frame.format != VIDEO_FORMAT_NONE);
}

/* --------------------------------------------------------------------------
 * Update Audio Format Cache (Called ONLY on stream start or caps change)
 * -------------------------------------------------------------------------- */
static bool update_audio_format_cache(struct gstreamer_source_data *data, GstCaps *caps)
{
	if (!data || !caps) return false;

	bool initial = (data->last_audio_caps == NULL);
	const char *source_name = data->gstreamer_source ? obs_source_get_name(data->gstreamer_source) : "gstreamer-source";
	gchar *caps_str = gst_caps_to_string(caps);
	blog(LOG_INFO, "[obs-gstreamer-source] %s: Audio caps %s: %s",
			source_name ? source_name : "gstreamer-source",
			initial ? "initial set" : "changed",
			caps_str ? caps_str : "unknown");
	if (caps_str) g_free(caps_str);

	if (data->last_audio_caps) gst_caps_unref(data->last_audio_caps);
	data->last_audio_caps = gst_caps_ref(caps);

	GstAudioInfo a_info;
	if (!gst_audio_info_from_caps(&a_info, caps))
	{
		data->cached_audio.format = AUDIO_FORMAT_UNKNOWN;
		return false;
	}

	data->cached_audio = (struct obs_source_audio){ 0 };
	data->cached_audio.samples_per_sec = a_info.rate;
	data->audio_bpf = a_info.bpf;

	switch (a_info.channels)
	{
		case 1: data->cached_audio.speakers = SPEAKERS_MONO; break;
		case 2: data->cached_audio.speakers = SPEAKERS_STEREO; break;
		case 3: data->cached_audio.speakers = SPEAKERS_2POINT1; break;
		case 4: data->cached_audio.speakers = SPEAKERS_4POINT0; break;
		case 5: data->cached_audio.speakers = SPEAKERS_4POINT1; break;
		case 6: data->cached_audio.speakers = SPEAKERS_5POINT1; break;
		case 8: data->cached_audio.speakers = SPEAKERS_7POINT1; break;
		default: data->cached_audio.speakers = SPEAKERS_UNKNOWN; break;
	}

	switch (a_info.finfo->format)
	{
		case GST_AUDIO_FORMAT_U8: data->cached_audio.format = AUDIO_FORMAT_U8BIT; break;
		case GST_AUDIO_FORMAT_S16LE: data->cached_audio.format = AUDIO_FORMAT_16BIT; break;
		case GST_AUDIO_FORMAT_S32LE: data->cached_audio.format = AUDIO_FORMAT_32BIT; break;
		case GST_AUDIO_FORMAT_F32LE: data->cached_audio.format = AUDIO_FORMAT_FLOAT; break;
		default: data->cached_audio.format = AUDIO_FORMAT_UNKNOWN; break;
	}

	return (data->cached_audio.format != AUDIO_FORMAT_UNKNOWN && data->audio_bpf > 0);
}

/* --------------------------------------------------------------------------
 * Video Sample Pull Callback (Hot path: executed 60-120x/sec)
 * -------------------------------------------------------------------------- */
static GstFlowReturn video_new_sample(GstAppSink *appsink, gpointer user_data)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data || !data->gstreamer_source) return GST_FLOW_OK;

	GstSample *sample = gst_app_sink_pull_sample(appsink);
	if (!sample) return GST_FLOW_OK;

	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstCaps *caps = gst_sample_get_caps(sample);
	if (!buffer || !caps)
	{
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	// 2-tier caps check: fast pointer compare + semantic equality
	if (caps != data->last_video_caps)
	{
		if (!data->last_video_caps || !gst_caps_is_equal(caps, data->last_video_caps))
		{
			if (!update_video_format_cache(data, caps))
			{
				gst_sample_unref(sample);
				return GST_FLOW_OK;
			}
		}
		else
		{
			gst_caps_unref(data->last_video_caps);
			data->last_video_caps = gst_caps_ref(caps);
		}
	}

	GstMapInfo info;
	if (!gst_buffer_map(buffer, &info, GST_MAP_READ))
	{
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	// Single struct assignment copies all pre-calculated dimensions, format, linesizes & color matrix
	struct obs_source_frame frame = data->cached_frame;

	if (data->use_timestamps_video && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) frame.timestamp = GST_BUFFER_PTS(buffer);
	else frame.timestamp = os_gettime_ns();

	frame.data[0] = info.data + data->video_offset[0];
	frame.data[1] = info.data + data->video_offset[1];
	frame.data[2] = info.data + data->video_offset[2];

	obs_source_output_video(data->gstreamer_source, &frame);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

/* --------------------------------------------------------------------------
 * Audio Sample Pull Callback (Hot path: executed 50-100x/sec)
 * -------------------------------------------------------------------------- */
static GstFlowReturn audio_new_sample(GstAppSink *appsink, gpointer user_data)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data || !data->gstreamer_source) return GST_FLOW_OK;

	GstSample *sample = gst_app_sink_pull_sample(appsink);
	if (!sample) return GST_FLOW_OK;

	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstCaps *caps = gst_sample_get_caps(sample);
	if (!buffer || !caps)
	{
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	// 2-tier caps check: fast pointer compare + semantic equality
	if (caps != data->last_audio_caps)
	{
		if (!data->last_audio_caps || !gst_caps_is_equal(caps, data->last_audio_caps))
		{
			if (!update_audio_format_cache(data, caps))
			{
				gst_sample_unref(sample);
				return GST_FLOW_OK;
			}
		}
		else
		{
			gst_caps_unref(data->last_audio_caps);
			data->last_audio_caps = gst_caps_ref(caps);
		}
	}

	GstMapInfo info;
	if (!gst_buffer_map(buffer, &info, GST_MAP_READ))
	{
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	if (info.size == 0 || data->audio_bpf == 0)
	{
		gst_buffer_unmap(buffer, &info);
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	// Single struct assignment copies all pre-calculated sample rate, speakers & format
	struct obs_source_audio audio = data->cached_audio;

	audio.frames = info.size / data->audio_bpf;
	audio.data[0] = info.data;

	if (data->use_timestamps_audio && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) audio.timestamp = GST_BUFFER_PTS(buffer);
	else audio.timestamp = os_gettime_ns();

	obs_source_output_audio(data->gstreamer_source, &audio);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

/* --------------------------------------------------------------------------
 * Pipeline Creation & Launch
 * -------------------------------------------------------------------------- */
#ifdef GST_VIDEO_FORMAT_I420_10LE
	#define DEFAULT_INTERNAL_VIDEO_PIPELINE "videoconvert name=video ! video/x-raw, format={I420,NV12,BGRA,BGRx,RGBx,RGBA,YUY2,YVYU,UYVY,I420_10LE,P010_10LE,I420_12LE,Y444_12LE} ! appsink name=video_appsink"
#else
	#define DEFAULT_INTERNAL_VIDEO_PIPELINE "videoconvert name=video ! video/x-raw, format={I420,NV12,BGRA,BGRx,RGBx,RGBA,YUY2,YVYU,UYVY} ! appsink name=video_appsink"
#endif

#define DEFAULT_INTERNAL_AUDIO_PIPELINE "audioconvert name=audio ! audioresample ! audio/x-raw, format={U8,S16LE,S32LE,F32LE}, channels={1,2,3,4,5,6,8}, layout=interleaved ! appsink name=audio_appsink"
#define DEFAULT_INTERNAL_PIPELINE DEFAULT_INTERNAL_VIDEO_PIPELINE " " DEFAULT_INTERNAL_AUDIO_PIPELINE

static void create_pipeline(struct gstreamer_source_data *data, obs_data_t *settings)
{
	if (!data || !settings) return;

	GError *err = NULL;
	const char *user_pipeline = obs_data_get_string(settings, "pipeline");
	if (!user_pipeline || !*user_pipeline)
	{
		blog(LOG_WARNING, "[obs-gstreamer-source] No pipeline specified");
		return;
	}

	bool override_internal = obs_data_get_bool(settings, "override_internal_pipeline");
	const char *default_internal = DEFAULT_INTERNAL_PIPELINE;
	if (data && data->gstreamer_source)
	{
		const char *id = obs_source_get_id(data->gstreamer_source);
		if (id && strcmp(id, "gstreamer-source-video") == 0)
			default_internal = DEFAULT_INTERNAL_VIDEO_PIPELINE;
	}
	const char *internal_pipeline = override_internal ? obs_data_get_string(settings, "internal_pipeline") : default_internal;

	gchar *pipeline_str = NULL;
	if (internal_pipeline && *internal_pipeline) pipeline_str = g_strdup_printf("%s %s", internal_pipeline, user_pipeline);
	else pipeline_str = g_strdup(user_pipeline);
	
	blog(LOG_INFO, "[obs-gstreamer-source] Launching pipeline (override_internal=%d): %s", override_internal, pipeline_str);

	data->pipe = gst_parse_launch(pipeline_str, &err);
	g_free(pipeline_str);

	if (err != NULL)
	{
		const char *source_name = obs_source_get_name(data->gstreamer_source);
		blog(LOG_ERROR, "[obs-gstreamer-source] %s: Cannot start pipeline: %s", source_name ? source_name : "gstreamer-source", err->message);
		g_error_free(err);

		data->obs_media_state = OBS_MEDIA_STATE_ERROR;
		obs_source_output_video(data->gstreamer_source, NULL);
		return;
	}

	// Locate video appsink (supports "video_appsink" or custom "video")
	GstElement *v_sink = gst_bin_get_by_name(GST_BIN(data->pipe), "video_appsink");
	if (!v_sink || !GST_IS_APP_SINK(v_sink))
	{
		if (v_sink) gst_object_unref(v_sink);
		v_sink = gst_bin_get_by_name(GST_BIN(data->pipe), "video");
	}
	if (v_sink)
	{
		if (GST_IS_APP_SINK(v_sink))
		{
			GstAppSinkCallbacks video_cbs = {NULL, NULL, video_new_sample};
			gst_app_sink_set_callbacks(GST_APP_SINK(v_sink), &video_cbs, data, NULL);

			bool sync_video = obs_data_get_bool(settings, "sync_appsink_video");
			g_object_set(v_sink, "sync", sync_video ? TRUE : FALSE, "max-buffers", 2, "drop", TRUE, NULL);
		}
		gst_object_unref(v_sink); // Bin holds primary reference; safe to unref lookup
	}

	// Locate audio appsink (supports "audio_appsink" or custom "audio")
	GstElement *a_sink = gst_bin_get_by_name(GST_BIN(data->pipe), "audio_appsink");
	if (!a_sink || !GST_IS_APP_SINK(a_sink))
	{
		if (a_sink) gst_object_unref(a_sink);
		a_sink = gst_bin_get_by_name(GST_BIN(data->pipe), "audio");
	}
	if (a_sink)
	{
		if (GST_IS_APP_SINK(a_sink))
		{
			GstAppSinkCallbacks audio_cbs = {NULL, NULL, audio_new_sample};
			gst_app_sink_set_callbacks(GST_APP_SINK(a_sink), &audio_cbs, data, NULL);

			bool sync_audio = obs_data_get_bool(settings, "sync_appsink_audio");
			g_object_set(a_sink, "sync", sync_audio ? TRUE : FALSE, "max-buffers", 8, "drop", TRUE, NULL);
		}
		gst_object_unref(a_sink); // Bin holds primary reference; safe to unref lookup
	}

	// Attach synchronous bus handler (Zero additional thread)
	GstBus *bus = gst_element_get_bus(data->pipe);
	if (bus)
	{
		gst_bus_set_sync_handler(bus, bus_sync_handler, data, NULL);
		gst_object_unref(bus);
	}

	// Configure optional fixed latency
	gint latency = (gint)obs_data_get_int(settings, "latency");
	if (latency > 0) gst_pipeline_set_latency(GST_PIPELINE(data->pipe), latency * GST_MSECOND);
	
}

static void start(struct gstreamer_source_data *data, obs_data_t *settings)
{
	if (!data) return;

	stop(data);

	// Cache hot-path timing flags
	data->use_timestamps_video = obs_data_get_bool(settings, "use_timestamps_video");
	data->use_timestamps_audio = obs_data_get_bool(settings, "use_timestamps_audio");
	data->obs_media_state = OBS_MEDIA_STATE_OPENING;

	create_pipeline(data, settings);
	if (data->pipe) gst_element_set_state(data->pipe, GST_STATE_PLAYING);
}

static void stop(struct gstreamer_source_data *data)
{
	if (!data || !data->pipe) return;

	data->obs_media_state = OBS_MEDIA_STATE_STOPPED;

	// Detach bus sync handler before transitioning state to prevent dangling callbacks
	GstBus *bus = gst_element_get_bus(data->pipe);
	if (bus)
	{
		gst_bus_set_sync_handler(bus, NULL, NULL, NULL);
		gst_object_unref(bus);
	}

	// Stopping the bin automatically stops all child elements (appsinks, decoders, sources)
	gst_element_set_state(data->pipe, GST_STATE_NULL);
	gst_object_unref(data->pipe);
	data->pipe = NULL;

	// Free cached caps
	if (data->last_video_caps)
	{
		gst_caps_unref(data->last_video_caps);
		data->last_video_caps = NULL;
	}
	if (data->last_audio_caps)
	{
		gst_caps_unref(data->last_audio_caps);
		data->last_audio_caps = NULL;
	}

	obs_source_output_video(data->gstreamer_source, NULL);
}

/* --------------------------------------------------------------------------
 * OBS Source Callbacks
 * -------------------------------------------------------------------------- */
const char *gstreamer_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Source";
}

const char *gstreamer_source_video_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Video Source";
}

void *gstreamer_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct gstreamer_source_data *data = bzalloc(sizeof(struct gstreamer_source_data));
	data->gstreamer_source = source;

	bool nobuf = obs_data_get_bool(settings, "no_buffer");
	obs_source_set_async_unbuffered(source, nobuf);

	if (!obs_data_get_bool(settings, "stop_on_hide")) start(data, settings);

	return data;
}

void gstreamer_source_destroy(void *user_data)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data) return;

	stop(data);
	bfree(data);
}

void gstreamer_source_update(void *user_data, obs_data_t *settings)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data) return;

	bool nobuf = obs_data_get_bool(settings, "no_buffer");
	obs_source_set_async_unbuffered(data->gstreamer_source, nobuf);

	if (obs_data_get_bool(settings, "stop_on_hide") && !obs_source_showing(data->gstreamer_source))
	{
		stop(data);
		return;
	}

	start(data, settings);
}

void gstreamer_source_show(void *user_data)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data) return;

	if (data->pipe == NULL)
	{
		obs_data_t *settings = obs_source_get_settings(data->gstreamer_source);
		start(data, settings);
		obs_data_release(settings);
	}
}

void gstreamer_source_hide(void *user_data)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data) return;

	obs_data_t *settings = obs_source_get_settings(data->gstreamer_source);
	if (obs_data_get_bool(settings, "stop_on_hide")) stop(data);
	obs_data_release(settings);
}

enum obs_media_state gstreamer_source_get_state(void *user_data)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	return data ? data->obs_media_state : OBS_MEDIA_STATE_NONE;
}

int64_t gstreamer_source_get_time(void *user_data)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data || !data->pipe) return 0;

	gint64 position = 0;
	if (gst_element_query_position(data->pipe, GST_FORMAT_TIME, &position))
	{
		if (GST_CLOCK_TIME_IS_VALID(position)) return GST_TIME_AS_MSECONDS(position);
	}
	return 0;
}

int64_t gstreamer_source_get_duration(void *user_data)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data || !data->pipe) return 0;

	gint64 duration = 0;
	if (gst_element_query_duration(data->pipe, GST_FORMAT_TIME, &duration))
	{
		if (GST_CLOCK_TIME_IS_VALID(duration)) return GST_TIME_AS_MSECONDS(duration);
	}
	return 0;
}

void gstreamer_source_set_time(void *user_data, int64_t ms)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data || !data->pipe) return;

	// Execute seek directly on pipeline from OBS thread (thread-safe in GStreamer)
	gst_element_seek_simple(data->pipe, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, ms * GST_MSECOND);
}

void gstreamer_source_play_pause(void *user_data, bool pause)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data || !data->pipe) return;

	gst_element_set_state(data->pipe, pause ? GST_STATE_PAUSED : GST_STATE_PLAYING);
}

void gstreamer_source_restart(void *user_data)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	if (!data) return;

	obs_data_t *settings = obs_source_get_settings(data->gstreamer_source);
	start(data, settings);
	obs_data_release(settings);
}

void gstreamer_source_stop(void *user_data)
{
	struct gstreamer_source_data *data = (struct gstreamer_source_data *)user_data;
	stop(data);
}

void gstreamer_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline", "videotestsrc is-live=true ! video/x-raw, framerate=25/1, width=1280, height=720 ! video. audiotestsrc wave=ticks is-live=true ! audio/x-raw, channels=2, rate=44100 ! audio.");
	obs_data_set_default_bool(settings, "override_internal_pipeline", false);
	obs_data_set_default_string(settings, "internal_pipeline", DEFAULT_INTERNAL_PIPELINE);
	obs_data_set_default_bool(settings, "use_timestamps_video", true);
	obs_data_set_default_bool(settings, "use_timestamps_audio", true);
	obs_data_set_default_bool(settings, "sync_appsink_video", false);
	obs_data_set_default_bool(settings, "sync_appsink_audio", false);
	obs_data_set_default_bool(settings, "no_buffer", false);
	obs_data_set_default_int(settings, "latency", 0);
	obs_data_set_default_bool(settings, "stop_on_hide", true);
	obs_data_set_default_bool(settings, "clear_on_end", false);
}

void gstreamer_source_video_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline", "videotestsrc is-live=true ! video/x-raw, framerate=25/1, width=128, height=720 ! video.");
	obs_data_set_default_bool(settings, "override_internal_pipeline", false);
	obs_data_set_default_string(settings, "internal_pipeline", DEFAULT_INTERNAL_VIDEO_PIPELINE);
	obs_data_set_default_bool(settings, "use_timestamps_video", true);
	obs_data_set_default_bool(settings, "sync_appsink_video", false);
	obs_data_set_default_bool(settings, "no_buffer", false);
	obs_data_set_default_int(settings, "latency", 0);
	obs_data_set_default_bool(settings, "stop_on_hide", true);
	obs_data_set_default_bool(settings, "clear_on_end", false);
}

static bool on_apply_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct gstreamer_source_data *src_data = (struct gstreamer_source_data *)data;
	if (src_data && src_data->gstreamer_source)
	{
		obs_data_t *settings = obs_source_get_settings(src_data->gstreamer_source);
		start(src_data, settings);
		obs_data_release(settings);
	}
	return false;
}

static bool override_internal_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	bool override = obs_data_get_bool(settings, "override_internal_pipeline");
	obs_property_t *internal_prop = obs_properties_get(props, "internal_pipeline");
	obs_property_t *reset_prop = obs_properties_get(props, "reset_internal_pipeline");

	if (internal_prop) obs_property_set_enabled(internal_prop, override);
	if (reset_prop) obs_property_set_enabled(reset_prop, override);

	return true;
}

static bool on_reset_internal_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct gstreamer_source_data *src_data = (struct gstreamer_source_data *)data;
	if (src_data && src_data->gstreamer_source)
	{
		const char *id = obs_source_get_id(src_data->gstreamer_source);
		const char *default_internal = (id && strcmp(id, "gstreamer-source-video") == 0)
			? DEFAULT_INTERNAL_VIDEO_PIPELINE
			: DEFAULT_INTERNAL_PIPELINE;
		obs_data_t *settings = obs_source_get_settings(src_data->gstreamer_source);
		obs_data_set_string(settings, "internal_pipeline", default_internal);
		obs_data_release(settings);
		return true;
	}
	return false;
}

obs_properties_t *gstreamer_source_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *prop = obs_properties_add_text(props, "pipeline", "Pipeline", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(prop, "Main user pipeline.\nConnect to video. and audio. elements, or define your own sinks.");

	obs_property_t *override_prop = obs_properties_add_bool(props, "override_internal_pipeline", "Override internal pipeline");
	obs_property_set_long_description(override_prop, "Enable to view and customize the internal conversion and appsink elements.");
	obs_property_set_modified_callback(override_prop, override_internal_modified);

	obs_property_t *internal_prop = obs_properties_add_text(props, "internal_pipeline", "Internal pipeline", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(internal_prop, "Defines the internal videoconvert/audioconvert and appsink elements connecting to OBS.\nSinks must be named 'video_appsink' (or 'video') and 'audio_appsink' (or 'audio').");

	obs_properties_add_button2(props, "reset_internal_pipeline", "Reset internal pipeline", on_reset_internal_clicked, data);

	struct gstreamer_source_data *src_data = (struct gstreamer_source_data *)data;
	bool override_enabled = false;
	if (src_data && src_data->gstreamer_source)
	{
		obs_data_t *settings = obs_source_get_settings(src_data->gstreamer_source);
		override_enabled = obs_data_get_bool(settings, "override_internal_pipeline");
		obs_data_release(settings);
	}
	obs_property_set_enabled(internal_prop, override_enabled);
	obs_property_t *reset_prop = obs_properties_get(props, "reset_internal_pipeline");
	if (reset_prop) obs_property_set_enabled(reset_prop, override_enabled);

	obs_properties_add_bool(props, "use_timestamps_video", "Use pipeline time stamps (video)");
	obs_properties_add_bool(props, "use_timestamps_audio", "Use pipeline time stamps (audio)");
	obs_properties_add_bool(props, "sync_appsink_video", "Sync appsink to clock (video)");
	obs_properties_add_bool(props, "sync_appsink_audio", "Sync appsink to clock (audio)");
	obs_properties_add_bool(props, "stop_on_hide", "Stop pipeline when hidden");
	obs_properties_add_bool(props, "clear_on_end", "Clear image data on EOS or error");
	obs_properties_add_bool(props, "no_buffer", "Disable buffering in OBS");

	obs_property_t *lat_prop = obs_properties_add_int(props, "latency", "Fixed latency (ms)", 0, 10000, 10);
	obs_property_set_long_description(lat_prop, "Sets a fixed pipeline latency in ms (0 = auto-detect).");

	obs_properties_add_button2(props, "apply", "Apply Changes", on_apply_clicked, data);

	return props;
}

obs_properties_t *gstreamer_source_video_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *prop = obs_properties_add_text(props, "pipeline", "Pipeline", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(prop, "Main user pipeline.\nConnect to video. element, or define your own sinks.");

	obs_property_t *override_prop = obs_properties_add_bool(props, "override_internal_pipeline", "Override internal pipeline");
	obs_property_set_long_description(override_prop, "Enable to view and customize the internal conversion and appsink elements.");
	obs_property_set_modified_callback(override_prop, override_internal_modified);

	obs_property_t *internal_prop = obs_properties_add_text(props, "internal_pipeline", "Internal pipeline", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(internal_prop, "Defines the internal videoconvert and appsink elements connecting to OBS.\nSink must be named 'video_appsink' (or 'video').");

	obs_properties_add_button2(props, "reset_internal_pipeline", "Reset internal pipeline", on_reset_internal_clicked, data);

	struct gstreamer_source_data *src_data = (struct gstreamer_source_data *)data;
	bool override_enabled = false;
	if (src_data && src_data->gstreamer_source)
	{
		obs_data_t *settings = obs_source_get_settings(src_data->gstreamer_source);
		override_enabled = obs_data_get_bool(settings, "override_internal_pipeline");
		obs_data_release(settings);
	}
	obs_property_set_enabled(internal_prop, override_enabled);
	obs_property_t *reset_prop = obs_properties_get(props, "reset_internal_pipeline");
	if (reset_prop) obs_property_set_enabled(reset_prop, override_enabled);

	obs_properties_add_bool(props, "use_timestamps_video", "Use pipeline time stamps (video)");
	obs_properties_add_bool(props, "sync_appsink_video", "Sync appsink to clock (video)");
	obs_properties_add_bool(props, "stop_on_hide", "Stop pipeline when hidden");
	obs_properties_add_bool(props, "clear_on_end", "Clear image data on EOS or error");
	obs_properties_add_bool(props, "no_buffer", "Disable buffering in OBS");

	obs_property_t *lat_prop = obs_properties_add_int(props, "latency", "Fixed latency (ms)", 0, 10000, 10);
	obs_property_set_long_description(lat_prop, "Sets a fixed pipeline latency in ms (0 = auto-detect).");

	obs_properties_add_button2(props, "apply", "Apply Changes", on_apply_clicked, data);

	return props;
}

/* --------------------------------------------------------------------------
 * OBS Source Registration Info
 * -------------------------------------------------------------------------- */
struct obs_source_info gstreamer_source_info =
{
	.id = "gstreamer-source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = gstreamer_source_get_name,
	.create = gstreamer_source_create,
	.destroy = gstreamer_source_destroy,
	.update = gstreamer_source_update,
	.get_defaults = gstreamer_source_get_defaults,
	.get_properties = gstreamer_source_get_properties,
	.show = gstreamer_source_show,
	.hide = gstreamer_source_hide,
	.media_get_state = gstreamer_source_get_state,
	.media_get_time = gstreamer_source_get_time,
	.media_get_duration = gstreamer_source_get_duration,
	.media_set_time = gstreamer_source_set_time,
	.media_play_pause = gstreamer_source_play_pause,
	.media_restart = gstreamer_source_restart,
	.media_stop = gstreamer_source_stop,
};

struct obs_source_info gstreamer_source_video_info =
{
	.id = "gstreamer-source-video",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = gstreamer_source_video_get_name,
	.create = gstreamer_source_create,
	.destroy = gstreamer_source_destroy,
	.update = gstreamer_source_update,
	.get_defaults = gstreamer_source_video_get_defaults,
	.get_properties = gstreamer_source_video_get_properties,
	.show = gstreamer_source_show,
	.hide = gstreamer_source_hide,
	.media_get_state = gstreamer_source_get_state,
	.media_get_time = gstreamer_source_get_time,
	.media_get_duration = gstreamer_source_get_duration,
	.media_set_time = gstreamer_source_set_time,
	.media_play_pause = gstreamer_source_play_pause,
	.media_restart = gstreamer_source_restart,
	.media_stop = gstreamer_source_stop,
};
