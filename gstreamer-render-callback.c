/*
 * obs-gstreamer: Direct GPU Render Callback Source for Master Pipeline
 */

#include "gstreamer-render-callback.h"
#include <obs-module.h>
#include <util/platform.h>

#if defined(__GNUC__) && !defined(_WIN32)
extern void obs_add_main_rendered_callback(void (*draw)(void *param), void *param) __attribute__((weak));
extern void obs_remove_main_rendered_callback(void (*draw)(void *param), void *param) __attribute__((weak));
#else
EXPORT void obs_add_main_rendered_callback(void (*draw)(void *param), void *param);
EXPORT void obs_remove_main_rendered_callback(void (*draw)(void *param), void *param);
#endif

struct gst_render_callback
{
	gst_master_pipeline_t *pipeline;
	gst_source_type_t source_type;
	obs_source_t *source;
	obs_view_t *view;

	uint32_t width;
	uint32_t height;

	gs_texrender_t *texrender;

	bool hook_active;
	bool main_texture_null_logged;
	bool main_texture_logged;
};

static void render_callback_handler(void *param, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	gst_render_callback_t *cb = (gst_render_callback_t *)param;
	if (!cb || !cb->hook_active || !cb->pipeline) return;

	if (cb->source_type == GST_SOURCE_MAIN_OUTPUT)
	{
		gs_texture_t *tex = obs_get_main_texture();
		if (tex)
		{
			if (!cb->main_texture_logged)
			{
				blog(LOG_INFO, "[obs-gstreamer-render-callback] Main output texture available: %ux%u, format=%d",
					gs_texture_get_width(tex), gs_texture_get_height(tex),
					(int)gs_texture_get_color_format(tex));
				cb->main_texture_logged = true;
			}
			gst_master_pipeline_push_texture(cb->pipeline, tex, os_gettime_ns());
		} else if (!cb->main_texture_null_logged) {
			blog(LOG_WARNING, "[obs-gstreamer-render-callback] Main output texture is not available");
			cb->main_texture_null_logged = true;
		}
		return;
	}

	if (!cb->texrender) return;

	gs_texrender_reset(cb->texrender);
	if (!gs_texrender_begin(cb->texrender, cb->width, cb->height))
		return;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, (float)cb->width, 0.0f, (float)cb->height, -100.0f, 100.0f);
	gs_enable_blending(false);

	if (cb->source_type == GST_SOURCE_SOURCE && cb->source)
	{
		obs_source_video_render(cb->source);
	}
	else if (cb->view)
	{
		obs_view_render(cb->view);
	}

	gs_texrender_end(cb->texrender);

	gs_texture_t *tex = gs_texrender_get_texture(cb->texrender);
	if (tex)
	{
		gst_master_pipeline_push_texture(cb->pipeline, tex, os_gettime_ns());
	}
}

static void render_callback_main_rendered_handler(void *param)
{
	render_callback_handler(param, 0, 0);
}

static void pipeline_state_change(bool active, void *user_data)
{
	gst_render_callback_t *cb = (gst_render_callback_t *)user_data;
	if (!cb) return;

	if (active && !cb->hook_active)
	{
		if (cb->source_type == GST_SOURCE_MAIN_OUTPUT && obs_add_main_rendered_callback)
			obs_add_main_rendered_callback(render_callback_main_rendered_handler, cb);
		else
			obs_add_main_render_callback(render_callback_handler, cb);
		cb->hook_active = true;
		blog(LOG_INFO, "[obs-gstreamer-render-callback] Pipeline active: attached render callback");
	}
	else if (!active && cb->hook_active)
	{
		if (cb->source_type == GST_SOURCE_MAIN_OUTPUT && obs_remove_main_rendered_callback)
			obs_remove_main_rendered_callback(render_callback_main_rendered_handler, cb);
		else
			obs_remove_main_render_callback(render_callback_handler, cb);
		cb->hook_active = false;
		blog(LOG_INFO, "[obs-gstreamer-render-callback] Pipeline idle: detached render callback (0%% GPU)");
	}
}

gst_render_callback_t *gst_render_callback_create(gst_source_type_t type, const char *scene_name, const char *item_name, const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec, uint32_t fps_num, uint32_t fps_den)
{
	gst_render_callback_t *cb = bzalloc(sizeof(gst_render_callback_t));
	cb->source_type = type;

	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
	{
		ovi.base_width = 1920;
		ovi.base_height = 1080;
	}
	cb->width = ovi.base_width ? ovi.base_width : 1920;
	cb->height = ovi.base_height ? ovi.base_height : 1080;

	if (type != GST_SOURCE_MAIN_OUTPUT)
	{
		cb->source = gst_resolve_target_source(type, scene_name, item_name);
		if (!cb->source)
		{
			blog(LOG_ERROR, "[obs-gstreamer-render-callback] Failed to resolve target OBS source");
			bfree(cb);
			return NULL;
		}

		if (type == GST_SOURCE_SOURCE)
		{
			uint32_t sw = obs_source_get_width(cb->source);
			uint32_t sh = obs_source_get_height(cb->source);
			if (sw > 0 && sh > 0)
			{
				cb->width = sw;
				cb->height = sh;
			}
		}
	}

	cb->pipeline = gst_master_pipeline_create(custom_master_pipe, encoder_type, bitrate_kbps, keyint_sec, cb->width, cb->height, fps_num, fps_den, true, pipeline_state_change, cb);
	if (!cb->pipeline)
	{
		if (cb->source) obs_source_release(cb->source);
		bfree(cb);
		return NULL;
	}

	if (cb->source && type == GST_SOURCE_SCENE)
	{
		cb->view = obs_view_create();
		obs_view_set_source(cb->view, 0, cb->source);
	}

	if (type != GST_SOURCE_MAIN_OUTPUT)
	{
		obs_enter_graphics();
		cb->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		obs_leave_graphics();
	}

	return cb;
}

gst_master_pipeline_t *gst_render_callback_get_pipeline(gst_render_callback_t *cb)
{
	return cb ? cb->pipeline : NULL;
}

void gst_render_callback_destroy(gst_render_callback_t *cb)
{
	if (!cb) return;

	if (cb->hook_active)
	{
		if (cb->source_type == GST_SOURCE_MAIN_OUTPUT && obs_remove_main_rendered_callback)
			obs_remove_main_rendered_callback(render_callback_main_rendered_handler, cb);
		else
			obs_remove_main_render_callback(render_callback_handler, cb);
		cb->hook_active = false;
	}

	if (cb->pipeline)
	{
		gst_master_pipeline_destroy(cb->pipeline);
		cb->pipeline = NULL;
	}

	if (cb->texrender)
	{
		obs_enter_graphics();
		gs_texrender_destroy(cb->texrender);
		cb->texrender = NULL;
		obs_leave_graphics();
	}

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
