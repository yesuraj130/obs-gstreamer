/*
 * obs-gstreamer: Master Hardware Encoder Pipeline & Pub-Sub Subscriber Hub
 */

#include "gstreamer-master-pipeline.h"
#include <obs-module.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#if HAVE_GST_D3D11
#include <d3d11.h>
#include <gst/d3d11/gstd3d11memory.h>
#endif

#if HAVE_GST_GL
#include <gst/gl/gstglmemory.h>
#include <gst/gl/gstglsyncmeta.h>
#if HAVE_GST_GL_EGL
#include <EGL/egl.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#endif
#if HAVE_GST_GL_X11 && defined(__linux__)
#include <obs-nix-platform.h>
#include <gst/gl/x11/gstgldisplay_x11.h>
#endif
#endif

bool gst_master_pipeline_element_available(const char *factory_name)
{
	if (!factory_name || !factory_name[0]) return false;
	GstElementFactory *factory = gst_element_factory_find(factory_name);
	if (!factory) return false;
	gst_object_unref(factory);
	return true;
}

static bool element_has_property(const char *factory_name, const char *prop_name)
{
	if (!factory_name || !prop_name) return false;
	GstElement *elem = gst_element_factory_make(factory_name, NULL);
	if (!elem) return false;
	GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(elem), prop_name);
	gst_object_unref(elem);
	return pspec != NULL;
}

#if HAVE_GST_D3D11 || HAVE_GST_GL
static bool encoder_supports_caps_feature(const char *factory_name, const char *feature_name)
{
	if (!factory_name || !factory_name[0] || !feature_name || !feature_name[0])
		return false;

	GstElementFactory *factory = gst_element_factory_find(factory_name);
	if (!factory)
		return false;

	bool supported = false;
	const GList *templates = gst_element_factory_get_static_pad_templates(factory);
	for (const GList *l = templates; l != NULL; l = l->next) {
		GstStaticPadTemplate *templ = (GstStaticPadTemplate *)l->data;
		if (templ->direction == GST_PAD_SINK) {
			GstCaps *caps = gst_static_pad_template_get_caps(templ);
			if (caps) {
				guint n_structs = gst_caps_get_size(caps);
				for (guint i = 0; i < n_structs; i++) {
					GstCapsFeatures *features = gst_caps_get_features(caps, i);
					if (features && gst_caps_features_contains(features, feature_name)) {
						supported = true;
						break;
					}
				}
				gst_caps_unref(caps);
			}
		}
		if (supported)
			break;
	}

	gst_object_unref(factory);
	return supported;
}
#endif

#if HAVE_GST_D3D11
static bool is_d3d11_memory_encoder(const char *name)
{
	if (!name || !name[0]) return false;

	/* Exclude Media Foundation from direct D3D11Memory input: on virtual machines/WARP,
	 * it advertises D3D11Memory statically but fails runtime MFT device binding. */
	if (g_str_has_prefix(name, "mf"))
		return false;

	/* Fast-path known hardware encoders that natively consume D3D11Memory */
	if (g_strcmp0(name, "nvd3d11h264enc") == 0 ||
	    g_strcmp0(name, "d3d11h264enc") == 0   ||
	    g_strcmp0(name, "qsvh264enc") == 0     ||
	    g_strcmp0(name, "amfh264enc") == 0)
		return true;

	/* Dynamic inspection for any custom or new encoder plugins */
#ifndef GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY
#define GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY "memory:D3D11Memory"
#endif
	return encoder_supports_caps_feature(name, GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY);
}
#endif

#ifndef GST_CAPS_FEATURE_MEMORY_GL_MEMORY
#define GST_CAPS_FEATURE_MEMORY_GL_MEMORY "memory:GLMemory"
#endif

#ifndef GST_CAPS_FEATURE_MEMORY_DMA_BUF
#define GST_CAPS_FEATURE_MEMORY_DMA_BUF "memory:DMABuf"
#endif

#if HAVE_GST_GL
static bool is_gl_memory_encoder(const char *name)
{
	if (!name || !name[0]) return false;

	/* Fast-path known hardware encoders that natively consume GLMemory */
	if (g_strcmp0(name, "nvh264enc") == 0 ||
	    g_strcmp0(name, "nvh265enc") == 0)
		return true;

	/* Dynamic inspection for any custom or new encoder plugins */
	return encoder_supports_caps_feature(name, GST_CAPS_FEATURE_MEMORY_GL_MEMORY);
}

static bool is_dmabuf_memory_encoder(const char *name)
{
	if (!name || !name[0]) return false;

	/* Fast-path known hardware encoders that natively consume DMABuf on Linux:
	 * - vah264enc / vah265enc / vaav1enc (Modern gstva plugin)
	 * - qsvh264enc / qsvh265enc / qsvav1enc (Modern oneVPL Linux plugin)
	 * - v4l2h264enc / v4l2h265enc (V4L2 M2M hardware encoders) */
	if (g_strcmp0(name, "vah264enc") == 0  ||
	    g_strcmp0(name, "vah265enc") == 0  ||
	    g_strcmp0(name, "vaav1enc") == 0   ||
	    g_strcmp0(name, "qsvh264enc") == 0 ||
	    g_strcmp0(name, "qsvh265enc") == 0 ||
	    g_strcmp0(name, "qsvav1enc") == 0  ||
	    g_strcmp0(name, "v4l2h264enc") == 0 ||
	    g_strcmp0(name, "v4l2h265enc") == 0)
		return true;

	/* Dynamic inspection for any custom or future encoder plugins */
	return encoder_supports_caps_feature(name, GST_CAPS_FEATURE_MEMORY_DMA_BUF);
}
#endif

static void log_gpu_and_encoder_capabilities_once(void)
{
	static gsize logged = 0;
	if (!g_once_init_enter(&logged)) return;

	obs_enter_graphics();
	int dev_type = gs_get_device_type();
	const char *dev_name = gs_get_device_name();
	obs_leave_graphics();

	const char *backend_str = (dev_type == GS_DEVICE_DIRECT3D_11) ? "Direct3D 11" :
	                          (dev_type == GS_DEVICE_OPENGL) ? "OpenGL" : "Unknown";

	static const char *const candidates[] = {
		"nvd3d11h264enc",
		"nvh264enc",
		"nvcudah264enc",
		"qsvh264enc",
		"amfh264enc",
		"vah264enc",
		"vaapih264enc",
		"v4l2h264enc",
		"mfh264enc",
		"vtenc_h264_hw",
		"x264enc",
		"openh264enc",
		NULL
	};

	GString *found_list = g_string_new("");
	for (int i = 0; candidates[i] != NULL; i++)
	{
		if (gst_master_pipeline_element_available(candidates[i]))
		{
			if (found_list->len > 0) g_string_append(found_list, ", ");
			g_string_append(found_list, candidates[i]);
		}
	}

	blog(LOG_INFO, "[obs-gstreamer-master] GPU Backend: %s (Adapter: %s) | Available GStreamer H.264 encoders: [%s]",
		backend_str, dev_name ? dev_name : "Unknown",
		found_list->len > 0 ? found_list->str : "none");
	g_string_free(found_list, TRUE);

	g_once_init_leave(&logged, 1);
}

const char *gst_master_pipeline_select_encoder(const char *requested_encoder, bool gpu_input)
{
	log_gpu_and_encoder_capabilities_once();

	obs_enter_graphics();
	int dev_type = gs_get_device_type();
	const char *dev_name = gs_get_device_name();
	obs_leave_graphics();

	bool is_d3d11 = (dev_type == GS_DEVICE_DIRECT3D_11);

	/* 1. Handle explicit encoder selection with smart platform alias resolution */
	if (requested_encoder && requested_encoder[0] && g_strcmp0(requested_encoder, "auto") != 0)
	{
		if (g_strcmp0(requested_encoder, "nvh264enc") == 0 ||
		    g_strcmp0(requested_encoder, "nvd3d11h264enc") == 0)
		{
			if (gpu_input && is_d3d11 && gst_master_pipeline_element_available("nvd3d11h264enc"))
				return "nvd3d11h264enc";
			if (gst_master_pipeline_element_available("nvh264enc"))
				return "nvh264enc";
			if (gst_master_pipeline_element_available("nvcudah264enc"))
				return "nvcudah264enc";
			if (gst_master_pipeline_element_available("nvd3d11h264enc"))
				return "nvd3d11h264enc";
		}
		else if (g_strcmp0(requested_encoder, "vaapih264enc") == 0 ||
		         g_strcmp0(requested_encoder, "vah264enc") == 0)
		{
			if (gst_master_pipeline_element_available(requested_encoder))
				return requested_encoder;
			if (gst_master_pipeline_element_available("vah264enc"))
				return "vah264enc";
			if (gst_master_pipeline_element_available("vaapih264enc"))
				return "vaapih264enc";
		}
		else if (gst_master_pipeline_element_available(requested_encoder))
		{
			return requested_encoder;
		}

		blog(LOG_WARNING, "[obs-gstreamer-master] Requested encoder '%s' is not available; probing best available encoder",
			requested_encoder);
	}

	/* 2. Auto-detect based on active OBS GPU vendor first */
	if (dev_name && dev_name[0])
	{
		if (g_strstr_len(dev_name, -1, "NVIDIA") || g_strstr_len(dev_name, -1, "GeForce") ||
		    g_strstr_len(dev_name, -1, "Quadro") || g_strstr_len(dev_name, -1, "RTX") ||
		    g_strstr_len(dev_name, -1, "GTX"))
		{
			if (is_d3d11 && gst_master_pipeline_element_available("nvd3d11h264enc"))
				return "nvd3d11h264enc";
			if (gst_master_pipeline_element_available("nvh264enc"))
				return "nvh264enc";
			if (gst_master_pipeline_element_available("nvcudah264enc"))
				return "nvcudah264enc";
		}
		else if (g_strstr_len(dev_name, -1, "Intel") || g_strstr_len(dev_name, -1, "Arc") ||
		         g_strstr_len(dev_name, -1, "Iris") || g_strstr_len(dev_name, -1, "UHD"))
		{
			if (gst_master_pipeline_element_available("qsvh264enc"))
				return "qsvh264enc";
			if (!is_d3d11 && gst_master_pipeline_element_available("vah264enc"))
				return "vah264enc";
			if (!is_d3d11 && gst_master_pipeline_element_available("vaapih264enc"))
				return "vaapih264enc";
		}
		else if (g_strstr_len(dev_name, -1, "AMD") || g_strstr_len(dev_name, -1, "Radeon"))
		{
			if (gst_master_pipeline_element_available("amfh264enc"))
				return "amfh264enc";
			if (!is_d3d11 && gst_master_pipeline_element_available("vah264enc"))
				return "vah264enc";
			if (!is_d3d11 && gst_master_pipeline_element_available("vaapih264enc"))
				return "vaapih264enc";
		}
	}

	/* 3. Probe all hardware encoders in platform priority order */
	if (is_d3d11)
	{
		static const char *const win_priority[] = {
			"nvd3d11h264enc",
			"nvh264enc",
			"qsvh264enc",
			"amfh264enc",
			"mfh264enc",
			"x264enc",
			"openh264enc",
			NULL
		};
		for (int i = 0; win_priority[i] != NULL; i++)
		{
			if (gst_master_pipeline_element_available(win_priority[i]))
				return win_priority[i];
		}
	}
	else
	{
		static const char *const nix_priority[] = {
			"nvh264enc",
			"nvcudah264enc",
			"vah264enc",
			"vaapih264enc",
			"qsvh264enc",
			"v4l2h264enc",
			"vtenc_h264_hw",
			"x264enc",
			"openh264enc",
			NULL
		};
		for (int i = 0; nix_priority[i] != NULL; i++)
		{
			if (gst_master_pipeline_element_available(nix_priority[i]))
				return nix_priority[i];
		}
	}

	return "x264enc";
}

static gchar *build_encoder_element_str(const char *enc_name, int gop_size, int bitrate_kbps)
{
	if (!enc_name || !enc_name[0]) enc_name = "x264enc";

	GString *str = g_string_new(enc_name);

	if (g_strcmp0(enc_name, "x264enc") == 0)
	{
		g_string_append_printf(str,
			" tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=%d bitrate=%d",
			gop_size, bitrate_kbps);
		return g_string_free(str, FALSE);
	}

	if (g_strcmp0(enc_name, "openh264enc") == 0)
	{
		if (element_has_property(enc_name, "bitrate"))
			g_string_append_printf(str, " bitrate=%d", bitrate_kbps * 1000);
		if (element_has_property(enc_name, "gop-size"))
			g_string_append_printf(str, " gop-size=%d", gop_size);
		return g_string_free(str, FALSE);
	}

	/* Hardware encoders: inspect supported properties dynamically so gst_parse_launch never fails */
	if (element_has_property(enc_name, "gop-size"))
		g_string_append_printf(str, " gop-size=%d", gop_size);
	else if (element_has_property(enc_name, "keyframe-period"))
		g_string_append_printf(str, " keyframe-period=%d", gop_size);
	else if (element_has_property(enc_name, "key-int-max"))
		g_string_append_printf(str, " key-int-max=%d", gop_size);

	if (element_has_property(enc_name, "bitrate"))
		g_string_append_printf(str, " bitrate=%d", bitrate_kbps);

	if (element_has_property(enc_name, "bframes"))
		g_string_append(str, " bframes=0");
	else if (element_has_property(enc_name, "b-frames"))
		g_string_append(str, " b-frames=0");

	if (element_has_property(enc_name, "rc-lookahead"))
		g_string_append(str, " rc-lookahead=0");

	if (element_has_property(enc_name, "low-latency"))
		g_string_append(str, " low-latency=true");

	if (g_strcmp0(enc_name, "nvh264enc") == 0 && element_has_property(enc_name, "zerolatency"))
		g_string_append(str, " zerolatency=true");

	return g_string_free(str, FALSE);
}

static GstBusSyncReply master_bus_sync_handler(GstBus *bus, GstMessage *msg, gpointer user_data)
{
	UNUSED_PARAMETER(bus);
	gst_master_pipeline_t *hub = (gst_master_pipeline_t *)user_data;
	if (!hub) return GST_BUS_PASS;

	if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT)
	{
		const gchar *context_type = NULL;
		if (!gst_message_parse_context_type(msg, &context_type) || !context_type)
			return GST_BUS_PASS;

#if HAVE_GST_D3D11
		if (hub->d3d11_device &&
		    (g_strcmp0(context_type, "gst.d3d11.device.handle") == 0 ||
		     g_strcmp0(context_type, "gst.d3d11.device") == 0))
		{
			GstContext *ctx = gst_context_new(context_type, TRUE);
			GstStructure *s = gst_context_writable_structure(ctx);
			gst_structure_set(s, "device", GST_TYPE_D3D11_DEVICE, hub->d3d11_device, NULL);
			gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), ctx);
			gst_context_unref(ctx);
			blog(LOG_INFO, "[obs-gstreamer-master] Shared OBS GstD3D11Device with element '%s' (%s)",
				GST_ELEMENT_NAME(GST_MESSAGE_SRC(msg)), context_type);
			return GST_BUS_DROP;
		}
#endif

#if HAVE_GST_GL
		if (hub->gl_display && g_strcmp0(context_type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0)
		{
			GstContext *ctx = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
			gst_context_set_gl_display(ctx, hub->gl_display);
			gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), ctx);
			gst_context_unref(ctx);
			blog(LOG_INFO, "[obs-gstreamer-master] Shared OBS GstGLDisplay with element '%s'",
				GST_ELEMENT_NAME(GST_MESSAGE_SRC(msg)));
			return GST_BUS_DROP;
		}
		if (hub->gl_app_context && g_strcmp0(context_type, "gst.gl.app_context") == 0)
		{
			GstContext *ctx = gst_context_new("gst.gl.app_context", TRUE);
			GstStructure *s = gst_context_writable_structure(ctx);
			gst_structure_set(s, "context", GST_TYPE_GL_CONTEXT, hub->gl_app_context, NULL);
			gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), ctx);
			gst_context_unref(ctx);
			blog(LOG_INFO, "[obs-gstreamer-master] Shared OBS GstGLContext with element '%s'",
				GST_ELEMENT_NAME(GST_MESSAGE_SRC(msg)));
			return GST_BUS_DROP;
		}
#endif
	}

	return GST_BUS_PASS;
}

#if HAVE_GST_D3D11
static bool init_obs_d3d11_device(gst_master_pipeline_t *hub)
{
	if (hub->d3d11_device) return true;

	obs_enter_graphics();
	int dev_type = gs_get_device_type();
	ID3D11Device *device = (dev_type == GS_DEVICE_DIRECT3D_11)
		? (ID3D11Device *)gs_get_device_obj()
		: NULL;
	if (device)
	{
		hub->d3d11_device = gst_d3d11_device_new_wrapped(device);
	}
	obs_leave_graphics();

	if (!hub->d3d11_device)
	{
		blog(LOG_WARNING, "[obs-gstreamer-master] Unable to wrap OBS D3D11Device for zero-copy");
		return false;
	}

	blog(LOG_INFO, "[obs-gstreamer-master] Wrapped OBS ID3D11Device for zero-copy D3D11Memory pipeline");
	return true;
}
#endif

#if HAVE_GST_GL
static bool init_obs_gl_context(gst_master_pipeline_t *hub)
{
	if (hub->gl_app_context && hub->gl_display) return true;

	obs_enter_graphics();
	if (gs_get_device_type() != GS_DEVICE_OPENGL)
	{
		obs_leave_graphics();
		return false;
	}

	GstGLPlatform platform = GST_GL_PLATFORM_EGL;
	guintptr gl_handle = gst_gl_context_get_current_gl_context(GST_GL_PLATFORM_EGL);
	if (!gl_handle)
	{
		platform = GST_GL_PLATFORM_GLX;
		gl_handle = gst_gl_context_get_current_gl_context(GST_GL_PLATFORM_GLX);
	}
	if (!gl_handle)
	{
		platform = GST_GL_PLATFORM_ANY;
		gl_handle = gst_gl_context_get_current_gl_context(GST_GL_PLATFORM_ANY);
	}

	if (!gl_handle)
	{
		obs_leave_graphics();
		blog(LOG_WARNING, "[obs-gstreamer-master] Could not query active OpenGL context handle from OBS");
		return false;
	}

	GstGLAPI gl_api = gst_gl_context_get_current_gl_api(platform, NULL, NULL);
	if (gl_api == GST_GL_API_NONE)
		gl_api = GST_GL_API_OPENGL3;

#if HAVE_GST_GL_EGL
	if (!hub->gl_display && platform == GST_GL_PLATFORM_EGL)
	{
		EGLDisplay egl_disp = eglGetCurrentDisplay();
		if (egl_disp != EGL_NO_DISPLAY)
			hub->gl_display = (GstGLDisplay *)gst_gl_display_egl_new_with_egl_display(egl_disp);
	}
#endif
#if HAVE_GST_GL_X11 && defined(__linux__)
	if (!hub->gl_display && platform == GST_GL_PLATFORM_GLX)
	{
		Display *x11_disp = (Display *)obs_get_nix_platform_display();
		if (x11_disp)
			hub->gl_display = (GstGLDisplay *)gst_gl_display_x11_new_with_display(x11_disp);
	}
#endif
	if (!hub->gl_display)
		hub->gl_display = gst_gl_display_new();

	hub->gl_app_context = gst_gl_context_new_wrapped(hub->gl_display, gl_handle, platform, gl_api);
	if (!hub->gl_app_context)
	{
		if (hub->gl_display)
		{
			gst_object_unref(hub->gl_display);
			hub->gl_display = NULL;
		}
		obs_leave_graphics();
		blog(LOG_WARNING, "[obs-gstreamer-master] Failed to wrap OBS OpenGL context into GstGLContext");
		return false;
	}

	GError *err = NULL;
	gst_gl_context_activate(hub->gl_app_context, TRUE);
	if (!gst_gl_context_fill_info(hub->gl_app_context, &err))
	{
		blog(LOG_WARNING, "[obs-gstreamer-master] gst_gl_context_fill_info warning: %s",
			err ? err->message : "unknown");
		g_clear_error(&err);
	}
	/* Unbind thread association before creating shared GStreamer GL context */
	gst_gl_context_activate(hub->gl_app_context, FALSE);

	obs_leave_graphics();

	if (!hub->gl_context)
	{
		gst_gl_display_lock(hub->gl_display);
		if (!gst_gl_display_create_context(hub->gl_display, hub->gl_app_context, &hub->gl_context, &err))
		{
			gst_gl_display_unlock(hub->gl_display);
			blog(LOG_WARNING, "[obs-gstreamer-master] Failed to create shared GstGLContext: %s",
				err ? err->message : "unknown");
			g_clear_error(&err);
			gst_object_unref(hub->gl_app_context);
			hub->gl_app_context = NULL;
			gst_object_unref(hub->gl_display);
			hub->gl_display = NULL;
			return false;
		}
		gst_gl_display_add_context(hub->gl_display, hub->gl_context);
		gst_gl_display_unlock(hub->gl_display);
	}

	blog(LOG_INFO, "[obs-gstreamer-master] Wrapped OBS OpenGL context (handle=%p, shared_ctx=%p, platform=%d, api=%d) for zero-copy GLMemory pipeline",
		(void *)gl_handle, (void *)hub->gl_context, (int)platform, (int)gl_api);
	return true;
}
#endif

#if HAVE_GST_D3D11 || HAVE_GST_GL
static void ensure_appsrc_gpu_caps(gst_master_pipeline_t *hub, GstVideoFormat format, const char *memory_feature, bool include_tex_target)
{
	if (!hub || !hub->appsrc || hub->appsrc_format == format) return;

	const char *fmt_str = gst_video_format_to_string(format);
	if (!fmt_str) return;

	gchar *caps_str = include_tex_target
		? g_strdup_printf("video/x-raw(%s), width=%u, height=%u, format=%s, framerate=%u/%u, texture-target=2D",
			memory_feature, hub->width, hub->height, fmt_str, hub->fps_num, hub->fps_den)
		: g_strdup_printf("video/x-raw(%s), width=%u, height=%u, format=%s, framerate=%u/%u",
			memory_feature, hub->width, hub->height, fmt_str, hub->fps_num, hub->fps_den);

	GstCaps *caps = gst_caps_from_string(caps_str);
	g_free(caps_str);
	if (caps)
	{
		gst_app_src_set_caps(GST_APP_SRC(hub->appsrc), caps);
		gst_caps_unref(caps);
		hub->appsrc_format = format;
	}
}
#endif

static GstVideoFormat get_obs_compositing_format(void)
{
	enum gs_color_format gs_fmt = GS_UNKNOWN;
	obs_enter_graphics();
	gs_texture_t *main_tex = obs_get_main_texture();
	if (main_tex)
		gs_fmt = gs_texture_get_color_format(main_tex);
	int dev_type = gs_get_device_type();
	obs_leave_graphics();

	bool is_gl = (dev_type == GS_DEVICE_OPENGL);
#if defined(__linux__) || defined(__APPLE__)
	is_gl = true;
#endif

	switch (gs_fmt)
	{
	case GS_RGBA:
	case GS_RGBA_UNORM:
		return GST_VIDEO_FORMAT_RGBA;
	case GS_BGRA:
	case GS_BGRA_UNORM:
		/*
		 * On OpenGL (Linux/macOS), OBS allocates GL_RGBA8 textures where
		 * channel 0 is Red and channel 2 is Blue.
		 * If we pass GST_VIDEO_FORMAT_BGRA to GStreamer GLMemory,
		 * glcolorconvert swizzles .bgra and inverts Red and Blue.
		 * On Direct3D 11 (Windows), DXGI format is native BGRA.
		 */
		return is_gl ? GST_VIDEO_FORMAT_RGBA : GST_VIDEO_FORMAT_BGRA;
	case GS_BGRX:
		return is_gl ? GST_VIDEO_FORMAT_RGBx : GST_VIDEO_FORMAT_BGRx;
	case GS_R10G10B10A2:
		return GST_VIDEO_FORMAT_RGB10A2_LE;
	case GS_RGBA16F:
		return GST_VIDEO_FORMAT_RGBA64_LE;
	default:
		break;
	}

	/* Fallback based on backend device if main texture is not yet rendered */
	if (is_gl)
		return GST_VIDEO_FORMAT_RGBA;
	return GST_VIDEO_FORMAT_BGRA;
}

static GstElement *create_master_encoder_pipeline(gst_master_pipeline_t *hub, const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec)
{
	gchar *pipe_desc = NULL;
	const char *convert_elem = "videoconvert ! video/x-raw, format=I420";

	if (custom_master_pipe && custom_master_pipe[0])
	{
		pipe_desc = g_strdup(custom_master_pipe);
	}
	else
	{
		int bitrate = bitrate_kbps > 0 ? bitrate_kbps : 4000;
		int keyint = keyint_sec > 0 ? keyint_sec : 1;
		int gop_size = keyint * (int)hub->fps_num / (int)hub->fps_den;
		if (gop_size < 15) gop_size = 30;

		const char *chosen_enc = hub->selected_encoder
			? hub->selected_encoder
			: gst_master_pipeline_select_encoder(encoder_type, hub->gpu_input);
		gchar *enc_str = build_encoder_element_str(chosen_enc, gop_size, bitrate);

		GstVideoFormat obs_fmt = get_obs_compositing_format();
		const char *obs_fmt_str = gst_video_format_to_string(obs_fmt);
		if (!obs_fmt_str) obs_fmt_str = "RGBA";

#if HAVE_GST_D3D11
		if (hub->gpu_input && hub->d3d11_device)
		{
			const char *post_convert = is_d3d11_memory_encoder(chosen_enc)
				? "d3d11convert ! video/x-raw(memory:D3D11Memory), format=NV12"
				: "d3d11convert ! d3d11download ! videoconvert";
			hub->appsrc_format = obs_fmt;
			pipe_desc = g_strdup_printf(
				"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true "
				"caps=\"video/x-raw(memory:D3D11Memory), width=%u, height=%u, "
				"format=%s, framerate=%u/%u\" ! "
				"%s ! %s ! h264parse config-interval=-1 ! "
				"video/x-h264, stream-format=byte-stream, alignment=au ! "
				"appsink name=hub_appsink sync=false drop=true max-buffers=1",
				hub->width, hub->height, obs_fmt_str, hub->fps_num, hub->fps_den, post_convert, enc_str);
			g_free(enc_str);
			goto launch_pipeline;
		}
#endif

#if HAVE_GST_GL
		if (hub->gpu_input && hub->gl_app_context && hub->gl_context)
		{
			const char *post_convert = is_gl_memory_encoder(chosen_enc)
				? "glcolorconvert ! video/x-raw(memory:GLMemory), format=NV12"
				: is_dmabuf_memory_encoder(chosen_enc)
					? "glcolorconvert ! gldownload ! video/x-raw(memory:DMABuf), format=NV12"
					: "glcolorconvert ! gldownload ! videoconvert";
			hub->appsrc_format = obs_fmt;
			pipe_desc = g_strdup_printf(
				"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true "
				"caps=\"video/x-raw(memory:GLMemory), width=%u, height=%u, "
				"format=%s, framerate=%u/%u, texture-target=2D\" ! "
				"%s ! %s ! h264parse config-interval=-1 ! "
				"video/x-h264, stream-format=byte-stream, alignment=au ! "
				"appsink name=hub_appsink sync=false drop=true max-buffers=1",
				hub->width, hub->height, obs_fmt_str, hub->fps_num, hub->fps_den, post_convert, enc_str);
			g_free(enc_str);
			goto launch_pipeline;
		}
#endif

		hub->appsrc_format = GST_VIDEO_FORMAT_RGBA;
		pipe_desc = g_strdup_printf(
			"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true "
			"caps=\"video/x-raw, width=%u, height=%u, format=RGBA, framerate=%u/%u\" ! "
			"%s ! %s ! h264parse config-interval=-1 ! video/x-h264, stream-format=byte-stream, alignment=au ! "
			"appsink name=hub_appsink sync=false drop=true max-buffers=1",
			hub->width, hub->height, hub->fps_num, hub->fps_den,
			convert_elem, enc_str);

		g_free(enc_str);
	}

#if HAVE_GST_D3D11 || HAVE_GST_GL
launch_pipeline:
#endif
	blog(LOG_INFO, "[obs-gstreamer-master] Launching Master Hardware Encoder Pipeline (gpu_zero_copy=%s, encoder=%s, appsrc_format=%s): %s",
		hub->gpu_input ? "yes" : "no",
		hub->selected_encoder ? hub->selected_encoder : "custom",
		gst_video_format_to_string(hub->appsrc_format),
		pipe_desc);
	GError *error = NULL;
	GstElement *pipe = gst_parse_launch(pipe_desc, &error);
	g_free(pipe_desc);

	if (error)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to create master pipeline: %s", error->message);
		g_clear_error(&error);
		return NULL;
	}

	if (pipe)
	{
		GstBus *bus = gst_element_get_bus(pipe);
		if (bus)
		{
			gst_bus_set_sync_handler(bus, master_bus_sync_handler, hub, NULL);
			gst_object_unref(bus);
		}
#if HAVE_GST_D3D11
		if (hub->d3d11_device)
		{
			GstContext *ctx = gst_context_new("gst.d3d11.device.handle", TRUE);
			GstStructure *s = gst_context_writable_structure(ctx);
			gst_structure_set(s, "device", GST_TYPE_D3D11_DEVICE, hub->d3d11_device, NULL);
			gst_element_set_context(pipe, ctx);
			gst_context_unref(ctx);
		}
#endif
#if HAVE_GST_GL
		if (hub->gl_display)
		{
			GstContext *ctx = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
			gst_context_set_gl_display(ctx, hub->gl_display);
			gst_element_set_context(pipe, ctx);
			gst_context_unref(ctx);
		}
		if (hub->gl_app_context)
		{
			GstContext *ctx = gst_context_new("gst.gl.app_context", TRUE);
			GstStructure *s = gst_context_writable_structure(ctx);
			gst_structure_set(s, "context", GST_TYPE_GL_CONTEXT, hub->gl_app_context, NULL);
			gst_element_set_context(pipe, ctx);
			gst_context_unref(ctx);
		}
#endif
	}

	return pipe;
}

static GstFlowReturn video_new_sample_cb(GstAppSink *sink, gpointer user_data)
{
	gst_master_pipeline_t *hub = (gst_master_pipeline_t *)user_data;
	if (!hub) return GST_FLOW_OK;

	GstSample *sample = gst_app_sink_pull_sample(sink);
	if (!sample) return GST_FLOW_OK;

	GstBuffer *buf = gst_sample_get_buffer(sample);
	if (!buf)
	{
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	const bool is_keyframe = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
	uint64_t sample_number = ++hub->encoded_sample_count;
	if (sample_number == 1 || sample_number % 300 == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] Encoded sample #%" PRIu64 ": %" G_GSIZE_FORMAT
			" bytes, keyframe=%s (gpu_zero_copy=%s, stagesurf=%s)",
			sample_number, gst_buffer_get_size(buf), is_keyframe ? "yes" : "no",
			hub->gpu_input ? "yes" : "no",
			hub->stagesurf ? "allocated" : "NONE");
	}

	GPtrArray *subs = (GPtrArray *)g_atomic_pointer_get(&hub->subscribers_atomic);
	if (subs)
	{
		for (guint i = 0; i < subs->len; i++)
		{
			gst_pipeline_subscriber_t *sub = g_ptr_array_index(subs, i);
			if (sub && sub->cb) sub->cb(buf, is_keyframe, sub->user_data);
		}
	}

	gst_sample_unref(sample);
	return GST_FLOW_OK;
}

#if HAVE_GST_D3D11
static void release_d3d11_texture(gpointer user_data)
{
	ID3D11Texture2D_Release((ID3D11Texture2D *)user_data);
}

static bool push_d3d11_texture(gst_master_pipeline_t *hub, gs_texture_t *texture, uint64_t pts)
{
	if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] GPU zero-copy requires the OBS Direct3D 11 graphics backend");
		return false;
	}

	uint32_t width = gs_texture_get_width(texture);
	uint32_t height = gs_texture_get_height(texture);
	if (width != hub->width || height != hub->height)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] D3D11 texture size %ux%u does not match pipeline size %ux%u",
			width, height, hub->width, hub->height);
		return false;
	}

	enum gs_color_format color_format = gs_texture_get_color_format(texture);
	GstVideoFormat format;

	switch (color_format)
	{
	case GS_RGBA:
	case GS_RGBA_UNORM: format = GST_VIDEO_FORMAT_RGBA; break;
	case GS_BGRA:
	case GS_BGRA_UNORM: format = GST_VIDEO_FORMAT_BGRA; break;
	case GS_BGRX: format = GST_VIDEO_FORMAT_BGRx; break;
	default:
		blog(LOG_ERROR, "[obs-gstreamer-master] Unsupported D3D11 texture format for zero-copy input: %d",
			(int)color_format);
		return false;
	}

	if (!hub->d3d11_device && !init_obs_d3d11_device(hub))
		return false;

	ID3D11Texture2D *d3d_texture = (ID3D11Texture2D *)gs_texture_get_obj(texture);
	if (!d3d_texture)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] OBS texture has no native D3D11 resource");
		return false;
	}

	if (!hub->d3d11_desc_logged)
	{
		D3D11_TEXTURE2D_DESC desc = {0};
		ID3D11Texture2D_GetDesc(d3d_texture, &desc);
		blog(LOG_INFO, "[obs-gstreamer-master] D3D11 native texture desc: %ux%u, DXGI_FORMAT=%u, BindFlags=0x%x, Usage=%u, MiscFlags=0x%x (Direct wrap, no CopyResource)",
			desc.Width, desc.Height, (unsigned)desc.Format, (unsigned)desc.BindFlags,
			(unsigned)desc.Usage, (unsigned)desc.MiscFlags);
		hub->d3d11_desc_logged = true;
	}

	ensure_appsrc_gpu_caps(hub, format, "memory:D3D11Memory", false);

	GstVideoInfo info;
	if (!gst_video_info_set_format(&info, format, width, height))
		return false;

	gst_d3d11_memory_init_once();
	GstAllocator *allocator = gst_allocator_find(GST_D3D11_MEMORY_NAME);
	if (!allocator)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] GStreamer D3D11 allocator is unavailable");
		return false;
	}

	ID3D11Texture2D_AddRef(d3d_texture);
	GstMemory *memory = gst_d3d11_allocator_alloc_wrapped(
		GST_D3D11_ALLOCATOR(allocator), hub->d3d11_device, d3d_texture,
		GST_VIDEO_INFO_SIZE(&info), d3d_texture, release_d3d11_texture);
	gst_object_unref(allocator);
	if (!memory)
	{
		ID3D11Texture2D_Release(d3d_texture);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to wrap OBS texture as GstD3D11Memory");
		return false;
	}

	GstBuffer *buffer = gst_buffer_new();
	if (!buffer)
	{
		gst_memory_unref(memory);
		return false;
	}
	gst_buffer_append_memory(buffer, memory);

	gsize offsets[GST_VIDEO_MAX_PLANES] = {0};
	gint strides[GST_VIDEO_MAX_PLANES] = {0};
	for (guint plane = 0; plane < GST_VIDEO_INFO_N_PLANES(&info); plane++)
	{
		offsets[plane] = GST_VIDEO_INFO_PLANE_OFFSET(&info, plane);
		strides[plane] = GST_VIDEO_INFO_PLANE_STRIDE(&info, plane);
	}
	if (!gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, format, width, height,
		GST_VIDEO_INFO_N_PLANES(&info), offsets, strides))
	{
		gst_buffer_unref(buffer);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to attach video metadata to D3D11 texture frame");
		return false;
	}

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(buffer) = pts;
	GST_BUFFER_DTS(buffer) = pts;
	GST_BUFFER_DURATION(buffer) = duration;
	hub->frame_index++;

	GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), buffer);
	if (flow != GST_FLOW_OK)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] D3D11 texture push failed: flow=%s",
			gst_flow_get_name(flow));
		return false;
	}

	uint64_t frame_number = ++hub->texture_push_count;
	if (frame_number == 1 || frame_number % 300 == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] D3D11 zero-copy texture frame #%" PRIu64
			" wrapped and pushed (%ux%u, format=%d, stagesurf=NONE)",
			frame_number, width, height, (int)color_format);
	}
	return true;
}
#endif

#if HAVE_GST_GL
static bool push_gl_texture(gst_master_pipeline_t *hub, gs_texture_t *texture, uint64_t pts)
{
	if (gs_get_device_type() != GS_DEVICE_OPENGL)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] OpenGL zero-copy requires the OBS OpenGL graphics backend");
		return false;
	}

	uint32_t width = gs_texture_get_width(texture);
	uint32_t height = gs_texture_get_height(texture);
	if (width != hub->width || height != hub->height)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] OpenGL texture size %ux%u does not match pipeline size %ux%u",
			width, height, hub->width, hub->height);
		return false;
	}

	enum gs_color_format color_format = gs_texture_get_color_format(texture);
	GstVideoFormat format;

	switch (color_format)
	{
	case GS_RGBA:
	case GS_RGBA_UNORM:
		format = GST_VIDEO_FORMAT_RGBA;
		break;
	case GS_BGRA:
	case GS_BGRA_UNORM:
		/*
		 * On OpenGL, OBS allocates GL_RGBA8 textures where component 0 is Red.
		 * If we pass GST_VIDEO_FORMAT_BGRA to GStreamer GLMemory,
		 * glcolorconvert swizzles .bgra and inverts Red and Blue!
		 * Map OBS GS_BGRA to GStreamer RGBA for OpenGL zero-copy.
		 */
		format = GST_VIDEO_FORMAT_RGBA;
		break;
	case GS_BGRX:
		format = GST_VIDEO_FORMAT_RGBx;
		break;
	default:
		blog(LOG_ERROR, "[obs-gstreamer-master] Unsupported OpenGL texture format for zero-copy input: %d",
			(int)color_format);
		return false;
	}

	if ((!hub->gl_app_context || !hub->gl_context || !hub->gl_display) && !init_obs_gl_context(hub))
		return false;

	const guint *tex_ptr = (const guint *)gs_texture_get_obj(texture);
	if (!tex_ptr || *tex_ptr == 0)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] OBS texture has no valid OpenGL texture ID");
		return false;
	}
	guint tex_id = *tex_ptr;

	if (!hub->gl_tex_logged)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] OpenGL native texture ID=%u (%ux%u, gs_format=%d -> gst_format=%s, Direct wrap, no CPU readback)",
			(unsigned)tex_id, width, height, (int)color_format, gst_video_format_to_string(format));
		hub->gl_tex_logged = true;
	}

	ensure_appsrc_gpu_caps(hub, format, "memory:GLMemory", true);

	GstVideoInfo info;
	if (!gst_video_info_set_format(&info, format, width, height))
		return false;

	gst_gl_memory_init_once();
	GstGLMemoryAllocator *allocator = gst_gl_memory_allocator_get_default(hub->gl_context);
	if (!allocator)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] GStreamer GLMemory allocator is unavailable");
		return false;
	}

	/* Flush OBS's GL command stream on the current render thread before GStreamer's shared GL context samples tex_id */
	gs_flush();

	GstGLFormat gl_format = gst_gl_format_from_video_info(hub->gl_context, &info, 0);
	GstGLVideoAllocationParams *params = gst_gl_video_allocation_params_new_wrapped_texture(
		hub->gl_context, NULL, &info, 0, NULL,
		GST_GL_TEXTURE_TARGET_2D, gl_format, tex_id, NULL, NULL);
	if (!params)
	{
		gst_object_unref(allocator);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to create GstGLVideoAllocationParams for texture %u",
			(unsigned)tex_id);
		return false;
	}

	GstGLBaseMemory *gl_mem = gst_gl_base_memory_alloc(
		GST_GL_BASE_MEMORY_ALLOCATOR(allocator), (GstGLAllocationParams *)params);
	gst_gl_allocation_params_free((GstGLAllocationParams *)params);
	gst_object_unref(allocator);

	if (!gl_mem)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to wrap OBS OpenGL texture %u as GstGLMemory",
			(unsigned)tex_id);
		return false;
	}

	GstBuffer *buffer = gst_buffer_new();
	if (!buffer)
	{
		gst_memory_unref(GST_MEMORY_CAST(gl_mem));
		return false;
	}
	gst_buffer_append_memory(buffer, GST_MEMORY_CAST(gl_mem));

	gsize offsets[GST_VIDEO_MAX_PLANES] = {0};
	gint strides[GST_VIDEO_MAX_PLANES] = {0};
	for (guint plane = 0; plane < GST_VIDEO_INFO_N_PLANES(&info); plane++)
	{
		offsets[plane] = GST_VIDEO_INFO_PLANE_OFFSET(&info, plane);
		strides[plane] = GST_VIDEO_INFO_PLANE_STRIDE(&info, plane);
	}
	if (!gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, format, width, height,
		GST_VIDEO_INFO_N_PLANES(&info), offsets, strides))
	{
		gst_buffer_unref(buffer);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to attach video metadata to OpenGL texture frame");
		return false;
	}

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(buffer) = pts;
	GST_BUFFER_DTS(buffer) = pts;
	GST_BUFFER_DURATION(buffer) = duration;
	hub->frame_index++;

	GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), buffer);
	if (flow != GST_FLOW_OK)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] OpenGL texture push failed: flow=%s",
			gst_flow_get_name(flow));
		return false;
	}

	uint64_t frame_number = ++hub->texture_push_count;
	if (frame_number == 1 || frame_number % 300 == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] OpenGL zero-copy texture frame #%" PRIu64
			" wrapped and pushed (%ux%u, tex_id=%u, format=%s, stagesurf=NONE)",
			frame_number, width, height, (unsigned)tex_id, gst_video_format_to_string(format));
	}
	return true;
}
#endif

void gst_master_pipeline_push_texture(gst_master_pipeline_t *hub, void *gpu_handle, uint64_t pts)
{
	if (!hub || !gpu_handle || g_atomic_int_get(&hub->active_destinations) <= 0 || !hub->appsrc) return;

	gs_texture_t *tex = (gs_texture_t *)gpu_handle;
#if HAVE_GST_D3D11
	if (hub->gpu_input && hub->d3d11_device)
	{
		push_d3d11_texture(hub, tex, pts);
		return;
	}
#endif
#if HAVE_GST_GL
	if (hub->gpu_input && hub->gl_app_context)
	{
		push_gl_texture(hub, tex, pts);
		return;
	}
#endif
	uint64_t frame_number = ++hub->texture_push_count;
	uint32_t tex_w = gs_texture_get_width(tex);
	uint32_t tex_h = gs_texture_get_height(tex);
	enum gs_color_format fmt = gs_texture_get_color_format(tex);

	if (tex_w == 0 || tex_h == 0) return;

	if (!hub->stagesurf ||
	    gs_stagesurface_get_width(hub->stagesurf) != tex_w ||
	    gs_stagesurface_get_height(hub->stagesurf) != tex_h ||
	    gs_stagesurface_get_color_format(hub->stagesurf) != fmt)
	{
		if (hub->stagesurf)
		{
			gs_stagesurface_destroy(hub->stagesurf);
			hub->stagesurf = NULL;
		}
		hub->stagesurf = gs_stagesurface_create(tex_w, tex_h, fmt);
	}

	if (!hub->stagesurf)
	{
		if (frame_number == 1)
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to create %ux%u staging surface for format=%d",
				tex_w, tex_h, (int)fmt);
		return;
	}

	gs_stage_texture(hub->stagesurf, tex);

	uint8_t *src_data = NULL;
	uint32_t src_linesize = 0;
	if (!gs_stagesurface_map(hub->stagesurf, &src_data, &src_linesize))
	{
		if (frame_number == 1)
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to map %ux%u staging surface",
				tex_w, tex_h);
		return;
	}

	uint32_t copy_w = tex_w < hub->width ? tex_w : hub->width;
	uint32_t copy_h = tex_h < hub->height ? tex_h : hub->height;
	size_t dst_stride = (size_t)hub->width * 4;
	size_t total_size = dst_stride * hub->height;

	GstBuffer *in_buf = gst_buffer_new_allocate(NULL, total_size, NULL);
	if (!in_buf)
	{
		if (frame_number == 1)
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to allocate input buffer for texture frame");
		gs_stagesurface_unmap(hub->stagesurf);
		return;
	}

	GstMapInfo map;
	if (!gst_buffer_map(in_buf, &map, GST_MAP_WRITE))
	{
		if (frame_number == 1)
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to map input buffer for texture frame");
		gst_buffer_unref(in_buf);
		gs_stagesurface_unmap(hub->stagesurf);
		return;
	}

	bool is_bgra = (fmt == GS_BGRA || fmt == GS_BGRA_UNORM || fmt == GS_BGRX);
	if ((frame_number == 1 || frame_number % 300 == 0) && copy_w > 0 && copy_h > 0)
	{
		uint32_t sample_x = copy_w / 2;
		uint32_t sample_y = copy_h / 2;
		const uint8_t *sample = src_data + (size_t)sample_y * src_linesize + (size_t)sample_x * 4;
		blog(LOG_INFO, "[obs-gstreamer-master] Texture frame #%" PRIu64
			" readback center pixel (raw)=%u,%u,%u,%u",
			frame_number, sample[0], sample[1], sample[2], sample[3]);
	}

	for (uint32_t y = 0; y < copy_h; y++)
	{
		const uint8_t *src_row = src_data + (size_t)y * src_linesize;
		uint8_t *dst_row = map.data + (size_t)y * dst_stride;
		if (!is_bgra)
		{
			memcpy(dst_row, src_row, (size_t)copy_w * 4);
		}
		else
		{
			for (uint32_t x = 0; x < copy_w; x++)
			{
				dst_row[x * 4 + 0] = src_row[x * 4 + 2];
				dst_row[x * 4 + 1] = src_row[x * 4 + 1];
				dst_row[x * 4 + 2] = src_row[x * 4 + 0];
				dst_row[x * 4 + 3] = src_row[x * 4 + 3];
			}
		}
	}
	gst_buffer_unmap(in_buf, &map);

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(in_buf) = pts > 0 ? pts : (hub->frame_index * duration);
	GST_BUFFER_DTS(in_buf) = GST_BUFFER_PTS(in_buf);
	GST_BUFFER_DURATION(in_buf) = duration;
	hub->frame_index++;

	GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), in_buf);
	if (flow != GST_FLOW_OK)
	{
		if (frame_number == 1 || frame_number % 300 == 0)
			blog(LOG_ERROR, "[obs-gstreamer-master] Texture frame #%" PRIu64
				" push to encoder failed: flow=%s",
				frame_number, gst_flow_get_name(flow));
	}
	else if (frame_number == 1 || frame_number % 300 == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] Texture frame #%" PRIu64
			" pushed to encoder (%ux%u texture, %ux%u submitted)",
			frame_number, tex_w, tex_h, copy_w, copy_h);
	}

	gs_stagesurface_unmap(hub->stagesurf);
}

void gst_master_pipeline_push_raw_video(gst_master_pipeline_t *hub, struct video_data *frame)
{
	if (!hub || !frame || !frame->data[0] || g_atomic_int_get(&hub->active_destinations) <= 0 || !hub->appsrc) return;

	size_t row_size = (size_t)hub->width * 4;
	if (hub->gpu_input || hub->height == 0 || row_size / 4 != hub->width ||
	    (size_t)frame->linesize[0] < row_size)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Invalid CPU frame layout for wrapped input");
		return;
	}

	size_t frame_size = (size_t)frame->linesize[0] * hub->height;
	if (frame_size / hub->height != (size_t)frame->linesize[0])
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] CPU frame size overflow while wrapping input");
		return;
	}

	GstBuffer *buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, frame->data[0],
		frame_size, 0, frame_size, NULL, NULL);
	if (!buf) return;

	gsize offsets[GST_VIDEO_MAX_PLANES] = {0};
	gint strides[GST_VIDEO_MAX_PLANES] = {(gint)frame->linesize[0]};
	if (!gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_RGBA,
		hub->width, hub->height, 1, offsets, strides))
	{
		gst_buffer_unref(buf);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to attach video metadata to wrapped CPU frame");
		return;
	}

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(buf) = frame->timestamp;
	GST_BUFFER_DTS(buf) = frame->timestamp;
	GST_BUFFER_DURATION(buf) = duration;
	hub->frame_index++;

	GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), buf);
	if (flow != GST_FLOW_OK)
		blog(LOG_ERROR, "[obs-gstreamer-master] Wrapped CPU frame push failed: flow=%s",
			gst_flow_get_name(flow));
}

static void update_active_destinations(gst_master_pipeline_t *hub, int delta)
{
	if (!hub) return;

	int prev = g_atomic_int_add(&hub->active_destinations, delta);
	int updated = prev + delta;
	if (updated < 0)
	{
		g_atomic_int_set(&hub->active_destinations, 0);
		updated = 0;
	}

	if (prev == 0 && updated > 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] First destination active! Starting master encoder.");
		if (hub->pipe) gst_element_set_state(hub->pipe, GST_STATE_PLAYING);

		if (hub->appsink)
		{
			GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
			gst_element_send_event(hub->appsink, event);
		}

		g_mutex_lock(&hub->state_mutex);
		gst_master_state_cb cb = hub->state_cb;
		gpointer ud = hub->state_user_data;
		g_mutex_unlock(&hub->state_mutex);
		if (cb) cb(true, ud);
	}
	else if (prev > 0 && updated == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] All destinations inactive. Pausing encoder pipeline (0%% CPU/GPU).");
		if (hub->pipe) gst_element_set_state(hub->pipe, GST_STATE_PAUSED);

		g_mutex_lock(&hub->state_mutex);
		gst_master_state_cb cb = hub->state_cb;
		gpointer ud = hub->state_user_data;
		g_mutex_unlock(&hub->state_mutex);
		if (cb) cb(false, ud);
	}
}

gst_pipeline_subscriber_t *gst_master_pipeline_subscribe(gst_master_pipeline_t *hub, gst_master_frame_cb cb, gpointer user_data, const char *name)
{
	if (!hub) return NULL;

	gst_pipeline_subscriber_t *sub = bzalloc(sizeof(gst_pipeline_subscriber_t));
	sub->cb = cb;
	sub->user_data = user_data;
	sub->name = bstrdup(name ? name : "Subscriber");
	sub->waiting_for_keyframe = true;

	g_mutex_lock(&hub->subscriber_mutex);

	GPtrArray *old_subs = (GPtrArray *)hub->subscribers_atomic;
	GPtrArray *new_subs = g_ptr_array_new();

	if (old_subs)
	{
		for (guint i = 0; i < old_subs->len; i++)
		{
			g_ptr_array_add(new_subs, g_ptr_array_index(old_subs, i));
		}
	}
	g_ptr_array_add(new_subs, sub);
	g_atomic_pointer_set(&hub->subscribers_atomic, new_subs);

	g_mutex_unlock(&hub->subscriber_mutex);

	if (old_subs) g_ptr_array_free(old_subs, TRUE);

	blog(LOG_INFO, "[obs-gstreamer-master] Subscriber '%s' connected", sub->name);
	update_active_destinations(hub, 1);

	/* Request immediate IDR keyframe so the new subscriber receives video immediately */
	if (hub->appsink)
	{
		GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
		gst_element_send_event(hub->appsink, event);
	}

	return sub;
}

void gst_master_pipeline_unsubscribe(gst_master_pipeline_t *hub, gst_pipeline_subscriber_t *sub)
{
	if (!hub || !sub) return;

	g_mutex_lock(&hub->subscriber_mutex);

	GPtrArray *old_subs = (GPtrArray *)hub->subscribers_atomic;
	GPtrArray *new_subs = g_ptr_array_new();

	if (old_subs)
	{
		for (guint i = 0; i < old_subs->len; i++)
		{
			gst_pipeline_subscriber_t *existing = g_ptr_array_index(old_subs, i);
			if (existing != sub) g_ptr_array_add(new_subs, existing);
		}
	}
	g_atomic_pointer_set(&hub->subscribers_atomic, new_subs);

	g_mutex_unlock(&hub->subscriber_mutex);

	if (old_subs) g_ptr_array_free(old_subs, TRUE);

	blog(LOG_INFO, "[obs-gstreamer-master] Subscriber '%s' disconnected", sub->name ? sub->name : "Subscriber");
	bfree(sub->name);
	bfree(sub);
	update_active_destinations(hub, -1);
}

void gst_master_pipeline_request_keyframe(gst_master_pipeline_t *hub)
{
	if (!hub || !hub->appsink) return;
	GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
	gst_element_send_event(hub->appsink, event);
}

gst_master_pipeline_t *gst_master_pipeline_create(const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, bool gpu_input, gst_master_state_cb state_cb, gpointer user_data)
{
	gst_master_pipeline_t *hub = bzalloc(sizeof(gst_master_pipeline_t));
	g_mutex_init(&hub->subscriber_mutex);
	g_mutex_init(&hub->state_mutex);

	hub->width = width;
	hub->height = height;
	hub->fps_num = fps_num;
	hub->fps_den = fps_den;
	hub->state_cb = state_cb;
	hub->state_user_data = user_data;
	hub->gpu_input = false;

	const char *chosen_enc = gst_master_pipeline_select_encoder(encoder_type, gpu_input);
	hub->selected_encoder = bstrdup(chosen_enc);

	bool allow_gpu_pipe = !custom_master_pipe || !custom_master_pipe[0] ||
	                      strstr(custom_master_pipe, "memory:D3D11Memory") != NULL ||
	                      strstr(custom_master_pipe, "memory:GLMemory") != NULL;

	if (gpu_input && allow_gpu_pipe)
	{
#if HAVE_GST_D3D11
		if ((gst_master_pipeline_element_available("d3d11convert") ||
		     (custom_master_pipe && strstr(custom_master_pipe, "memory:D3D11Memory") != NULL)) &&
		    init_obs_d3d11_device(hub))
		{
			hub->gpu_input = true;
		}
#endif
#if HAVE_GST_GL
		if (!hub->gpu_input &&
		    ((gst_master_pipeline_element_available("glcolorconvert") &&
		      gst_master_pipeline_element_available("gldownload")) ||
		     (custom_master_pipe && strstr(custom_master_pipe, "memory:GLMemory") != NULL)) &&
		    init_obs_gl_context(hub))
		{
			hub->gpu_input = true;
		}
#endif
		if (!hub->gpu_input)
		{
			blog(LOG_WARNING, "[obs-gstreamer-master] GPU zero-copy context unavailable; falling back to CPU staging surface");
		}
	}

	hub->pipe = create_master_encoder_pipeline(hub, custom_master_pipe, encoder_type, bitrate_kbps, keyint_sec);
	if (hub->pipe)
	{
		hub->appsrc = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsrc");
		hub->appsink = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsink");
	}

	hub->active_destinations = 0;
	GstStateChangeReturn state_ret = GST_STATE_CHANGE_FAILURE;
	if (hub->pipe && hub->appsrc && hub->appsink)
	{
		state_ret = gst_element_set_state(hub->pipe, GST_STATE_PAUSED);
	}

	/* If initial encoder failed to parse or failed state change, try fallback encoders */
	if ((!hub->pipe || !hub->appsrc || !hub->appsink || state_ret == GST_STATE_CHANGE_FAILURE) &&
	    (!custom_master_pipe || !custom_master_pipe[0]))
	{
		static const char *const fallback_encoders[] = {
#if HAVE_GST_D3D11
			"nvd3d11h264enc",
			"mfh264enc",
#endif
#if HAVE_GST_GL
			"vah264enc",
			"vaapih264enc",
#endif
			"nvh264enc",
			"qsvh264enc",
			"amfh264enc",
			"openh264enc",
			"x264enc",
			NULL
		};

		for (int i = 0; fallback_encoders[i] != NULL; i++)
		{
			const char *candidate = fallback_encoders[i];
			if (hub->selected_encoder && g_strcmp0(candidate, hub->selected_encoder) == 0)
				continue;
			if (!gst_master_pipeline_element_available(candidate))
				continue;

			blog(LOG_WARNING, "[obs-gstreamer-master] Encoder '%s' failed state change; trying '%s' (gpu_zero_copy=%s)",
				hub->selected_encoder, candidate, hub->gpu_input ? "yes" : "no");

			gst_element_set_state(hub->pipe, GST_STATE_NULL);
			gst_object_unref(hub->appsrc);
			gst_object_unref(hub->appsink);
			hub->appsrc = NULL;
			hub->appsink = NULL;
			gst_object_unref(hub->pipe);
			hub->pipe = NULL;

			bfree(hub->selected_encoder);
			hub->selected_encoder = bstrdup(candidate);

			hub->pipe = create_master_encoder_pipeline(hub, custom_master_pipe, candidate, bitrate_kbps, keyint_sec);
			if (!hub->pipe) continue;

			hub->appsrc = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsrc");
			hub->appsink = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsink");
			if (!hub->appsrc || !hub->appsink) continue;

			state_ret = gst_element_set_state(hub->pipe, GST_STATE_PAUSED);
			if (state_ret != GST_STATE_CHANGE_FAILURE)
				break;
		}
	}

	if (state_ret == GST_STATE_CHANGE_FAILURE && hub->gpu_input)
	{
		blog(LOG_WARNING, "[obs-gstreamer-master] GPU zero-copy pipeline failed to initialize; retrying with CPU texture readback");
		if (hub->pipe) gst_element_set_state(hub->pipe, GST_STATE_NULL);
		if (hub->appsrc) gst_object_unref(hub->appsrc);
		if (hub->appsink) gst_object_unref(hub->appsink);
		hub->appsrc = NULL;
		hub->appsink = NULL;
		if (hub->pipe) gst_object_unref(hub->pipe);
		hub->pipe = NULL;
		hub->gpu_input = false;

		hub->pipe = create_master_encoder_pipeline(hub, custom_master_pipe, encoder_type, bitrate_kbps, keyint_sec);
		if (!hub->pipe)
		{
			gst_master_pipeline_destroy(hub);
			return NULL;
		}
		hub->appsrc = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsrc");
		hub->appsink = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsink");
		if (!hub->appsrc || !hub->appsink)
		{
			blog(LOG_ERROR, "[obs-gstreamer-master] Fallback pipeline missing hub_appsrc or hub_appsink");
			gst_master_pipeline_destroy(hub);
			return NULL;
		}
		state_ret = gst_element_set_state(hub->pipe, GST_STATE_PAUSED);
	}

	if (state_ret == GST_STATE_CHANGE_FAILURE)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to initialize master pipeline");
		gst_master_pipeline_destroy(hub);
		return NULL;
	}

	GstAppSinkCallbacks sink_cbs;
	memset(&sink_cbs, 0, sizeof(sink_cbs));
	sink_cbs.new_sample = video_new_sample_cb;
	gst_app_sink_set_callbacks(GST_APP_SINK(hub->appsink), &sink_cbs, hub, NULL);

	if (state_cb) state_cb(false, user_data);

	return hub;
}

void gst_master_pipeline_destroy(gst_master_pipeline_t *hub)
{
	if (!hub) return;

	if (hub->pipe)
	{
		GstBus *bus = gst_element_get_bus(hub->pipe);
		if (bus)
		{
			gst_bus_set_sync_handler(bus, NULL, NULL, NULL);
			gst_object_unref(bus);
		}
		gst_element_set_state(hub->pipe, GST_STATE_NULL);
		if (hub->appsrc) gst_object_unref(hub->appsrc);
		if (hub->appsink) gst_object_unref(hub->appsink);
		gst_object_unref(hub->pipe);
		hub->pipe = NULL;
	}

	if (hub->stagesurf)
	{
		obs_enter_graphics();
		gs_stagesurface_destroy(hub->stagesurf);
		hub->stagesurf = NULL;
		obs_leave_graphics();
	}

#if HAVE_GST_D3D11
	if (hub->d3d11_device)
	{
		gst_object_unref(hub->d3d11_device);
		hub->d3d11_device = NULL;
	}
#endif

#if HAVE_GST_GL
	if (hub->gl_context)
	{
		gst_object_unref(hub->gl_context);
		hub->gl_context = NULL;
	}
	if (hub->gl_app_context)
	{
		gst_object_unref(hub->gl_app_context);
		hub->gl_app_context = NULL;
	}
	if (hub->gl_display)
	{
		gst_object_unref(hub->gl_display);
		hub->gl_display = NULL;
	}
#endif

	if (hub->selected_encoder)
	{
		bfree(hub->selected_encoder);
		hub->selected_encoder = NULL;
	}

	GPtrArray *subs = (GPtrArray *)hub->subscribers_atomic;
	if (subs)
	{
		for (guint i = 0; i < subs->len; i++)
		{
			gst_pipeline_subscriber_t *sub = g_ptr_array_index(subs, i);
			if (sub)
			{
				bfree(sub->name);
				bfree(sub);
			}
		}
		g_ptr_array_free(subs, TRUE);
	}

	g_mutex_clear(&hub->subscriber_mutex);
	g_mutex_clear(&hub->state_mutex);
	bfree(hub);
}
