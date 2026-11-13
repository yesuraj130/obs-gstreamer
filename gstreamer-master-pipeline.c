/*
 * obs-gstreamer: Master Hardware Encoder Pipeline & Pub-Sub Subscriber Hub
 */

#include "gstreamer-master-pipeline.h"
#include "gstreamer-helper.h"
#include <obs-module.h>
#include <util/platform.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#if HAVE_GST_D3D11
#include <d3d11.h>
#include <d3d10_1.h>
#include <gst/d3d11/gstd3d11memory.h>

#ifndef IID_ID3D10Multithread
static const GUID IID_ID3D10Multithread = { 0x9b7e4e00, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };
#endif
#ifndef IID_ID3D11Multithread
static const GUID IID_ID3D11Multithread = { 0x03024be4, 0xc065, 0x4c45, { 0x80, 0x13, 0x0d, 0xb3, 0xd7, 0x73, 0x62, 0xc2 } };
#endif
#ifndef IID_IDXGIDevice
static const GUID IID_IDXGIDevice = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
#endif
#ifndef IID_IDXGIResource
static const GUID IID_IDXGIResource = { 0x035f3ab4, 0x482e, 0x4e50, { 0xb4, 0x1f, 0x8a, 0x7f, 0x8b, 0xd8, 0x96, 0x0b } };
#endif
#ifndef IID_ID3D11Texture2D
static const GUID IID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
#endif
static const GUID gst_IID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
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

static bool caps_use_only_system_memory(const GstCapsFeatures *features)
{
	if (gst_caps_features_is_any(features))
		return false;

	for (guint i = 0; i < gst_caps_features_get_size(features); i++) {
		const char *feature = gst_caps_features_get_nth(features, i);
		if (g_str_has_prefix(feature, "memory:") &&
		    !g_str_equal(feature, GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY))
			return false;
	}
	return true;
}

static bool validate_custom_master_appsrc(gst_master_pipeline_t *hub)
{
	if (!hub || !hub->appsrc)
		return false;

	GstCaps *caps = gst_app_src_get_caps(GST_APP_SRC(hub->appsrc));
	if (!caps || gst_caps_is_any(caps) || gst_caps_is_empty(caps)) {
		if (caps) gst_caps_unref(caps);
		GstPad *srcpad = gst_element_get_static_pad(hub->appsrc, "src");
		caps = srcpad ? gst_pad_query_caps(srcpad, NULL) : NULL;
		if (srcpad) gst_object_unref(srcpad);
	}

	bool valid = false;
	if (!caps || !gst_caps_is_fixed(caps) || gst_caps_get_size(caps) != 1)
		goto done;

	const GstStructure *structure = gst_caps_get_structure(caps, 0);
	const char *media_type = gst_structure_get_name(structure);
	const char *format = gst_structure_get_string(structure, "format");
	gint width = 0;
	gint height = 0;
	if (!g_str_equal(media_type, "video/x-raw") || !format ||
	    !gst_structure_get_int(structure, "width", &width) ||
	    !gst_structure_get_int(structure, "height", &height) ||
	    width < 0 || height < 0 ||
	    (guint)width != hub->width || (guint)height != hub->height)
		goto done;

	GstCapsFeatures *features = gst_caps_get_features(caps, 0);
	GstVideoFormat gst_format = gst_video_format_from_string(format);
	if (hub->gpu_input) {
#if HAVE_GST_D3D11
		if (hub->d3d11_device &&
		    gst_caps_features_contains(features, "memory:D3D11Memory") &&
		    (gst_format == GST_VIDEO_FORMAT_RGBA || gst_format == GST_VIDEO_FORMAT_BGRA)) {
			hub->appsrc_format = gst_format;
			hub->custom_d3d11_memory = true;
			valid = true;
			goto done;
		}
#endif
#if HAVE_GST_GL
		if (hub->gl_app_context &&
		    gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_GL_MEMORY) &&
		    gst_format == GST_VIDEO_FORMAT_RGBA) {
			hub->appsrc_format = GST_VIDEO_FORMAT_RGBA;
			hub->gl_rgb_pipeline = true;
			valid = true;
			goto done;
		}
#endif
	} else if (caps_use_only_system_memory(features) &&
		   (gst_format == GST_VIDEO_FORMAT_RGBA ||
		    (hub->requested_raw_format != GST_VIDEO_FORMAT_UNKNOWN &&
		     gst_format == hub->requested_raw_format))) {
		hub->appsrc_format = gst_format;
		valid = true;
	}

done:
	if (caps) gst_caps_unref(caps);
	if (!valid)
		blog(LOG_ERROR, "[obs-gstreamer-master] Custom master appsrc caps must explicitly match %ux%u video/x-raw and the selected CPU/D3D11/GL input memory and format",
			hub->width, hub->height);
	return valid;
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

const char *gst_master_pipeline_select_encoder(const char *requested_encoder, const char *video_codec, bool gpu_input)
{
	log_gpu_and_encoder_capabilities_once();

	obs_enter_graphics();
	int dev_type = gs_get_device_type();
	const char *dev_name = gs_get_device_name();
	obs_leave_graphics();

	bool is_d3d11 = (dev_type == GS_DEVICE_DIRECT3D_11);
	bool is_h265 = video_codec && g_strcmp0(video_codec, "h265") == 0;

	/* 1. Handle explicit encoder selection with smart platform alias resolution */
	if (requested_encoder && requested_encoder[0] && g_strcmp0(requested_encoder, "auto") != 0)
	{
		if (gst_master_pipeline_element_available(requested_encoder))
			return requested_encoder;

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
			if (is_h265)
			{
				if (is_d3d11 && gst_master_pipeline_element_available("nvd3d11h265enc"))
					return "nvd3d11h265enc";
				if (gst_master_pipeline_element_available("nvh265enc"))
					return "nvh265enc";
			}
			else
			{
				if (is_d3d11 && gst_master_pipeline_element_available("nvd3d11h264enc"))
					return "nvd3d11h264enc";
				if (gst_master_pipeline_element_available("nvh264enc"))
					return "nvh264enc";
				if (gst_master_pipeline_element_available("nvcudah264enc"))
					return "nvcudah264enc";
			}
		}
		else if (g_strstr_len(dev_name, -1, "Intel") || g_strstr_len(dev_name, -1, "Arc") ||
		         g_strstr_len(dev_name, -1, "Iris") || g_strstr_len(dev_name, -1, "UHD"))
		{
			if (is_h265)
			{
				if (gst_master_pipeline_element_available("qsvh265enc"))
					return "qsvh265enc";
				if (!is_d3d11 && gst_master_pipeline_element_available("vah265enc"))
					return "vah265enc";
			}
			else
			{
				if (gst_master_pipeline_element_available("qsvh264enc"))
					return "qsvh264enc";
				if (!is_d3d11 && gst_master_pipeline_element_available("vah264enc"))
					return "vah264enc";
				if (!is_d3d11 && gst_master_pipeline_element_available("vaapih264enc"))
					return "vaapih264enc";
			}
		}
		else if (g_strstr_len(dev_name, -1, "AMD") || g_strstr_len(dev_name, -1, "Radeon"))
		{
			if (is_h265)
			{
				if (gst_master_pipeline_element_available("amfh265enc"))
					return "amfh265enc";
				if (!is_d3d11 && gst_master_pipeline_element_available("vah265enc"))
					return "vah265enc";
			}
			else
			{
				if (gst_master_pipeline_element_available("amfh264enc"))
					return "amfh264enc";
				if (!is_d3d11 && gst_master_pipeline_element_available("vah264enc"))
					return "vah264enc";
			}
		}
	}

	/* 3. Probe all hardware encoders in platform priority order */
	if (is_d3d11)
	{
		static const char *const win_h264_priority[] = {
			"nvd3d11h264enc", "nvh264enc", "qsvh264enc", "amfh264enc", "mfh264enc", "x264enc", NULL
		};
		static const char *const win_h265_priority[] = {
			"nvd3d11h265enc", "nvh265enc", "qsvh265enc", "amfh265enc", "mfh265enc", "x265enc", "x264enc", NULL
		};
		const char *const *win_priority = is_h265 ? win_h265_priority : win_h264_priority;
		for (int i = 0; win_priority[i] != NULL; i++)
		{
			if (gst_master_pipeline_element_available(win_priority[i]))
				return win_priority[i];
		}
	}
	else
	{
		static const char *const nix_h264_priority[] = {
			"nvh264enc", "nvcudah264enc", "vah264enc", "vaapih264enc", "qsvh264enc", "v4l2h264enc", "vtenc_h264_hw", "x264enc", NULL
		};
		static const char *const nix_h265_priority[] = {
			"nvh265enc", "vah265enc", "qsvh265enc", "x265enc", "x264enc", NULL
		};
		const char *const *nix_priority = is_h265 ? nix_h265_priority : nix_h264_priority;
		for (int i = 0; nix_priority[i] != NULL; i++)
		{
			if (gst_master_pipeline_element_available(nix_priority[i]))
				return nix_priority[i];
		}
	}

	return is_h265 ? "x265enc" : "x264enc";
}

static gchar *build_encoder_element_str(const char *enc_name, int gop_size, int bitrate_kbps)
{
	if (!enc_name || !enc_name[0]) enc_name = "x264enc";

	GString *str = g_string_new(enc_name);

	if (g_strcmp0(enc_name, "x264enc") == 0)
	{
		g_string_append_printf(str,
			" tune=zerolatency sliced-threads=false speed-preset=ultrafast bframes=0 key-int-max=%d bitrate=%d",
			gop_size, bitrate_kbps);
		return g_string_free(str, FALSE);
	}

	if (g_strcmp0(enc_name, "x265enc") == 0)
	{
		g_string_append_printf(str,
			" tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=%d bitrate=%d",
			gop_size, bitrate_kbps);
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

	if (element_has_property(enc_name, "rate-control"))
		g_string_append(str, " rate-control=cbr");
	else if (element_has_property(enc_name, "rc-mode"))
		g_string_append(str, " rc-mode=cbr");

	if (element_has_property(enc_name, "bframes"))
		g_string_append(str, " bframes=0");
	else if (element_has_property(enc_name, "b-frames"))
		g_string_append(str, " b-frames=0");

	if (element_has_property(enc_name, "rc-lookahead"))
		g_string_append(str, " rc-lookahead=0");

	if (element_has_property(enc_name, "low-latency"))
		g_string_append(str, " low-latency=true");

	if (element_has_property(enc_name, "zerolatency"))
		g_string_append(str, " zerolatency=true");

	if (element_has_property(enc_name, "target-usage"))
		g_string_append(str, " target-usage=6");

	/* AMF encoders already have b-frames=0 and rc-lookahead=0 set above if supported;
	 * avoid forcing usage=ultra-low-latency as older AMD APUs (VCE 1/2/3) return
	 * AMF_NOT_SUPPORTED and fail caps negotiation (-4). */

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

	if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
	{
		GError *err = NULL;
		gchar *debug_info = NULL;
		gst_message_parse_error(msg, &err, &debug_info);
		blog(LOG_ERROR, "[obs-gstreamer-master] Pipeline error from '%s': %s (debug: %s)",
			GST_ELEMENT_NAME(GST_MESSAGE_SRC(msg)),
			err ? err->message : "unknown",
			debug_info ? debug_info : "none");
		if (err) g_error_free(err);
		g_free(debug_info);
		g_atomic_int_set(&hub->pipeline_failed, 1);
#if HAVE_GST_D3D11
		g_atomic_int_set(&hub->d3d11_slot.in_use, 0);
#endif
#if HAVE_GST_GL
		g_atomic_int_set(&hub->gl_slot.in_use, 0);
#endif
	}
	else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING)
	{
		GError *err = NULL;
		gchar *debug_info = NULL;
		gst_message_parse_warning(msg, &err, &debug_info);
		blog(LOG_WARNING, "[obs-gstreamer-master] Pipeline warning from '%s': %s (debug: %s)",
			GST_ELEMENT_NAME(GST_MESSAGE_SRC(msg)),
			err ? err->message : "unknown",
			debug_info ? debug_info : "none");
		if (err) g_error_free(err);
		g_free(debug_info);
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
		ID3D11DeviceContext *ctx = NULL;
		ID3D11Device_GetImmediateContext(device, &ctx);
		if (ctx)
		{
			ID3D10Multithread *multithread = NULL;
			HRESULT hr = ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11Multithread, (void **)&multithread);
			if (FAILED(hr) || !multithread)
				hr = ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D10Multithread, (void **)&multithread);
			if (FAILED(hr) || !multithread)
				hr = ID3D11Device_QueryInterface(device, &IID_ID3D11Multithread, (void **)&multithread);
			if (FAILED(hr) || !multithread)
				hr = ID3D11Device_QueryInterface(device, &IID_ID3D10Multithread, (void **)&multithread);
			if (SUCCEEDED(hr) && multithread)
			{
				ID3D10Multithread_SetMultithreadProtected(multithread, TRUE);
				ID3D10Multithread_Release(multithread);
				blog(LOG_INFO, "[obs-gstreamer-master] Enabled Direct3D 11 multithread protection on OBS device/context");
			}
			else
			{
				blog(LOG_WARNING, "[obs-gstreamer-master] Failed to query ID3D10/11Multithread on device/context: 0x%08lx", (unsigned long)hr);
			}
			ID3D11DeviceContext_Release(ctx);
		}
#ifdef _WIN32
		/* Query physical DXGI adapter from OBS device to run GStreamer on its own isolated D3D11 context */
		IDXGIDevice *dxgi_dev = NULL;
		IDXGIAdapter *adapter = NULL;
		HRESULT hr_ad = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_dev);
		if (SUCCEEDED(hr_ad) && dxgi_dev)
		{
			IDXGIDevice_GetAdapter(dxgi_dev, &adapter);
			IDXGIDevice_Release(dxgi_dev);
		}

		HMODULE hD3D11 = GetModuleHandleW(L"d3d11.dll");
		if (!hD3D11) hD3D11 = LoadLibraryW(L"d3d11.dll");
		PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice = hD3D11
			? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice")
			: NULL;

		ID3D11Device *gst_native_dev = NULL;
		ID3D11DeviceContext *gst_native_ctx = NULL;
		if (pD3D11CreateDevice && adapter)
		{
			HRESULT hr_create = pD3D11CreateDevice(
				adapter,
				D3D_DRIVER_TYPE_UNKNOWN,
				NULL,
				D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
				NULL, 0,
				D3D11_SDK_VERSION,
				&gst_native_dev,
				NULL,
				&gst_native_ctx);
			if (SUCCEEDED(hr_create) && gst_native_dev)
			{
				ID3D10Multithread *gst_mt = NULL;
				if (gst_native_ctx && (SUCCEEDED(ID3D11DeviceContext_QueryInterface(gst_native_ctx, &IID_ID3D11Multithread, (void **)&gst_mt)) ||
				                       SUCCEEDED(ID3D11DeviceContext_QueryInterface(gst_native_ctx, &IID_ID3D10Multithread, (void **)&gst_mt))))
				{
					ID3D10Multithread_SetMultithreadProtected(gst_mt, TRUE);
					ID3D10Multithread_Release(gst_mt);
				}
				if (gst_native_ctx) ID3D11DeviceContext_Release(gst_native_ctx);
				hub->d3d11_native_device = gst_native_dev;
				hub->d3d11_device = gst_d3d11_device_new_wrapped(gst_native_dev);
				blog(LOG_INFO, "[obs-gstreamer-master] Created dedicated GStreamer Direct3D 11 device on OBS GPU adapter (full context isolation)");
			}
			else
			{
				blog(LOG_WARNING, "[obs-gstreamer-master] D3D11CreateDevice for dedicated device returned 0x%08lx; falling back to wrapped OBS device", (unsigned long)hr_create);
			}
			IDXGIAdapter_Release(adapter);
		}
#endif
		if (!hub->d3d11_device)
		{
			hub->d3d11_device = gst_d3d11_device_new_wrapped(device);
			blog(LOG_INFO, "[obs-gstreamer-master] Wrapped OBS ID3D11Device for zero-copy D3D11Memory pipeline");
		}
	}
	obs_leave_graphics();

	if (!hub->d3d11_device)
	{
		blog(LOG_WARNING, "[obs-gstreamer-master] Unable to wrap OBS D3D11Device for zero-copy");
		return false;
	}

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

	blog(LOG_INFO, "[obs-gstreamer-master] Wrapped OBS OpenGL context (handle=%p, shared_ctx=%p, platform=%d, api=%d) for GstGLMemory pipeline",
		(void *)gl_handle, (void *)hub->gl_context, (int)platform, (int)gl_api);
	return true;
}
#endif

#if HAVE_GST_D3D11
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
#endif

static void init_hub_colorimetry(gst_master_pipeline_t *hub)
{
	struct obs_video_info ovi;
	enum video_colorspace cs = VIDEO_CS_709;
	enum video_range_type range = VIDEO_RANGE_PARTIAL;
	if (obs_get_video_info(&ovi))
	{
		cs = ovi.colorspace;
		range = ovi.range;
	}

	GstVideoColorimetry colorimetry = {0};
	colorimetry.range = (range == VIDEO_RANGE_FULL)
		? GST_VIDEO_COLOR_RANGE_0_255
		: GST_VIDEO_COLOR_RANGE_16_235;
	if (cs == VIDEO_CS_601)
	{
		colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT601;
		colorimetry.transfer = GST_VIDEO_TRANSFER_BT601;
		colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTE170M;
	}
	else if (cs == VIDEO_CS_2100_PQ)
	{
		colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT2020;
		colorimetry.transfer = GST_VIDEO_TRANSFER_SMPTE2084;
		colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_BT2020;
	}
	else if (cs == VIDEO_CS_2100_HLG)
	{
		colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT2020;
		colorimetry.transfer = GST_VIDEO_TRANSFER_ARIB_STD_B67;
		colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_BT2020;
	}
	else if (cs == VIDEO_CS_SRGB)
	{
		colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
		colorimetry.transfer = GST_VIDEO_TRANSFER_SRGB;
		colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_BT709;
	}
	else
	{
		colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
		colorimetry.transfer = GST_VIDEO_TRANSFER_BT709;
		colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_BT709;
	}

	g_free(hub->colorimetry_str);
	hub->colorimetry_str = gst_video_colorimetry_to_string(&colorimetry);
	if (!hub->colorimetry_str)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to serialize OBS video colorimetry");
		return;
	}
}

static GstElement *create_master_encoder_pipeline(gst_master_pipeline_t *hub, const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec)
{
	gchar *pipe_desc = NULL;
#if HAVE_GST_GL
	hub->gl_rgb_pipeline = false;
#endif
	init_hub_colorimetry(hub);
	if (!hub->colorimetry_str)
		return NULL;

	if (custom_master_pipe && custom_master_pipe[0])
	{
#if HAVE_GST_GL
		hub->gl_rgb_pipeline = hub->gpu_input && hub->gl_app_context &&
			strstr(custom_master_pipe, "memory:GLMemory") != NULL &&
			strstr(custom_master_pipe, "glcolorconvert") != NULL &&
			strstr(custom_master_pipe, "format=RGBA") != NULL;
		if (hub->gl_rgb_pipeline)
			hub->appsrc_format = GST_VIDEO_FORMAT_RGBA;
#endif
		if (!hub->gpu_input)
			hub->appsrc_format = (hub->requested_raw_format != GST_VIDEO_FORMAT_UNKNOWN)
				? hub->requested_raw_format
				: GST_VIDEO_FORMAT_RGBA;
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
			: gst_master_pipeline_select_encoder(encoder_type, hub->video_codec, hub->gpu_input);
		gchar *enc_str = build_encoder_element_str(chosen_enc, gop_size, bitrate);

		bool is_h265 = hub->video_codec && g_strcmp0(hub->video_codec, "h265") == 0;
		const char *parse_elem = is_h265 ? "h265parse config-interval=-1" : "h264parse config-interval=-1";
		const char *caps_elem = is_h265 ? "video/x-h265" : "video/x-h264";

#if HAVE_GST_D3D11
		if (hub->gpu_input && hub->d3d11_device && is_d3d11_memory_encoder(chosen_enc))
		{
			GstVideoFormat obs_fmt = get_obs_compositing_format();
			hub->appsrc_format = (obs_fmt == GST_VIDEO_FORMAT_RGBA) ? GST_VIDEO_FORMAT_RGBA : GST_VIDEO_FORMAT_BGRA;
			pipe_desc = g_strdup_printf(
				"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=false min-latency=0 max-latency=0 "
				"caps=\"video/x-raw(memory:D3D11Memory), width=%u, height=%u, "
				"format=%s, framerate=%u/%u\" ! "
				"d3d11convert ! video/x-raw(memory:D3D11Memory), format=NV12 ! "
				"%s ! %s ! "
				"%s, stream-format=byte-stream, alignment=au ! "
				"appsink name=hub_appsink sync=false drop=false max-buffers=4",
				hub->width, hub->height,
				gst_video_format_to_string(hub->appsrc_format),
				hub->fps_num, hub->fps_den,
				enc_str, parse_elem, caps_elem);
			g_free(enc_str);
			goto launch_pipeline;
		}
#endif

#if HAVE_GST_GL
		if (hub->gpu_input && hub->gl_app_context && hub->gl_context &&
		    is_gl_memory_encoder(chosen_enc) &&
		    gst_master_pipeline_element_available("glcolorconvert"))
		{
			hub->appsrc_format = GST_VIDEO_FORMAT_RGBA;
			hub->gl_rgb_pipeline = true;
			pipe_desc = g_strdup_printf(
				"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=false min-latency=0 max-latency=0 "
				"caps=\"video/x-raw(memory:GLMemory), width=%u, height=%u, "
				"format=RGBA, framerate=%u/%u, texture-target=2D, colorimetry=%s\" ! "
				"glcolorconvert ! video/x-raw(memory:GLMemory), format=NV12, texture-target=2D, colorimetry=%s ! "
				"%s ! %s ! "
				"%s, stream-format=byte-stream, alignment=au ! "
				"appsink name=hub_appsink sync=false drop=false max-buffers=4",
				hub->width, hub->height, hub->fps_num, hub->fps_den, hub->colorimetry_str,
				hub->colorimetry_str, enc_str, parse_elem, caps_elem);
			g_free(enc_str);
			goto launch_pipeline;
		}
#endif

		hub->appsrc_format = (hub->requested_raw_format != GST_VIDEO_FORMAT_UNKNOWN)
			? hub->requested_raw_format
			: GST_VIDEO_FORMAT_RGBA;
		if (hub->appsrc_format == GST_VIDEO_FORMAT_NV12)
		{
			pipe_desc = g_strdup_printf(
				"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=false min-latency=0 max-latency=0 "
				"max-buffers=1 leaky-type=downstream "
				"caps=\"video/x-raw, width=%u, height=%u, format=NV12, framerate=%u/%u, colorimetry=%s\" ! "
				"%s ! %s ! %s, stream-format=byte-stream, alignment=au ! "
				"appsink name=hub_appsink sync=false drop=false max-buffers=4",
				hub->width, hub->height, hub->fps_num, hub->fps_den, hub->colorimetry_str,
				enc_str, parse_elem, caps_elem);
		}
		else
		{
			pipe_desc = g_strdup_printf(
				"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=false min-latency=0 max-latency=0 "
				"max-buffers=1 leaky-type=downstream "
				"caps=\"video/x-raw, width=%u, height=%u, format=%s, framerate=%u/%u, colorimetry=%s\" ! "
				"videoconvert ! video/x-raw, format=NV12, colorimetry=%s ! "
				"%s ! %s ! %s, stream-format=byte-stream, alignment=au ! "
				"appsink name=hub_appsink sync=false drop=false max-buffers=4",
				hub->width, hub->height, gst_video_format_to_string(hub->appsrc_format),
				hub->fps_num, hub->fps_den, hub->colorimetry_str, hub->colorimetry_str,
				enc_str, parse_elem, caps_elem);
		}

		g_free(enc_str);
	}

#if HAVE_GST_D3D11 || HAVE_GST_GL
launch_pipeline:
#endif
	blog(LOG_INFO, "[obs-gstreamer-master] Launching Master Hardware Encoder Pipeline (gpu_input=%s, encoder=%s, appsrc_format=%s): %s",
		hub->gpu_input ? "yes" : "no",
		hub->selected_encoder ? hub->selected_encoder : "custom",
		gst_video_format_to_string(hub->appsrc_format),
		pipe_desc);
	GError *error = NULL;
	GstElement *pipe = gstreamer_parse_launch_checked(pipe_desc, &error);
	g_free(pipe_desc);

	if (error)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to create master pipeline: %s", error->message);
		g_clear_error(&error);
		if (pipe) gst_object_unref(pipe);
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

			GstContext *ctx2 = gst_context_new("gst.d3d11.device", TRUE);
			GstStructure *s2 = gst_context_writable_structure(ctx2);
			gst_structure_set(s2, "device", GST_TYPE_D3D11_DEVICE, hub->d3d11_device, NULL);
			gst_element_set_context(pipe, ctx2);
			gst_context_unref(ctx2);
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
			" bytes, keyframe=%s (gpu_input=%s, stagesurf=%s)",
			sample_number, gst_buffer_get_size(buf), is_keyframe ? "yes" : "no",
			hub->gpu_input ? "yes" : "no",
			hub->stagesurf ? "allocated" : "NONE");
	}

	g_mutex_lock(&hub->subscriber_mutex);
	GPtrArray *subs = hub->subscribers;
	if (subs)
	{
		for (guint i = 0; i < subs->len; i++)
		{
			gst_pipeline_subscriber_t *sub = g_ptr_array_index(subs, i);
			if (!sub || !sub->cb) continue;
			if (sub->waiting_for_keyframe)
			{
				if (!is_keyframe) continue;
				sub->waiting_for_keyframe = false;
				blog(LOG_INFO, "[obs-gstreamer-master] Subscriber '%s' received its first keyframe",
					sub->name ? sub->name : "Subscriber");
			}
			sub->cb(buf, is_keyframe, sub->user_data);
		}
	}
	g_mutex_unlock(&hub->subscriber_mutex);

	gst_sample_unref(sample);
	return GST_FLOW_OK;
}

#if HAVE_GST_D3D11
static void d3d11_texture_slot_release_notify(gpointer user_data)
{
	gst_d3d11_texture_slot_t *slot = (gst_d3d11_texture_slot_t *)user_data;
	if (slot)
	{
		g_atomic_int_set(&slot->in_use, 0);
	}
}

static void destroy_d3d11_texture_slot(gst_master_pipeline_t *hub)
{
	if (!hub) return;
	gst_d3d11_texture_slot_t *slot = &hub->d3d11_slot;
	if (slot->gst_texture && slot->gst_texture != slot->texture)
	{
		ID3D11Texture2D_Release(slot->gst_texture);
		slot->gst_texture = NULL;
	}
	if (slot->texture)
	{
		ID3D11Texture2D_Release(slot->texture);
		slot->texture = NULL;
	}
	slot->shared_handle = NULL;
	g_atomic_int_set(&slot->in_use, 0);
	hub->d3d11_texture_ready = false;
}

static bool init_d3d11_texture_slot(gst_master_pipeline_t *hub)
{
	if (hub->d3d11_texture_ready) return true;
	if (!hub->d3d11_device && !init_obs_d3d11_device(hub)) return false;

	obs_enter_graphics();
	ID3D11Device *obs_device = (gs_get_device_type() == GS_DEVICE_DIRECT3D_11)
		? (ID3D11Device *)gs_get_device_obj()
		: NULL;
	obs_leave_graphics();
	if (!obs_device) return false;

	DXGI_FORMAT tex_dxgi_format = (hub->appsrc_format == GST_VIDEO_FORMAT_RGBA)
		? DXGI_FORMAT_R8G8B8A8_UNORM
		: DXGI_FORMAT_B8G8R8A8_UNORM;

	gst_d3d11_texture_slot_t *slot = &hub->d3d11_slot;
	D3D11_TEXTURE2D_DESC tex_desc = {0};
	tex_desc.Width = hub->width;
	tex_desc.Height = hub->height;
	tex_desc.MipLevels = 1;
	tex_desc.ArraySize = 1;
	tex_desc.Format = tex_dxgi_format;
	tex_desc.SampleDesc.Count = 1;
	tex_desc.Usage = D3D11_USAGE_DEFAULT;
	tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (hub->d3d11_native_device)
		tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	HRESULT hr = ID3D11Device_CreateTexture2D(obs_device, &tex_desc, NULL, &slot->texture);
	if (FAILED(hr))
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to create D3D11 texture slot (format=%d): 0x%08lx",
			(int)tex_dxgi_format, (unsigned long)hr);
		return false;
	}

	if (hub->d3d11_native_device)
	{
		IDXGIResource *dxgi_res = NULL;
		HRESULT hr_res = ID3D11Texture2D_QueryInterface(slot->texture, &IID_IDXGIResource, (void **)&dxgi_res);
		if (SUCCEEDED(hr_res) && dxgi_res)
		{
			IDXGIResource_GetSharedHandle(dxgi_res, &slot->shared_handle);
			IDXGIResource_Release(dxgi_res);
		}
		if (slot->shared_handle)
		{
			hr = ID3D11Device_OpenSharedResource(hub->d3d11_native_device, slot->shared_handle,
				&gst_IID_ID3D11Texture2D, (void **)&slot->gst_texture);
			if (FAILED(hr))
			{
				blog(LOG_ERROR, "[obs-gstreamer-master] Failed to open shared texture slot on GStreamer device: 0x%08lx",
					(unsigned long)hr);
				return false;
			}
		}
	}
	else
	{
		slot->gst_texture = slot->texture;
		ID3D11Texture2D_AddRef(slot->gst_texture);
	}

	g_atomic_int_set(&slot->in_use, 0);

	hub->d3d11_texture_ready = true;
	blog(LOG_INFO, "[obs-gstreamer-master] Initialized D3D11 zero-copy direct texture slot (appsrc_format=%s)",
		gst_video_format_to_string(hub->appsrc_format));
	return true;
}

static bool push_d3d11_texture(gst_master_pipeline_t *hub, gs_texture_t *texture, uint64_t pts)
{
	if (g_atomic_int_get(&hub->pipeline_failed))
		return false;

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

	if (!hub->d3d11_texture_ready && !init_d3d11_texture_slot(hub))
		return false;

	gst_d3d11_texture_slot_t *slot = &hub->d3d11_slot;

	/* Strict Direct Processing: If encoder is busy, drop frame immediately */
	if (g_atomic_int_get(&slot->in_use) != 0)
	{
		hub->texture_drop_count++;
		uint64_t drops = hub->texture_drop_count;
		if (drops == 1 || drops % 120 == 0)
		{
			blog(LOG_WARNING, "[obs-gstreamer-master] Encoder busy: dropped D3D11 frame #%" PRIu64 " (slot in use, total drops: %" PRIu64 ")",
				hub->texture_push_count + 1, drops);
		}
		return true;
	}

	g_atomic_int_set(&slot->in_use, 1);

	ID3D11Texture2D *d3d_texture = (ID3D11Texture2D *)gs_texture_get_obj(texture);
	if (!d3d_texture)
	{
		g_atomic_int_set(&slot->in_use, 0);
		blog(LOG_ERROR, "[obs-gstreamer-master] OBS texture has no native D3D11 resource");
		return false;
	}

	ID3D11Device *obs_dev = (ID3D11Device *)gs_get_device_obj();
	ID3D11DeviceContext *obs_ctx = NULL;
	if (obs_dev) ID3D11Device_GetImmediateContext(obs_dev, &obs_ctx);
	if (obs_ctx)
	{
		ID3D11DeviceContext_CopyResource(obs_ctx, (ID3D11Resource *)slot->texture, (ID3D11Resource *)d3d_texture);
		if (hub->d3d11_native_device)
		{
			ID3D11DeviceContext_Flush(obs_ctx);
		}
		ID3D11DeviceContext_Release(obs_ctx);
	}
	else
	{
		g_atomic_int_set(&slot->in_use, 0);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to get OBS immediate context for D3D11 CopyResource");
		return false;
	}

	GstVideoInfo info;
	if (!gst_video_info_set_format(&info, hub->appsrc_format, width, height))
	{
		g_atomic_int_set(&slot->in_use, 0);
		return false;
	}

	gst_d3d11_memory_init_once();
	GstAllocator *allocator = gst_allocator_find(GST_D3D11_MEMORY_NAME);
	if (!allocator)
	{
		g_atomic_int_set(&slot->in_use, 0);
		blog(LOG_ERROR, "[obs-gstreamer-master] GStreamer D3D11 allocator is unavailable");
		return false;
	}

	ID3D11Texture2D *push_tex = (slot->gst_texture) ? slot->gst_texture : slot->texture;
	ID3D11Texture2D_AddRef(push_tex);
	GstMemory *memory = gst_d3d11_allocator_alloc_wrapped(
		GST_D3D11_ALLOCATOR(allocator), hub->d3d11_device, (ID3D11Resource *)push_tex,
		GST_VIDEO_INFO_SIZE(&info), slot, d3d11_texture_slot_release_notify);
	gst_object_unref(allocator);
	if (!memory)
	{
		ID3D11Texture2D_Release(push_tex);
		g_atomic_int_set(&slot->in_use, 0);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to wrap D3D11 texture slot as GstD3D11Memory");
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
	if (!gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, hub->appsrc_format, width, height,
		GST_VIDEO_INFO_N_PLANES(&info), offsets, strides))
	{
		gst_buffer_unref(buffer);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to attach video metadata to D3D11 frame");
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
		blog(LOG_INFO, "[obs-gstreamer-master] D3D11 zero-copy frame #%" PRIu64
			" pushed (%ux%u, format=%s, drops=%" PRIu64 ")",
			frame_number, width, height,
			gst_video_format_to_string(hub->appsrc_format),
			hub->texture_drop_count);
	}
	return true;
}
#endif

#if HAVE_GST_GL
static void gl_texture_release_notify(gpointer user_data)
{
	gst_gl_texture_slot_t *slot = (gst_gl_texture_slot_t *)user_data;
	if (slot) g_atomic_int_set(&slot->in_use, 0);
}

static void destroy_gl_texture_slot(gst_master_pipeline_t *hub)
{
	if (!hub) return;
	if (hub->gl_context)
	{
		GstGLFuncs *gl = hub->gl_context->gl_vtable;
		gst_gl_texture_slot_t *slot = &hub->gl_slot;
		if (slot->read_fbo) { gl->DeleteFramebuffers(1, &slot->read_fbo); slot->read_fbo = 0; }
		if (slot->draw_fbo) { gl->DeleteFramebuffers(1, &slot->draw_fbo); slot->draw_fbo = 0; }
		if (slot->texture) { gl->DeleteTextures(1, &slot->texture); slot->texture = 0; }
		slot->read_attachment_texture = 0;
		g_atomic_int_set(&slot->in_use, 0);
	}
	hub->gl_texture_ready = false;
}

static bool init_gl_texture_slot(gst_master_pipeline_t *hub)
{
	if (hub->gl_texture_ready) return true;
	if ((!hub->gl_app_context || !hub->gl_context || !hub->gl_display) && !init_obs_gl_context(hub))
		return false;

	GstGLFuncs *gl = hub->gl_context->gl_vtable;
	if (!gl) return false;

	gst_gl_texture_slot_t *slot = &hub->gl_slot;
	GLint previous_texture = 0;
	GLint previous_read_fbo = 0;
	GLint previous_draw_fbo = 0;
	gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
	gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_fbo);
	gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_fbo);
	gl->GenTextures(1, &slot->texture);
	gl->BindTexture(GL_TEXTURE_2D, slot->texture);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, hub->width, hub->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	gl->GenFramebuffers(1, &slot->read_fbo);
	gl->GenFramebuffers(1, &slot->draw_fbo);
	if (slot->draw_fbo && slot->texture) {
		gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, slot->draw_fbo);
		gl->FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, slot->texture, 0);
	}
	GLenum draw_status = slot->draw_fbo
		? gl->CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)
		: 0;
	gl->BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read_fbo);
	gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_draw_fbo);
	gl->BindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);

	if (!slot->texture || !slot->read_fbo || !slot->draw_fbo ||
	    draw_status != GL_FRAMEBUFFER_COMPLETE || gl->GetError() != GL_NO_ERROR)
	{
		destroy_gl_texture_slot(hub);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to allocate OpenGL RGB texture slot");
		return false;
	}

	g_atomic_int_set(&slot->in_use, 0);
	hub->gl_texture_ready = true;
	blog(LOG_INFO, "[obs-gstreamer-master] Initialized OpenGL RGB texture slot for GstGLMemory");
	return true;
}

static bool push_gl_texture(gst_master_pipeline_t *hub, gs_texture_t *texture, uint64_t pts)
{
	if (gs_get_device_type() != GS_DEVICE_OPENGL)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] OpenGL texture input requires the OBS OpenGL graphics backend");
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

	if (!hub->gl_texture_ready && !init_gl_texture_slot(hub))
		return false;

	gst_gl_texture_slot_t *slot = &hub->gl_slot;

	/* Strict Direct Processing: If encoder is busy, drop frame immediately */
	if (g_atomic_int_get(&slot->in_use) != 0)
	{
		hub->texture_drop_count++;
		uint64_t drops = hub->texture_drop_count;
		if (drops == 1 || drops % 120 == 0)
		{
			blog(LOG_WARNING, "[obs-gstreamer-master] Encoder busy: dropped OpenGL frame #%" PRIu64 " (slot in use, total drops: %" PRIu64 ")",
				hub->texture_push_count + 1, drops);
		}
		return true;
	}

	g_atomic_int_set(&slot->in_use, 1);

	const guint *tex_ptr = (const guint *)gs_texture_get_obj(texture);
	if (!tex_ptr || *tex_ptr == 0)
	{
		g_atomic_int_set(&slot->in_use, 0);
		blog(LOG_ERROR, "[obs-gstreamer-master] OBS texture has no valid OpenGL texture ID");
		return false;
	}
	guint tex_id = *tex_ptr;

	gs_flush();

	GstGLFuncs *gl = hub->gl_context->gl_vtable;
	GLint previous_read_fbo = 0;
	GLint previous_draw_fbo = 0;
	gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_fbo);
	gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_fbo);
	gl->BindFramebuffer(GL_READ_FRAMEBUFFER, slot->read_fbo);
	if (slot->read_attachment_texture != tex_id) {
		gl->FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, tex_id, 0);
		if (gl->CheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			gl->BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read_fbo);
			gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_draw_fbo);
			g_atomic_int_set(&slot->in_use, 0);
			blog(LOG_ERROR, "[obs-gstreamer-master] OpenGL source texture framebuffer is incomplete");
			return false;
		}
		slot->read_attachment_texture = tex_id;
	}
	gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, slot->draw_fbo);
	gl->BlitFramebuffer(0, (GLint)height, (GLint)width, 0,
		0, 0, (GLint)width, (GLint)height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	GLenum copy_error = gl->GetError();
	gl->BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read_fbo);
	gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_draw_fbo);
	if (copy_error != GL_NO_ERROR)
	{
		g_atomic_int_set(&slot->in_use, 0);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to copy OBS texture into the owned OpenGL slot: 0x%04x",
			(unsigned int)copy_error);
		return false;
	}
	gl->Flush();

	GstVideoInfo info;
	if (!gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, width, height))
	{
		g_atomic_int_set(&slot->in_use, 0);
		return false;
	}

	gst_gl_memory_init_once();
	GstGLMemoryAllocator *allocator = gst_gl_memory_allocator_get_default(hub->gl_context);
	if (!allocator)
	{
		g_atomic_int_set(&slot->in_use, 0);
		blog(LOG_ERROR, "[obs-gstreamer-master] GStreamer GLMemory allocator is unavailable");
		return false;
	}

	GstGLVideoAllocationParams *params = gst_gl_video_allocation_params_new_wrapped_texture(
		hub->gl_context, NULL, &info, 0, NULL,
		GST_GL_TEXTURE_TARGET_2D, GST_GL_RGBA8, slot->texture, slot, gl_texture_release_notify);
	GstGLBaseMemory *gl_mem = gst_gl_base_memory_alloc(
		GST_GL_BASE_MEMORY_ALLOCATOR(allocator), (GstGLAllocationParams *)params);
	gst_gl_allocation_params_free((GstGLAllocationParams *)params);
	gst_object_unref(allocator);

	if (!gl_mem)
	{
		g_atomic_int_set(&slot->in_use, 0);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to wrap copied OpenGL texture as GstGLMemory");
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
	offsets[0] = GST_VIDEO_INFO_PLANE_OFFSET(&info, 0);
	strides[0] = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
	if (!gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_RGBA, width, height,
		1, offsets, strides))
	{
		gst_buffer_unref(buffer);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to attach RGBA video metadata to OpenGL frame");
		return false;
	}
	GstGLSyncMeta *sync_meta = gst_buffer_add_gl_sync_meta(hub->gl_context, buffer);
	if (!sync_meta)
	{
		gst_buffer_unref(buffer);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to add OpenGL synchronization metadata");
		return false;
	}
	gst_gl_sync_meta_set_sync_point(sync_meta, hub->gl_context);

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(buffer) = pts;
	GST_BUFFER_DTS(buffer) = pts;
	GST_BUFFER_DURATION(buffer) = duration;
	hub->frame_index++;

	GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), buffer);
	if (flow != GST_FLOW_OK)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] OpenGL RGB texture push failed: flow=%s",
			gst_flow_get_name(flow));
		return false;
	}

	uint64_t frame_number = ++hub->texture_push_count;
	if (frame_number == 1 || frame_number % 300 == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] OpenGL RGB texture frame #%" PRIu64
			" copied and pushed for GStreamer conversion (%ux%u, drops=%" PRIu64 ")",
			frame_number, width, height, hub->texture_drop_count);
	}
	return true;
}
#endif

void gst_master_pipeline_push_texture(gst_master_pipeline_t *hub, void *gpu_handle, uint64_t pts)
{
	if (!hub || !gpu_handle || g_atomic_int_get(&hub->active_destinations) <= 0 || !hub->appsrc) return;

	gs_texture_t *tex = (gs_texture_t *)gpu_handle;
#if HAVE_GST_D3D11
	if (hub->gpu_input && hub->d3d11_device &&
	    (is_d3d11_memory_encoder(hub->selected_encoder) || hub->custom_d3d11_memory))
	{
		push_d3d11_texture(hub, tex, pts);
		return;
	}
#endif
#if HAVE_GST_GL
	if (hub->gpu_input && hub->gl_app_context && hub->gl_rgb_pipeline)
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
		static uint64_t last_map_fail_log = 0;
		uint64_t now_fail = os_gettime_ns();
		if (now_fail - last_map_fail_log > 5000000000ULL)
		{
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to map %ux%u staging surface (device reset?); releasing surface",
				tex_w, tex_h);
			last_map_fail_log = now_fail;
		}
		gs_stagesurface_destroy(hub->stagesurf);
		hub->stagesurf = NULL;
		return;
	}

	if (fmt != GS_RGBA && fmt != GS_RGBA_UNORM && fmt != GS_BGRA &&
	    fmt != GS_BGRA_UNORM && fmt != GS_BGRX && fmt != GS_BGRX_UNORM)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Unsupported CPU staging texture format=%d for RGBA input",
			(int)fmt);
		gs_stagesurface_unmap(hub->stagesurf);
		return;
	}

	if ((size_t)tex_w > SIZE_MAX / 4 ||
	    (size_t)src_linesize < (size_t)tex_w * 4 ||
	    hub->height == 0 || hub->width > SIZE_MAX / 4 ||
	    (size_t)hub->width * 4 > SIZE_MAX / hub->height ||
	    (size_t)hub->width * 4 > G_MAXINT)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Invalid stride or RGBA frame size overflow in CPU staging path");
		gs_stagesurface_unmap(hub->stagesurf);
		return;
	}

	uint32_t copy_w = tex_w < hub->width ? tex_w : hub->width;
	uint32_t copy_h = tex_h < hub->height ? tex_h : hub->height;
	size_t row_bytes = (size_t)hub->width * 4;
	size_t total_size = row_bytes * hub->height;
	GstBuffer *rgba_buf = gst_buffer_new_allocate(NULL, total_size, NULL);
	GstMapInfo rgba_map;
	if (!rgba_buf || !gst_buffer_map(rgba_buf, &rgba_map, GST_MAP_WRITE))
	{
		if (rgba_buf) gst_buffer_unref(rgba_buf);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to allocate or map RGBA staging buffer");
		gs_stagesurface_unmap(hub->stagesurf);
		return;
	}

	bool swap_rb = fmt == GS_BGRA || fmt == GS_BGRA_UNORM || fmt == GS_BGRX || fmt == GS_BGRX_UNORM;
	bool force_opaque_alpha = fmt == GS_BGRX || fmt == GS_BGRX_UNORM;
	memset(rgba_map.data, 0, total_size);
	for (uint32_t y = 0; y < copy_h; y++)
	{
		const uint8_t *src_row = src_data + (size_t)y * src_linesize;
		uint8_t *dst_row = rgba_map.data + (size_t)y * row_bytes;
		for (uint32_t x = 0; x < copy_w; x++)
		{
			size_t pixel_offset = (size_t)x * 4;
			dst_row[pixel_offset + 0] = src_row[pixel_offset + (swap_rb ? 2 : 0)];
			dst_row[pixel_offset + 1] = src_row[pixel_offset + 1];
			dst_row[pixel_offset + 2] = src_row[pixel_offset + (swap_rb ? 0 : 2)];
			dst_row[pixel_offset + 3] = force_opaque_alpha ? 255 : src_row[pixel_offset + 3];
		}
	}
	gst_buffer_unmap(rgba_buf, &rgba_map);

	gsize rgba_offsets[GST_VIDEO_MAX_PLANES] = {0};
	gint rgba_strides[GST_VIDEO_MAX_PLANES] = {(gint)row_bytes};
	if (!gst_buffer_add_video_meta_full(rgba_buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_RGBA,
		hub->width, hub->height, 1, rgba_offsets, rgba_strides))
	{
		gst_buffer_unref(rgba_buf);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to attach RGBA video metadata to staging frame");
		gs_stagesurface_unmap(hub->stagesurf);
		return;
	}

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(rgba_buf) = pts > 0 ? pts : hub->frame_index * duration;
	GST_BUFFER_DTS(rgba_buf) = GST_BUFFER_PTS(rgba_buf);
	GST_BUFFER_DURATION(rgba_buf) = duration;
	hub->frame_index++;

	GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), rgba_buf);
	if (flow != GST_FLOW_OK)
		blog(LOG_ERROR, "[obs-gstreamer-master] RGBA staging frame push failed: flow=%s",
			gst_flow_get_name(flow));
	else if (frame_number == 1 || frame_number % 300 == 0)
		blog(LOG_INFO, "[obs-gstreamer-master] Texture frame #%" PRIu64
			" pushed as RGBA (%ux%u texture, %ux%u submitted)",
			frame_number, tex_w, tex_h, copy_w, copy_h);

	gs_stagesurface_unmap(hub->stagesurf);
}

void gst_master_pipeline_push_raw_video(gst_master_pipeline_t *hub, struct video_data *frame)
{
	if (!hub || !frame || !frame->data[0] || g_atomic_int_get(&hub->active_destinations) <= 0 || !hub->appsrc) return;
	if (g_atomic_int_get(&hub->pipeline_failed)) return;

	if (hub->gpu_input)
	{
		blog(LOG_ERROR, "[obs-gstreamer-master] Invalid CPU frame push on GPU pipeline");
		return;
	}

	gstreamer_video_frame_cache_t *cache = (gstreamer_video_frame_cache_t *)hub->cpu_frame_cache;
	if (!cache) return;

	const bool is_first_frame = (cache->mode == GSTREAMER_FRAME_MODE_UNSET);
	uint64_t t_start = is_first_frame ? os_gettime_ns() : 0;

	bool slot_busy = false;
	GstBuffer *buf = gstreamer_create_obs_video_buffer(
		&hub->raw_video_info,
		cache,
		(gstreamer_cpu_frame_slot_t *)hub->cpu_frame_slot,
		frame,
		&slot_busy);
	if (!buf)
	{
		if (slot_busy)
		{
			uint64_t dropped = ++hub->cpu_frame_drop_count;
			if (dropped == 1 || dropped % 300 == 0)
				blog(LOG_WARNING, "[obs-gstreamer-master] CPU encoder busy: dropped raw frame #%" PRIu64,
					dropped);
		}
		else
		{
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to prepare CPU raw video buffer");
		}
		return;
	}

	uint64_t duration = gst_util_uint64_scale_int(GST_SECOND, hub->fps_den, hub->fps_num);
	GST_BUFFER_PTS(buf) = frame->timestamp;
	GST_BUFFER_DTS(buf) = frame->timestamp;
	GST_BUFFER_DURATION(buf) = duration;
	hub->frame_index++;

	GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(hub->appsrc), buf);
	if (is_first_frame)
	{
		uint64_t t_elapsed_ns = os_gettime_ns() - t_start;
		blog(LOG_INFO, "[obs-gstreamer-master] Chosen video frame path: %s (%ux%u %s, linesize=[%u,%u], first_frame_push=%.3f ms, flow=%s)",
			gstreamer_frame_mode_name(cache->mode),
			hub->width, hub->height, gst_video_format_to_string(hub->appsrc_format),
			frame->linesize[0], frame->linesize[1],
			(double)t_elapsed_ns / 1000000.0, gst_flow_get_name(flow));
	}
	if (flow != GST_FLOW_OK)
		blog(LOG_ERROR, "[obs-gstreamer-master] Raw CPU frame push failed: flow=%s",
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
		g_atomic_int_set(&hub->pipeline_failed, 0);
		hub->texture_drop_count = 0;
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

	GPtrArray *subs = hub->subscribers;
	if (!subs)
	{
		subs = g_ptr_array_new();
		hub->subscribers = subs;
	}
	g_ptr_array_add(subs, sub);

	g_mutex_unlock(&hub->subscriber_mutex);

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

	GPtrArray *subs = hub->subscribers;
	bool removed = false;
	if (subs)
	{
		for (guint i = 0; i < subs->len; i++)
		{
			if (g_ptr_array_index(subs, i) == sub)
			{
				g_ptr_array_remove_index(subs, i);
				removed = true;
				break;
			}
		}
	}

	g_mutex_unlock(&hub->subscriber_mutex);

	blog(LOG_INFO, "[obs-gstreamer-master] Subscriber '%s' disconnected", sub->name ? sub->name : "Subscriber");
	if (removed)
	{
		bfree(sub->name);
		bfree(sub);
		update_active_destinations(hub, -1);
	}
}

void gst_master_pipeline_request_keyframe(gst_master_pipeline_t *hub)
{
	if (!hub || !hub->appsink) return;
	GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
	gst_element_send_event(hub->appsink, event);
}

gst_master_pipeline_t *gst_master_pipeline_create(const char *custom_master_pipe, const char *encoder_type, const char *video_codec, int bitrate_kbps, int keyint_sec, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, bool gpu_input, gst_master_state_cb state_cb, gpointer user_data)
{
	return gst_master_pipeline_create_with_format(custom_master_pipe, encoder_type, video_codec, bitrate_kbps, keyint_sec, width, height, fps_num, fps_den, gpu_input, GST_VIDEO_FORMAT_RGBA, state_cb, user_data);
}

gst_master_pipeline_t *gst_master_pipeline_create_with_format(const char *custom_master_pipe, const char *encoder_type, const char *video_codec, int bitrate_kbps, int keyint_sec, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, bool gpu_input, GstVideoFormat raw_format, gst_master_state_cb state_cb, gpointer user_data)
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
	hub->requested_raw_format = (raw_format != GST_VIDEO_FORMAT_UNKNOWN) ? raw_format : GST_VIDEO_FORMAT_RGBA;
	hub->video_codec = bstrdup(video_codec ? video_codec : "h264");

	const char *chosen_enc = gst_master_pipeline_select_encoder(encoder_type, hub->video_codec, gpu_input);
	hub->selected_encoder = bstrdup(chosen_enc);

	bool is_hw_gpu_enc = false;
#if HAVE_GST_D3D11
	if (is_d3d11_memory_encoder(chosen_enc))
		is_hw_gpu_enc = true;
#endif
#if HAVE_GST_GL
	if (is_gl_memory_encoder(chosen_enc) &&
	    gst_master_pipeline_element_available("glcolorconvert"))
		is_hw_gpu_enc = true;
#endif

	bool custom_gpu_pipe = false;
	if (custom_master_pipe && custom_master_pipe[0])
	{
#if HAVE_GST_D3D11
		custom_gpu_pipe = strstr(custom_master_pipe, "memory:D3D11Memory") != NULL;
#endif
#if HAVE_GST_GL
		custom_gpu_pipe = custom_gpu_pipe ||
			(strstr(custom_master_pipe, "memory:GLMemory") != NULL &&
			 strstr(custom_master_pipe, "glcolorconvert") != NULL &&
			 strstr(custom_master_pipe, "format=RGBA") != NULL);
#endif
	}
	bool allow_gpu_pipe = (!custom_master_pipe || !custom_master_pipe[0])
		? is_hw_gpu_enc
		: custom_gpu_pipe;

	if (gpu_input && allow_gpu_pipe)
	{
#if HAVE_GST_D3D11
		if (init_obs_d3d11_device(hub))
		{
			hub->gpu_input = true;
		}
#endif
#if HAVE_GST_GL
		if (!hub->gpu_input && init_obs_gl_context(hub))
		{
			hub->gpu_input = true;
		}
#endif
		if (!hub->gpu_input)
		{
			blog(LOG_WARNING, "[obs-gstreamer-master] GPU input context unavailable; falling back to CPU staging surface");
		}
	}

	hub->pipe = create_master_encoder_pipeline(hub, custom_master_pipe, encoder_type, bitrate_kbps, keyint_sec);
	if (hub->pipe)
	{
		hub->appsrc = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsrc");
		hub->appsink = gst_bin_get_by_name(GST_BIN(hub->pipe), "hub_appsink");
	}
	if (custom_master_pipe && custom_master_pipe[0] &&
	    hub->appsrc && hub->appsink &&
	    !validate_custom_master_appsrc(hub))
	{
		gst_master_pipeline_destroy(hub);
		return NULL;
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

			blog(LOG_WARNING, "[obs-gstreamer-master] Encoder '%s' failed state change; trying '%s' (gpu_input=%s)",
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
		blog(LOG_WARNING, "[obs-gstreamer-master] GPU input pipeline failed to initialize; retrying with CPU texture readback");
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
		GstBus *bus = (hub && hub->pipe) ? gst_element_get_bus(hub->pipe) : NULL;
		if (bus)
		{
			GstMessage *msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
			if (msg)
			{
				GError *err = NULL;
				gchar *debug_info = NULL;
				gst_message_parse_error(msg, &err, &debug_info);
				blog(LOG_ERROR, "[obs-gstreamer-master] Failed to initialize master pipeline from element '%s': %s (debug: %s)",
					GST_ELEMENT_NAME(GST_MESSAGE_SRC(msg)),
					err ? err->message : "State change failure",
					debug_info ? debug_info : "none");
				if (err) g_error_free(err);
				g_free(debug_info);
				gst_message_unref(msg);
			}
			else
			{
				blog(LOG_ERROR, "[obs-gstreamer-master] Failed to initialize master pipeline (state change failure)");
			}
			gst_object_unref(bus);
		}
		else
		{
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to initialize master pipeline");
		}
		gst_master_pipeline_destroy(hub);
		return NULL;
	}

	if (!hub->gpu_input)
	{
		gst_video_info_init(&hub->raw_video_info);
		GstVideoFormat slot_fmt = (hub->appsrc_format != GST_VIDEO_FORMAT_UNKNOWN)
			? hub->appsrc_format
			: GST_VIDEO_FORMAT_RGBA;
		if (width == 0 || height == 0 ||
		    !gst_video_info_set_format(&hub->raw_video_info, slot_fmt, width, height) ||
		    GST_VIDEO_INFO_SIZE(&hub->raw_video_info) == 0) {
			blog(LOG_ERROR, "[obs-gstreamer-master] Invalid dimensions/format for reusable CPU frame slot (%ux%u, format=%s)",
				width, height, gst_video_format_to_string(slot_fmt));
			gst_master_pipeline_destroy(hub);
			return NULL;
		}

		gstreamer_cpu_frame_slot_t *slot = gstreamer_cpu_frame_slot_create(GST_VIDEO_INFO_SIZE(&hub->raw_video_info));
		if (!slot) {
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to allocate reusable CPU frame slot");
			gst_master_pipeline_destroy(hub);
			return NULL;
		}
		hub->cpu_frame_slot = slot;
		hub->cpu_frame_cache = g_new0(gstreamer_video_frame_cache_t, 1);
	}

	GstAppSinkCallbacks sink_cbs;
	memset(&sink_cbs, 0, sizeof(sink_cbs));
	sink_cbs.new_sample = video_new_sample_cb;
	gst_app_sink_set_callbacks(GST_APP_SINK(hub->appsink), &sink_cbs, hub, NULL);

	if (state_cb) state_cb(false, user_data);

	return hub;
}

const char *gst_master_pipeline_get_codec(gst_master_pipeline_t *hub)
{
	return (hub && hub->video_codec) ? hub->video_codec : "h264";
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
	destroy_d3d11_texture_slot(hub);
	if (hub->d3d11_device)
	{
		gst_object_unref(hub->d3d11_device);
		hub->d3d11_device = NULL;
	}
	if (hub->d3d11_native_device)
	{
		ID3D11Device_Release(hub->d3d11_native_device);
		hub->d3d11_native_device = NULL;
	}
#endif

#if HAVE_GST_GL
	destroy_gl_texture_slot(hub);
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

	if (hub->video_codec)
	{
		bfree(hub->video_codec);
		hub->video_codec = NULL;
	}
	g_free(hub->colorimetry_str);
	hub->colorimetry_str = NULL;
	gstreamer_cpu_frame_slot_unref((gstreamer_cpu_frame_slot_t *)hub->cpu_frame_slot);
	hub->cpu_frame_slot = NULL;
	g_free(hub->cpu_frame_cache);
	hub->cpu_frame_cache = NULL;

	GPtrArray *subs = hub->subscribers;
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
