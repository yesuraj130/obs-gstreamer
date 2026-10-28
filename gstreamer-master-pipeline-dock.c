/*
 * obs-gstreamer: Master Pipeline Hub Dock & Properties (Pure C, Zero Qt)
 *
 * Implements native OBS property-based UI for:
 * - 4 Capture Callbacks (Render Callback, Filter Callback, Encoder Callback, Output Callback)
 * - Master Hardware Encoder Hub (Zero-copy GPU capture -> hardware encode once)
 * - Multiple simultaneous destinations (RTSP Server, WebRTC WHEP, Custom Pipeline)
 * - Independent & Batch Start/Stop controls
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <gst/gst.h>

#include "gstreamer-master-pipeline.h"
#include "gstreamer-rtspserver.h"
#include "gstreamer-render-callback.h"
#include "gstreamer-filter-callback.h"
#include "gstreamer-encoder-callback.h"
#include "gstreamer-output-callback.h"
#include "gstreamer-helper.h"

#if defined(__has_include)
#if __has_include(<obs-frontend-api.h>)
#include <obs-frontend-api.h>
#define HAVE_OBS_FRONTEND 1
#endif
#endif

#ifndef HAVE_OBS_FRONTEND
#define HAVE_OBS_FRONTEND 0
#endif

#define DEFAULT_MASTER_PIPELINE \
	"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true " \
	"caps=\"video/x-raw, format=BGRA, width=%u, height=%u, framerate=%u/%u\" ! " \
	"videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=30 bitrate=4000 ! " \
	"h264parse config-interval=1 ! appsink name=hub_appsink sync=false drop=true max-buffers=1"

#define DEFAULT_RTSP_PIPELINE \
	"( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true " \
	"caps=\"video/x-h264, stream-format=byte-stream, alignment=au\" ! " \
	"h264parse config-interval=1 ! rtph264pay name=pay0 pt=96 config-interval=1 )"

/* --------------------------------------------------------------------------
 * State Management for Master Pipeline & Destinations
 * -------------------------------------------------------------------------- */
typedef struct {
	/* Master Source & Encoder */
	char *pipeline_name;
	char *source_type;      /* "Program Output", "Scene", "Source" */
	char *source_name;
	char *scene_name;
	char *render_mode;      /* The 4 Callbacks */
	char *gpu_encoder_type; /* "auto", "nvenc", "vaapi", "vtenc", "x264enc" */
	int bitrate_kbps;
	int keyint_sec;
	char *master_pipeline;

	/* Destination 1: RTSP */
	bool rtsp_enabled;
	char *rtsp_service;
	char *rtsp_mount;
	char *rtsp_pipeline;
	bool rtsp_autostart;
	gst_hub_branch_t *rtsp_branch;

	/* Destination 2: WebRTC (WHEP) */
	bool webrtc_enabled;
	char *webrtc_http_port;
	char *webrtc_stun_server;
	char *webrtc_web_root;
	bool webrtc_autostart;
	obs_output_t *webrtc_output;

	/* Destination 3: Custom Pipeline */
	bool custom_enabled;
	char *custom_pipeline;
	bool custom_autostart;
	obs_output_t *custom_output;

	/* Active Callback Handles (if running standalone) */
	gst_render_callback_t *active_render_cb;
	obs_source_t *source_obj;
	obs_view_t *view_obj;
	video_t *custom_video;
} gstreamer_master_dock_t;

static gstreamer_master_dock_t g_master_dock = {
	.bitrate_kbps = 4000,
	.keyint_sec = 1,
	.rtsp_enabled = true,
	.webrtc_enabled = false,
	.custom_enabled = false,
};

/* --------------------------------------------------------------------------
 * Helper Enumeration for Scenes and Sources
 * -------------------------------------------------------------------------- */
static bool enum_sources_cb(void *param, obs_source_t *source)
{
	obs_property_t *p = (obs_property_t *)param;
	const char *name = obs_source_get_name(source);
	if (name && *name) {
		obs_property_list_add_string(p, name, name);
	}
	return true;
}

static bool enum_scenes_cb(void *param, obs_source_t *scene)
{
	obs_property_t *p = (obs_property_t *)param;
	const char *name = obs_source_get_name(scene);
	if (name && *name) {
		obs_property_list_add_string(p, name, name);
	}
	return true;
}

/* --------------------------------------------------------------------------
 * Destination Control Helpers
 * -------------------------------------------------------------------------- */
static void stop_rtsp_destination(gstreamer_master_dock_t *dock)
{
	if (dock->rtsp_branch) {
		blog(LOG_INFO, "[obs-gstreamer-dock] Stopping RTSP server branch (port: %s, mount: %s)",
			dock->rtsp_service ? dock->rtsp_service : "8554",
			dock->rtsp_mount ? dock->rtsp_mount : "/live");
		gst_render_hub_stop_branch(dock->rtsp_branch);
		dock->rtsp_branch = NULL;
	}
}

static bool start_rtsp_destination(gstreamer_master_dock_t *dock)
{
	stop_rtsp_destination(dock);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "name", dock->pipeline_name ? dock->pipeline_name : "Master RTSP");
	obs_data_set_string(settings, "source_type", dock->source_type ? dock->source_type : "Program Output");

	const char *src_name = "";
	if (dock->source_type && strcmp(dock->source_type, "Scene") == 0) {
		src_name = dock->scene_name ? dock->scene_name : "";
	} else if (dock->source_type && strcmp(dock->source_type, "Source") == 0) {
		src_name = dock->source_name ? dock->source_name : "";
	}
	obs_data_set_string(settings, "source_name", src_name);
	obs_data_set_string(settings, "render_mode", dock->render_mode ? dock->render_mode : "Render Callback (GPU Direct Texture)");

	int port = dock->rtsp_service ? atoi(dock->rtsp_service) : 8554;
	if (port <= 0) port = 8554;
	obs_data_set_int(settings, "rtsp_port", port);
	obs_data_set_string(settings, "rtsp_service", dock->rtsp_service ? dock->rtsp_service : "8554");
	obs_data_set_string(settings, "rtsp_mount", (dock->rtsp_mount && *dock->rtsp_mount) ? dock->rtsp_mount : "/live");

	obs_data_set_string(settings, "encoder_type", dock->gpu_encoder_type ? dock->gpu_encoder_type : "auto");
	obs_data_set_int(settings, "bitrate_kbps", dock->bitrate_kbps > 0 ? dock->bitrate_kbps : 4000);
	obs_data_set_int(settings, "keyint_sec", dock->keyint_sec > 0 ? dock->keyint_sec : 1);
	obs_data_set_string(settings, "master_pipeline", (dock->master_pipeline && *dock->master_pipeline) ? dock->master_pipeline : DEFAULT_MASTER_PIPELINE);
	obs_data_set_string(settings, "rtsp_pipeline", (dock->rtsp_pipeline && *dock->rtsp_pipeline) ? dock->rtsp_pipeline : DEFAULT_RTSP_PIPELINE);

	blog(LOG_INFO, "[obs-gstreamer-dock] Starting RTSP destination on port %s%s...",
		dock->rtsp_service ? dock->rtsp_service : "8554",
		(dock->rtsp_mount && *dock->rtsp_mount) ? dock->rtsp_mount : "/live");

	dock->rtsp_branch = gst_render_hub_start_branch_data(settings);
	obs_data_release(settings);

	if (!dock->rtsp_branch) {
		blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to start RTSP server branch");
		return false;
	}

	blog(LOG_INFO, "[obs-gstreamer-dock] RTSP destination active");
	return true;
}

static void stop_webrtc_destination(gstreamer_master_dock_t *dock)
{
	if (dock->webrtc_output) {
		if (obs_output_active(dock->webrtc_output)) {
			obs_output_stop(dock->webrtc_output);
		}
		obs_output_release(dock->webrtc_output);
		dock->webrtc_output = NULL;
		blog(LOG_INFO, "[obs-gstreamer-dock] WebRTC output stopped");
	}
}

static bool start_webrtc_destination(gstreamer_master_dock_t *dock)
{
	stop_webrtc_destination(dock);

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "webrtc_output", true);
	obs_data_set_string(settings, "webrtc_http_port", dock->webrtc_http_port ? dock->webrtc_http_port : "8888");
	obs_data_set_string(settings, "webrtc_stun_server", dock->webrtc_stun_server ? dock->webrtc_stun_server : "");
	obs_data_set_string(settings, "webrtc_web_root", dock->webrtc_web_root ? dock->webrtc_web_root : "");
	obs_data_set_string(settings, "pipeline", "autovideosink sync=false");

	dock->webrtc_output = obs_output_create("hjm-gstreamer-output", "GStreamer WebRTC Output", settings, NULL);
	obs_data_release(settings);

	if (!dock->webrtc_output) {
		blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to create WebRTC output object");
		return false;
	}

	obs_output_set_media(dock->webrtc_output, obs_get_video(), obs_get_audio());
	if (!obs_output_start(dock->webrtc_output)) {
		blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to start WebRTC output: %s",
			obs_output_get_last_error(dock->webrtc_output));
		stop_webrtc_destination(dock);
		return false;
	}

	blog(LOG_INFO, "[obs-gstreamer-dock] WebRTC output started on port %s",
		dock->webrtc_http_port ? dock->webrtc_http_port : "8888");
	return true;
}

static void stop_custom_destination(gstreamer_master_dock_t *dock)
{
	if (dock->custom_output) {
		if (obs_output_active(dock->custom_output)) {
			obs_output_stop(dock->custom_output);
		}
		obs_output_release(dock->custom_output);
		dock->custom_output = NULL;
		blog(LOG_INFO, "[obs-gstreamer-dock] Custom output stopped");
	}
}

static bool start_custom_destination(gstreamer_master_dock_t *dock)
{
	stop_custom_destination(dock);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "pipeline",
		(dock->custom_pipeline && *dock->custom_pipeline) ? dock->custom_pipeline : "autovideosink sync=false");

	dock->custom_output = obs_output_create("hjm-gstreamer-output", "GStreamer Custom Pipeline", settings, NULL);
	obs_data_release(settings);

	if (!dock->custom_output) {
		blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to create custom pipeline output");
		return false;
	}

	obs_output_set_media(dock->custom_output, obs_get_video(), obs_get_audio());
	if (!obs_output_start(dock->custom_output)) {
		blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to start custom output: %s",
			obs_output_get_last_error(dock->custom_output));
		stop_custom_destination(dock);
		return false;
	}

	blog(LOG_INFO, "[obs-gstreamer-dock] Custom pipeline output started successfully");
	return true;
}

/* --------------------------------------------------------------------------
 * Button Callbacks for OBS Properties
 * -------------------------------------------------------------------------- */
static bool on_start_all_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	gstreamer_master_dock_t *dock = (gstreamer_master_dock_t *)data;
	if (!dock) dock = &g_master_dock;

	blog(LOG_INFO, "[obs-gstreamer-dock] Starting all active destination branches...");
	if (dock->rtsp_enabled) start_rtsp_destination(dock);
	if (dock->webrtc_enabled) start_webrtc_destination(dock);
	if (dock->custom_enabled) start_custom_destination(dock);
	return true;
}

static bool on_stop_all_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	gstreamer_master_dock_t *dock = (gstreamer_master_dock_t *)data;
	if (!dock) dock = &g_master_dock;

	blog(LOG_INFO, "[obs-gstreamer-dock] Stopping all active destination branches...");
	stop_rtsp_destination(dock);
	stop_webrtc_destination(dock);
	stop_custom_destination(dock);
	return true;
}

static bool on_toggle_rtsp_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	gstreamer_master_dock_t *dock = (gstreamer_master_dock_t *)data;
	if (!dock) dock = &g_master_dock;

	if (dock->rtsp_branch && gst_render_hub_is_branch_active(dock->rtsp_branch)) {
		stop_rtsp_destination(dock);
	} else {
		start_rtsp_destination(dock);
	}
	return true;
}

static bool on_toggle_webrtc_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	gstreamer_master_dock_t *dock = (gstreamer_master_dock_t *)data;
	if (!dock) dock = &g_master_dock;

	if (dock->webrtc_output && obs_output_active(dock->webrtc_output)) {
		stop_webrtc_destination(dock);
	} else {
		start_webrtc_destination(dock);
	}
	return true;
}

static bool on_toggle_custom_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	gstreamer_master_dock_t *dock = (gstreamer_master_dock_t *)data;
	if (!dock) dock = &g_master_dock;

	if (dock->custom_output && obs_output_active(dock->custom_output)) {
		stop_custom_destination(dock);
	} else {
		start_custom_destination(dock);
	}
	return true;
}

/* --------------------------------------------------------------------------
 * Dynamic Visibility Modified Callback
 * -------------------------------------------------------------------------- */
static bool on_source_type_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const char *st = obs_data_get_string(settings, "source_type");
	bool is_scene  = (st && strcmp(st, "Scene") == 0);
	bool is_source = (st && strcmp(st, "Source") == 0);

	obs_property_t *p_scene = obs_properties_get(props, "scene_name");
	obs_property_t *p_source = obs_properties_get(props, "source_name");

	if (p_scene) obs_property_set_visible(p_scene, is_scene);
	if (p_source) obs_property_set_visible(p_source, is_source);
	return true;
}

/* --------------------------------------------------------------------------
 * Core Properties Construction (All Destinations on One Screen)
 * -------------------------------------------------------------------------- */
obs_properties_t *gstreamer_master_pipeline_get_properties(void *data)
{
	gstreamer_master_dock_t *dock = (gstreamer_master_dock_t *)data;
	if (!dock) dock = &g_master_dock;

	obs_properties_t *props = obs_properties_create();

	/* ----------------------------------------------------------------------
	 * PART 1: MASTER CAPTURE & HARDWARE ENCODER (Source Level)
	 * ---------------------------------------------------------------------- */
	obs_properties_add_text(props, "pipeline_name", "Pipeline Name", OBS_TEXT_DEFAULT);

	obs_property_t *st = obs_properties_add_list(props, "source_type", "Source Type",
	                                             OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(st, "Program Output", "Program Output");
	obs_property_list_add_string(st, "Scene", "Scene");
	obs_property_list_add_string(st, "Source", "Source");
	obs_property_set_modified_callback(st, on_source_type_modified);

	obs_property_t *sc = obs_properties_add_list(props, "scene_name", "Target Scene",
	                                             OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_enum_scenes(enum_scenes_cb, sc);

	obs_property_t *so = obs_properties_add_list(props, "source_name", "Target Source",
	                                             OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(enum_sources_cb, so);

	obs_property_t *rm = obs_properties_add_list(props, "render_mode", "Capture Callback",
	                                             OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(rm, "Render Callback (GPU Direct Texture)", "Render Callback (GPU Direct Texture)");
	obs_property_list_add_string(rm, "Filter Callback (Private Filter Tap)", "Filter Callback (Private Filter Tap)");
	obs_property_list_add_string(rm, "OBS Texture Encoder (GPU Texture)", "OBS Texture Encoder");
	obs_property_list_add_string(rm, "OBS Output (CPU RAM)", "OBS Output (CPU RAM)");

	obs_property_t *enc = obs_properties_add_list(props, "gpu_encoder_type", "Hardware Encoder",
	                                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(enc, "Auto Detect", "auto");
	obs_property_list_add_string(enc, "NVIDIA NVENC", "nvenc");
	obs_property_list_add_string(enc, "Intel / AMD QuickSync (VAAPI)", "vaapi");
	obs_property_list_add_string(enc, "Apple VideoToolbox", "vtenc");
	obs_property_list_add_string(enc, "Software (x264enc)", "x264enc");

	obs_properties_add_int(props, "bitrate_kbps", "Master Bitrate (kbps)", 500, 50000, 500);
	obs_properties_add_int(props, "keyint_sec", "Keyframe Interval (sec)", 1, 10, 1);
	obs_properties_add_text(props, "master_pipeline", "Master GStreamer Pipeline", OBS_TEXT_MULTILINE);

	/* ----------------------------------------------------------------------
	 * PART 2: DESTINATIONS (Simultaneous View & Controls for All Outputs)
	 * ---------------------------------------------------------------------- */
	obs_properties_add_button(props, "btn_start_all", "▶ Start All Active Destinations", on_start_all_clicked);
	obs_properties_add_button(props, "btn_stop_all", "⏹ Stop All Destinations", on_stop_all_clicked);

	/* Destination 1: RTSP Server Group */
	obs_properties_t *rtsp_props = obs_properties_create();
	obs_properties_add_bool(rtsp_props, "rtsp_enabled", "Enable RTSP Server");
	obs_properties_add_text(rtsp_props, "rtsp_service", "RTSP Port", OBS_TEXT_DEFAULT);
	obs_properties_add_text(rtsp_props, "rtsp_mount", "RTSP Mount Path", OBS_TEXT_DEFAULT);
	obs_properties_add_text(rtsp_props, "rtsp_pipeline", "Payloader Pipeline", OBS_TEXT_MULTILINE);
	obs_properties_add_bool(rtsp_props, "rtsp_autostart", "Auto-Start when OBS Launches");
	obs_properties_add_button(rtsp_props, "rtsp_toggle", "Start / Stop RTSP", on_toggle_rtsp_clicked);
	obs_properties_add_group(props, "group_rtsp", "Destination 1: RTSP Server", OBS_GROUP_NORMAL, rtsp_props);

	/* Destination 2: WebRTC (WHEP) Group */
	obs_properties_t *webrtc_props = obs_properties_create();
	obs_properties_add_bool(webrtc_props, "webrtc_enabled", "Enable WebRTC Server");
	obs_properties_add_text(webrtc_props, "webrtc_http_port", "WebRTC HTTP Port", OBS_TEXT_DEFAULT);
	obs_properties_add_text(webrtc_props, "webrtc_stun_server", "STUN Server URL", OBS_TEXT_DEFAULT);
	obs_properties_add_text(webrtc_props, "webrtc_web_root", "Web Root Path", OBS_TEXT_DEFAULT);
	obs_properties_add_bool(webrtc_props, "webrtc_autostart", "Auto-Start when OBS Launches");
	obs_properties_add_button(webrtc_props, "webrtc_toggle", "Start / Stop WebRTC", on_toggle_webrtc_clicked);
	obs_properties_add_group(props, "group_webrtc", "Destination 2: WebRTC (WHEP Embedded)", OBS_GROUP_NORMAL, webrtc_props);

	/* Destination 3: Custom GStreamer Pipeline Group */
	obs_properties_t *custom_props = obs_properties_create();
	obs_properties_add_bool(custom_props, "custom_enabled", "Enable Custom Pipeline");
	obs_properties_add_text(custom_props, "custom_pipeline", "Custom Sink Pipeline", OBS_TEXT_MULTILINE);
	obs_properties_add_bool(custom_props, "custom_autostart", "Auto-Start when OBS Launches");
	obs_properties_add_button(custom_props, "custom_toggle", "Start / Stop Custom Sink", on_toggle_custom_clicked);
	obs_properties_add_group(props, "group_custom", "Destination 3: Custom Sink Pipeline", OBS_GROUP_NORMAL, custom_props);

	return props;
}

void gstreamer_master_pipeline_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline_name", "GStreamer Master Pipeline");
	obs_data_set_default_string(settings, "source_type", "Program Output");
	obs_data_set_default_string(settings, "render_mode", "Render Callback (GPU Direct Texture)");
	obs_data_set_default_string(settings, "gpu_encoder_type", "auto");
	obs_data_set_default_int(settings, "bitrate_kbps", 4000);
	obs_data_set_default_int(settings, "keyint_sec", 1);
	obs_data_set_default_string(settings, "master_pipeline", DEFAULT_MASTER_PIPELINE);

	/* RTSP Defaults */
	obs_data_set_default_bool(settings, "rtsp_enabled", true);
	obs_data_set_default_string(settings, "rtsp_service", "8554");
	obs_data_set_default_string(settings, "rtsp_mount", "/live");
	obs_data_set_default_string(settings, "rtsp_pipeline", DEFAULT_RTSP_PIPELINE);
	obs_data_set_default_bool(settings, "rtsp_autostart", false);

	/* WebRTC Defaults */
	obs_data_set_default_bool(settings, "webrtc_enabled", false);
	obs_data_set_default_string(settings, "webrtc_http_port", "8888");
	obs_data_set_default_string(settings, "webrtc_stun_server", "");
	obs_data_set_default_string(settings, "webrtc_web_root", "");
	obs_data_set_default_bool(settings, "webrtc_autostart", false);

	/* Custom Defaults */
	obs_data_set_default_bool(settings, "custom_enabled", false);
	obs_data_set_default_string(settings, "custom_pipeline", "autovideosink sync=false");
	obs_data_set_default_bool(settings, "custom_autostart", false);
}

void gstreamer_master_pipeline_update(void *data, obs_data_t *settings)
{
	gstreamer_master_dock_t *dock = (gstreamer_master_dock_t *)data;
	if (!dock) dock = &g_master_dock;

	bfree(dock->pipeline_name);
	bfree(dock->source_type);
	bfree(dock->source_name);
	bfree(dock->scene_name);
	bfree(dock->render_mode);
	bfree(dock->gpu_encoder_type);
	bfree(dock->master_pipeline);

	dock->pipeline_name    = bstrdup(obs_data_get_string(settings, "pipeline_name"));
	dock->source_type      = bstrdup(obs_data_get_string(settings, "source_type"));
	dock->source_name      = bstrdup(obs_data_get_string(settings, "source_name"));
	dock->scene_name       = bstrdup(obs_data_get_string(settings, "scene_name"));
	dock->render_mode      = bstrdup(obs_data_get_string(settings, "render_mode"));
	dock->gpu_encoder_type = bstrdup(obs_data_get_string(settings, "gpu_encoder_type"));
	dock->bitrate_kbps     = (int)obs_data_get_int(settings, "bitrate_kbps");
	dock->keyint_sec       = (int)obs_data_get_int(settings, "keyint_sec");
	dock->master_pipeline  = bstrdup(obs_data_get_string(settings, "master_pipeline"));

	/* RTSP */
	dock->rtsp_enabled     = obs_data_get_bool(settings, "rtsp_enabled");
	bfree(dock->rtsp_service);
	bfree(dock->rtsp_mount);
	bfree(dock->rtsp_pipeline);
	dock->rtsp_service     = bstrdup(obs_data_get_string(settings, "rtsp_service"));
	dock->rtsp_mount       = bstrdup(obs_data_get_string(settings, "rtsp_mount"));
	dock->rtsp_pipeline    = bstrdup(obs_data_get_string(settings, "rtsp_pipeline"));
	dock->rtsp_autostart   = obs_data_get_bool(settings, "rtsp_autostart");

	/* WebRTC */
	dock->webrtc_enabled   = obs_data_get_bool(settings, "webrtc_enabled");
	bfree(dock->webrtc_http_port);
	bfree(dock->webrtc_stun_server);
	bfree(dock->webrtc_web_root);
	dock->webrtc_http_port = bstrdup(obs_data_get_string(settings, "webrtc_http_port"));
	dock->webrtc_stun_server = bstrdup(obs_data_get_string(settings, "webrtc_stun_server"));
	dock->webrtc_web_root  = bstrdup(obs_data_get_string(settings, "webrtc_web_root"));
	dock->webrtc_autostart = obs_data_get_bool(settings, "webrtc_autostart");

	/* Custom */
	dock->custom_enabled   = obs_data_get_bool(settings, "custom_enabled");
	bfree(dock->custom_pipeline);
	dock->custom_pipeline  = bstrdup(obs_data_get_string(settings, "custom_pipeline"));
	dock->custom_autostart = obs_data_get_bool(settings, "custom_autostart");
}

/* --------------------------------------------------------------------------
 * OBS Source / Filter Registration
 * -------------------------------------------------------------------------- */
static const char *gst_master_filter_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Master Pipeline Hub";
}

static void *gst_master_filter_create(obs_data_t *settings, obs_source_t *source)
{
	gstreamer_master_dock_t *dock = bzalloc(sizeof(gstreamer_master_dock_t));
	dock->source_obj = source;
	gstreamer_master_pipeline_update(dock, settings);
	return dock;
}

static void gst_master_filter_destroy(void *data)
{
	gstreamer_master_dock_t *dock = (gstreamer_master_dock_t *)data;
	if (!dock) return;

	stop_rtsp_destination(dock);
	stop_webrtc_destination(dock);
	stop_custom_destination(dock);

	bfree(dock->pipeline_name);
	bfree(dock->source_type);
	bfree(dock->source_name);
	bfree(dock->scene_name);
	bfree(dock->render_mode);
	bfree(dock->gpu_encoder_type);
	bfree(dock->master_pipeline);
	bfree(dock->rtsp_service);
	bfree(dock->rtsp_mount);
	bfree(dock->rtsp_pipeline);
	bfree(dock->webrtc_http_port);
	bfree(dock->webrtc_stun_server);
	bfree(dock->webrtc_web_root);
	bfree(dock->custom_pipeline);
	bfree(dock);
}

static struct obs_source_info gstreamer_master_filter_info = {
	.id           = "gstreamer-master-pipeline-filter",
	.type         = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name     = gst_master_filter_get_name,
	.create       = gst_master_filter_create,
	.destroy      = gst_master_filter_destroy,
	.get_defaults = gstreamer_master_pipeline_get_defaults,
	.get_properties = gstreamer_master_pipeline_get_properties,
	.update       = gstreamer_master_pipeline_update,
};

static struct obs_source_info gstreamer_master_source_info = {
	.id           = "gstreamer-master-pipeline-source",
	.type         = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name     = gst_master_filter_get_name,
	.create       = gst_master_filter_create,
	.destroy      = gst_master_filter_destroy,
	.get_defaults = gstreamer_master_pipeline_get_defaults,
	.get_properties = gstreamer_master_pipeline_get_properties,
	.update       = gstreamer_master_pipeline_update,
};

#if HAVE_OBS_FRONTEND
static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		blog(LOG_INFO, "[obs-gstreamer-dock] OBS finished loading: evaluating auto-start destinations...");
		if (g_master_dock.rtsp_autostart) {
			blog(LOG_INFO, "[obs-gstreamer-dock] Auto-starting RTSP destination...");
			start_rtsp_destination(&g_master_dock);
		}
		if (g_master_dock.webrtc_autostart) {
			blog(LOG_INFO, "[obs-gstreamer-dock] Auto-starting WebRTC destination...");
			start_webrtc_destination(&g_master_dock);
		}
		if (g_master_dock.custom_autostart) {
			blog(LOG_INFO, "[obs-gstreamer-dock] Auto-starting Custom destination...");
			start_custom_destination(&g_master_dock);
		}
	}
}
#endif

/* --------------------------------------------------------------------------
 * Module Lifecycle Entry Points
 * -------------------------------------------------------------------------- */
void gstreamer_dock_register(void)
{
	obs_register_source(&gstreamer_master_filter_info);
	obs_register_source(&gstreamer_master_source_info);
	blog(LOG_INFO, "[obs-gstreamer-dock] Registered pure C Master Pipeline Filter & Source modules");

#if HAVE_OBS_FRONTEND
	obs_frontend_add_event_callback(on_frontend_event, NULL);
#endif
}

void gstreamer_dock_unregister(void)
{
#if HAVE_OBS_FRONTEND
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
#endif

	stop_rtsp_destination(&g_master_dock);
	stop_webrtc_destination(&g_master_dock);
	stop_custom_destination(&g_master_dock);
	blog(LOG_INFO, "[obs-gstreamer-dock] Unregistered Master Pipeline modules");
}
