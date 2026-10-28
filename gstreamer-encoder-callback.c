/*
 * obs-gstreamer: Direct GPU Texture Encoder Callback Source for Master Pipeline
 */

#include "gstreamer-encoder-callback.h"
#include <obs-module.h>

/*
 * -----------------------------------------------------------------------------
 * 1. Private Texture Encoder Implementation
 * -----------------------------------------------------------------------------
 */

typedef struct
{
	obs_encoder_t *encoder;
	gst_master_pipeline_t *pipeline;
	bool active;
} encoder_private_data_t;

static const char *encoder_cb_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Master Pipeline GPU Texture Encoder";
}

static void encoder_cb_get_video_info(void *data, struct video_scale_info *info)
{
	UNUSED_PARAMETER(data);
	if (!info) return;
	info->format = VIDEO_FORMAT_BGRA;
}

static void *encoder_cb_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	UNUSED_PARAMETER(settings);
	encoder_private_data_t *data = bzalloc(sizeof(encoder_private_data_t));
	data->encoder = encoder;
	data->active = false;
	return data;
}

static void encoder_cb_destroy(void *p)
{
	encoder_private_data_t *data = (encoder_private_data_t *)p;
	if (data) bfree(data);
}

static bool encoder_cb_encode(void *p, uint32_t handle, int64_t pts, uint64_t lock_key, uint64_t *next_key, struct encoder_packet *packet, bool *received_packet)
{
	if (next_key) *next_key = lock_key;
	UNUSED_PARAMETER(packet);
	if (received_packet) *received_packet = false;

	encoder_private_data_t *data = (encoder_private_data_t *)p;
	if (!data || !data->active || !data->pipeline) return true;

	gst_master_pipeline_push_texture(data->pipeline, (void *)(uintptr_t)handle, (uint64_t)pts);
	return true;
}

static struct obs_encoder_info gstreamer_encoder_cb_info = 
{
	.id           = "gstreamer-encoder-callback",
	.type         = OBS_ENCODER_VIDEO,
	.codec        = "raw",
	.caps         = OBS_ENCODER_CAP_PASS_TEXTURE,
	.get_name     = encoder_cb_get_name,
	.create       = encoder_cb_create,
	.destroy      = encoder_cb_destroy,
	.encode_texture = encoder_cb_encode,
	.get_video_info = encoder_cb_get_video_info,
};

static struct obs_encoder_info gstreamer_encoder_cb_legacy_info = 
{
	.id           = "hjm-gstreamer-encoder-callback",
	.type         = OBS_ENCODER_VIDEO,
	.codec        = "raw",
	.caps         = OBS_ENCODER_CAP_PASS_TEXTURE,
	.get_name     = encoder_cb_get_name,
	.create       = encoder_cb_create,
	.destroy      = encoder_cb_destroy,
	.encode_texture = encoder_cb_encode,
	.get_video_info = encoder_cb_get_video_info,
};

/*
 * -----------------------------------------------------------------------------
 * 2. Private Output Driver Implementation (Drives the Texture Encoder)
 * -----------------------------------------------------------------------------
 */

static const char *encoder_driver_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Texture Encoder Driver Output";
}

static void *encoder_driver_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	return output;
}

static void encoder_driver_destroy(void *data)
{
	UNUSED_PARAMETER(data);
}

static bool encoder_driver_start(void *data)
{
	obs_output_t *output = (obs_output_t *)data;
	return obs_output_begin_data_capture(output, 0);
}

static void encoder_driver_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	obs_output_t *output = (obs_output_t *)data;
	obs_output_end_data_capture(output);
}

static void encoder_driver_encoded_packet(void *data, struct encoder_packet *packet)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(packet);
}

static struct obs_output_info gstreamer_encoder_driver_output_info = 
{
	.id             = "gst_encoder_driver_output",
	.flags          = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED,
	.get_name       = encoder_driver_get_name,
	.create         = encoder_driver_create,
	.destroy        = encoder_driver_destroy,
	.start          = encoder_driver_start,
	.stop           = encoder_driver_stop,
	.encoded_packet = encoder_driver_encoded_packet,
};

void gstreamer_encoder_callback_register(void)
{
	obs_register_encoder(&gstreamer_encoder_cb_info);
	obs_register_encoder(&gstreamer_encoder_cb_legacy_info);
	obs_register_output(&gstreamer_encoder_driver_output_info);
	blog(LOG_INFO, "[obs-gstreamer-encoder-callback] Registered master GPU texture encoder & driver");
}

void gstreamer_encoder_callback_unregister(void)
{
}

/*
 * -----------------------------------------------------------------------------
 * 3. Master Pipeline Encoder Callback Wrapper
 * -----------------------------------------------------------------------------
 */

struct gst_encoder_callback
{
	obs_output_t *output;
	obs_encoder_t *encoder;
	gst_master_pipeline_t *pipeline;
	gst_source_type_t source_type;
	obs_source_t *source;
	obs_view_t *view;
	video_t *custom_video;
	bool active;
};

static void pipeline_state_change(bool active, void *user_data)
{
	gst_encoder_callback_t *cb = (gst_encoder_callback_t *)user_data;
	if (!cb || !cb->output || !cb->encoder) return;

	encoder_private_data_t *priv = (encoder_private_data_t *)obs_encoder_get_type_data(cb->encoder);
	if (priv) priv->active = active;
	cb->active = active;

	if (active && !obs_output_active(cb->output))
	{
		obs_output_start(cb->output);
	}
	else if (!active && obs_output_active(cb->output))
	{
		obs_output_stop(cb->output);
	}
}

gst_encoder_callback_t *gst_encoder_callback_create(gst_source_type_t type, const char *scene_name, const char *item_name, const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec, uint32_t fps_num, uint32_t fps_den)
{
	gst_encoder_callback_t *cb = bzalloc(sizeof(gst_encoder_callback_t));
	cb->source_type = type;

	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
	{
		ovi.base_width = 1920;
		ovi.base_height = 1080;
	}
	uint32_t width = ovi.base_width ? ovi.base_width : 1920;
	uint32_t height = ovi.base_height ? ovi.base_height : 1080;

	if (type == GST_SOURCE_MAIN_OUTPUT)
	{
		cb->source = obs_get_output_source(0);
	}
	else
	{
		cb->source = gst_resolve_target_source(type, scene_name, item_name);
		if (!cb->source)
		{
			blog(LOG_ERROR, "[obs-gstreamer-encoder-callback] Failed to resolve target OBS source");
			bfree(cb);
			return NULL;
		}

		if (type == GST_SOURCE_SOURCE && cb->source)
		{
			uint32_t sw = obs_source_get_width(cb->source);
			uint32_t sh = obs_source_get_height(cb->source);
			if (sw > 0 && sh > 0)
			{
				width = sw;
				height = sh;
			}
		}
	}

	cb->pipeline = gst_master_pipeline_create(custom_master_pipe, encoder_type, bitrate_kbps, keyint_sec, width, height, fps_num, fps_den, pipeline_state_change, cb);
	if (!cb->pipeline)
	{
		if (cb->source) obs_source_release(cb->source);
		bfree(cb);
		return NULL;
	}

	cb->encoder = obs_video_encoder_create("gstreamer-encoder-callback", "master_enc_cb", NULL, NULL);
	if (!cb->encoder)
	{
		blog(LOG_ERROR, "[obs-gstreamer-encoder-callback] Failed to create video encoder");
		gst_master_pipeline_destroy(cb->pipeline);
		if (cb->source) obs_source_release(cb->source);
		bfree(cb);
		return NULL;
	}

	encoder_private_data_t *priv = (encoder_private_data_t *)obs_encoder_get_type_data(cb->encoder);
	if (priv)
	{
		priv->pipeline = cb->pipeline;
		priv->active = false;
	}

	cb->output = obs_output_create("gst_encoder_driver_output", "master_enc_driver", NULL, NULL);
	if (!cb->output)
	{
		blog(LOG_ERROR, "[obs-gstreamer-encoder-callback] Failed to create driver output");
		obs_encoder_release(cb->encoder);
		gst_master_pipeline_destroy(cb->pipeline);
		if (cb->source) obs_source_release(cb->source);
		bfree(cb);
		return NULL;
	}

	obs_output_set_video_encoder(cb->output, cb->encoder);

	if (type == GST_SOURCE_MAIN_OUTPUT)
	{
		obs_output_set_media(cb->output, obs_get_video(), obs_get_audio());
	}
	else if (cb->source)
	{
		cb->view = obs_view_create();
		obs_view_set_source(cb->view, 0, cb->source);
		cb->custom_video = obs_view_add(cb->view);
		obs_output_set_media(cb->output, cb->custom_video, obs_get_audio());
	}

	return cb;
}

gst_master_pipeline_t *gst_encoder_callback_get_pipeline(gst_encoder_callback_t *cb)
{
	return cb ? cb->pipeline : NULL;
}

void gst_encoder_callback_destroy(gst_encoder_callback_t *cb)
{
	if (!cb) return;

	if (cb->encoder)
	{
		encoder_private_data_t *priv = (encoder_private_data_t *)obs_encoder_get_type_data(cb->encoder);
		if (priv)
		{
			priv->active = false;
			priv->pipeline = NULL;
		}
	}

	if (cb->output) if (obs_output_active(cb->output)) obs_output_stop(cb->output);
	

	if (cb->pipeline)
	{
		gst_master_pipeline_destroy(cb->pipeline);
		cb->pipeline = NULL;
	}

	if (cb->output)
	{
		obs_output_release(cb->output);
		cb->output = NULL;
	}

	if (cb->encoder)
	{
		obs_encoder_release(cb->encoder);
		cb->encoder = NULL;
	}

	if (cb->view)
	{
		if (cb->custom_video)
		{
			obs_view_remove(cb->view);
			cb->custom_video = NULL;
		}
		obs_view_destroy(cb->view);
		cb->view = NULL;
	}

	if (cb->source)
	{
		obs_source_release(cb->source);
		cb->source = NULL;
	}

	bfree(cb);
}
