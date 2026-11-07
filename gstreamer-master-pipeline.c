/*
 * obs-gstreamer: Master Hardware Encoder Pipeline & Pub-Sub Subscriber Hub
 */

#include "gstreamer-master-pipeline.h"
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
			" tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=%d bitrate=%d",
			gop_size, bitrate_kbps);
		return g_string_free(str, FALSE);
	}

	if (g_strcmp0(enc_name, "x265enc") == 0)
	{
		g_string_append_printf(str,
			" tune=zerolatency speed-preset=ultrafast key-int-max=%d bitrate=%d",
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
		uint32_t active_slots = (hub->ring_slots >= 2 && hub->ring_slots <= GST_MASTER_MAX_RING_SLOTS)
			? hub->ring_slots : GST_MASTER_DEFAULT_RING_SLOTS;
#if HAVE_GST_D3D11
		for (uint32_t i = 0; i < active_slots; i++)
			g_atomic_int_set(&hub->d3d11_slots[i].in_use, 0);
#endif
#if HAVE_GST_GL
		for (uint32_t i = 0; i < active_slots; i++)
		{
			g_atomic_int_set(&hub->gl_slots[i].in_use, 0);
			g_atomic_int_set(&hub->gl_slots[i].plane_refs, 0);
		}
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

static void init_hub_color_matrix(gst_master_pipeline_t *hub)
{
	struct obs_video_info ovi;
	enum video_colorspace cs = VIDEO_CS_709;
	enum video_range_type range = VIDEO_RANGE_PARTIAL;
	if (obs_get_video_info(&ovi))
	{
		cs = ovi.colorspace;
		range = ovi.range;
	}

	bool full_range = (range == VIDEO_RANGE_FULL);
	hub->colorimetry_str = "bt709";

	if (cs == VIDEO_CS_601)
	{
		hub->colorimetry_str = "bt601";
		if (full_range)
		{
			hub->color_matrix.y_coeffs[0] = 0.299000f;  hub->color_matrix.y_coeffs[1] = 0.587000f;  hub->color_matrix.y_coeffs[2] = 0.114000f;  hub->color_matrix.y_coeffs[3] = 0.0f;
			hub->color_matrix.u_coeffs[0] = -0.168736f; hub->color_matrix.u_coeffs[1] = -0.331264f; hub->color_matrix.u_coeffs[2] = 0.500000f;  hub->color_matrix.u_coeffs[3] = 0.5f;
			hub->color_matrix.v_coeffs[0] = 0.500000f;  hub->color_matrix.v_coeffs[1] = -0.418688f; hub->color_matrix.v_coeffs[2] = -0.081312f; hub->color_matrix.v_coeffs[3] = 0.5f;
		}
		else
		{
			hub->color_matrix.y_coeffs[0] = 0.256788f;  hub->color_matrix.y_coeffs[1] = 0.504129f;  hub->color_matrix.y_coeffs[2] = 0.097906f;  hub->color_matrix.y_coeffs[3] = 16.0f / 255.0f;
			hub->color_matrix.u_coeffs[0] = -0.148223f; hub->color_matrix.u_coeffs[1] = -0.290993f; hub->color_matrix.u_coeffs[2] = 0.439216f;  hub->color_matrix.u_coeffs[3] = 128.0f / 255.0f;
			hub->color_matrix.v_coeffs[0] = 0.439216f;  hub->color_matrix.v_coeffs[1] = -0.367788f; hub->color_matrix.v_coeffs[2] = -0.071427f; hub->color_matrix.v_coeffs[3] = 128.0f / 255.0f;
		}
	}
	else if (cs == VIDEO_CS_2100_PQ || cs == VIDEO_CS_2100_HLG)
	{
		hub->colorimetry_str = "bt2020";
		hub->color_matrix.y_coeffs[0] = 0.225613f;  hub->color_matrix.y_coeffs[1] = 0.582282f;  hub->color_matrix.y_coeffs[2] = 0.050929f;  hub->color_matrix.y_coeffs[3] = 16.0f / 255.0f;
		hub->color_matrix.u_coeffs[0] = -0.122655f; hub->color_matrix.u_coeffs[1] = -0.316560f; hub->color_matrix.u_coeffs[2] = 0.439216f;  hub->color_matrix.u_coeffs[3] = 128.0f / 255.0f;
		hub->color_matrix.v_coeffs[0] = 0.439216f;  hub->color_matrix.v_coeffs[1] = -0.403890f; hub->color_matrix.v_coeffs[2] = -0.035326f; hub->color_matrix.v_coeffs[3] = 128.0f / 255.0f;
	}
	else
	{
		if (full_range)
		{
			hub->color_matrix.y_coeffs[0] = 0.212600f;  hub->color_matrix.y_coeffs[1] = 0.715200f;  hub->color_matrix.y_coeffs[2] = 0.072200f;  hub->color_matrix.y_coeffs[3] = 0.0f;
			hub->color_matrix.u_coeffs[0] = -0.114572f; hub->color_matrix.u_coeffs[1] = -0.385428f; hub->color_matrix.u_coeffs[2] = 0.500000f;  hub->color_matrix.u_coeffs[3] = 0.5f;
			hub->color_matrix.v_coeffs[0] = 0.500000f;  hub->color_matrix.v_coeffs[1] = -0.454153f; hub->color_matrix.v_coeffs[2] = -0.045847f; hub->color_matrix.v_coeffs[3] = 0.5f;
		}
		else
		{
			hub->color_matrix.y_coeffs[0] = 0.182586f;  hub->color_matrix.y_coeffs[1] = 0.614231f;  hub->color_matrix.y_coeffs[2] = 0.062007f;  hub->color_matrix.y_coeffs[3] = 16.0f / 255.0f;
			hub->color_matrix.u_coeffs[0] = -0.100644f; hub->color_matrix.u_coeffs[1] = -0.338572f; hub->color_matrix.u_coeffs[2] = 0.439216f;  hub->color_matrix.u_coeffs[3] = 128.0f / 255.0f;
			hub->color_matrix.v_coeffs[0] = 0.439216f;  hub->color_matrix.v_coeffs[1] = -0.398942f; hub->color_matrix.v_coeffs[2] = -0.040274f; hub->color_matrix.v_coeffs[3] = 128.0f / 255.0f;
		}
	}
}

static GstElement *create_master_encoder_pipeline(gst_master_pipeline_t *hub, const char *custom_master_pipe, const char *encoder_type, int bitrate_kbps, int keyint_sec)
{
	gchar *pipe_desc = NULL;

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
			: gst_master_pipeline_select_encoder(encoder_type, hub->video_codec, hub->gpu_input);
		gchar *enc_str = build_encoder_element_str(chosen_enc, gop_size, bitrate);

		init_hub_color_matrix(hub);

		bool is_h265 = hub->video_codec && g_strcmp0(hub->video_codec, "h265") == 0;
		const char *parse_elem = is_h265 ? "h265parse" : "h264parse";
		const char *caps_elem = is_h265 ? "video/x-h265" : "video/x-h264";

#if HAVE_GST_D3D11
		if (hub->gpu_input && hub->d3d11_device && is_d3d11_memory_encoder(chosen_enc))
		{
			GstVideoFormat obs_fmt = get_obs_compositing_format();
			hub->appsrc_format = (obs_fmt == GST_VIDEO_FORMAT_RGBA) ? GST_VIDEO_FORMAT_RGBA : GST_VIDEO_FORMAT_BGRA;
			pipe_desc = g_strdup_printf(
				"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true leaky-type=downstream max-buffers=1 "
				"caps=\"video/x-raw(memory:D3D11Memory), width=%u, height=%u, "
				"format=%s, framerate=%u/%u\" ! "
				"d3d11convert ! video/x-raw(memory:D3D11Memory), format=NV12 ! "
				"%s ! %s config-interval=1 ! "
				"%s, stream-format=byte-stream, alignment=au ! "
				"appsink name=hub_appsink sync=false drop=true max-buffers=1",
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
		    (is_gl_memory_encoder(chosen_enc) || is_dmabuf_memory_encoder(chosen_enc)))
		{
			pipe_desc = g_strdup_printf(
				"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true leaky-type=downstream max-buffers=1 "
				"caps=\"video/x-raw(memory:GLMemory), width=%u, height=%u, "
				"format=NV12, framerate=%u/%u, texture-target=2D, colorimetry=%s\" ! "
				"%s ! %s config-interval=1 ! "
				"%s, stream-format=byte-stream, alignment=au ! "
				"appsink name=hub_appsink sync=false drop=true max-buffers=1",
				hub->width, hub->height, hub->fps_num, hub->fps_den, hub->colorimetry_str,
				enc_str, parse_elem, caps_elem);
			g_free(enc_str);
			goto launch_pipeline;
		}
#endif

		pipe_desc = g_strdup_printf(
			"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true leaky-type=downstream max-buffers=1 "
			"caps=\"video/x-raw, width=%u, height=%u, format=NV12, framerate=%u/%u, colorimetry=%s\" ! "
			"%s ! %s config-interval=1 ! %s, stream-format=byte-stream, alignment=au ! "
			"appsink name=hub_appsink sync=false drop=true max-buffers=1",
			hub->width, hub->height, hub->fps_num, hub->fps_den, hub->colorimetry_str,
			enc_str, parse_elem, caps_elem);

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
static void d3d11_nv12_slot_release_notify(gpointer user_data)
{
	gst_d3d11_nv12_slot_t *slot = (gst_d3d11_nv12_slot_t *)user_data;
	if (slot)
	{
		g_atomic_int_set(&slot->in_use, 0);
	}
}

static void destroy_d3d11_nv12_converter(gst_master_pipeline_t *hub)
{
	if (!hub) return;
	uint32_t slots = (hub->ring_slots >= 2 && hub->ring_slots <= GST_MASTER_MAX_RING_SLOTS)
		? hub->ring_slots : GST_MASTER_DEFAULT_RING_SLOTS;
	for (uint32_t i = 0; i < slots; i++)
	{
		gst_d3d11_nv12_slot_t *slot = &hub->d3d11_slots[i];
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
	}
	hub->d3d11_nv12_ready = false;
}

static bool init_d3d11_nv12_converter(gst_master_pipeline_t *hub)
{
	if (hub->d3d11_nv12_ready) return true;
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

	uint32_t slots = (hub->ring_slots >= 2 && hub->ring_slots <= GST_MASTER_MAX_RING_SLOTS)
		? hub->ring_slots : GST_MASTER_DEFAULT_RING_SLOTS;

	for (uint32_t i = 0; i < slots; i++)
	{
		gst_d3d11_nv12_slot_t *slot = &hub->d3d11_slots[i];
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
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to create D3D11 texture slot %u (format=%d): 0x%08lx",
				i, (int)tex_dxgi_format, (unsigned long)hr);
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
					blog(LOG_ERROR, "[obs-gstreamer-master] Failed to open shared texture slot %u on GStreamer device: 0x%08lx",
						i, (unsigned long)hr);
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
	}

	hub->d3d11_nv12_ready = true;
	blog(LOG_INFO, "[obs-gstreamer-master] Initialized D3D11 zero-copy ring buffer (%u slots, appsrc_format=%s)",
		slots, gst_video_format_to_string(hub->appsrc_format));
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

	if (!hub->d3d11_nv12_ready && !init_d3d11_nv12_converter(hub))
		return false;

	uint32_t slots = (hub->ring_slots >= 2 && hub->ring_slots <= GST_MASTER_MAX_RING_SLOTS)
		? hub->ring_slots : GST_MASTER_DEFAULT_RING_SLOTS;

	/* Multi-slot ring buffer check: locate any available slot starting from next_slot */
	int free_idx = -1;
	for (uint32_t i = 0; i < slots; i++)
	{
		uint32_t candidate = (hub->d3d11_next_slot + i) % slots;
		if (g_atomic_int_get(&hub->d3d11_slots[candidate].in_use) == 0)
		{
			free_idx = (int)candidate;
			break;
		}
	}

	if (free_idx < 0)
	{
		hub->texture_drop_count++;
		uint64_t drops = hub->texture_drop_count;
		if (drops == 1 || drops % 120 == 0)
		{
			blog(LOG_WARNING, "[obs-gstreamer-master] Encoder busy: dropped D3D11 frame #%" PRIu64 " (all %u slots in use, total drops: %" PRIu64 ")",
				hub->texture_push_count + 1, slots, drops);
		}
		return true;
	}

	uint32_t slot_idx = (uint32_t)free_idx;
	gst_d3d11_nv12_slot_t *slot = &hub->d3d11_slots[slot_idx];

	/* Claim slot and advance ring pointer */
	g_atomic_int_set(&slot->in_use, 1);
	hub->d3d11_next_slot = (slot_idx + 1) % slots;

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
		GST_VIDEO_INFO_SIZE(&info), slot, d3d11_nv12_slot_release_notify);
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
			" pushed (%ux%u, format=%s, slot=%u, drops=%" PRIu64 ")",
			frame_number, width, height,
			gst_video_format_to_string(hub->appsrc_format),
			slot_idx, hub->texture_drop_count);
	}
	return true;
}
#endif

#if HAVE_GST_GL
static void gl_nv12_plane_release_notify(gpointer user_data)
{
	gst_gl_nv12_slot_t *slot = (gst_gl_nv12_slot_t *)user_data;
	if (slot)
	{
		if (g_atomic_int_dec_and_test(&slot->plane_refs))
		{
			g_atomic_int_set(&slot->in_use, 0);
		}
	}
}

static void destroy_gl_nv12_converter(gst_master_pipeline_t *hub)
{
	if (!hub) return;
	if (hub->gl_context)
	{
		GstGLFuncs *gl = hub->gl_context->gl_vtable;
		uint32_t slots = (hub->ring_slots >= 2 && hub->ring_slots <= GST_MASTER_MAX_RING_SLOTS)
			? hub->ring_slots : GST_MASTER_DEFAULT_RING_SLOTS;
		for (uint32_t i = 0; i < slots; i++)
		{
			gst_gl_nv12_slot_t *slot = &hub->gl_slots[i];
			if (slot->fbo_y) { gl->DeleteFramebuffers(1, &slot->fbo_y); slot->fbo_y = 0; }
			if (slot->fbo_uv) { gl->DeleteFramebuffers(1, &slot->fbo_uv); slot->fbo_uv = 0; }
			if (slot->tex_y) { gl->DeleteTextures(1, &slot->tex_y); slot->tex_y = 0; }
			if (slot->tex_uv) { gl->DeleteTextures(1, &slot->tex_uv); slot->tex_uv = 0; }
			g_atomic_int_set(&slot->in_use, 0);
		}
		if (hub->gl_nv12_prog_y) { gl->DeleteProgram(hub->gl_nv12_prog_y); hub->gl_nv12_prog_y = 0; }
		if (hub->gl_nv12_prog_uv) { gl->DeleteProgram(hub->gl_nv12_prog_uv); hub->gl_nv12_prog_uv = 0; }
		if (hub->gl_nv12_vs) { gl->DeleteShader(hub->gl_nv12_vs); hub->gl_nv12_vs = 0; }
		if (hub->gl_nv12_ps_y) { gl->DeleteShader(hub->gl_nv12_ps_y); hub->gl_nv12_ps_y = 0; }
		if (hub->gl_nv12_ps_uv) { gl->DeleteShader(hub->gl_nv12_ps_uv); hub->gl_nv12_ps_uv = 0; }
	}
	hub->gl_nv12_ready = false;
}

static GLuint compile_gl_shader(GstGLFuncs *gl, GLenum type, const char *src)
{
	GLuint s = gl->CreateShader(type);
	gl->ShaderSource(s, 1, &src, NULL);
	gl->CompileShader(s);
	GLint status = 0;
	gl->GetShaderiv(s, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char log[1024] = {0};
		gl->GetShaderInfoLog(s, sizeof(log), NULL, log);
		blog(LOG_ERROR, "[obs-gstreamer-master] GLSL shader compile error: %s", log);
		gl->DeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint link_gl_program(GstGLFuncs *gl, GLuint vs, GLuint fs)
{
	GLuint p = gl->CreateProgram();
	gl->AttachShader(p, vs);
	gl->AttachShader(p, fs);
	gl->LinkProgram(p);
	GLint status = 0;
	gl->GetProgramiv(p, GL_LINK_STATUS, &status);
	if (!status)
	{
		char log[1024] = {0};
		gl->GetProgramInfoLog(p, sizeof(log), NULL, log);
		blog(LOG_ERROR, "[obs-gstreamer-master] GLSL program link error: %s", log);
		gl->DeleteProgram(p);
		return 0;
	}
	return p;
}

static bool init_gl_nv12_converter(gst_master_pipeline_t *hub)
{
	if (hub->gl_nv12_ready) return true;
	if ((!hub->gl_app_context || !hub->gl_context || !hub->gl_display) && !init_obs_gl_context(hub))
		return false;

	GstGLFuncs *gl = hub->gl_context->gl_vtable;
	if (!gl) return false;

	const char *vs_src =
		"#version 330 core\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    uint vid = uint(gl_VertexID);\n"
		"    vec2 uv = vec2(float((vid << 1u) & 2u), float(vid & 2u));\n"
		"    v_uv = vec2(uv.x, 1.0 - uv.y);\n"
		"    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);\n"
		"}\n";

	const char *ps_y_src =
		"#version 330 core\n"
		"in vec2 v_uv;\n"
		"uniform sampler2D s_tex;\n"
		"uniform vec4 u_y_coeffs;\n"
		"uniform int u_is_srgb;\n"
		"out vec4 fragColor;\n"
		"vec3 linear_to_srgb(vec3 lin) {\n"
		"    bvec3 cutoff = lessThanEqual(lin, vec3(0.0031308));\n"
		"    vec3 higher = vec3(1.055) * pow(max(lin, vec3(0.0)), vec3(1.0 / 2.4)) - vec3(0.055);\n"
		"    vec3 lower = lin * vec3(12.92);\n"
		"    return mix(higher, lower, cutoff);\n"
		"}\n"
		"void main() {\n"
		"    vec3 c = texture(s_tex, v_uv).rgb;\n"
		"    if (u_is_srgb != 0) c = linear_to_srgb(c);\n"
		"    float y = clamp(dot(c, u_y_coeffs.rgb) + u_y_coeffs.w, 0.0, 1.0);\n"
		"    fragColor = vec4(y, 0.0, 0.0, 1.0);\n"
		"}\n";

	const char *ps_uv_src =
		"#version 330 core\n"
		"in vec2 v_uv;\n"
		"uniform sampler2D s_tex;\n"
		"uniform vec4 u_u_coeffs;\n"
		"uniform vec4 u_v_coeffs;\n"
		"uniform int u_is_srgb;\n"
		"out vec4 fragColor;\n"
		"vec3 linear_to_srgb(vec3 lin) {\n"
		"    bvec3 cutoff = lessThanEqual(lin, vec3(0.0031308));\n"
		"    vec3 higher = vec3(1.055) * pow(max(lin, vec3(0.0)), vec3(1.0 / 2.4)) - vec3(0.055);\n"
		"    vec3 lower = lin * vec3(12.92);\n"
		"    return mix(higher, lower, cutoff);\n"
		"}\n"
		"void main() {\n"
		"    vec3 c = texture(s_tex, v_uv).rgb;\n"
		"    if (u_is_srgb != 0) c = linear_to_srgb(c);\n"
		"    float u = clamp(dot(c, u_u_coeffs.rgb) + u_u_coeffs.w, 0.0, 1.0);\n"
		"    float v = clamp(dot(c, u_v_coeffs.rgb) + u_v_coeffs.w, 0.0, 1.0);\n"
		"    fragColor = vec4(u, v, 0.0, 1.0);\n"
		"}\n";

	hub->gl_nv12_vs = compile_gl_shader(gl, GL_VERTEX_SHADER, vs_src);
	hub->gl_nv12_ps_y = compile_gl_shader(gl, GL_FRAGMENT_SHADER, ps_y_src);
	hub->gl_nv12_ps_uv = compile_gl_shader(gl, GL_FRAGMENT_SHADER, ps_uv_src);

	if (!hub->gl_nv12_vs || !hub->gl_nv12_ps_y || !hub->gl_nv12_ps_uv)
		return false;

	hub->gl_nv12_prog_y = link_gl_program(gl, hub->gl_nv12_vs, hub->gl_nv12_ps_y);
	hub->gl_nv12_prog_uv = link_gl_program(gl, hub->gl_nv12_vs, hub->gl_nv12_ps_uv);

	if (!hub->gl_nv12_prog_y || !hub->gl_nv12_prog_uv)
		return false;

	uint32_t slots = (hub->ring_slots >= 2 && hub->ring_slots <= GST_MASTER_MAX_RING_SLOTS)
		? hub->ring_slots : GST_MASTER_DEFAULT_RING_SLOTS;

	for (uint32_t i = 0; i < slots; i++)
	{
		gst_gl_nv12_slot_t *slot = &hub->gl_slots[i];

		/* Texture Y (GL_R8, W x H) */
		gl->GenTextures(1, &slot->tex_y);
		gl->BindTexture(GL_TEXTURE_2D, slot->tex_y);
		gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl->TexImage2D(GL_TEXTURE_2D, 0, GL_R8, hub->width, hub->height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);

		gl->GenFramebuffers(1, &slot->fbo_y);
		gl->BindFramebuffer(GL_FRAMEBUFFER, slot->fbo_y);
		gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot->tex_y, 0);

		/* Texture UV (GL_RG8, W/2 x H/2) */
		gl->GenTextures(1, &slot->tex_uv);
		gl->BindTexture(GL_TEXTURE_2D, slot->tex_uv);
		gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RG8, hub->width / 2, hub->height / 2, 0, GL_RG, GL_UNSIGNED_BYTE, NULL);

		gl->GenFramebuffers(1, &slot->fbo_uv);
		gl->BindFramebuffer(GL_FRAMEBUFFER, slot->fbo_uv);
		gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot->tex_uv, 0);

		gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
		gl->BindTexture(GL_TEXTURE_2D, 0);

		g_atomic_int_set(&slot->in_use, 0);
		g_atomic_int_set(&slot->plane_refs, 0);
	}

	hub->gl_nv12_ready = true;
	blog(LOG_INFO, "[obs-gstreamer-master] Initialized OpenGL NV12 converter (%u-slot ring buffer, colorimetry=%s)",
		slots, hub->colorimetry_str);
	return true;
}

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

	if (!hub->gl_nv12_ready && !init_gl_nv12_converter(hub))
		return false;

	uint32_t slots = (hub->ring_slots >= 2 && hub->ring_slots <= GST_MASTER_MAX_RING_SLOTS)
		? hub->ring_slots : GST_MASTER_DEFAULT_RING_SLOTS;

	/* Multi-slot ring buffer check: locate any available slot starting from next_slot */
	int free_idx = -1;
	for (uint32_t i = 0; i < slots; i++)
	{
		uint32_t candidate = (hub->gl_next_slot + i) % slots;
		if (g_atomic_int_get(&hub->gl_slots[candidate].in_use) == 0)
		{
			free_idx = (int)candidate;
			break;
		}
	}

	if (free_idx < 0)
	{
		hub->texture_drop_count++;
		uint64_t drops = hub->texture_drop_count;
		if (drops == 1 || drops % 120 == 0)
		{
			blog(LOG_WARNING, "[obs-gstreamer-master] Encoder busy: dropped OpenGL frame #%" PRIu64 " (all %u slots in use, total drops: %" PRIu64 ")",
				hub->texture_push_count + 1, slots, drops);
		}
		return true;
	}

	uint32_t slot_idx = (uint32_t)free_idx;
	gst_gl_nv12_slot_t *slot = &hub->gl_slots[slot_idx];

	g_atomic_int_set(&slot->in_use, 1);
	g_atomic_int_set(&slot->plane_refs, 2); /* 2 planes: Y and UV */
	hub->gl_next_slot = (slot_idx + 1) % slots;

	const guint *tex_ptr = (const guint *)gs_texture_get_obj(texture);
	if (!tex_ptr || *tex_ptr == 0)
	{
		g_atomic_int_set(&slot->in_use, 0);
		blog(LOG_ERROR, "[obs-gstreamer-master] OBS texture has no valid OpenGL texture ID");
		return false;
	}
	guint tex_id = *tex_ptr;

	enum gs_color_format color_format = gs_texture_get_color_format(texture);
	int is_srgb = gs_is_srgb_format(color_format) ? 1 : 0;

	/* Flush OBS's GL commands before shared context reads texture */
	gs_flush();

	GstGLFuncs *gl = hub->gl_context->gl_vtable;

	GLint prev_vp[4] = {0, 0, (GLint)hub->width, (GLint)hub->height};
	gl->GetIntegerv(GL_VIEWPORT, prev_vp);

#ifndef GL_TEXTURE_SRGB_DECODE_EXT
#define GL_TEXTURE_SRGB_DECODE_EXT 0x8A48
#endif
#ifndef GL_DECODE_EXT
#define GL_DECODE_EXT 0x8A49
#endif
#ifndef GL_SKIP_DECODE_EXT
#define GL_SKIP_DECODE_EXT 0x8A4A
#endif

	gl->ActiveTexture(GL_TEXTURE0);
	gl->BindTexture(GL_TEXTURE_2D, tex_id);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	bool srgb_skip_active = false;
	int shader_srgb_fallback = 0;
	if (is_srgb)
	{
		while (gl->GetError() != GL_NO_ERROR) {}
		gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
		if (gl->GetError() == GL_NO_ERROR)
			srgb_skip_active = true;
		else
			shader_srgb_fallback = 1;
	}

	/* Render Pass 1: Y Plane (FBO Y, full resolution) */
	gl->BindFramebuffer(GL_FRAMEBUFFER, slot->fbo_y);
	gl->Viewport(0, 0, hub->width, hub->height);
	gl->UseProgram(hub->gl_nv12_prog_y);
	GLint loc_y = gl->GetUniformLocation(hub->gl_nv12_prog_y, "u_y_coeffs");
	GLint loc_srgb_y = gl->GetUniformLocation(hub->gl_nv12_prog_y, "u_is_srgb");
	gl->Uniform4fv(loc_y, 1, hub->color_matrix.y_coeffs);
	gl->Uniform1i(loc_srgb_y, shader_srgb_fallback);
	gl->DrawArrays(GL_TRIANGLES, 0, 3);

	/* Render Pass 2: UV Plane (FBO UV, half resolution) */
	gl->BindFramebuffer(GL_FRAMEBUFFER, slot->fbo_uv);
	gl->Viewport(0, 0, hub->width / 2, hub->height / 2);
	gl->UseProgram(hub->gl_nv12_prog_uv);
	GLint loc_u = gl->GetUniformLocation(hub->gl_nv12_prog_uv, "u_u_coeffs");
	GLint loc_v = gl->GetUniformLocation(hub->gl_nv12_prog_uv, "u_v_coeffs");
	GLint loc_srgb_uv = gl->GetUniformLocation(hub->gl_nv12_prog_uv, "u_is_srgb");
	gl->Uniform4fv(loc_u, 1, hub->color_matrix.u_coeffs);
	gl->Uniform4fv(loc_v, 1, hub->color_matrix.v_coeffs);
	gl->Uniform1i(loc_srgb_uv, shader_srgb_fallback);
	gl->DrawArrays(GL_TRIANGLES, 0, 3);

	if (srgb_skip_active)
		gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_DECODE_EXT);

	gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
	gl->BindTexture(GL_TEXTURE_2D, 0);
	gl->UseProgram(0);
	gl->Viewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
	gl->Flush();

	GstVideoInfo info;
	if (!gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, width, height))
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

	/* Wrap Plane 0: Y */
	GstGLVideoAllocationParams *params_y = gst_gl_video_allocation_params_new_wrapped_texture(
		hub->gl_context, NULL, &info, 0, NULL,
		GST_GL_TEXTURE_TARGET_2D, GST_GL_R8, slot->tex_y, slot, gl_nv12_plane_release_notify);
	GstGLBaseMemory *gl_mem_y = gst_gl_base_memory_alloc(
		GST_GL_BASE_MEMORY_ALLOCATOR(allocator), (GstGLAllocationParams *)params_y);
	gst_gl_allocation_params_free((GstGLAllocationParams *)params_y);

	/* Wrap Plane 1: UV */
	GstGLVideoAllocationParams *params_uv = gst_gl_video_allocation_params_new_wrapped_texture(
		hub->gl_context, NULL, &info, 1, NULL,
		GST_GL_TEXTURE_TARGET_2D, GST_GL_RG8, slot->tex_uv, slot, gl_nv12_plane_release_notify);
	GstGLBaseMemory *gl_mem_uv = gst_gl_base_memory_alloc(
		GST_GL_BASE_MEMORY_ALLOCATOR(allocator), (GstGLAllocationParams *)params_uv);
	gst_gl_allocation_params_free((GstGLAllocationParams *)params_uv);
	gst_object_unref(allocator);

	if (!gl_mem_y || !gl_mem_uv)
	{
		if (gl_mem_y) gst_memory_unref(GST_MEMORY_CAST(gl_mem_y));
		if (gl_mem_uv) gst_memory_unref(GST_MEMORY_CAST(gl_mem_uv));
		g_atomic_int_set(&slot->in_use, 0);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to wrap OpenGL NV12 textures as GstGLMemory");
		return false;
	}

	GstBuffer *buffer = gst_buffer_new();
	if (!buffer)
	{
		gst_memory_unref(GST_MEMORY_CAST(gl_mem_y));
		gst_memory_unref(GST_MEMORY_CAST(gl_mem_uv));
		return false;
	}
	gst_buffer_append_memory(buffer, GST_MEMORY_CAST(gl_mem_y));
	gst_buffer_append_memory(buffer, GST_MEMORY_CAST(gl_mem_uv));

	gsize offsets[GST_VIDEO_MAX_PLANES] = {0};
	gint strides[GST_VIDEO_MAX_PLANES] = {0};
	for (guint plane = 0; plane < GST_VIDEO_INFO_N_PLANES(&info); plane++)
	{
		offsets[plane] = GST_VIDEO_INFO_PLANE_OFFSET(&info, plane);
		strides[plane] = GST_VIDEO_INFO_PLANE_STRIDE(&info, plane);
	}
	if (!gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12, width, height,
		GST_VIDEO_INFO_N_PLANES(&info), offsets, strides))
	{
		gst_buffer_unref(buffer);
		blog(LOG_ERROR, "[obs-gstreamer-master] Failed to attach NV12 video metadata to OpenGL frame");
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
		blog(LOG_ERROR, "[obs-gstreamer-master] OpenGL NV12 texture push failed: flow=%s",
			gst_flow_get_name(flow));
		return false;
	}

	uint64_t frame_number = ++hub->texture_push_count;
	if (frame_number == 1 || frame_number % 300 == 0)
	{
		blog(LOG_INFO, "[obs-gstreamer-master] OpenGL NV12 zero-copy frame #%" PRIu64
			" converted and pushed (%ux%u, slot=%u, drops=%" PRIu64 ")",
			frame_number, width, height, slot_idx, hub->texture_drop_count);
	}
	return true;
}
#endif

void gst_master_pipeline_push_texture(gst_master_pipeline_t *hub, void *gpu_handle, uint64_t pts)
{
	if (!hub || !gpu_handle || g_atomic_int_get(&hub->active_destinations) <= 0 || !hub->appsrc) return;

	gs_texture_t *tex = (gs_texture_t *)gpu_handle;
#if HAVE_GST_D3D11
	if (hub->gpu_input && hub->d3d11_device && is_d3d11_memory_encoder(hub->selected_encoder))
	{
		push_d3d11_texture(hub, tex, pts);
		return;
	}
#endif
#if HAVE_GST_GL
	if (hub->gpu_input && hub->gl_app_context &&
	    (is_gl_memory_encoder(hub->selected_encoder) || is_dmabuf_memory_encoder(hub->selected_encoder)))
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

	uint32_t copy_w = tex_w < hub->width ? tex_w : hub->width;
	uint32_t copy_h = tex_h < hub->height ? tex_h : hub->height;
	size_t y_size = (size_t)hub->width * hub->height;
	size_t uv_size = (size_t)hub->width * (hub->height / 2);
	size_t total_size = y_size + uv_size;

	GstBuffer *in_buf = gst_buffer_new_allocate(NULL, total_size, NULL);
	if (!in_buf)
	{
		if (frame_number == 1)
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to allocate NV12 buffer for staging frame");
		gs_stagesurface_unmap(hub->stagesurf);
		return;
	}

	GstMapInfo map;
	if (!gst_buffer_map(in_buf, &map, GST_MAP_WRITE))
	{
		if (frame_number == 1)
			blog(LOG_ERROR, "[obs-gstreamer-master] Failed to map NV12 buffer for staging frame");
		gst_buffer_unref(in_buf);
		gs_stagesurface_unmap(hub->stagesurf);
		return;
	}

	bool is_bgra = (fmt == GS_BGRA || fmt == GS_BGRA_UNORM || fmt == GS_BGRX);
	uint8_t *dst_y = map.data;
	uint8_t *dst_uv = map.data + y_size;
	const float *yc = hub->color_matrix.y_coeffs;
	const float *uc = hub->color_matrix.u_coeffs;
	const float *vc = hub->color_matrix.v_coeffs;

	for (uint32_t y = 0; y < copy_h; y++)
	{
		const uint8_t *src_row = src_data + (size_t)y * src_linesize;
		uint8_t *out_y_row = dst_y + (size_t)y * hub->width;
		for (uint32_t x = 0; x < copy_w; x++)
		{
			float r = (float)(is_bgra ? src_row[x * 4 + 2] : src_row[x * 4 + 0]) / 255.0f;
			float g = (float)(src_row[x * 4 + 1]) / 255.0f;
			float b = (float)(is_bgra ? src_row[x * 4 + 0] : src_row[x * 4 + 2]) / 255.0f;

			float y_val = r * yc[0] + g * yc[1] + b * yc[2] + yc[3];
			if (y_val < 0.0f) y_val = 0.0f;
			else if (y_val > 1.0f) y_val = 1.0f;
			out_y_row[x] = (uint8_t)(y_val * 255.0f + 0.5f);
		}
	}

	for (uint32_t y = 0; y < copy_h / 2; y++)
	{
		const uint8_t *src_row0 = src_data + (size_t)(y * 2) * src_linesize;
		const uint8_t *src_row1 = src_data + (size_t)(y * 2 + 1) * src_linesize;
		uint8_t *out_uv_row = dst_uv + (size_t)y * hub->width;
		for (uint32_t x = 0; x < copy_w / 2; x++)
		{
			uint32_t x2 = x * 2;
			float r = (float)(is_bgra ?
				(src_row0[x2 * 4 + 2] + src_row0[(x2 + 1) * 4 + 2] + src_row1[x2 * 4 + 2] + src_row1[(x2 + 1) * 4 + 2]) :
				(src_row0[x2 * 4 + 0] + src_row0[(x2 + 1) * 4 + 0] + src_row1[x2 * 4 + 0] + src_row1[(x2 + 1) * 4 + 0])) / (4.0f * 255.0f);
			float g = (float)(src_row0[x2 * 4 + 1] + src_row0[(x2 + 1) * 4 + 1] + src_row1[x2 * 4 + 1] + src_row1[(x2 + 1) * 4 + 1]) / (4.0f * 255.0f);
			float b = (float)(is_bgra ?
				(src_row0[x2 * 4 + 0] + src_row0[(x2 + 1) * 4 + 0] + src_row1[x2 * 4 + 0] + src_row1[(x2 + 1) * 4 + 0]) :
				(src_row0[x2 * 4 + 2] + src_row0[(x2 + 1) * 4 + 2] + src_row1[x2 * 4 + 2] + src_row1[(x2 + 1) * 4 + 2])) / (4.0f * 255.0f);

			float u_val = r * uc[0] + g * uc[1] + b * uc[2] + uc[3];
			float v_val = r * vc[0] + g * vc[1] + b * vc[2] + vc[3];
			if (u_val < 0.0f) u_val = 0.0f; else if (u_val > 1.0f) u_val = 1.0f;
			if (v_val < 0.0f) v_val = 0.0f; else if (v_val > 1.0f) v_val = 1.0f;

			out_uv_row[x * 2 + 0] = (uint8_t)(u_val * 255.0f + 0.5f);
			out_uv_row[x * 2 + 1] = (uint8_t)(v_val * 255.0f + 0.5f);
		}
	}
	gst_buffer_unmap(in_buf, &map);

	gsize offsets[GST_VIDEO_MAX_PLANES] = {0, y_size, 0, 0};
	gint strides[GST_VIDEO_MAX_PLANES] = {(gint)hub->width, (gint)hub->width, 0, 0};
	gst_buffer_add_video_meta_full(in_buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12,
		hub->width, hub->height, 2, offsets, strides);

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
	if (g_atomic_int_get(&hub->pipeline_failed)) return;

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

gst_master_pipeline_t *gst_master_pipeline_create(const char *custom_master_pipe, const char *encoder_type, const char *video_codec, int bitrate_kbps, int keyint_sec, uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den, bool gpu_input, gst_master_state_cb state_cb, gpointer user_data)
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
	hub->ring_slots = GST_MASTER_DEFAULT_RING_SLOTS;
	hub->video_codec = bstrdup(video_codec ? video_codec : "h264");

	const char *chosen_enc = gst_master_pipeline_select_encoder(encoder_type, hub->video_codec, gpu_input);
	hub->selected_encoder = bstrdup(chosen_enc);

	bool is_hw_gpu_enc = false;
#if HAVE_GST_D3D11
	if (is_d3d11_memory_encoder(chosen_enc))
		is_hw_gpu_enc = true;
#endif
#if HAVE_GST_GL
	if (is_gl_memory_encoder(chosen_enc) || is_dmabuf_memory_encoder(chosen_enc))
		is_hw_gpu_enc = true;
#endif

	bool allow_gpu_pipe = (!custom_master_pipe || !custom_master_pipe[0])
		? is_hw_gpu_enc
		: (strstr(custom_master_pipe, "memory:D3D11Memory") != NULL ||
		   strstr(custom_master_pipe, "memory:GLMemory") != NULL);

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

void gst_master_pipeline_set_ring_slots(gst_master_pipeline_t *hub, uint32_t ring_slots)
{
	if (!hub) return;
	if (ring_slots < 2) ring_slots = 2;
	if (ring_slots > GST_MASTER_MAX_RING_SLOTS) ring_slots = GST_MASTER_MAX_RING_SLOTS;
	hub->ring_slots = ring_slots;
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
	destroy_d3d11_nv12_converter(hub);
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
	destroy_gl_nv12_converter(hub);
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
