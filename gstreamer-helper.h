#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
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
GstElement *gstreamer_parse_launch_checked(const gchar *description, GError **error);
bool gstreamer_copy_obs_video_data_to_buffer(const GstVideoInfo *info,
	uint8_t *const data[], const uint32_t linesize[], GstBuffer *buffer);

typedef void (*obs_main_rendered_callback_fn)(void (*draw)(void *param), void *param);
obs_main_rendered_callback_fn obs_resolve_add_main_rendered_callback(void);
obs_main_rendered_callback_fn obs_resolve_remove_main_rendered_callback(void);

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

static inline gsize gst_get_plane_row_bytes(const GstVideoInfo *info, guint plane)
{
	if (!info || !info->finfo) return 0;
	if (info->finfo->format == GST_VIDEO_FORMAT_v210 && plane == 0)
		return ((gsize)GST_VIDEO_INFO_WIDTH(info) + 5) / 6 * 16;

	gsize row_bytes = 0;
	for (guint comp = 0; comp < GST_VIDEO_INFO_N_COMPONENTS(info); comp++)
	{
		if (GST_VIDEO_INFO_COMP_PLANE(info, comp) == plane)
		{
			gsize comp_row_bytes =
				(gsize)GST_VIDEO_INFO_COMP_WIDTH(info, comp) *
				GST_VIDEO_FORMAT_INFO_PSTRIDE(info->finfo, comp);
			if (comp_row_bytes > row_bytes)
				row_bytes = comp_row_bytes;
		}
	}
	return row_bytes;
}

typedef struct gstreamer_cpu_frame_slot
{
	gatomicrefcount ref_count;
	gint in_use;
	guint8 *data;
	gsize size;
} gstreamer_cpu_frame_slot_t;

static inline gstreamer_cpu_frame_slot_t *gstreamer_cpu_frame_slot_ref(gstreamer_cpu_frame_slot_t *slot)
{
	if (slot) g_atomic_ref_count_inc(&slot->ref_count);
	return slot;
}

static inline void gstreamer_cpu_frame_slot_unref(gstreamer_cpu_frame_slot_t *slot)
{
	if (slot && g_atomic_ref_count_dec(&slot->ref_count))
	{
		g_free(slot->data);
		g_free(slot);
	}
}

static inline gstreamer_cpu_frame_slot_t *gstreamer_cpu_frame_slot_create(gsize size)
{
	if (size == 0) return NULL;
	gstreamer_cpu_frame_slot_t *slot = g_new0(gstreamer_cpu_frame_slot_t, 1);
	g_atomic_ref_count_init(&slot->ref_count);
	slot->size = size;
	slot->data = (guint8 *)g_try_malloc(size);
	if (!slot->data)
	{
		gstreamer_cpu_frame_slot_unref(slot);
		return NULL;
	}
	return slot;
}

static inline void gstreamer_cpu_frame_slot_release_notify(gpointer user_data)
{
	gstreamer_cpu_frame_slot_t *slot = (gstreamer_cpu_frame_slot_t *)user_data;
	g_atomic_int_set(&slot->in_use, 0);
	gstreamer_cpu_frame_slot_unref(slot);
}

typedef enum {
	GSTREAMER_FRAME_MODE_UNSET = 0,
	GSTREAMER_FRAME_MODE_WRAP,
	GSTREAMER_FRAME_MODE_PLANE_FAST_COPY,
	GSTREAMER_FRAME_MODE_ROW_COPY,
} gstreamer_frame_mode_t;

typedef struct gstreamer_video_frame_cache
{
	gstreamer_frame_mode_t mode;
	GstVideoFormat format;
	guint width;
	guint height;
	guint n_planes;
	gsize total_size;
	gsize offsets[GST_VIDEO_MAX_PLANES];
	gint dst_strides[GST_VIDEO_MAX_PLANES];
	uint32_t src_strides[GST_VIDEO_MAX_PLANES];
	guint rows[GST_VIDEO_MAX_PLANES];
	gsize row_bytes[GST_VIDEO_MAX_PLANES];
	gsize plane_bytes[GST_VIDEO_MAX_PLANES];
} gstreamer_video_frame_cache_t;

static inline const char *gstreamer_frame_mode_name(gstreamer_frame_mode_t mode)
{
	switch (mode)
	{
	case GSTREAMER_FRAME_MODE_WRAP: return "zero-copy-wrap";
	case GSTREAMER_FRAME_MODE_PLANE_FAST_COPY: return "plane-fast-copy";
	case GSTREAMER_FRAME_MODE_ROW_COPY: return "row-fallback-copy";
	default: return "unset";
	}
}

static inline bool gstreamer_init_video_frame_cache(const GstVideoInfo *info,
	gstreamer_video_frame_cache_t *cache, const struct video_data *frame, gsize slot_size)
{
	if (!info || !cache || !frame || !frame->data[0]) return false;

	guint n_planes = GST_VIDEO_INFO_N_PLANES(info);
	gsize total_size = GST_VIDEO_INFO_SIZE(info);
	if (total_size == 0 || n_planes == 0 || n_planes > GST_VIDEO_MAX_PLANES || n_planes > MAX_AV_PLANES)
		return false;

	cache->format = GST_VIDEO_INFO_FORMAT(info);
	cache->width = GST_VIDEO_INFO_WIDTH(info);
	cache->height = GST_VIDEO_INFO_HEIGHT(info);
	cache->n_planes = n_planes;
	cache->total_size = total_size;

	bool can_wrap = true;
	bool all_fast_copy = true;
	for (guint p = 0; p < n_planes; p++)
	{
		gsize offset = GST_VIDEO_INFO_PLANE_OFFSET(info, p);
		gint dst_stride = GST_VIDEO_INFO_PLANE_STRIDE(info, p);
		guint rows = gst_get_plane_height(info, p);
		gsize row_bytes = gst_get_plane_row_bytes(info, p);
		uint32_t src_stride = frame->linesize[p];

		if (!frame->data[p] || dst_stride <= 0 || src_stride < row_bytes || (gsize)dst_stride < row_bytes)
		{
			cache->mode = GSTREAMER_FRAME_MODE_UNSET;
			return false;
		}

		if (src_stride != (uint32_t)dst_stride || frame->data[p] != frame->data[0] + offset)
			can_wrap = false;

		if (src_stride != (uint32_t)dst_stride)
			all_fast_copy = false;

		cache->offsets[p] = offset;
		cache->dst_strides[p] = dst_stride;
		cache->src_strides[p] = src_stride;
		cache->rows[p] = rows;
		cache->row_bytes[p] = row_bytes;
		cache->plane_bytes[p] = (rows > 0)
			? ((gsize)(rows - 1) * (gsize)dst_stride + row_bytes)
			: 0;
	}

	if (can_wrap)
	{
		cache->mode = GSTREAMER_FRAME_MODE_WRAP;
		return true;
	}

	if (slot_size < total_size)
	{
		cache->mode = GSTREAMER_FRAME_MODE_UNSET;
		return false;
	}

	for (guint p = 0; p < n_planes; p++)
	{
		if (cache->rows[p] > 0 && (cache->offsets[p] + cache->plane_bytes[p]) > slot_size)
		{
			cache->mode = GSTREAMER_FRAME_MODE_UNSET;
			return false;
		}
	}

	cache->mode = all_fast_copy
		? GSTREAMER_FRAME_MODE_PLANE_FAST_COPY
		: GSTREAMER_FRAME_MODE_ROW_COPY;
	return true;
}

static inline GstBuffer *gstreamer_create_obs_video_buffer(const GstVideoInfo *info,
	gstreamer_video_frame_cache_t *cache, gstreamer_cpu_frame_slot_t *slot,
	const struct video_data *frame, bool *slot_busy)
{
	if (slot_busy) *slot_busy = false;
	if (!cache || !frame || !frame->data[0]) return NULL;

	if (cache->mode == GSTREAMER_FRAME_MODE_UNSET)
	{
		if (!gstreamer_init_video_frame_cache(info, cache, frame, slot ? slot->size : 0))
			return NULL;
	}

	if (cache->mode == GSTREAMER_FRAME_MODE_WRAP)
	{
		GstBuffer *buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,
			frame->data[0], cache->total_size, 0, cache->total_size, NULL, NULL);
		if (!buf) return NULL;

		if (!gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE,
			cache->format, cache->width, cache->height,
			cache->n_planes, cache->offsets, cache->dst_strides))
		{
			gst_buffer_unref(buf);
			return NULL;
		}
		return buf;
	}

	if (!slot || slot->size < cache->total_size) return NULL;
	if (!g_atomic_int_compare_and_exchange(&slot->in_use, 0, 1))
	{
		if (slot_busy) *slot_busy = true;
		return NULL;
	}

	if (cache->mode == GSTREAMER_FRAME_MODE_PLANE_FAST_COPY)
	{
		for (guint p = 0; p < cache->n_planes; p++)
		{
			if (cache->plane_bytes[p] > 0)
				memcpy(slot->data + cache->offsets[p], frame->data[p], cache->plane_bytes[p]);
		}
	}
	else
	{
		for (guint p = 0; p < cache->n_planes; p++)
		{
			uint8_t *dst_plane = slot->data + cache->offsets[p];
			const uint8_t *src_plane = frame->data[p];
			gsize dst_stride = (gsize)cache->dst_strides[p];
			gsize src_stride = (gsize)cache->src_strides[p];
			gsize row_bytes = cache->row_bytes[p];
			guint rows = cache->rows[p];
			for (guint y = 0; y < rows; y++)
				memcpy(dst_plane + (gsize)y * dst_stride,
					src_plane + (gsize)y * src_stride, row_bytes);
		}
	}

	GstBuffer *buf = gst_buffer_new();
	if (!buf)
	{
		g_atomic_int_set(&slot->in_use, 0);
		return NULL;
	}

	gstreamer_cpu_frame_slot_t *memory_slot = gstreamer_cpu_frame_slot_ref(slot);
	GstMemory *memory = gst_memory_new_wrapped(GST_MEMORY_FLAG_READONLY,
		slot->data, slot->size, 0, slot->size,
		memory_slot, gstreamer_cpu_frame_slot_release_notify);
	if (!memory)
	{
		g_atomic_int_set(&slot->in_use, 0);
		gstreamer_cpu_frame_slot_unref(memory_slot);
		gst_buffer_unref(buf);
		return NULL;
	}
	gst_buffer_append_memory(buf, memory);

	if (!gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE,
		cache->format, cache->width, cache->height,
		cache->n_planes, cache->offsets, cache->dst_strides))
	{
		gst_buffer_unref(buf);
		return NULL;
	}

	return buf;
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
