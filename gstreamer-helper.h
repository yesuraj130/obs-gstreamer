#pragma once

#include <obs-module.h>
#include <gst/gst.h>
#include <gst/video/video.h>

// Safe source name retrieval with fallback
static inline const char *obs_source_get_name_safe(obs_source_t *source, const char *fallback)
{
	const char *name = source ? obs_source_get_name(source) : NULL;
	return (name && *name) ? name : (fallback ? fallback : "");
}

GstVideoFormat obs_to_gst_video_format(enum video_format format);
const char *obs_to_gst_video_format_str(enum video_format format);
enum video_format gst_to_obs_video_format(GstVideoFormat format);

// Calculate plane height using GStreamer's native format metadata
static inline guint gst_get_plane_height(const GstVideoInfo *info, guint plane)
{
	if (!info || !info->finfo) return 0;
	if (plane == 0 || GST_VIDEO_INFO_N_PLANES(info) == 1) return GST_VIDEO_INFO_HEIGHT(info);

	for (guint c = 0; c < GST_VIDEO_INFO_N_COMPONENTS(info); c++)
	{
		if (GST_VIDEO_INFO_COMP_PLANE(info, c) == plane) return GST_VIDEO_INFO_COMP_HEIGHT(info, c);
	}

	return GST_VIDEO_INFO_HEIGHT(info);
}

typedef enum {
	GST_SOURCE_MAIN_OUTPUT = 0,
	GST_SOURCE_SCENE,
	GST_SOURCE_SOURCE,
} gst_source_type_t;

static inline obs_source_t *gst_resolve_target_source(gst_source_type_t type, const char *scene_name, const char *item_name)
{
	switch (type)
	{
		case GST_SOURCE_MAIN_OUTPUT: return obs_get_output_source(0);

		case GST_SOURCE_SCENE:
		{
			const char *name = (scene_name && *scene_name) ? scene_name : item_name;
			if (!name || !*name) return NULL;

			obs_scene_t *sc = obs_get_scene_by_name(name);
			if (!sc) return NULL;

			obs_source_t *src = obs_scene_get_source(sc);
			if (src) obs_source_get_ref(src);

			obs_scene_release(sc);
			return src;
		}

		case GST_SOURCE_SOURCE:
		{
			const char *name = (item_name && *item_name) ? item_name : scene_name;
			if (!name || !*name) return NULL;

			return obs_get_source_by_name(name);
		}
	}
	return NULL;
}

