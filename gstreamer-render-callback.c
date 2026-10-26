/*
 * obs-gstreamer: Direct GPU Render Callback Source for Master Pipeline
 */

#include "gstreamer-render-callback.h"
#include <obs-module.h>

struct gst_render_callback
{
	gst_master_pipeline_t *pipeline;
	gst_source_type_t source_type;
	obs_source_t *source;
	obs_sceneitem_t *scene_item;
	obs_view_t *view;

	uint32_t width;
	uint32_t height;

	gs_texrender_t *texrender[2];
	uint32_t pingpong_idx;

	bool hook_active;
	bool running;
};

static void render_callback_handler(void *param, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	gst_render_callback_t *cb = (gst_render_callback_t *)param;
	if (!cb || !cb->running || !cb->pipeline) return;

	if (gst_master_pipeline_get_active_destinations(cb->pipeline) <= 0) return;

	uint32_t cur = cb->pingpong_idx;
	if (!cb->texrender[cur]) return;

	gs_texrender_begin(cb->texrender[cur], cb->width, cb->height);
	gs_enable_blending(false);

	if (cb->source_type == GST_SOURCE_SCENE_ITEM && cb->scene_item)
	{
		obs_sceneitem_render(cb->scene_item);
	}
	else if (cb->view)
	{
		obs_view_render(cb->view);
	}

	gs_texrender_end(cb->texrender[cur]);

	gs_texture_t *tex = gs_texrender_get_texture(cb->texrender[cur]);
	if (tex)
	{
		void *handle = gs_texture_get_obj(tex);
		if (handle) gst_master_pipeline_push_texture(cb->pipeline, handle, os_gettime_ns());
	}

	cb->pingpong_idx = 1 - cur;
}

static void pipeline_state_change(bool active, void *user_data)
{
	gst_render_callback_t *cb = (gst_render_callback_t *)user_data;
	if (!cb) return;

	if (active && !cb->hook_active)
	{
		obs_add_main_render_callback(render_callback_handler, cb);
		cb->hook_active = true;
		cb->running = true;
		blog(LOG_INFO, "[obs-gstreamer-render-callback] Pipeline active: attached render callback");
	}
	else if (!active && cb->hook_active)
	{
		obs_remove_main_render_callback(render_callback_handler, cb);
		cb->hook_active = false;
		cb->running = false;
		blog(LOG_INFO, "[obs-gstreamer-render-callback] Pipeline idle: detached render callback (0%% GPU)");
	}
}

gst_render_callback_t *gst_render_callback_create(gst_master_pipeline_t *pipe, gst_source_type_t type, const char *scene_name, const char *item_name, uint32_t width, uint32_t height)
{
	if (!pipe) return NULL;

	gst_render_callback_t *cb = bzalloc(sizeof(gst_render_callback_t));
	cb->pipeline = pipe;
	cb->source_type = type;
	cb->width = width ? width : pipe->width;
	cb->height = height ? height : pipe->height;

	if (type == GST_SOURCE_SCENE_ITEM && scene_name && item_name)
	{
		obs_scene_t *sc = obs_get_scene_by_name(scene_name);
		if (sc)
		{
			cb->scene_item = obs_scene_find_source(sc, item_name);
			cb->source = cb->scene_item ? obs_sceneitem_get_source(cb->scene_item) : NULL;
			if (cb->source) obs_source_get_ref(cb->source);
			obs_scene_release(sc);
		}
	}
	else
	{
		cb->source = gst_resolve_target_source(type, scene_name, item_name);
	}

	if (!cb->source)
	{
		blog(LOG_ERROR, "[obs-gstreamer-render-callback] Failed to resolve target OBS source");
		bfree(cb);
		return NULL;
	}

	cb->view = obs_view_create();
	obs_view_set_source(cb->view, 0, cb->source);

	obs_enter_graphics();
	cb->texrender[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	cb->texrender[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	cb->pingpong_idx = 0;

	gst_master_pipeline_set_state_listener(pipe, pipeline_state_change, cb);

	return cb;
}

void gst_render_callback_destroy(gst_render_callback_t *cb)
{
	if (!cb) return;

	if (cb->pipeline)
	{
		gst_master_pipeline_set_state_listener(cb->pipeline, NULL, NULL);
	}

	if (cb->hook_active)
	{
		obs_remove_main_render_callback(render_callback_handler, cb);
		cb->hook_active = false;
	}

	obs_enter_graphics();
	for (int i = 0; i < 2; i++)
	{
		if (cb->texrender[i])
		{
			gs_texrender_destroy(cb->texrender[i]);
			cb->texrender[i] = NULL;
		}
	}
	obs_leave_graphics();

	if (cb->view)
	{
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
