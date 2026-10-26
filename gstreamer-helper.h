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

// Map OBS video format to GStreamer GstVideoFormat 
static inline GstVideoFormat obs_to_gst_video_format(enum video_format format)
{
	switch (format)
	{
		//Planar 4:2:0
		case VIDEO_FORMAT_I420: return GST_VIDEO_FORMAT_I420;
		case VIDEO_FORMAT_NV12: return GST_VIDEO_FORMAT_NV12;

		//Packed 4:2:2
		case VIDEO_FORMAT_YVYU: return GST_VIDEO_FORMAT_YVYU;
		case VIDEO_FORMAT_YUY2: return GST_VIDEO_FORMAT_YUY2;
		case VIDEO_FORMAT_UYVY: return GST_VIDEO_FORMAT_UYVY;

		//Planar 4:2:2
		case VIDEO_FORMAT_I422: return GST_VIDEO_FORMAT_Y42B;

		//Planar 4:4:4
		case VIDEO_FORMAT_I444: return GST_VIDEO_FORMAT_Y444;

		//RGB Packed 4:4:4
		case VIDEO_FORMAT_RGBA: return GST_VIDEO_FORMAT_RGBA;
		case VIDEO_FORMAT_BGRA: return GST_VIDEO_FORMAT_BGRA;
		case VIDEO_FORMAT_BGRX: return GST_VIDEO_FORMAT_BGRx;
		case VIDEO_FORMAT_BGR3: return GST_VIDEO_FORMAT_BGR;

		//Grayscale
		case VIDEO_FORMAT_Y800: return GST_VIDEO_FORMAT_GRAY8;

		//Formats with Alpha
		case VIDEO_FORMAT_AYUV: return GST_VIDEO_FORMAT_AYUV;
		case VIDEO_FORMAT_I40A: return GST_VIDEO_FORMAT_A420;
		case VIDEO_FORMAT_I42A: return GST_VIDEO_FORMAT_A422;

		//0-bit and 12-bit HDR formats
		case VIDEO_FORMAT_I010: return GST_VIDEO_FORMAT_I420_10LE;
		case VIDEO_FORMAT_P010: return GST_VIDEO_FORMAT_P010_10LE;
		case VIDEO_FORMAT_I210: return GST_VIDEO_FORMAT_I422_10LE;
		case VIDEO_FORMAT_I412: return GST_VIDEO_FORMAT_Y444_12LE;
		case VIDEO_FORMAT_V210: return GST_VIDEO_FORMAT_v210;

		default: return GST_VIDEO_FORMAT_UNKNOWN;
	}
}

//Map OBS video format to GStreamer format string (e.g. "NV12", "I420")
static inline const char *obs_to_gst_video_format_str(enum video_format format)
{
	GstVideoFormat gst_fmt = obs_to_gst_video_format(format);
	if (gst_fmt == GST_VIDEO_FORMAT_UNKNOWN) return NULL;
	return gst_video_format_to_string(gst_fmt);
}

//Map GStreamer GstVideoFormat to OBS video format
static inline enum video_format gst_to_obs_video_format(GstVideoFormat format)
{
	switch (format)
	{
		//Planar 4:2:0
		case GST_VIDEO_FORMAT_I420: return VIDEO_FORMAT_I420;
		case GST_VIDEO_FORMAT_NV12: return VIDEO_FORMAT_NV12;

		//Packed 4:2:2 
		case GST_VIDEO_FORMAT_YVYU: return VIDEO_FORMAT_YVYU;
		case GST_VIDEO_FORMAT_YUY2: return VIDEO_FORMAT_YUY2;
		case GST_VIDEO_FORMAT_UYVY: return VIDEO_FORMAT_UYVY;

		//Planar 4:2:2
		case GST_VIDEO_FORMAT_Y42B: return VIDEO_FORMAT_I422;

		//Planar 4:4:4
		case GST_VIDEO_FORMAT_Y444: return VIDEO_FORMAT_I444;

		//RGB Packed 4:4:4
		case GST_VIDEO_FORMAT_BGRA: return VIDEO_FORMAT_BGRA;
		case GST_VIDEO_FORMAT_BGRx: return VIDEO_FORMAT_BGRX;
		case GST_VIDEO_FORMAT_RGBA: return VIDEO_FORMAT_RGBA;
		case GST_VIDEO_FORMAT_BGR: return VIDEO_FORMAT_BGR3;

		//Grayscale
		case GST_VIDEO_FORMAT_GRAY8: return VIDEO_FORMAT_Y800;

		//Formats with Alpha
		case GST_VIDEO_FORMAT_AYUV: return VIDEO_FORMAT_AYUV;
		case GST_VIDEO_FORMAT_A420: return VIDEO_FORMAT_I40A;
		case GST_VIDEO_FORMAT_A422: return VIDEO_FORMAT_I42A;

		//10-bit and 12-bit HDR formats
		case GST_VIDEO_FORMAT_I420_10LE: return VIDEO_FORMAT_I010;
		case GST_VIDEO_FORMAT_P010_10LE: return VIDEO_FORMAT_P010;
		case GST_VIDEO_FORMAT_I422_10LE: return VIDEO_FORMAT_I210;
		case GST_VIDEO_FORMAT_Y444_12LE: return VIDEO_FORMAT_I412;
		case GST_VIDEO_FORMAT_v210: return VIDEO_FORMAT_V210;

		default: return VIDEO_FORMAT_NONE;
	}
}

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
	GST_SOURCE_SCENE_ITEM,
} gst_source_type_t;

static inline obs_source_t *gst_resolve_target_source(gst_source_type_t type, const char *scene_name, const char *item_name)
{
	switch (type)
	{
		case GST_SOURCE_MAIN_OUTPUT: return obs_get_output_source(0);

		case GST_SOURCE_SCENE:
		{
			if (!scene_name || !*scene_name) return NULL;

			obs_scene_t *sc = obs_get_scene_by_name(scene_name);
			if (!sc) return NULL;

			obs_source_t *src = obs_scene_get_source(sc);
			if (src) obs_source_get_ref(src);

			obs_scene_release(sc);
			return src;
		}

		case GST_SOURCE_SCENE_ITEM:
		{
			if (!scene_name || !*scene_name || !item_name || !*item_name) return NULL;

			obs_scene_t *sc = obs_get_scene_by_name(scene_name);
			if (!sc) return NULL;

			obs_sceneitem_t *item = obs_scene_find_source(sc, item_name);
			obs_source_t *src = item ? obs_sceneitem_get_source(item) : NULL;
			if (src) obs_source_get_ref(src);
			obs_scene_release(sc);
			return src;
		}
	}
	return NULL;
}


