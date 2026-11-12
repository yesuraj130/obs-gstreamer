/*
 * obs-gstreamer: Dedicated OBS Output Callback Source for Master Pipeline
 */

#include "gstreamer-output-callback.h"
#include <obs-module.h>

struct gst_output_callback
{
	obs_output_t *output;
	gst_master_pipeline_t *pipeline;
	gst_source_type_t source_type;
	obs_source_t *source;
	obs_view_t *view;
	video_t *custom_video;
	volatile gint running;
};

static const char *output_cb_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Master Pipeline Output Callback";
}

static void *output_cb_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	gst_output_callback_t *data = bzalloc(sizeof(gst_output_callback_t));
	data->output = output;
	return data;
}

static void output_cb_destroy(void *p)
{
	gst_output_callback_t *data = (gst_output_callback_t *)p;
	if (!data) return;

	if (data->view)
	{
#if defined(LIBOBS_API_MAJOR_VER) && (LIBOBS_API_MAJOR_VER >= 28)
		if (data->custom_video)
		{
			obs_view_remove(data->view);
			data->custom_video = NULL;
		}
#endif
		obs_view_destroy(data->view);
		data->view = NULL;
	}
	if (data->source)
	{
		obs_source_release(data->source);
		data->source = NULL;
	}
	bfree(data);
}

static bool output_cb_start(void *p)
{
	gst_output_callback_t *data = (gst_output_callback_t *)p;
	if (!data) return false;

	if (!obs_output_begin_data_capture(data->output, 0)) return false;
	g_atomic_int_set(&data->running, 1);
	blog(LOG_INFO, "[obs-gstreamer-output-callback] Data capture started");
	return true;
}

static void output_cb_stop(void *p, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	gst_output_callback_t *data = (gst_output_callback_t *)p;
	if (!data) return;

	g_atomic_int_set(&data->running, 0);
	obs_output_end_data_capture(data->output);
	blog(LOG_INFO, "[obs-gstreamer-output-callback] Data capture stopped");
}

static void output_cb_raw_video(void *p, struct video_data *frame)
{
	gst_output_callback_t *data = (gst_output_callback_t *)p;
	if (!data || !g_atomic_int_get(&data->running) || !data->pipeline || !frame) return;

	gst_master_pipeline_push_raw_video(data->pipeline, frame);
}

static struct obs_output_info gstreamer_output_cb_info =
{
	.id = "gstreamer-output-callback",
	.flags = OBS_OUTPUT_VIDEO,
	.get_name = output_cb_get_name,
	.create = output_cb_create,
	.destroy = output_cb_destroy,
	.start = output_cb_start,
	.stop = output_cb_stop,
	.raw_video = output_cb_raw_video,
};

void gstreamer_output_callback_register(void)
{
	obs_register_output(&gstreamer_output_cb_info);
	blog(LOG_INFO, "[obs-gstreamer-output-callback] Registered master output callback module");
}

void gstreamer_output_callback_unregister(void)
{
}

static void pipeline_state_change(bool active, void *user_data)
{
	gst_output_callback_t *cb = (gst_output_callback_t *)user_data;
	if (!cb || !cb->output) return;

	if (active && !obs_output_active(cb->output)) obs_output_start(cb->output);
	else if (!active && obs_output_active(cb->output)) obs_output_stop(cb->output);
}

gst_output_callback_t *gst_output_callback_create(gst_source_type_t type, const char *scene_name, const char *item_name, const char *custom_master_pipe, const char *encoder_type, const char *video_codec, int bitrate_kbps, int keyint_sec, uint32_t fps_num, uint32_t fps_den)
{
	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
	{
		blog(LOG_ERROR, "[obs-gstreamer-output-callback] Failed to query OBS video info");
		return NULL;
	}

	obs_output_t *obs_out = obs_output_create("gstreamer-output-callback", "master_output_cb", NULL, NULL);
	if (!obs_out) return NULL;

	gst_output_callback_t *cb = (gst_output_callback_t *)obs_obj_get_data(obs_out);
	if (!cb)
	{
		blog(LOG_ERROR, "[obs-gstreamer-output-callback] Failed to resolve output instance data");
		obs_output_release(obs_out);
		return NULL;
	}
	cb->source_type = type;

	video_t *target_video = NULL;
	if (type == GST_SOURCE_MAIN_OUTPUT)
	{
		target_video = obs_get_video();
		if (!target_video)
		{
			blog(LOG_ERROR, "[obs-gstreamer-output-callback] Main OBS video output is unavailable");
			obs_output_release(obs_out);
			return NULL;
		}
		obs_output_set_media(obs_out, target_video, obs_get_audio());
	}
	else
	{
		cb->source = gst_resolve_target_source(type, scene_name, item_name);
		if (!cb->source)
		{
			blog(LOG_ERROR, "[obs-gstreamer-output-callback] Failed to resolve target OBS source");
			obs_output_release(obs_out);
			return NULL;
		}

		cb->view = obs_view_create();
		obs_view_set_source(cb->view, 0, cb->source);

		if (type == GST_SOURCE_SOURCE)
		{
			uint32_t sw = obs_source_get_width(cb->source);
			uint32_t sh = obs_source_get_height(cb->source);
			if (sw > 0 && sh > 0 && (sw != ovi.output_width || sh != ovi.output_height))
			{
				struct obs_video_info custom_ovi = ovi;
				custom_ovi.base_width = sw;
				custom_ovi.base_height = sh;
				custom_ovi.output_width = sw;
				custom_ovi.output_height = sh;
				custom_ovi.output_format = VIDEO_FORMAT_NV12;
				cb->custom_video = obs_view_add2(cb->view, &custom_ovi);
			}
			else
			{
				cb->custom_video = obs_view_add(cb->view);
			}
		}
		else
		{
			cb->custom_video = obs_view_add(cb->view);
		}

		if (!cb->custom_video)
		{
			blog(LOG_ERROR, "[obs-gstreamer-output-callback] Failed to create view video output");
			obs_output_release(obs_out);
			return NULL;
		}
		target_video = cb->custom_video;
		obs_output_set_media(obs_out, target_video, obs_get_audio());
	}

	enum video_format obs_fmt = video_output_get_format(target_video);
	if (obs_fmt != VIDEO_FORMAT_NV12)
	{
		const char *fmt_name = obs_to_gst_video_format_str(obs_fmt);
		blog(LOG_ERROR, "[obs-gstreamer-output-callback] Expected NV12 video_t format, got %d (%s); refusing conversion fallback",
			(int)obs_fmt, fmt_name ? fmt_name : "unknown");
		obs_output_release(obs_out);
		return NULL;
	}

	uint32_t width = video_output_get_width(target_video);
	uint32_t height = video_output_get_height(target_video);
	if (width == 0 || height == 0)
	{
		blog(LOG_ERROR, "[obs-gstreamer-output-callback] Invalid video_t dimensions: %ux%u", width, height);
		obs_output_release(obs_out);
		return NULL;
	}

	cb->pipeline = gst_master_pipeline_create_with_format(custom_master_pipe, encoder_type, video_codec, bitrate_kbps, keyint_sec, width, height, fps_num, fps_den, false, GST_VIDEO_FORMAT_NV12, pipeline_state_change, cb);
	if (!cb->pipeline)
	{
		obs_output_release(obs_out);
		return NULL;
	}

	return cb;
}

gst_master_pipeline_t *gst_output_callback_get_pipeline(gst_output_callback_t *cb)
{
	return cb ? cb->pipeline : NULL;
}

void gst_output_callback_destroy(gst_output_callback_t *cb)
{
	if (!cb) return;

	g_atomic_int_set(&cb->running, 0);
	gst_master_pipeline_t *pipeline = cb->pipeline;
	cb->pipeline = NULL;
	obs_output_t *out = cb->output;

	if (out && obs_output_active(out)) obs_output_stop(out);
	if (pipeline)
	{
		gst_master_pipeline_destroy(pipeline);
	}
	if (out) obs_output_release(out);
}
