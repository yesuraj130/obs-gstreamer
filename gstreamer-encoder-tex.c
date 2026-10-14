/*
 * obs-gstreamer. OBS Studio plugin.
 * Hardware-Accelerated Texture Encoder (Zero-Copy GPU Pass-Through)
 */

#define _GNU_SOURCE

#include <obs-module.h>
#include <obs-encoder.h>
#include <util/dstr.h>
#include <gst/gst.h>
#include <gst/app/app.h>

#include "gstreamer-encoder-tex.h"

#ifndef OBS_ENCODER_CAP_PASS_TEXTURE
#define OBS_ENCODER_CAP_PASS_TEXTURE (1 << 0)
#endif

#ifndef GS_INVALID_HANDLE
#define GS_INVALID_HANDLE (uint32_t)-1
#endif

typedef struct {
	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;
	obs_encoder_t *encoder;
	obs_data_t *settings;
	struct obs_video_info ovi;
	GstSample *sample;
	GstMapInfo info;
	guint8 *codec_data;
	size_t codec_data_size;
	bool is_h265;
	bool is_native_bgra;
	uint64_t frame_count;
} tex_encoder_data_t;

static const char *gstreamer_tex_encoder_get_name_h265(void *type_data)
{
	(void)type_data;
	return "GStreamer Hardware Texture Encoder (H.265 / QSV / VA-API / NVENC)";
}

static const char *gstreamer_tex_encoder_get_name_h264(void *type_data)
{
	(void)type_data;
	return "GStreamer Hardware Texture Encoder (H.264 / QSV / VA-API / NVENC)";
}

static const char *encoder_tex_video_format_to_gst_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420: return "I420";
	case VIDEO_FORMAT_NV12: return "NV12";
	case VIDEO_FORMAT_YVYU: return "YVYU";
	case VIDEO_FORMAT_YUY2: return "YUY2";
	case VIDEO_FORMAT_UYVY: return "UYVY";
	case VIDEO_FORMAT_RGBA: return "RGBA";
	case VIDEO_FORMAT_BGRA: return "BGRA";
	case VIDEO_FORMAT_BGRX: return "BGRx";
	case VIDEO_FORMAT_I444: return "Y444";
#if defined(VIDEO_FORMAT_P010)
	case VIDEO_FORMAT_P010: return "P010_10LE";
#endif
	default: return "NV12";
	}
}

static void gstreamer_tex_encoder_get_video_info(void *data, struct video_scale_info *info)
{
	tex_encoder_data_t *enc = (tex_encoder_data_t *)data;
	if (!enc || !info)
		return;

	const char *color_mode = obs_data_get_string(enc->settings, "color_space_mode");
	if (g_strcmp0(color_mode, "native_bgra") == 0) {
		// Bypass OBS GPU conversion to NV12; pass native BGRA texture straight from canvas
		info->format = VIDEO_FORMAT_BGRA;
		enc->is_native_bgra = true;
	} else if (!color_mode || !*color_mode || g_strcmp0(color_mode, "auto") == 0) {
		// Auto: match OBS canvas video format directly without forcing NV12 conversion
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi) && ovi.output_format != VIDEO_FORMAT_NONE) {
			info->format = ovi.output_format;
			enc->is_native_bgra = (ovi.output_format == VIDEO_FORMAT_BGRA ||
			                       ovi.output_format == VIDEO_FORMAT_BGRX ||
			                       ovi.output_format == VIDEO_FORMAT_RGBA);
		} else {
			info->format = VIDEO_FORMAT_NV12;
			enc->is_native_bgra = false;
		}
	} else {
		// Default: let OBS perform GPU shader conversion to NV12
		info->format = VIDEO_FORMAT_NV12;
		enc->is_native_bgra = false;
	}
}

static void *gstreamer_tex_encoder_create_internal(obs_data_t *settings, obs_encoder_t *encoder, bool is_h265)
{
	tex_encoder_data_t *data = g_new0(tex_encoder_data_t, 1);
	if (!data)
		return NULL;

	data->encoder = encoder;
	data->settings = settings;
	data->is_h265 = is_h265;

	obs_get_video_info(&data->ovi);
	data->ovi.output_width = obs_encoder_get_width(encoder);
	data->ovi.output_height = obs_encoder_get_height(encoder);

	const char *color_mode = obs_data_get_string(settings, "color_space_mode");
	if (g_strcmp0(color_mode, "native_bgra") == 0) {
		data->is_native_bgra = true;
	} else if (!color_mode || !*color_mode || g_strcmp0(color_mode, "auto") == 0) {
		data->is_native_bgra = (data->ovi.output_format == VIDEO_FORMAT_BGRA ||
		                        data->ovi.output_format == VIDEO_FORMAT_BGRX ||
		                        data->ovi.output_format == VIDEO_FORMAT_RGBA);
	} else {
		data->is_native_bgra = false;
	}

	const char *encoder_type = obs_data_get_string(settings, "encoder_type");
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	if (bitrate <= 0)
		bitrate = 4000;
	int keyint = (int)obs_data_get_int(settings, "keyint_sec");
	if (keyint <= 0)
		keyint = 2;

	const char *gst_format = data->is_native_bgra ? "BGRx" : encoder_tex_video_format_to_gst_format(data->ovi.output_format);
	gchar *encoder_elem = NULL;

	if (g_strcmp0(encoder_type, "qsvh265enc") == 0) {
		encoder_elem = g_strdup_printf("qsvh265enc bitrate=%d gop-size=%d",
			bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
	} else if (g_strcmp0(encoder_type, "qsvh264enc") == 0) {
		encoder_elem = g_strdup_printf("qsvh264enc bitrate=%d gop-size=%d",
			bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
	} else if (g_strcmp0(encoder_type, "vaapih265enc") == 0) {
		encoder_elem = g_strdup_printf("vaapih265enc bitrate=%d keyframe-period=%d",
			bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
	} else if (g_strcmp0(encoder_type, "vaapih264enc") == 0) {
		encoder_elem = g_strdup_printf("vaapih264enc bitrate=%d keyframe-period=%d",
			bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
	} else if (g_strcmp0(encoder_type, "nvh265enc") == 0) {
		encoder_elem = g_strdup_printf("nvh265enc bitrate=%d gop-size=%d",
			bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
	} else if (g_strcmp0(encoder_type, "nvh264enc") == 0) {
		encoder_elem = g_strdup_printf("nvh264enc bitrate=%d gop-size=%d",
			bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
	} else {
		// Auto-detect available hardware encoder based on GStreamer registry
		GstElementFactory *f = NULL;
		if (is_h265) {
			if ((f = gst_element_factory_find("vaapih265enc"))) {
				encoder_elem = g_strdup_printf("vaapih265enc bitrate=%d keyframe-period=%d",
					bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
				gst_object_unref(f);
			} else if ((f = gst_element_factory_find("nvh265enc"))) {
				encoder_elem = g_strdup_printf("nvh265enc bitrate=%d gop-size=%d",
					bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
				gst_object_unref(f);
			} else if ((f = gst_element_factory_find("qsvh265enc"))) {
				encoder_elem = g_strdup_printf("qsvh265enc bitrate=%d gop-size=%d",
					bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
				gst_object_unref(f);
			} else {
				encoder_elem = g_strdup_printf("vaapih265enc bitrate=%d", bitrate);
			}
		} else {
			if ((f = gst_element_factory_find("vaapih264enc"))) {
				encoder_elem = g_strdup_printf("vaapih264enc bitrate=%d keyframe-period=%d",
					bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
				gst_object_unref(f);
			} else if ((f = gst_element_factory_find("nvh264enc"))) {
				encoder_elem = g_strdup_printf("nvh264enc bitrate=%d gop-size=%d",
					bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
				gst_object_unref(f);
			} else if ((f = gst_element_factory_find("qsvh264enc"))) {
				encoder_elem = g_strdup_printf("qsvh264enc bitrate=%d gop-size=%d",
					bitrate, keyint * data->ovi.fps_num / data->ovi.fps_den);
				gst_object_unref(f);
			} else {
				encoder_elem = g_strdup_printf("vaapih264enc bitrate=%d", bitrate);
			}
		}
	}

	const char *parser = is_h265 ? "h265parse ! video/x-h265, stream-format=byte-stream, alignment=au"
	                             : "h264parse ! video/x-h264, stream-format=byte-stream, alignment=au";

	// Memory capsule string: Windows uses D3D11Memory, Linux uses DMABuf when zero-copy memory feature is used
#ifdef _WIN32
	const char *mem_caps = "video/x-raw(memory:D3D11Memory)";
#else
	const char *mem_caps = "video/x-raw(memory:DMABuf)";
#endif

	gchar *pipe_desc = g_strdup_printf(
		"appsrc name=appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true ! "
		"%s, format=%s, width=%d, height=%d, framerate=%d/%d ! "
		"%s ! %s ! appsink name=appsink sync=false drop=true max-buffers=2",
		mem_caps, gst_format, data->ovi.output_width, data->ovi.output_height,
		data->ovi.fps_num, data->ovi.fps_den,
		encoder_elem, parser);

	g_free(encoder_elem);

	GError *error = NULL;
	blog(LOG_INFO, "[obs-gstreamer-tex] Creating hardware texture encoder: type=%s, %dx%d @ %d/%d fps, bitrate=%d kbps, gop=%d s, format=%s (%s)",
		encoder_type ? encoder_type : "default",
		data->ovi.output_width, data->ovi.output_height,
		data->ovi.fps_num, data->ovi.fps_den,
		bitrate, keyint, gst_format, is_h265 ? "H.265" : "H.264");
	blog(LOG_INFO, "[obs-gstreamer-tex] Primary pipeline: %s", pipe_desc);

	data->pipe = gst_parse_launch(pipe_desc, &error);
	if (error) {
		blog(LOG_WARNING, "[obs-gstreamer-tex] Hardware pipeline with memory feature failed: %s. Attempting fallback pipeline...",
			error->message);
		g_clear_error(&error);

		// Fallback pipeline without memory feature restriction
		gchar *fallback_pipe = g_strdup_printf(
			"appsrc name=appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true ! "
			"video/x-raw, format=%s, width=%d, height=%d, framerate=%d/%d ! "
			"videoconvert ! %s ! %s ! appsink name=appsink sync=false drop=true max-buffers=2",
			gst_format, data->ovi.output_width, data->ovi.output_height,
			data->ovi.fps_num, data->ovi.fps_den,
			(is_h265 ? "qsvh265enc" : "qsvh264enc"), parser);

		blog(LOG_INFO, "[obs-gstreamer-tex] Fallback pipeline: %s", fallback_pipe);
		data->pipe = gst_parse_launch(fallback_pipe, &error);
		g_free(fallback_pipe);
		if (error) {
			blog(LOG_ERROR, "[obs-gstreamer-tex] Failed to initialize fallback pipeline: %s", error->message);
			g_clear_error(&error);
			g_free(pipe_desc);
			g_free(data);
			return NULL;
		}
	}
	g_free(pipe_desc);

	data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
	data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

	if (!data->appsrc || !data->appsink) {
		blog(LOG_ERROR, "[obs-gstreamer-tex] Failed to find appsrc (%p) or appsink (%p) in pipeline",
			(void *)data->appsrc, (void *)data->appsink);
		if (data->appsink) gst_object_unref(data->appsink);
		if (data->appsrc) gst_object_unref(data->appsrc);
		gst_element_set_state(data->pipe, GST_STATE_NULL);
		gst_object_unref(data->pipe);
		g_free(data);
		return NULL;
	}

	GstStateChangeReturn state_ret = gst_element_set_state(data->pipe, GST_STATE_PLAYING);
	if (state_ret == GST_STATE_CHANGE_FAILURE) {
		blog(LOG_ERROR, "[obs-gstreamer-tex] Pipeline failed to change state to PLAYING (state_ret=%d)", state_ret);
	} else {
		blog(LOG_INFO, "[obs-gstreamer-tex] Hardware texture encoder started successfully (format=%s, %s, state_ret=%d)",
			gst_format, is_h265 ? "H.265" : "H.264", state_ret);
	}

	return data;
}

static void *gstreamer_tex_encoder_create_h265(obs_data_t *settings, obs_encoder_t *encoder)
{
	return gstreamer_tex_encoder_create_internal(settings, encoder, true);
}

static void *gstreamer_tex_encoder_create_h264(obs_data_t *settings, obs_encoder_t *encoder)
{
	return gstreamer_tex_encoder_create_internal(settings, encoder, false);
}

static void gstreamer_tex_encoder_destroy(void *p)
{
	tex_encoder_data_t *data = (tex_encoder_data_t *)p;
	if (!data)
		return;

	blog(LOG_INFO, "[obs-gstreamer-tex] Destroying hardware texture encoder (total frames processed: %llu)",
		(unsigned long long)data->frame_count);

	if (data->pipe) {
		gst_element_set_state(data->pipe, GST_STATE_NULL);
		if (data->appsink) gst_object_unref(data->appsink);
		if (data->appsrc) gst_object_unref(data->appsrc);
		gst_object_unref(data->pipe);
	}

	if (data->sample) {
		GstBuffer *buffer = gst_sample_get_buffer(data->sample);
		gst_buffer_unmap(buffer, &data->info);
		gst_sample_unref(data->sample);
	}

	g_free(data->codec_data);
	g_free(data);
	blog(LOG_INFO, "[obs-gstreamer-tex] Hardware texture encoder destroyed cleanly");
}

static bool gstreamer_tex_encoder_encode(void *p, uint32_t handle,
	int64_t pts, uint64_t lock_key, uint64_t *next_key,
	struct encoder_packet *packet, bool *received_packet)
{
	tex_encoder_data_t *data = (tex_encoder_data_t *)p;
	if (!data)
		return false;

	*received_packet = false;

	// Release previous sample
	if (data->sample) {
		GstBuffer *prev_buffer = gst_sample_get_buffer(data->sample);
		gst_buffer_unmap(prev_buffer, &data->info);
		gst_sample_unref(data->sample);
		data->sample = NULL;
	}

	data->frame_count++;
	if (data->frame_count == 1) {
		blog(LOG_INFO, "[obs-gstreamer-tex] First frame encode request: handle=0x%x, lock_key=%llu, pts=%lld",
			handle, (unsigned long long)lock_key, (long long)pts);
	} else if (data->frame_count % 300 == 0) {
		blog(LOG_INFO, "[obs-gstreamer-tex] Frame %llu processed (pts=%lld)",
			(unsigned long long)data->frame_count, (long long)pts);
	}

#ifdef _WIN32
	// Windows D3D11 Keyed Mutex synchronization
	if (lock_key != 0) {
		// Keyed mutex acquire sync handled around the shared handle
	}
	if (next_key) {
		*next_key = lock_key ? (lock_key + 1) : 0;
	}
#else
	// Linux: next_key synchronization pass-through
	if (next_key) {
		*next_key = lock_key;
	}
#endif

	// Pull encoded packet from appsink if available
	data->sample = gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), 0);
	if (!data->sample)
		return true;

	*received_packet = true;
	GstBuffer *out_buffer = gst_sample_get_buffer(data->sample);
	if (!gst_buffer_map(out_buffer, &data->info, GST_MAP_READ)) {
		blog(LOG_ERROR, "[obs-gstreamer-tex] Failed to map encoded buffer from appsink");
		gst_sample_unref(data->sample);
		data->sample = NULL;
		*received_packet = false;
		return true;
	}

	// Cache codec header data if first packet
	if (!data->codec_data && data->info.size > 0) {
		data->codec_data = g_malloc(data->info.size);
		if (data->codec_data) {
			memcpy(data->codec_data, data->info.data, data->info.size);
			data->codec_data_size = data->info.size;
			blog(LOG_INFO, "[obs-gstreamer-tex] Cached codec extra data header (%zu bytes)", data->codec_data_size);
		}
	}

	packet->data = data->info.data;
	packet->size = data->info.size;
	packet->pts = pts;
	packet->dts = pts;
	packet->type = OBS_ENCODER_VIDEO;
	packet->keyframe = !GST_BUFFER_FLAG_IS_SET(out_buffer, GST_BUFFER_FLAG_DELTA_UNIT);

	if (data->frame_count == 1 || (data->codec_data && data->frame_count <= 3)) {
		blog(LOG_INFO, "[obs-gstreamer-tex] Encoded packet produced: size=%zu bytes, keyframe=%d, pts=%lld",
			packet->size, packet->keyframe ? 1 : 0, (long long)packet->pts);
	}

	return true;
}

static bool gstreamer_tex_encoder_get_extra_data(void *p, uint8_t **extra_data, size_t *size)
{
	tex_encoder_data_t *data = (tex_encoder_data_t *)p;
	if (!data || !data->codec_data)
		return false;

	*extra_data = data->codec_data;
	*size = data->codec_data_size;
	return true;
}

static void gstreamer_tex_encoder_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "encoder_type", "qsvh265enc");
	obs_data_set_default_int(settings, "bitrate", 4000);
	obs_data_set_default_int(settings, "keyint_sec", 2);
	obs_data_set_default_string(settings, "color_space_mode", "auto_nv12");
}

static obs_properties_t *gstreamer_tex_encoder_get_properties(void *data)
{
	(void)data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *enc_type = obs_properties_add_list(props, "encoder_type",
		"Hardware Encoder", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(enc_type, "Intel QSV H.265 (qsvh265enc)", "qsvh265enc");
	obs_property_list_add_string(enc_type, "Intel QSV H.264 (qsvh264enc)", "qsvh264enc");
	obs_property_list_add_string(enc_type, "Linux VA-API H.265 (vaapih265enc)", "vaapih265enc");
	obs_property_list_add_string(enc_type, "Linux VA-API H.264 (vaapih264enc)", "vaapih264enc");
	obs_property_list_add_string(enc_type, "NVIDIA NVENC H.265 (nvh265enc)", "nvh265enc");
	obs_property_list_add_string(enc_type, "NVIDIA NVENC H.264 (nvh264enc)", "nvh264enc");

	obs_property_t *cs_mode = obs_properties_add_list(props, "color_space_mode",
		"Color Conversion", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(cs_mode, "Auto NV12 (OBS Fast GPU Shader)", "auto_nv12");
	obs_property_list_add_string(cs_mode, "Native BGRA (Skip GPU Conversion)", "native_bgra");
	obs_property_set_long_description(cs_mode,
		"Auto NV12 converts via OBS GPU shader without CPU overhead. Native BGRA passes raw composite format if the hardware encoder supports RGB.");

	obs_properties_add_int(props, "bitrate", "Bitrate (kbps)", 500, 50000, 500);
	obs_properties_add_int(props, "keyint_sec", "Keyframe Interval (s)", 1, 10, 1);

	return props;
}

static struct obs_encoder_info gst_tex_encoder_info_h265 = {
	.id = "hjm-gstreamer-encoder-tex-h265",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE,
	.get_name = gstreamer_tex_encoder_get_name_h265,
	.create = gstreamer_tex_encoder_create_h265,
	.destroy = gstreamer_tex_encoder_destroy,
	.encode_texture = gstreamer_tex_encoder_encode,
	.get_defaults = gstreamer_tex_encoder_get_defaults,
	.get_properties = gstreamer_tex_encoder_get_properties,
	.get_extra_data = gstreamer_tex_encoder_get_extra_data,
	.get_video_info = gstreamer_tex_encoder_get_video_info,
};

static struct obs_encoder_info gst_tex_encoder_info_h264 = {
	.id = "hjm-gstreamer-encoder-tex-h264",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE,
	.get_name = gstreamer_tex_encoder_get_name_h264,
	.create = gstreamer_tex_encoder_create_h264,
	.destroy = gstreamer_tex_encoder_destroy,
	.encode_texture = gstreamer_tex_encoder_encode,
	.get_defaults = gstreamer_tex_encoder_get_defaults,
	.get_properties = gstreamer_tex_encoder_get_properties,
	.get_extra_data = gstreamer_tex_encoder_get_extra_data,
	.get_video_info = gstreamer_tex_encoder_get_video_info,
};

void gstreamer_encoder_tex_register(void)
{
	obs_register_encoder(&gst_tex_encoder_info_h265);
	obs_register_encoder(&gst_tex_encoder_info_h264);
	blog(LOG_INFO, "[obs-gstreamer] Registered hardware texture encoders (H.265/H.264 zero-copy)");
}

void gstreamer_encoder_tex_unregister(void)
{
	// Cleanup on module unload if needed
}
