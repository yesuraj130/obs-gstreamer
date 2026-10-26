/*
 * obs-gstreamer: Direct GPU Texture Encoder Callback Source for Master Pipeline
 */

#include "gstreamer-encoder-callback.h"
#include <obs-module.h>

#ifndef OBS_ENCODER_CAP_PASS_TEXTURE
#define OBS_ENCODER_CAP_PASS_TEXTURE (1 << 0)
#endif

typedef struct
{
	obs_encoder_t *encoder;
	gst_master_pipeline_t *pipeline;
	bool is_active;
} encoder_callback_data_t;

static const char *encoder_callback_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Master Pipeline GPU Texture Encoder";
}

static void encoder_callback_get_video_info(void *data, struct video_scale_info *info)
{
	UNUSED_PARAMETER(data);
	if (!info) return;
	info->format = VIDEO_FORMAT_BGRA;
}

static void *encoder_callback_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	UNUSED_PARAMETER(settings);
	encoder_callback_data_t *enc = bzalloc(sizeof(encoder_callback_data_t));
	enc->encoder = encoder;
	enc->is_active = true;
	return enc;
}

static void encoder_callback_destroy(void *p)
{
	encoder_callback_data_t *enc = (encoder_callback_data_t *)p;
	if (enc) bfree(enc);
}

static bool encoder_callback_encode(void *p, uint32_t handle, int64_t pts, uint64_t lock_key, uint64_t *next_key, struct encoder_packet *packet, bool *received_packet)
{
	UNUSED_PARAMETER(lock_key);
	UNUSED_PARAMETER(next_key);
	UNUSED_PARAMETER(packet);
	if (received_packet) *received_packet = false;

	encoder_callback_data_t *enc = (encoder_callback_data_t *)p;
	if (!enc || !enc->pipeline) return true;

	gst_master_pipeline_push_texture(enc->pipeline, (void *)(uintptr_t)handle, (uint64_t)pts);
	return true;
}

void gstreamer_encoder_callback_set_master_pipeline(obs_encoder_t *encoder, gst_master_pipeline_t *pipe)
{
	if (!encoder) return;
	encoder_callback_data_t *data = (encoder_callback_data_t *)obs_encoder_get_type_data(encoder);
	if (data) data->pipeline = pipe;
}

static struct obs_encoder_info gstreamer_encoder_cb_info = {
	.id = "hjm-gstreamer-encoder-callback",
	.type = OBS_ENCODER_VIDEO,
	.codec = "raw",
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE,
	.get_name = encoder_callback_get_name,
	.create = encoder_callback_create,
	.destroy = encoder_callback_destroy,
	.encode_texture = encoder_callback_encode,
	.get_video_info = encoder_callback_get_video_info,
};

void gstreamer_encoder_callback_register(void)
{
	obs_register_encoder(&gstreamer_encoder_cb_info);
	blog(LOG_INFO, "[obs-gstreamer-encoder-callback] Registered master GPU texture encoder");
}

void gstreamer_encoder_callback_unregister(void)
{
}
