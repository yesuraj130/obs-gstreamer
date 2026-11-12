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

#ifdef _MSC_VER
#pragma warning(disable : 4047)
#pragma warning(disable : 4244)
#endif

#include <obs-module.h>
#include <util/platform.h>
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <plugin-support.h>

#include "gstreamer-helper.h"
#include "gstreamer-webrtc.h"

struct gstreamer_output_data {
	GstElement *pipe;
	GstElement *video;
	GstRTSPServer *server;
	guint server_source_id;
	GMainContext *rtsp_context;
	GMainLoop *rtsp_loop;
	GThread *rtsp_thread;
	GstRTSPMountPoints *mounts;
	GstRTSPMediaFactory *factory;
	GstRTSPMedia *media;
	char *mount_point;
	bool rtsp_server;
	bool webrtc_output;
	struct gstreamer_webrtc *webrtc;
	GstElement *audio;
	gsize buffer_size;
	GstVideoInfo raw_video_info;
	gstreamer_video_frame_cache_t frame_cache;
	gstreamer_cpu_frame_slot_t *cpu_frame_slot;
	uint64_t raw_video_frames;
	uint64_t raw_video_drops;
	obs_output_t *output;
	obs_data_t *settings;
	struct obs_video_info ovi;
	volatile gint streaming;
	volatile gint in_flight;
	GMutex lock;
};

static inline void swap_appsrc_elements(struct gstreamer_output_data *data,
	GstElement *new_video, GstElement *new_audio,
	GstElement **out_old_video, GstElement **out_old_audio)
{
	g_mutex_lock(&data->lock);
	GstElement *old_video = (GstElement *)g_atomic_pointer_get(&data->video);
	GstElement *old_audio = (GstElement *)g_atomic_pointer_get(&data->audio);
	g_atomic_pointer_set(&data->video, new_video);
	g_atomic_pointer_set(&data->audio, new_audio);
	while (g_atomic_int_get(&data->in_flight) > 0)
		g_usleep(100);
	g_mutex_unlock(&data->lock);

	if (out_old_video)
		*out_old_video = old_video;
	else if (old_video)
		gst_object_unref(old_video);

	if (out_old_audio)
		*out_old_audio = old_audio;
	else if (old_audio)
		gst_object_unref(old_audio);
}

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer user_data);

static void client_closed_cb(GstRTSPClient *client, gpointer user_data)
{
	blog(LOG_INFO, "[obs-gstreamer] RTSP client disconnected");
}

static void client_connected_cb(GstRTSPServer *server, GstRTSPClient *client, gpointer user_data)
{
	blog(LOG_INFO, "[obs-gstreamer] RTSP client connected");
	g_signal_connect(client, "closed", G_CALLBACK(client_closed_cb), user_data);
}

static void media_unprepared_cb(GstRTSPMedia *media, gpointer user_data)
{
	(void)media;
	struct gstreamer_output_data *data = user_data;
	blog(LOG_INFO, "[obs-gstreamer] RTSP media unprepared (all clients disconnected), resetting appsrc");

	swap_appsrc_elements(data, NULL, NULL, NULL, NULL);

	g_mutex_lock(&data->lock);
	if (data->media) {
		g_signal_handlers_disconnect_by_data(data->media, data);
		gst_object_unref(data->media);
		data->media = NULL;
	}
	g_mutex_unlock(&data->lock);
}

static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data)
{
	struct gstreamer_output_data *data = user_data;
	GstElement *element = gst_rtsp_media_get_element(media);
	GstElement *appsrc = gst_bin_get_by_name(GST_BIN(element), "appsrc_video");
	GstBus *bus = gst_element_get_bus(element);

	if (!appsrc) {
		blog(LOG_WARNING, "[obs-gstreamer] rtsp media has no appsrc_video");
		gst_object_unref(bus);
		gst_object_unref(element);
		return;
	}

	GstElement *appsrc_audio = gst_bin_get_by_name(GST_BIN(element), "appsrc_audio");
	if (appsrc_audio)
		gst_app_src_set_stream_type(GST_APP_SRC(appsrc_audio), GST_APP_STREAM_TYPE_STREAM);
	gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);

	swap_appsrc_elements(data, appsrc, appsrc_audio, NULL, NULL);

	g_mutex_lock(&data->lock);
	if (data->media && data->media != media) {
		g_signal_handlers_disconnect_by_data(data->media, data);
		gst_object_unref(data->media);
		data->media = NULL;
	}
	if (!data->media) {
		data->media = GST_RTSP_MEDIA(gst_object_ref(media));
		g_signal_connect(media, "unprepared", G_CALLBACK(media_unprepared_cb), data);
	}
	g_mutex_unlock(&data->lock);

	GSource *bus_source = gst_bus_create_watch(bus);
	g_source_set_callback(bus_source, (GSourceFunc)bus_callback, data, NULL);
	g_source_attach(bus_source, data->rtsp_context ? data->rtsp_context : NULL);
	g_source_unref(bus_source);
	gst_object_unref(bus);
	blog(LOG_INFO, "[obs-gstreamer] RTSP media configured, appsrc ready for scene output");

	if (gst_element_set_state(element, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		blog(LOG_ERROR, "[obs-gstreamer] RTSP media pipeline failed to enter PLAYING");
	}

	gst_object_unref(element);
}

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer user_data)
{
	struct gstreamer_output_data *data = user_data;
	const char *output_name = (data && data->output) ? obs_output_get_name(data->output) : "GStreamer Output";
	const char *src_name = GST_MESSAGE_SRC_NAME(message);

	switch (GST_MESSAGE_TYPE(message)) {
		case GST_MESSAGE_ERROR: {
			GError *err = NULL;
			gchar *debug = NULL;
			gst_message_parse_error(message, &err, &debug);
			blog(LOG_ERROR, "[obs-gstreamer] %s [%s] ERROR: %s (debug: %s)",
				output_name, src_name ? src_name : "pipeline",
				err ? err->message : "unknown error",
				debug ? debug : "none");
			if (err) g_error_free(err);
			g_free(debug);
			break;
		}
		case GST_MESSAGE_WARNING: {
			GError *err = NULL;
			gchar *debug = NULL;
			gst_message_parse_warning(message, &err, &debug);
			blog(LOG_WARNING, "[obs-gstreamer] %s [%s] WARNING: %s (debug: %s)",
				output_name, src_name ? src_name : "pipeline",
				err ? err->message : "unknown warning",
				debug ? debug : "none");
			if (err) g_error_free(err);
			g_free(debug);
			break;
		}
		case GST_MESSAGE_STATE_CHANGED: {
			if (GST_MESSAGE_SRC(message) == GST_OBJECT(data->pipe)) {
				GstState old_s, new_s, pending_s;
				gst_message_parse_state_changed(message, &old_s, &new_s, &pending_s);
				blog(LOG_INFO, "[obs-gstreamer] %s state changed: %s -> %s (pending: %s)",
					output_name,
					gst_element_state_get_name(old_s),
					gst_element_state_get_name(new_s),
					gst_element_state_get_name(pending_s));
			}
			break;
		}
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
	struct gstreamer_output_data *data = g_new0(struct gstreamer_output_data, 1);

	g_mutex_init(&data->lock);
	data->output = output;
	data->settings = settings;
	blog(LOG_INFO, "[obs-gstreamer] Output instance created (output=%p, name=%s)",
		(void *)output, output ? obs_output_get_name(output) : "unnamed");
	return data;
}

void gstreamer_output_destroy(void *p)
{
	struct gstreamer_output_data *data = (struct gstreamer_output_data *)p;
	if (!data) return;

	blog(LOG_INFO, "[obs-gstreamer] Destroying output instance (total raw video frames: %llu)",
		(unsigned long long)data->raw_video_frames);

	g_atomic_int_set(&data->streaming, 0);
	swap_appsrc_elements(data, NULL, NULL, NULL, NULL);

	if (data->webrtc) {
		gstreamer_webrtc_destroy(data->webrtc);
		data->webrtc = NULL;
	}

	gstreamer_cpu_frame_slot_unref(data->cpu_frame_slot);
	data->cpu_frame_slot = NULL;

	if (data->media) {
		g_signal_handlers_disconnect_by_data(data->media, data);
		gst_object_unref(data->media);
		data->media = NULL;
	}

	if (data->rtsp_loop) {
		g_main_loop_quit(data->rtsp_loop);
	}
	if (data->rtsp_thread) {
		g_thread_join(data->rtsp_thread);
		data->rtsp_thread = NULL;
	}
	if (data->rtsp_loop) {
		g_main_loop_unref(data->rtsp_loop);
		data->rtsp_loop = NULL;
	}

	if (data->server) {
		if (data->server_source_id > 0) {
			if (data->rtsp_context) {
				GSource *source = g_main_context_find_source_by_id(data->rtsp_context, data->server_source_id);
				if (source)
					g_source_destroy(source);
			} else {
				g_source_remove(data->server_source_id);
			}
			data->server_source_id = 0;
		}
		if (data->mounts && data->mount_point) {
			gst_rtsp_mount_points_remove_factory(data->mounts, data->mount_point);
		}
		if (data->factory) {
			g_signal_handlers_disconnect_by_data(data->factory, data);
			g_object_unref(data->factory);
			data->factory = NULL;
		}
		if (data->mounts) {
			g_object_unref(data->mounts);
			data->mounts = NULL;
		}
		if (data->server) {
			g_signal_handlers_disconnect_by_data(data->server, data);
			gst_object_unref(data->server);
			data->server = NULL;
		}
	}

	if (data->rtsp_context) {
		g_main_context_unref(data->rtsp_context);
		data->rtsp_context = NULL;
	}

	g_free(data->mount_point);
	data->mount_point = NULL;
	g_mutex_clear(&data->lock);
	g_free(data);
	blog(LOG_INFO, "[obs-gstreamer] Output destroyed cleanly");
}

bool gstreamer_output_start(void *p)
{
	struct gstreamer_output_data *data = (struct gstreamer_output_data *)p;
	if (!data) return false;

	blog(LOG_INFO, "[obs-gstreamer] Attempting to start output '%s'",
		data->output ? obs_output_get_name(data->output) : "unnamed");

	if (!obs_output_can_begin_data_capture(data->output, 0)) {
		blog(LOG_ERROR, "[obs-gstreamer] obs_output_can_begin_data_capture returned false");
		return false;
	}

	obs_get_video_info(&data->ovi);
	video_t *out_video = obs_output_video(data->output);
	if (!out_video)
		out_video = obs_get_video();
	if (!out_video) {
		blog(LOG_ERROR, "[obs-gstreamer] No OBS video_t available for output");
		obs_output_set_last_error(data->output, "No OBS video output available");
		return false;
	}

	enum video_format obs_fmt = video_output_get_format(out_video);
	if (obs_fmt != VIDEO_FORMAT_NV12) {
		const char *fmt_name = obs_to_gst_video_format_str(obs_fmt);
		blog(LOG_ERROR, "[obs-gstreamer] Output video format must be NV12 (got %d / %s)",
			(int)obs_fmt, fmt_name ? fmt_name : "unknown");
		obs_output_set_last_error(data->output, "OBS video format must be NV12");
		return false;
	}

	uint32_t out_w = video_output_get_width(out_video);
	uint32_t out_h = video_output_get_height(out_video);
	if (out_w > 0) data->ovi.output_width = out_w;
	if (out_h > 0) data->ovi.output_height = out_h;
	data->ovi.output_format = VIDEO_FORMAT_NV12;

	data->rtsp_server = obs_data_get_bool(data->settings, "rtsp_server");
	data->webrtc_output = obs_data_get_bool(data->settings, "webrtc_output");
	const char *gst_format = "NV12";
	gst_video_info_init(&data->raw_video_info);
	if (!gst_video_info_set_format(&data->raw_video_info, GST_VIDEO_FORMAT_NV12,
		    data->ovi.output_width, data->ovi.output_height)) {
		blog(LOG_ERROR, "[obs-gstreamer] Failed to initialize NV12 video layout (%ux%u)",
			data->ovi.output_width, data->ovi.output_height);
		return false;
	}
	data->buffer_size = GST_VIDEO_INFO_SIZE(&data->raw_video_info);
	gstreamer_cpu_frame_slot_unref(data->cpu_frame_slot);
	data->cpu_frame_slot = gstreamer_cpu_frame_slot_create(data->buffer_size);
	if (!data->cpu_frame_slot) {
		blog(LOG_ERROR, "[obs-gstreamer] Failed to allocate CPU frame slot (%zu bytes)", data->buffer_size);
		return false;
	}
	data->raw_video_frames = 0;
	data->raw_video_drops = 0;
	memset(&data->frame_cache, 0, sizeof(data->frame_cache));

	blog(LOG_INFO, "[obs-gstreamer] Output video config: %dx%d @ %d/%d fps, format=%d (%s), frame size=%zu bytes, mode=%s",
		data->ovi.output_width, data->ovi.output_height,
		data->ovi.fps_num, data->ovi.fps_den,
		data->ovi.output_format, gst_format, data->buffer_size,
		data->rtsp_server ? "RTSP Server" : (data->webrtc_output ? "WebRTC (WHEP)" : "Custom Pipeline"));

	if (data->rtsp_server) {
		const char *mount = obs_data_get_string(data->settings, "rtsp_mount");
		const char *service = obs_data_get_string(data->settings, "rtsp_service");
		const char *pipeline = obs_data_get_string(data->settings, "rtsp_pipeline");
		const char *default_pipe = "( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true max-buffers=1 leaky-type=downstream ! video/x-raw, format=%s, width=%d, height=%d, framerate=%d/%d ! x264enc tune=zerolatency speed-preset=veryfast bitrate=3000 key-int-max=30 ! video/x-h264, stream-format=byte-stream, alignment=au ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=-1 )";
		const char *chosen_pipe = (pipeline && pipeline[0]) ? pipeline : default_pipe;
		char *launch = NULL;
		if (strstr(chosen_pipe, "%s")) {
			launch = g_strdup_printf(chosen_pipe, gst_format, data->ovi.output_width, data->ovi.output_height,
				data->ovi.fps_num, data->ovi.fps_den);
		} else {
			launch = g_strdup(chosen_pipe);
		}

		blog(LOG_INFO, "[obs-gstreamer] Starting RTSP server: service/port=%s, mount=%s",
			service && service[0] ? service : "8554", mount && mount[0] ? mount : "/live");
		blog(LOG_INFO, "[obs-gstreamer] RTSP pipeline string: %s", launch);

		data->server = gst_rtsp_server_new();
		gst_rtsp_server_set_address(data->server, "0.0.0.0");
		if (service && service[0])
			gst_rtsp_server_set_service(data->server, service);
		data->mount_point = g_strdup(mount && mount[0] ? mount : "/live");
		data->mounts = gst_rtsp_server_get_mount_points(data->server);
		data->factory = gst_rtsp_media_factory_new();
		gst_rtsp_media_factory_set_shared(data->factory, TRUE);
		gst_rtsp_media_factory_set_protocols(data->factory, GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_UDP_MCAST | GST_RTSP_LOWER_TRANS_TCP);
		gst_rtsp_media_factory_set_launch(data->factory, launch);
		g_signal_connect(data->server, "client-connected", G_CALLBACK(client_connected_cb), data);
		g_signal_connect(data->factory, "media-configure", G_CALLBACK(media_configure_cb), data);
		gst_rtsp_mount_points_add_factory(data->mounts, data->mount_point, data->factory);

		data->rtsp_context = g_main_context_new();
		data->rtsp_loop = g_main_loop_new(data->rtsp_context, FALSE);
		data->server_source_id = gst_rtsp_server_attach(data->server, data->rtsp_context);
		if (data->server_source_id == 0) {
			blog(LOG_ERROR, "[obs-gstreamer] Failed to attach RTSP server on port %s: address already in use or bind failed", service && service[0] ? service : "8554");
			obs_output_set_last_error(data->output, "Failed to start RTSP server: port already in use or bind failed");
			if (data->rtsp_loop) {
				g_main_loop_unref(data->rtsp_loop);
				data->rtsp_loop = NULL;
			}
			if (data->rtsp_context) {
				g_main_context_unref(data->rtsp_context);
				data->rtsp_context = NULL;
			}
			if (data->mounts && data->mount_point) {
				gst_rtsp_mount_points_remove_factory(data->mounts, data->mount_point);
			}
			if (data->factory) {
				g_signal_handlers_disconnect_by_data(data->factory, data);
				g_object_unref(data->factory);
				data->factory = NULL;
			}
			if (data->mounts) {
				g_object_unref(data->mounts);
				data->mounts = NULL;
			}
			if (data->server) {
				g_signal_handlers_disconnect_by_data(data->server, data);
				gst_object_unref(data->server);
				data->server = NULL;
			}
			g_free(data->mount_point);
			data->mount_point = NULL;
			g_free(launch);
			return false;
		}
		data->rtsp_thread = g_thread_new("obs_gst_rtsp_loop", (GThreadFunc)g_main_loop_run, data->rtsp_loop);
		g_free(launch);
		blog(LOG_INFO, "[obs-gstreamer] RTSP server started at rtsp://0.0.0.0:%s%s", gst_rtsp_server_get_service(data->server), data->mount_point);
	} else if (data->webrtc_output) {
		char *error = NULL;
		blog(LOG_INFO, "[obs-gstreamer] Initializing WebRTC (WHEP) output...");
		data->webrtc = gstreamer_webrtc_create(data->output, data->settings, &data->ovi, &error);
		if (!data->webrtc) {
			blog(LOG_ERROR, "[obs-gstreamer] WebRTC init failed: %s",
				error ? error : "unknown error");
			obs_output_set_last_error(data->output, error ? error : "WebRTC output failed to start");
			g_free(error);
			return false;
		}
		g_free(error);
		GstElement *webrtc_video = gstreamer_webrtc_appsrc(data->webrtc);
		swap_appsrc_elements(data, webrtc_video, NULL, NULL, NULL);
		blog(LOG_INFO, "[obs-gstreamer] WebRTC output started successfully");
	} else {
		GError *err = NULL;
		char *pipe_string = NULL;
		const char *user_pipe = obs_data_get_string(data->settings, "pipeline");
		bool is_encoded = (obs_output_get_flags(data->output) & OBS_OUTPUT_ENCODED) != 0;

		if (user_pipe && strstr(user_pipe, "appsrc")) {
			pipe_string = g_strdup(user_pipe);
		} else if (is_encoded) {
			pipe_string = g_strdup_printf(
				"appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true caps=\"video/x-h264, stream-format=byte-stream, alignment=au\" ! %s",
				(user_pipe && user_pipe[0]) ? user_pipe : "fakesink");
		} else {
			pipe_string = g_strdup_printf(
				"appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true max-buffers=1 leaky-type=downstream ! video/x-raw, format=%s, width=%d, height=%d, framerate=%d/%d ! videoconvert ! %s",
				gst_format, data->ovi.output_width, data->ovi.output_height,
				data->ovi.fps_num, data->ovi.fps_den,
				(user_pipe && user_pipe[0]) ? user_pipe : "autovideosink sync=false");
		}

		blog(LOG_INFO, "[obs-gstreamer] Launching custom pipeline: %s", pipe_string);
		data->pipe = gstreamer_parse_launch_checked(pipe_string, &err);
		g_free(pipe_string);

		if (err) {
			blog(LOG_ERROR, "[obs-gstreamer] gst_parse_launch error: %s", err->message);
			obs_output_set_last_error(data->output, err->message);
			g_error_free(err);
			return false;
		}

		GstElement *video_el = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc_video");
		if (!video_el) {
			blog(LOG_ERROR, "[obs-gstreamer] 'appsrc_video' element not found in custom pipeline");
			obs_output_set_last_error(data->output, "Pipeline missing 'appsrc_video' element");
			gst_object_unref(data->pipe);
			data->pipe = NULL;
			return false;
		}
		GstElement *audio_el = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc_audio");
		swap_appsrc_elements(data, video_el, audio_el, NULL, NULL);

		GstBus *bus = gst_element_get_bus(data->pipe);
		gst_bus_add_watch(bus, bus_callback, data);
		gst_object_unref(bus);

		GstStateChangeReturn s_ret = gst_element_set_state(data->pipe, GST_STATE_PLAYING);
		blog(LOG_INFO, "[obs-gstreamer] Custom pipeline state set to PLAYING (ret=%d)", s_ret);
	}

	g_atomic_int_set(&data->streaming, 1);
	if (!obs_output_begin_data_capture(data->output, 0)) {
		g_atomic_int_set(&data->streaming, 0);
		blog(LOG_ERROR, "[obs-gstreamer] obs_output_begin_data_capture failed");
		return false;
	}
	blog(LOG_INFO, "[obs-gstreamer] Output data capture began successfully");
	return true;
}

void gstreamer_output_stop(void *p, uint64_t ts)
{
	struct gstreamer_output_data *data = (struct gstreamer_output_data *)p;
	if (!data) return;

	blog(LOG_INFO, "[obs-gstreamer] Stopping output '%s'...",
		data->output ? obs_output_get_name(data->output) : "unnamed");

	g_atomic_int_set(&data->streaming, 0);
	obs_output_end_data_capture(data->output);
	blog(LOG_INFO, "[obs-gstreamer] obs_output_end_data_capture stopped");

	GstElement *video = NULL;
	GstElement *audio = NULL;
	swap_appsrc_elements(data, NULL, NULL, &video, &audio);

	if (video) {
		gst_app_src_end_of_stream(GST_APP_SRC(video));
		gst_object_unref(video);
	}
	if (audio) {
		gst_app_src_end_of_stream(GST_APP_SRC(audio));
		gst_object_unref(audio);
	}

	if (data->media) {
		g_signal_handlers_disconnect_by_data(data->media, data);
		gst_object_unref(data->media);
		data->media = NULL;
	}

	if (data->rtsp_loop) {
		g_main_loop_quit(data->rtsp_loop);
	}
	if (data->rtsp_thread) {
		g_thread_join(data->rtsp_thread);
		data->rtsp_thread = NULL;
	}
	if (data->rtsp_loop) {
		g_main_loop_unref(data->rtsp_loop);
		data->rtsp_loop = NULL;
	}

	if (data->server) {
		if (data->server_source_id > 0) {
			if (data->rtsp_context) {
				GSource *source = g_main_context_find_source_by_id(data->rtsp_context, data->server_source_id);
				if (source)
					g_source_destroy(source);
			} else {
				g_source_remove(data->server_source_id);
			}
			data->server_source_id = 0;
		}
		if (data->mounts && data->mount_point) {
			gst_rtsp_mount_points_remove_factory(data->mounts, data->mount_point);
		}
		if (data->factory) {
			g_signal_handlers_disconnect_by_data(data->factory, data);
			g_object_unref(data->factory);
			data->factory = NULL;
		}
		if (data->mounts) {
			g_object_unref(data->mounts);
			data->mounts = NULL;
		}
		if (data->server) {
			g_signal_handlers_disconnect_by_data(data->server, data);
			gst_object_unref(data->server);
			data->server = NULL;
		}
		g_free(data->mount_point);
		data->mount_point = NULL;
		blog(LOG_INFO, "[obs-gstreamer] RTSP server stopped");
	}

	if (data->rtsp_context) {
		g_main_context_unref(data->rtsp_context);
		data->rtsp_context = NULL;
	}

	if (data->webrtc) {
		gstreamer_webrtc_destroy(data->webrtc);
		data->webrtc = NULL;
		blog(LOG_INFO, "[obs-gstreamer] WebRTC server stopped");
	}

	if (data->pipe) {
		GstBus *bus = gst_element_get_bus(data->pipe);
		if (bus) {
			gst_bus_remove_watch(bus);
			gst_object_unref(bus);
		}

		gst_element_set_state(data->pipe, GST_STATE_NULL);
		gst_object_unref(data->pipe);
		data->pipe = NULL;
		blog(LOG_INFO, "[obs-gstreamer] Custom pipeline stopped and unref complete");
	}

	gstreamer_cpu_frame_slot_unref(data->cpu_frame_slot);
	data->cpu_frame_slot = NULL;

	blog(LOG_INFO, "[obs-gstreamer] Output stopped cleanly");
}

void gstreamer_output_encoded_packet(void *p, struct encoder_packet *packet)
{
	struct gstreamer_output_data *data = (struct gstreamer_output_data *)p;
	if (!data || !packet || !g_atomic_int_get(&data->streaming))
		return;

	g_atomic_int_inc(&data->in_flight);
	if (!g_atomic_int_get(&data->streaming)) {
		g_atomic_int_dec_and_test(&data->in_flight);
		return;
	}

	GstElement *video_el = (GstElement *)g_atomic_pointer_get(&data->video);
	GstElement *video = video_el ? gst_object_ref(video_el) : NULL;
	if (!video) {
		g_atomic_int_dec_and_test(&data->in_flight);
		return;
	}

	GstBuffer *buffer = gst_buffer_new_allocate(NULL, packet->size, NULL);
	gst_buffer_fill(buffer, 0, packet->data, packet->size);

	GST_BUFFER_PTS(buffer) = packet->pts * GST_SECOND / (packet->timebase_den / packet->timebase_num);
	GST_BUFFER_DTS(buffer) = packet->dts * GST_SECOND / (packet->timebase_den / packet->timebase_num);

	gst_buffer_set_flags(buffer, packet->keyframe ? 0 : GST_BUFFER_FLAG_DELTA_UNIT);

	GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(video), buffer);
	if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING) {
		blog(LOG_WARNING, "[obs-gstreamer] Encoded packet appsrc push failed: %s (size=%zu)",
			gst_flow_get_name(ret), packet->size);
	}
	gst_object_unref(video);
	g_atomic_int_dec_and_test(&data->in_flight);
}

void gstreamer_output_raw_video(void *p, struct video_data *frame)
{
	struct gstreamer_output_data *data = (struct gstreamer_output_data *)p;
	if (!data || !frame || !frame->data[0] || !g_atomic_int_get(&data->streaming))
		return;

	g_atomic_int_inc(&data->in_flight);
	if (!g_atomic_int_get(&data->streaming)) {
		g_atomic_int_dec_and_test(&data->in_flight);
		return;
	}

	GstElement *video_el = (GstElement *)g_atomic_pointer_get(&data->video);
	GstElement *video = video_el ? gst_object_ref(video_el) : NULL;
	if (!video) {
		g_atomic_int_dec_and_test(&data->in_flight);
		return;
	}

	uint64_t frame_num = ++data->raw_video_frames;
	const bool is_first_frame = (data->frame_cache.mode == GSTREAMER_FRAME_MODE_UNSET);
	uint64_t t_start = is_first_frame ? os_gettime_ns() : 0;

	bool slot_busy = false;
	GstBuffer *buffer = gstreamer_create_obs_video_buffer(
		&data->raw_video_info,
		&data->frame_cache,
		data->cpu_frame_slot,
		frame,
		&slot_busy);
	if (!buffer) {
		if (slot_busy) {
			uint64_t dropped = ++data->raw_video_drops;
			if (dropped == 1 || dropped % 300 == 0)
				blog(LOG_WARNING, "[obs-gstreamer] Raw video slot busy: dropped frame #%llu",
					(unsigned long long)dropped);
		} else {
			blog(LOG_ERROR, "[obs-gstreamer] Invalid plane layout while preparing raw video frame");
		}
		gst_object_unref(video);
		g_atomic_int_dec_and_test(&data->in_flight);
		return;
	}

	GST_BUFFER_PTS(buffer) = frame->timestamp;
	GST_BUFFER_DTS(buffer) = frame->timestamp;
	if (data->ovi.fps_num > 0) {
		GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale_int(GST_SECOND, data->ovi.fps_den, data->ovi.fps_num);
	}

	GstFlowReturn result = gst_app_src_push_buffer(GST_APP_SRC(video), buffer);
	if (is_first_frame) {
		uint64_t t_elapsed_ns = os_gettime_ns() - t_start;
		blog(LOG_INFO, "[obs-gstreamer] Chosen video frame path: %s (%dx%d NV12, %zu bytes, linesize=[%u,%u], first_frame_push=%.3f ms, result=%s)",
			gstreamer_frame_mode_name(data->frame_cache.mode),
			data->ovi.output_width, data->ovi.output_height, data->buffer_size,
			frame->linesize[0], frame->linesize[1],
			(double)t_elapsed_ns / 1000000.0, gst_flow_get_name(result));
	} else if (frame_num % 300 == 0) {
		blog(LOG_INFO, "[obs-gstreamer] Raw video frame %llu pushed (flow=%s)",
			(unsigned long long)frame_num, gst_flow_get_name(result));
	}

	if (result != GST_FLOW_OK && result != GST_FLOW_FLUSHING) {
		blog(LOG_WARNING, "[obs-gstreamer] Raw video appsrc push failed: %s (frame %llu)",
			gst_flow_get_name(result), (unsigned long long)frame_num);
	}

	gst_object_unref(video);
	g_atomic_int_dec_and_test(&data->in_flight);
}

void gstreamer_output_raw_audio(void *p, struct audio_data *frame)
{
	struct gstreamer_output_data *data = (struct gstreamer_output_data *)p;
	if (!data || !frame || frame->frames == 0 || !g_atomic_int_get(&data->streaming))
		return;

	g_atomic_int_inc(&data->in_flight);
	if (!g_atomic_int_get(&data->streaming)) {
		g_atomic_int_dec_and_test(&data->in_flight);
		return;
	}

	GstElement *audio_el = (GstElement *)g_atomic_pointer_get(&data->audio);
	GstElement *audio = audio_el ? gst_object_ref(audio_el) : NULL;
	if (!audio) {
		g_atomic_int_dec_and_test(&data->in_flight);
		return;
	}

	struct obs_audio_info oai;
	if (!obs_get_audio_info(&oai)) {
		gst_object_unref(audio);
		g_atomic_int_dec_and_test(&data->in_flight);
		return;
	}

	size_t channels = get_audio_channels(oai.speakers);
	if (channels == 0) {
		gst_object_unref(audio);
		g_atomic_int_dec_and_test(&data->in_flight);
		return;
	}

	size_t total_size = frame->frames * channels * sizeof(float);
	GstBuffer *buffer = gst_buffer_new_allocate(NULL, total_size, NULL);
	if (!buffer) {
		gst_object_unref(audio);
		g_atomic_int_dec_and_test(&data->in_flight);
		return;
	}

	GstMapInfo map;
	if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
		float *dst = (float *)map.data;
		for (size_t f = 0; f < frame->frames; f++) {
			for (size_t c = 0; c < channels; c++) {
				if (frame->data[c]) {
					const float *src = (const float *)frame->data[c];
					*dst++ = src[f];
				} else {
					*dst++ = 0.0f;
				}
			}
		}
		gst_buffer_unmap(buffer, &map);
	}

	GST_BUFFER_PTS(buffer) = frame->timestamp;
	GST_BUFFER_DTS(buffer) = frame->timestamp;
	if (oai.samples_per_sec > 0)
		GST_BUFFER_DURATION(buffer) = (GstClockTime)frame->frames * GST_SECOND / oai.samples_per_sec;

	GstFlowReturn a_ret = gst_app_src_push_buffer(GST_APP_SRC(audio), buffer);
	if (a_ret != GST_FLOW_OK && a_ret != GST_FLOW_FLUSHING) {
		blog(LOG_WARNING, "[obs-gstreamer] Raw audio appsrc push failed: %s", gst_flow_get_name(a_ret));
	}
	gst_object_unref(audio);
	g_atomic_int_dec_and_test(&data->in_flight);
}

void gstreamer_output_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline", "autovideosink sync=false");
	obs_data_set_default_bool(settings, "rtsp_server", false);
	obs_data_set_default_string(settings, "rtsp_mount", "/live");
	obs_data_set_default_string(settings, "rtsp_service", "8554");
	obs_data_set_default_string(settings, "rtsp_pipeline", "( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true max-buffers=1 leaky-type=downstream ! video/x-raw, format=%s, width=%d, height=%d, framerate=%d/%d ! x264enc tune=zerolatency speed-preset=veryfast bitrate=3000 key-int-max=30 ! video/x-h264, stream-format=byte-stream, alignment=au ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=-1 )");
	obs_data_set_default_bool(settings, "webrtc_output", false);
	obs_data_set_default_string(settings, "webrtc_http_port", "8888");
	obs_data_set_default_string(settings, "webrtc_stun_server", "");
	obs_data_set_default_string(settings, "webrtc_turn_server", "");
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

	obs_property_t *turn = obs_properties_add_text(props, "webrtc_turn_server", "WebRTC TURN server", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(turn, "Optional TURN relay (e.g. turn://user:pass@host:port or turns://user:pass@host:443?transport=tcp). Required for Cloud/WAN without open UDP; leave empty for LAN.");

	obs_property_t *web_root = obs_properties_add_text(props, "webrtc_web_root", "WebRTC web root", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(web_root, "Folder with viewer page files (index.html, style.css, app.js). Empty = ~/.local/share/obs-gstreamer/webrtc");

	// Legacy key kept for saved configs; ignored at runtime.
	obs_properties_add_text(props, "webrtc_signaling_url", "WebRTC signaling URL (legacy, ignored)", OBS_TEXT_DEFAULT);

	obs_property_t *prop = obs_properties_add_text(props, "pipeline", "Pipeline", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(prop, "pipeline for gstreamer-output. This is ignored when RTSP server or WebRTC output mode is enabled.");
	obs_property_set_description(prop, "Pipeline");
	return props;
}
#ifdef _MSC_VER
#pragma warning(default : 4047)
#pragma warning(default : 4244)
#endif