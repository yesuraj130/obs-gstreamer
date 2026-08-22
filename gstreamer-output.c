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

#pragma warning(disable : 4047)
#pragma warning(disable : 4244)

#include <obs-module.h>
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <plugin-support.h>

#include "gstreamer-webrtc.h"

typedef struct {
	GstElement *pipe;
	GstElement *video;
	GstRTSPServer *server;
	GstRTSPMountPoints *mounts;
	GstRTSPMediaFactory *factory;
	char *mount_point;
	bool rtsp_server;
	bool webrtc_output;
	struct gstreamer_webrtc *webrtc;
	//GstElement *audio;
	gsize buffer_size;
	uint64_t raw_video_frames;
	obs_output_t *output;
	obs_data_t *settings;
	struct obs_video_info ovi;
} data_t;

static const char *obs_video_format_to_gst_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
		return "I420";
	case VIDEO_FORMAT_NV12:
		return "NV12";
	case VIDEO_FORMAT_YVYU:
		return "YVYU";
	case VIDEO_FORMAT_YUY2:
		return "YUY2";
	case VIDEO_FORMAT_UYVY:
		return "UYVY";
	case VIDEO_FORMAT_I422:
		return "Y42B";
	case VIDEO_FORMAT_RGBA:
		return "RGBA";
	case VIDEO_FORMAT_BGRA:
		return "BGRA";
	case VIDEO_FORMAT_BGRX:
		return "BGRX";
	case VIDEO_FORMAT_I444:
		return "Y444";
	default:
		return NULL;
	}
}

static gsize obs_video_format_buffer_size(enum video_format format, int width, int height)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
		return width * height * 3 / 2;
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_I422:
		return width * height * 2;
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_I444:
		return width * height * 4;
	default:
		return 0;
	}
}

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer user_data);

static void client_connected_cb(GstRTSPServer *server, GstRTSPClient *client, gpointer user_data)
{
	blog(LOG_INFO, "[obs-gstreamer] RTSP client connected");
}

static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data)
{
	data_t *data = user_data;
	GstElement *element = gst_rtsp_media_get_element(media);
	GstElement *appsrc = gst_bin_get_by_name(GST_BIN(element), "appsrc_video");
	GstBus *bus = gst_element_get_bus(element);

	if (!appsrc) {
		blog(LOG_WARNING, "[obs-gstreamer] rtsp media has no appsrc_video");
		gst_object_unref(bus);
		gst_object_unref(element);
		return;
	}

	if (data->video)
		gst_object_unref(data->video);

	data->video = appsrc;
	gst_bus_add_watch(bus, bus_callback, data);
	gst_object_unref(bus);
	gst_app_src_set_stream_type(GST_APP_SRC(data->video), GST_APP_STREAM_TYPE_STREAM);
	blog(LOG_INFO, "[obs-gstreamer] RTSP media configured, appsrc ready for scene output");

	if (gst_element_set_state(element, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		blog(LOG_ERROR, "[obs-gstreamer] RTSP media pipeline failed to enter PLAYING");
	}

	gst_object_unref(element);
}

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer user_data)
{
	blog(LOG_INFO, "bus_callback = called");
	data_t *data = user_data;

	switch (GST_MESSAGE_TYPE(message)) {
		case GST_MESSAGE_ERROR: {
			GError *err;
			gst_message_parse_error(message, &err, NULL);
			const char *source_name = "obs_source_get_name(data->source)";
			blog(LOG_ERROR, "[obs-gstreamer] %s: %s", source_name, err->message);
			g_error_free(err);
			break;
		}
		case GST_MESSAGE_WARNING: {
			GError *err;
			gst_message_parse_warning(message, &err, NULL);
			const char *source_name = "obs_source_get_name(data->source)";
			blog(LOG_WARNING, "[obs-gstreamer] %s: %s", source_name, err->message);
			g_error_free(err);
		} break;
		default:
			break;
	}

	return TRUE;
}

const char *gstreamer_output_get_name(void *type_data)
{
	return "HJM GStreamer Output";
}

void *gstreamer_output_create(obs_data_t *settings, obs_output_t *output)
{
	data_t *data = g_new0(data_t, 1);

	data->output = output;
	data->settings = settings;
	blog(LOG_INFO, "gstreamer_output_create = called");
	return data;
}

void gstreamer_output_destroy(void *p)
{
	data_t *data = (data_t *)p;

	if (data->webrtc) {
		if (data->video) {
			gst_object_unref(data->video);
			data->video = NULL;
		}
		gstreamer_webrtc_destroy(data->webrtc);
		data->webrtc = NULL;
	}

	if (data->video) {
		gst_object_unref(data->video);
		data->video = NULL;
	}

	if (data->server) {
		if (data->mounts && data->mount_point) {
			gst_rtsp_mount_points_remove_factory(data->mounts, data->mount_point);
		}
		if (data->factory) {
			g_object_unref(data->factory);
			data->factory = NULL;
		}
		if (data->mounts) {
			g_object_unref(data->mounts);
			data->mounts = NULL;
		}
		gst_object_unref(data->server);
		data->server = NULL;
	}

	g_free(data->mount_point);
	data->mount_point = NULL;
	g_free(data);
	blog(LOG_INFO, "gstreamer_output_destroy = end");
}

bool gstreamer_output_start(void *p)
{
	blog(LOG_INFO, "gstreamer_output_start = called");
	data_t *data = (data_t *)p;

	if (!obs_output_can_begin_data_capture(data->output, 0)) {
		blog(LOG_INFO, "output obs_output_can_begin_data_capture = false");
		return false;
	}
	blog(LOG_INFO, "output obs_output_can_begin_data_capture = true");

	obs_get_video_info(&data->ovi);
	data->rtsp_server = obs_data_get_bool(data->settings, "rtsp_server");
	data->webrtc_output = obs_data_get_bool(data->settings, "webrtc_output");
	const char *gst_format = obs_video_format_to_gst_format(data->ovi.output_format);
	if (!gst_format) {
		blog(LOG_ERROR, "unhandled output format: %d", data->ovi.output_format);
		return false;
	}
	data->buffer_size = obs_video_format_buffer_size(data->ovi.output_format,
		data->ovi.output_width, data->ovi.output_height);
	if (data->webrtc_output)
		blog(LOG_INFO, "[obs-gstreamer] WebRTC video format=%d (%s), size=%dx%d, frame bytes=%zu",
			data->ovi.output_format, gst_format, data->ovi.output_width,
			data->ovi.output_height, data->buffer_size);

	if (data->rtsp_server) {
		const char *mount = obs_data_get_string(data->settings, "rtsp_mount");
		const char *service = obs_data_get_string(data->settings, "rtsp_service");
		const char *pipeline = obs_data_get_string(data->settings, "rtsp_pipeline");
		char *launch = g_strdup_printf(
			pipeline && pipeline[0] ? pipeline : "( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true block=true ! queue ! video/x-raw, format=%s, width=%d, height=%d, framerate=%d/%d ! videoconvert ! x264enc tune=zerolatency speed-preset=veryfast bitrate=3000 key-int-max=30 ! video/x-h264, stream-format=byte-stream, alignment=au ! h264parse ! rtph264pay name=pay0 pt=96 )",
			gst_format, data->ovi.output_width, data->ovi.output_height,
			data->ovi.fps_num, data->ovi.fps_den);

		data->server = gst_rtsp_server_new();
		if (service && service[0])
			gst_rtsp_server_set_service(data->server, service);
		data->mount_point = g_strdup(mount && mount[0] ? mount : "/live");
		data->mounts = gst_rtsp_server_get_mount_points(data->server);
		data->factory = gst_rtsp_media_factory_new();
		gst_rtsp_media_factory_set_shared(data->factory, TRUE);
		gst_rtsp_media_factory_set_launch(data->factory, launch);
		g_signal_connect(data->server, "client-connected", G_CALLBACK(client_connected_cb), data);
		g_signal_connect(data->factory, "media-configure", G_CALLBACK(media_configure_cb), data);
		gst_rtsp_mount_points_add_factory(data->mounts, data->mount_point, data->factory);
		gst_rtsp_server_attach(data->server, NULL);
		gst_rtsp_server_set_service(data->server, service);
		g_free(launch);
		blog(LOG_INFO, "[obs-gstreamer] RTSP server started at rtsp://127.0.0.1:%s%s", gst_rtsp_server_get_service(data->server), data->mount_point);
	} else if (data->webrtc_output) {
		char *error = NULL;
		data->webrtc = gstreamer_webrtc_create(data->output, data->settings, &data->ovi, &error);
		if (!data->webrtc) {
			blog(LOG_ERROR, "gstreamer_output_start = WebRTC init failed: %s",
				error ? error : "unknown error");
			obs_output_set_last_error(data->output, error ? error : "WebRTC output failed to start");
			g_free(error);
			return false;
		}
		g_free(error);
		data->video = gstreamer_webrtc_appsrc(data->webrtc);
		blog(LOG_INFO, "[obs-gstreamer] WebRTC output started");
	} else {
		GError *err = NULL;
		char *pipe_string = g_strdup_printf(
			"appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true ! queue ! video/x-raw, format=%s, width=%d, height=%d, framerate=%d/%d ! videoconvert ! %s",
			gst_format, data->ovi.output_width, data->ovi.output_height,
			data->ovi.fps_num, data->ovi.fps_den,
			obs_data_get_string(data->settings, "pipeline"));

		data->pipe = gst_parse_launch(pipe_string, &err);
		g_free(pipe_string);

		if (err) {
			blog(LOG_ERROR, "gstreamer_output_start = gst_parse_launch error: %s", err->message);
			g_error_free(err);
			return false;
		}

		data->video = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc_video");
		if (!data->video) {
			blog(LOG_ERROR, "gstreamer_output_start = appsrc_video element not found in pipeline");
			gst_object_unref(data->pipe);
			data->pipe = NULL;
			return false;
		}
		GstBus *bus = gst_element_get_bus(data->pipe);
		gst_bus_add_watch(bus, bus_callback, data);
		gst_object_unref(bus);
		gst_element_set_state(data->pipe, GST_STATE_PLAYING);
	}

	obs_output_begin_data_capture(data->output, 0);
	blog(LOG_INFO, "obs_output_begin_data_capture = end");
	return true;
}

void gstreamer_output_stop(void *p, uint64_t ts)
{
	blog(LOG_INFO, "gstreamer_output_stop = called");
	data_t *data = (data_t *)p;

	obs_output_end_data_capture(data->output);
	blog(LOG_INFO, "gstreamer_output_stop = obs_output_end_data_capture stopped");

	if (data->server) {
		if (data->video) {
			gst_app_src_end_of_stream(GST_APP_SRC(data->video));
			gst_object_unref(data->video);
			data->video = NULL;
		}
		if (data->mounts && data->mount_point) {
			gst_rtsp_mount_points_remove_factory(data->mounts, data->mount_point);
		}
		if (data->factory) {
			g_object_unref(data->factory);
			data->factory = NULL;
		}
		if (data->mounts) {
			g_object_unref(data->mounts);
			data->mounts = NULL;
		}
		if (data->server) {
			gst_object_unref(data->server);
			data->server = NULL;
		}
		blog(LOG_INFO, "gstreamer_output_stop = RTSP server stopped");
	}

	if (data->webrtc) {
		if (data->video) {
			gst_object_unref(data->video);
			data->video = NULL;
		}
		gstreamer_webrtc_destroy(data->webrtc);
		data->webrtc = NULL;
		blog(LOG_INFO, "gstreamer_output_stop = WebRTC server stopped");
	}

	if (data->pipe) {
		if (data->video) {
			gst_app_src_end_of_stream(GST_APP_SRC(data->video));
			gst_object_unref(data->video);
			data->video = NULL;
		}
		GstBus *bus = gst_element_get_bus(data->pipe);
		if (bus) {
			gst_bus_remove_watch(bus);
			gst_object_unref(bus);
		}

		gst_element_set_state(data->pipe, GST_STATE_NULL);
		gst_object_unref(data->pipe);
		data->pipe = NULL;
		blog(LOG_INFO, "gstreamer_output_stop = unref complete");
	}

	blog(LOG_INFO, "gstreamer_output_stop = end");
}

void gstreamer_output_encoded_packet(void *p, struct encoder_packet *packet)
{
	data_t *data = (data_t *)p;

	GstBuffer *buffer = gst_buffer_new_allocate(NULL, packet->size, NULL);
	gst_buffer_fill(buffer, 0, packet->data, packet->size);

	GST_BUFFER_PTS(buffer) = packet->pts * GST_SECOND / (packet->timebase_den / packet->timebase_num);
	GST_BUFFER_DTS(buffer) = packet->dts * GST_SECOND / (packet->timebase_den / packet->timebase_num);

	gst_buffer_set_flags(buffer, packet->keyframe ? 0 : GST_BUFFER_FLAG_DELTA_UNIT);

	GstElement *appsrc = data->video;

	gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);

	//blog(LOG_INFO, "gstreamer_output_encoded_packet = complete");
}

void gstreamer_output_raw_video(void *p, struct video_data *frame)
{
	data_t *data = (data_t *)p;
	if (!data->video)
		return;
	data->raw_video_frames++;

	// OBS reuses frame memory after this callback; GStreamer must own a copy.
	GstBuffer *buffer = gst_buffer_new_allocate(NULL, data->buffer_size, NULL);
	gst_buffer_fill(buffer, 0, frame->data[0], data->buffer_size);

	//GST_BUFFER_PTS(buffer) = frame->timestamp;

	GstFlowReturn result = gst_app_src_push_buffer(GST_APP_SRC(data->video), buffer);
	if (data->webrtc_output && (data->raw_video_frames == 1 || data->raw_video_frames % 60 == 0))
		blog(LOG_INFO, "[obs-gstreamer] WebRTC raw frame %llu pushed, result=%s, size=%zu",
			(unsigned long long)data->raw_video_frames, gst_flow_get_name(result), data->buffer_size);
	if (result != GST_FLOW_OK && result != GST_FLOW_FLUSHING)
		blog(LOG_WARNING, "[obs-gstreamer] RTSP appsrc push failed: %s", gst_flow_get_name(result));
	//blog(LOG_INFO, "gstreamer_output_raw_video");
}

void gstreamer_output_raw_audio(void *p, struct audio_data *frame)
{
	data_t *data = (data_t *)p;

	GstBuffer *buffer = gst_buffer_new_allocate(NULL, data->buffer_size, NULL);
	gst_buffer_fill(buffer, 0, frame->data[0], data->buffer_size);

	GST_BUFFER_PTS(buffer) = frame->timestamp;
	GST_BUFFER_DTS(buffer) = frame->timestamp;
	GST_BUFFER_OFFSET(buffer) = 0;
	//gst_buffer_set_flags(buffer, packet->keyframe ? 0 : GST_BUFFER_FLAG_DELTA_UNIT);

	GstElement *appsrc = data->video;

	gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
}

void gstreamer_output_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline", "autovideosink sync=false");
	obs_data_set_default_bool(settings, "rtsp_server", false);
	obs_data_set_default_string(settings, "rtsp_mount", "/live");
	obs_data_set_default_string(settings, "rtsp_service", "8554");
	obs_data_set_default_string(settings, "rtsp_pipeline", "( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true block=true ! queue ! video/x-raw, format=%s, width=%d, height=%d, framerate=%d/%d ! videoconvert ! x264enc tune=zerolatency speed-preset=veryfast bitrate=3000 key-int-max=30 ! video/x-h264, stream-format=byte-stream, alignment=au ! h264parse ! rtph264pay name=pay0 pt=96 )");
	obs_data_set_default_bool(settings, "webrtc_output", false);
	obs_data_set_default_string(settings, "webrtc_http_port", "8888");
	obs_data_set_default_string(settings, "webrtc_stun_server", "");
	obs_data_set_default_string(settings, "webrtc_web_root", "");
	obs_data_set_default_string(settings, "webrtc_signaling_url", "ws://127.0.0.1:8443");
}

obs_properties_t *gstreamer_output_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *rtsp_enabled = obs_properties_add_bool(props, "rtsp_server", "Start RTSP server");
	obs_property_set_long_description(rtsp_enabled,
		"Start a GStreamer RTSP server and serve the OBS scene output on the configured mount point.");

	obs_property_t *mount = obs_properties_add_text(props, "rtsp_mount", "RTSP mount", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(mount, "RTSP mount path such as /live");

	obs_property_t *service = obs_properties_add_text(props, "rtsp_service", "RTSP service", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(service, "RTSP port or service name such as 8554");
	obs_property_t *rtsp_pipeline = obs_properties_add_text(props, "rtsp_pipeline", "RTSP pipeline", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(rtsp_pipeline, "RTSP media-factory pipeline. Keep appsrc named appsrc_video and the RTP payloader named pay0.");

	obs_property_t *webrtc_enabled = obs_properties_add_bool(props, "webrtc_output", "Start WebRTC output");
	obs_property_set_long_description(webrtc_enabled,
		"Serve the OBS scene to browsers via an embedded WHEP (WebRTC) server on http://<host>:8888/.");

	obs_property_t *http_port = obs_properties_add_text(props, "webrtc_http_port", "WebRTC HTTP port", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(http_port, "HTTP port for the embedded WHEP server and viewer page, default 8888");

	obs_property_t *stun = obs_properties_add_text(props, "webrtc_stun_server", "WebRTC STUN server", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(stun, "Optional STUN server such as stun://stun.l.google.com:19302. Leave empty for LAN-only viewing.");

	obs_property_t *web_root = obs_properties_add_text(props, "webrtc_web_root", "WebRTC web root", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(web_root, "Folder with viewer page files (index.html, style.css, app.js). Empty = ~/.local/share/obs-gstreamer/webrtc");

	// Legacy key kept for saved configs; ignored at runtime.
	obs_properties_add_text(props, "webrtc_signaling_url", "WebRTC signaling URL (legacy, ignored)", OBS_TEXT_DEFAULT);

	obs_property_t *prop = obs_properties_add_text(props, "pipeline", "Pipeline", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(prop, "pipeline for gstreamer-output. This is ignored when RTSP server or WebRTC output mode is enabled.");
	obs_property_set_description(prop, "Pipeline");
	return props;
}
#pragma warning(default : 4047)
#pragma warning(default : 4244)