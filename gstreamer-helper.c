#include "gstreamer-helper.h"
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__GNUC__)
extern void obs_add_main_rendered_callback(void (*draw)(void *param), void *param) __attribute__((weak));
extern void obs_remove_main_rendered_callback(void (*draw)(void *param), void *param) __attribute__((weak));
#endif

GstElement *gstreamer_parse_launch_checked(const gchar *description, GError **error)
{
	GError *parse_error = NULL;
	GstElement *pipeline = gst_parse_launch(description, &parse_error);
	if (!pipeline && !parse_error)
		g_set_error_literal(&parse_error, GST_CORE_ERROR, GST_CORE_ERROR_FAILED,
			"Pipeline description produced no pipeline");

	if (parse_error) {
		if (pipeline)
			gst_object_unref(pipeline);
		if (error)
			*error = parse_error;
		else
			g_error_free(parse_error);
		return NULL;
	}

	return pipeline;
}

bool gstreamer_copy_obs_video_data_to_buffer(const GstVideoInfo *info,
	uint8_t *const data[], const uint32_t linesize[], GstBuffer *buffer)
{
	GstVideoFrame mapped;
	if (!info || !data || !linesize || !buffer ||
	    !gst_video_frame_map(&mapped, info, buffer, GST_MAP_WRITE))
		return false;

	bool copied = true;
	for (guint plane = 0; plane < GST_VIDEO_INFO_N_PLANES(info); plane++) {
		gsize row_bytes = 0;
		if (info->finfo->format == GST_VIDEO_FORMAT_v210 && plane == 0) {
			row_bytes = ((gsize)GST_VIDEO_INFO_WIDTH(info) + 5) / 6 * 16;
		} else {
			for (guint component = 0; component < GST_VIDEO_INFO_N_COMPONENTS(info); component++) {
				if (GST_VIDEO_INFO_COMP_PLANE(info, component) == plane) {
					gsize component_row_bytes =
						(gsize)GST_VIDEO_INFO_COMP_WIDTH(info, component) *
						GST_VIDEO_FORMAT_INFO_PSTRIDE(info->finfo, component);
					row_bytes = MAX(row_bytes, component_row_bytes);
				}
			}
		}

		guint rows = gst_get_plane_height(info, plane);
		gint dst_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, plane);
		if (!data[plane] || linesize[plane] < row_bytes ||
		    dst_stride < 0 || (gsize)dst_stride < row_bytes) {
			copied = false;
			break;
		}

		guint8 *dst = GST_VIDEO_FRAME_PLANE_DATA(&mapped, plane);
		for (guint row = 0; row < rows; row++) {
			memcpy(dst + (gsize)row * dst_stride,
				data[plane] + (gsize)row * linesize[plane], row_bytes);
		}
	}

	gst_video_frame_unmap(&mapped);
	return copied;
}

GstVideoFormat obs_to_gst_video_format(enum video_format format)
{
	switch (format)
	{
	case VIDEO_FORMAT_I420: return GST_VIDEO_FORMAT_I420;
	case VIDEO_FORMAT_NV12: return GST_VIDEO_FORMAT_NV12;
	case VIDEO_FORMAT_YVYU: return GST_VIDEO_FORMAT_YVYU;
	case VIDEO_FORMAT_YUY2: return GST_VIDEO_FORMAT_YUY2;
	case VIDEO_FORMAT_UYVY: return GST_VIDEO_FORMAT_UYVY;
	case VIDEO_FORMAT_I422: return GST_VIDEO_FORMAT_Y42B;
	case VIDEO_FORMAT_I444: return GST_VIDEO_FORMAT_Y444;
	case VIDEO_FORMAT_RGBA: return GST_VIDEO_FORMAT_RGBA;
	case VIDEO_FORMAT_BGRA: return GST_VIDEO_FORMAT_BGRA;
	case VIDEO_FORMAT_BGRX: return GST_VIDEO_FORMAT_BGRx;
	case VIDEO_FORMAT_BGR3: return GST_VIDEO_FORMAT_BGR;
	case VIDEO_FORMAT_Y800: return GST_VIDEO_FORMAT_GRAY8;
	case VIDEO_FORMAT_AYUV: return GST_VIDEO_FORMAT_AYUV;
	case VIDEO_FORMAT_I40A: return GST_VIDEO_FORMAT_A420;
	case VIDEO_FORMAT_I42A: return GST_VIDEO_FORMAT_A422;
	case VIDEO_FORMAT_I010: return GST_VIDEO_FORMAT_I420_10LE;
	case VIDEO_FORMAT_P010: return GST_VIDEO_FORMAT_P010_10LE;
	case VIDEO_FORMAT_I210: return GST_VIDEO_FORMAT_I422_10LE;
	case VIDEO_FORMAT_I412: return GST_VIDEO_FORMAT_Y444_12LE;
#if LIBOBS_API_MAJOR_VER >= 30
	case VIDEO_FORMAT_V210: return GST_VIDEO_FORMAT_v210;
#endif
	default: return GST_VIDEO_FORMAT_UNKNOWN;
	}
}

const char *obs_to_gst_video_format_str(enum video_format format)
{
	GstVideoFormat gst_format = obs_to_gst_video_format(format);
	return gst_format == GST_VIDEO_FORMAT_UNKNOWN
		? NULL
		: gst_video_format_to_string(gst_format);
}

enum video_format gst_to_obs_video_format(GstVideoFormat format)
{
	switch (format)
	{
	case GST_VIDEO_FORMAT_I420: return VIDEO_FORMAT_I420;
	case GST_VIDEO_FORMAT_NV12: return VIDEO_FORMAT_NV12;
	case GST_VIDEO_FORMAT_YVYU: return VIDEO_FORMAT_YVYU;
	case GST_VIDEO_FORMAT_YUY2: return VIDEO_FORMAT_YUY2;
	case GST_VIDEO_FORMAT_UYVY: return VIDEO_FORMAT_UYVY;
	case GST_VIDEO_FORMAT_Y42B: return VIDEO_FORMAT_I422;
	case GST_VIDEO_FORMAT_Y444: return VIDEO_FORMAT_I444;
	case GST_VIDEO_FORMAT_BGRA: return VIDEO_FORMAT_BGRA;
	case GST_VIDEO_FORMAT_BGRx: return VIDEO_FORMAT_BGRX;
	case GST_VIDEO_FORMAT_RGBA: return VIDEO_FORMAT_RGBA;
	case GST_VIDEO_FORMAT_BGR: return VIDEO_FORMAT_BGR3;
	case GST_VIDEO_FORMAT_GRAY8: return VIDEO_FORMAT_Y800;
	case GST_VIDEO_FORMAT_AYUV: return VIDEO_FORMAT_AYUV;
	case GST_VIDEO_FORMAT_A420: return VIDEO_FORMAT_I40A;
	case GST_VIDEO_FORMAT_A422: return VIDEO_FORMAT_I42A;
	case GST_VIDEO_FORMAT_I420_10LE: return VIDEO_FORMAT_I010;
	case GST_VIDEO_FORMAT_P010_10LE: return VIDEO_FORMAT_P010;
	case GST_VIDEO_FORMAT_I422_10LE: return VIDEO_FORMAT_I210;
	case GST_VIDEO_FORMAT_Y444_12LE: return VIDEO_FORMAT_I412;
#if LIBOBS_API_MAJOR_VER >= 30
	case GST_VIDEO_FORMAT_v210: return VIDEO_FORMAT_V210;
#endif
	default: return VIDEO_FORMAT_NONE;
	}
}

obs_main_rendered_callback_fn obs_resolve_add_main_rendered_callback(void)
{
	static gsize initialized;
	static obs_main_rendered_callback_fn function;
	if (g_once_init_enter(&initialized)) {
#ifdef _WIN32
		HMODULE obs_module = GetModuleHandleW(L"obs.dll");
		if (!obs_module)
			obs_module = GetModuleHandleA("obs.dll");
		if (obs_module)
			function = (obs_main_rendered_callback_fn)GetProcAddress(obs_module, "obs_add_main_rendered_callback");
#elif defined(__GNUC__)
		function = obs_add_main_rendered_callback;
#endif
		g_once_init_leave(&initialized, 1);
	}
	return function;
}

obs_main_rendered_callback_fn obs_resolve_remove_main_rendered_callback(void)
{
	static gsize initialized;
	static obs_main_rendered_callback_fn function;
	if (g_once_init_enter(&initialized)) {
#ifdef _WIN32
		HMODULE obs_module = GetModuleHandleW(L"obs.dll");
		if (!obs_module)
			obs_module = GetModuleHandleA("obs.dll");
		if (obs_module)
			function = (obs_main_rendered_callback_fn)GetProcAddress(obs_module, "obs_remove_main_rendered_callback");
#elif defined(__GNUC__)
		function = obs_remove_main_rendered_callback;
#endif
		g_once_init_leave(&initialized, 1);
	}
	return function;
}
