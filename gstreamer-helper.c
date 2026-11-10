#include "gstreamer-helper.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__GNUC__)
extern void obs_add_main_rendered_callback(void (*draw)(void *param), void *param) __attribute__((weak));
extern void obs_remove_main_rendered_callback(void (*draw)(void *param), void *param) __attribute__((weak));
#endif

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
	case VIDEO_FORMAT_V210: return GST_VIDEO_FORMAT_v210;
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
	case GST_VIDEO_FORMAT_v210: return VIDEO_FORMAT_V210;
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
