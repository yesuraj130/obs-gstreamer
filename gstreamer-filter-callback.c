/*
 * obs-gstreamer: Private Filter Callback Source for Master Pipeline (Invisible in UI)
 */

#include "gstreamer-filter-callback.h"
#include <obs-module.h>
#include <util/platform.h>

struct gst_filter_callback
{
	obs_source_t *filter;
	obs_source_t *target;
	gst_master_pipeline_t *pipeline;
};

typedef struct
{
	obs_source_t *source;
	gst_master_pipeline_t *pipeline;
	bool active;
	gs_texrender_t *texrender;
} filter_private_data_t;

static const char *filter_cb_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "Master Pipeline Filter Tap";
}

static void *filter_cb_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	filter_private_data_t *data = bzalloc(sizeof(filter_private_data_t));
	data->source = source;
	data->active = false;
	data->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	return data;
}

static void filter_cb_destroy(void *p)
{
	filter_private_data_t *data = (filter_private_data_t *)p;
	if (data) {
		if (data->texrender) gs_texrender_destroy(data->texrender);
		bfree(data);
	}
}

static void filter_cb_video_render(void *p, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	filter_private_data_t *data = (filter_private_data_t *)p;
	if (!data || !data->active || !data->pipeline)
	{
		obs_source_skip_video_filter(data ? data->source : NULL);
		return;
	}

	obs_source_t *target = obs_filter_get_target(data->source);
	uint32_t width = target ? obs_source_get_base_width(target) : 0;
	uint32_t height = target ? obs_source_get_base_height(target) : 0;

	if (width > 0 && height > 0 && data->texrender)
	{
		gs_texrender_begin(data->texrender, width, height);
		struct vec4 clear_color;
		vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, 0.0f);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		obs_source_video_render(target);
		gs_texrender_end(data->texrender);

		gs_texture_t *tex = gs_texrender_get_texture(data->texrender);
		if (tex)
		{
			void *handle = gs_texture_get_obj(tex);
			if (handle) gst_master_pipeline_push_texture(data->pipeline, handle, os_gettime_ns());
		}
	}

	obs_source_skip_video_filter(data->source);
}

static struct obs_source_info gst_filter_callback_info = {
	.id           = "gst_private_filter_callback",
	.type         = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name     = filter_cb_get_name,
	.create       = filter_cb_create,
	.destroy      = filter_cb_destroy,
	.video_render = filter_cb_video_render,
};

void gstreamer_filter_callback_register(void)
{
	obs_register_source(&gst_filter_callback_info);
	blog(LOG_INFO, "[obs-gstreamer-filter-callback] Registered private filter callback");
}

void gstreamer_filter_callback_unregister(void)
{
}

static void pipeline_state_change(bool active, void *user_data)
{
	gst_filter_callback_t *cb = (gst_filter_callback_t *)user_data;
	if (!cb || !cb->filter) return;

	filter_private_data_t *priv = (filter_private_data_t *)obs_obj_get_data(cb->filter);
	if (priv) priv->active = active;
}

gst_filter_callback_t *gst_filter_callback_create(gst_source_type_t type, const char *scene_name, const char *item_name, const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec, uint32_t fps_num, uint32_t fps_den)
{
	gst_filter_callback_t *cb = bzalloc(sizeof(gst_filter_callback_t));

	obs_source_t *target = gst_resolve_target_source(type, scene_name, item_name);
	if (!target)
	{
		blog(LOG_ERROR, "[obs-gstreamer-filter-callback] Failed to resolve target OBS source");
		bfree(cb);
		return NULL;
	}

	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		ovi.base_width = 1920;
		ovi.base_height = 1080;
	}
	uint32_t width = ovi.base_width ? ovi.base_width : 1920;
	uint32_t height = ovi.base_height ? ovi.base_height : 1080;

	if (type == GST_SOURCE_SOURCE)
	{
		uint32_t sw = obs_source_get_width(target);
		uint32_t sh = obs_source_get_height(target);
		if (sw > 0 && sh > 0)
		{
			width = sw;
			height = sh;
		}
	}

	cb->pipeline = gst_master_pipeline_create(custom_master_pipe, encoder_type, bitrate_kbps, keyint_sec,
	                                          width, height,
	                                          fps_num, fps_den,
	                                          pipeline_state_change, cb);
	if (!cb->pipeline)
	{
		obs_source_release(target);
		bfree(cb);
		return NULL;
	}

	obs_source_t *filter = obs_source_create_private("gst_private_filter_callback", "Master Pipeline Filter Tap", NULL);
	if (!filter)
	{
		obs_source_release(target);
		gst_master_pipeline_destroy(cb->pipeline);
		bfree(cb);
		return NULL;
	}

	filter_private_data_t *priv = (filter_private_data_t *)obs_obj_get_data(filter);
	if (priv)
	{
		priv->pipeline = cb->pipeline;
		priv->active = false;
	}

	obs_source_filter_add(target, filter);

	cb->filter = filter;
	cb->target = target;

	return cb;
}

gst_master_pipeline_t *gst_filter_callback_get_pipeline(gst_filter_callback_t *cb)
{
	return cb ? cb->pipeline : NULL;
}

void gst_filter_callback_destroy(gst_filter_callback_t *cb)
{
	if (!cb) return;

	if (cb->filter)
	{
		filter_private_data_t *priv = (filter_private_data_t *)obs_obj_get_data(cb->filter);
		if (priv)
		{
			priv->active = false;
			priv->pipeline = NULL;
		}
	}

	if (cb->target && cb->filter)
	{
		obs_source_filter_remove(cb->target, cb->filter);
	}

	if (cb->pipeline)
	{
		gst_master_pipeline_destroy(cb->pipeline);
		cb->pipeline = NULL;
	}

	if (cb->filter) obs_source_release(cb->filter);
	if (cb->target) obs_source_release(cb->target);
	bfree(cb);
}
