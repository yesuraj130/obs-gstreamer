/*
 * obs-gstreamer: Private Filter Callback Source for Master Pipeline (Invisible in UI)
 */

#include "gstreamer-filter-callback.h"
#include <obs-module.h>

struct gst_filter_callback
{
	obs_source_t *filter;
	obs_source_t *target;
	gst_master_pipeline_t *pipeline;
	bool active;
};

typedef struct
{
	obs_source_t *source;
	gst_master_pipeline_t *pipeline;
	bool active;
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
	return data;
}

static void filter_cb_destroy(void *p)
{
	filter_private_data_t *data = (filter_private_data_t *)p;
	if (data) bfree(data);
}

static void filter_cb_video_render(void *p, gs_effect_t *effect)
{
	filter_private_data_t *data = (filter_private_data_t *)p;
	if (!data || !data->active || !data->pipeline)
	{
		obs_source_skip_video_filter(data ? data->source : NULL);
		return;
	}

	if (obs_source_process_filter_begin(data->source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
	{
		obs_source_process_filter_end(data->source, effect, 0, 0);

		gs_texture_t *tex = gs_filter_get_target_texture(data->source);
		if (tex)
		{
			void *handle = gs_texture_get_obj(tex);
			if (handle) gst_master_pipeline_push_texture(data->pipeline, handle, os_gettime_ns());
		}
	}
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
	cb->active = active;
}

gst_filter_callback_t *gst_filter_callback_create(gst_master_pipeline_t *pipe, gst_source_type_t type, const char *scene_name, const char *item_name)
{
	if (!pipe) return NULL;

	obs_source_t *target = gst_resolve_target_source(type, scene_name, item_name);
	if (!target)
	{
		blog(LOG_ERROR, "[obs-gstreamer-filter-callback] Failed to resolve target OBS source");
		return NULL;
	}

	obs_source_t *filter = obs_source_create_private("gst_private_filter_callback", "Master Pipeline Filter Tap", NULL);
	if (!filter)
	{
		obs_source_release(target);
		return NULL;
	}

	filter_private_data_t *priv = (filter_private_data_t *)obs_obj_get_data(filter);
	if (priv)
	{
		priv->pipeline = pipe;
		priv->active = (gst_master_pipeline_get_active_destinations(pipe) > 0);
	}

	obs_source_filter_add(target, filter);

	gst_filter_callback_t *cb = bzalloc(sizeof(gst_filter_callback_t));
	cb->filter = filter;
	cb->target = target;
	cb->pipeline = pipe;
	cb->active = (gst_master_pipeline_get_active_destinations(pipe) > 0);

	if (pipe)
	{
		pipe->state_cb = pipeline_state_change;
		pipe->state_user_data = cb;
	}
	return cb;
}

void gst_filter_callback_destroy(gst_filter_callback_t *cb)
{
	if (!cb) return;

	if (cb->pipeline)
	{
		cb->pipeline->state_cb = NULL;
		cb->pipeline->state_user_data = NULL;
	}

	if (cb->target && cb->filter)
	{
		obs_source_filter_remove(cb->target, cb->filter);
	}

	if (cb->filter) obs_source_release(cb->filter);
	if (cb->target) obs_source_release(cb->target);
	bfree(cb);
}
