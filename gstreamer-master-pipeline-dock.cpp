/*
 * obs-gstreamer: Master Pipeline Hub Dock (OBS 32+ / Qt 6)
 *
 * Implements a native OBS Dock using obs_frontend_add_dock_by_id:
 * - Main Window: Listed in OBS "Docks" menu, displays all configured Master Hubs.
 * - Child Window (Double-click / Configure): Opens detailed editor for master encoder & destinations.
 * - Destination Editor Dialog: Edit individual RTSP, WebRTC, or Custom pipeline properties.
 * - Scene Collection Persistence: Saves & restores Master Hub settings through OBS.
 * - Live Status Export: Writes JSON telemetry to /tmp/obs_gstreamer_hubs.json for Web UI monitoring.
 */

#include <obs.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <gst/gst.h>

#include "gstreamer-master-pipeline.h"
#include "gstreamer-rtspserver.h"
#include "gstreamer-webrtc.h"
#include "gstreamer-render-callback.h"
#include "gstreamer-encoder-callback.h"
#include "gstreamer-output-callback.h"
#include "gstreamer-filter-callback.h"
#include "gstreamer-helper.h"

#include <QtCore/qglobal.h>

extern "C" {
bool obs_frontend_add_dock_by_id(const char *id, const char *title, void *dock) __attribute__((weak));
}

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QFile>
#include <QtCore/QDateTime>

#include <QtGui/QColor>
#include <QtGui/QBrush>
#include <QtWidgets/QWidget>
#include <QtWidgets/QDialog>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QListWidgetItem>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QToolButton>
#include <QtGui/QAction>
#include <QtGui/QIcon>
#include <QtCore/QTimer>

#include <vector>
#include <string>
#include <memory>

#define DEFAULT_MASTER_PIPELINE \
	"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true " \
	"caps=\"video/x-raw, format=RGBx, width=%u, height=%u, framerate=%u/%u\" ! " \
	"videoconvert ! video/x-raw, format=I420 ! x264enc tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=30 bitrate=4000 ! " \
	"h264parse config-interval=-1 ! video/x-h264, stream-format=byte-stream, alignment=au ! " \
	"appsink name=hub_appsink sync=false drop=true max-buffers=1"

#define DEFAULT_RTSP_PIPELINE \
	"( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true " \
	"caps=\"video/x-h264, stream-format=byte-stream, alignment=au\" ! " \
	"h264parse config-interval=-1 ! rtph264pay name=pay0 pt=96 config-interval=1 )"

/* --------------------------------------------------------------------------
 * Destination Data Structures & Types
 * -------------------------------------------------------------------------- */
enum DestinationType {
	DEST_TYPE_RTSP = 0,
	DEST_TYPE_WEBRTC,
	DEST_TYPE_CUSTOM
};

struct GstDestination {
	DestinationType type;
	std::string name;
	bool enabled;
	bool autostart;
	bool is_active;

	/* RTSP specifics */
	std::string rtsp_port;
	std::string rtsp_mount;
	std::string rtsp_pipeline;
	struct gst_hub_branch *rtsp_branch;

	/* WebRTC specifics */
	std::string webrtc_port;
	std::string webrtc_stun;
	std::string webrtc_web_root;
	struct gstreamer_webrtc *webrtc_branch;

	/* Custom Pipeline specifics */
	std::string custom_pipeline;
	struct gst_hub_branch *custom_branch;

	GstDestination()
		: type(DEST_TYPE_RTSP),
		  name("New Destination"),
		  enabled(true),
		  autostart(false),
		  is_active(false),
		  rtsp_port("8554"),
		  rtsp_mount("/live"),
		  rtsp_pipeline(DEFAULT_RTSP_PIPELINE),
		  rtsp_branch(nullptr),
		  webrtc_port("8888"),
		  webrtc_stun(""),
		  webrtc_web_root(""),
		  webrtc_branch(nullptr),
		  custom_pipeline("autovideosink sync=false"),
		  custom_branch(nullptr)
	{
	}
};

struct GstMasterHub {
	std::string name;
	std::string source_type;      /* "Program Output", "Scene", "Source" */
	std::string source_name;
	std::string scene_name;
	std::string render_mode;      /* Deprecated compatibility */
	std::string capture_method;   /* 4 Callbacks */
	std::string gpu_encoder_type; /* "auto", "nvenc", "vaapi", "vtenc", "x264enc" */
	int bitrate_kbps;
	int keyint_sec;
	std::string master_pipeline;

	bool is_active;
	gst_render_callback_t *active_render_cb;
	gst_encoder_callback_t *active_encoder_cb;
	gst_output_callback_t *active_output_cb;
	gst_filter_callback_t *active_filter_cb;

	std::vector<std::shared_ptr<GstDestination>> destinations;

	GstMasterHub()
		: name("Master Hub"),
		  source_type("Program Output"),
		  source_name(""),
		  scene_name(""),
		  render_mode("Render Callback (GPU Direct Texture)"),
		  capture_method("Render Callback (GPU Direct Texture)"),
		  gpu_encoder_type("auto"),
		  bitrate_kbps(4000),
		  keyint_sec(1),
		  master_pipeline(""),
		  is_active(false),
		  active_render_cb(nullptr),
		  active_encoder_cb(nullptr),
		  active_output_cb(nullptr),
		  active_filter_cb(nullptr)
	{
	}
};

static inline bool is_hub_pipeline_active(const GstMasterHub *hub)
{
	return hub && (hub->active_render_cb != nullptr ||
	               hub->active_encoder_cb != nullptr ||
	               hub->active_output_cb != nullptr ||
	               hub->active_filter_cb != nullptr);
}

/* Global Master Hubs managed by the Dock */
static std::vector<std::shared_ptr<GstMasterHub>> g_master_hubs;

class GstMasterDockWidget;
static GstMasterDockWidget *g_dock_widget = nullptr;
static void stop_destination(GstMasterHub *hub, GstDestination *dest);

/* --------------------------------------------------------------------------
 * Persistence & Telemetry Helpers
 * -------------------------------------------------------------------------- */
static void export_dock_status_json()
{
	QJsonArray hubsArray;
	for (const auto &hub : g_master_hubs) {
		QJsonObject hubObj;
		hubObj["name"] = QString::fromStdString(hub->name);
		hubObj["source_type"] = QString::fromStdString(hub->source_type);
		hubObj["source_name"] = QString::fromStdString(hub->source_name);
		hubObj["scene_name"] = QString::fromStdString(hub->scene_name);
		hubObj["capture_method"] = QString::fromStdString(hub->capture_method);
		hubObj["gpu_encoder_type"] = QString::fromStdString(hub->gpu_encoder_type);
		hubObj["bitrate_kbps"] = hub->bitrate_kbps;
		hubObj["is_active"] = hub->is_active || is_hub_pipeline_active(hub.get());

		QJsonArray destsArray;
		for (const auto &d : hub->destinations) {
			QJsonObject dObj;
			dObj["name"] = QString::fromStdString(d->name);
			dObj["type"] = (d->type == DEST_TYPE_RTSP) ? "RTSP" :
			               (d->type == DEST_TYPE_WEBRTC) ? "WebRTC" : "Custom";
			dObj["enabled"] = d->enabled;
			dObj["autostart"] = d->autostart;
			dObj["is_active"] = d->is_active;
			dObj["rtsp_port"] = QString::fromStdString(d->rtsp_port);
			dObj["rtsp_mount"] = QString::fromStdString(d->rtsp_mount);
			dObj["webrtc_port"] = QString::fromStdString(d->webrtc_port);
			dObj["custom_pipeline"] = QString::fromStdString(d->custom_pipeline);
			destsArray.append(dObj);
		}
		hubObj["destinations"] = destsArray;
		hubsArray.append(hubObj);
	}

	QJsonObject rootObj;
	rootObj["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
	rootObj["master_hubs"] = hubsArray;

	QFile file("/tmp/obs_gstreamer_hubs.json");
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(rootObj).toJson(QJsonDocument::Indented));
		file.close();
	}
}

static std::string read_data_string(obs_data_t *data, const char *key, const char *fallback = "")
{
	const char *value = obs_data_get_string(data, key);
	return value ? value : fallback;
}

static void save_dock_config(obs_data_t *save_data)
{
	obs_data_array_t *hubsArray = obs_data_array_create();
	for (const auto &hub : g_master_hubs) {
		obs_data_t *hubObj = obs_data_create();
		obs_data_set_string(hubObj, "name", hub->name.c_str());
		obs_data_set_string(hubObj, "source_type", hub->source_type.c_str());
		obs_data_set_string(hubObj, "source_name", hub->source_name.c_str());
		obs_data_set_string(hubObj, "scene_name", hub->scene_name.c_str());
		obs_data_set_string(hubObj, "capture_method", hub->capture_method.c_str());
		obs_data_set_string(hubObj, "gpu_encoder_type", hub->gpu_encoder_type.c_str());
		obs_data_set_int(hubObj, "bitrate_kbps", hub->bitrate_kbps);
		obs_data_set_int(hubObj, "keyint_sec", hub->keyint_sec);
		obs_data_set_string(hubObj, "master_pipeline", hub->master_pipeline.c_str());

		obs_data_array_t *destsArray = obs_data_array_create();
		for (const auto &d : hub->destinations) {
			obs_data_t *dObj = obs_data_create();
			obs_data_set_string(dObj, "name", d->name.c_str());
			obs_data_set_int(dObj, "type", (int)d->type);
			obs_data_set_bool(dObj, "enabled", d->enabled);
			obs_data_set_bool(dObj, "autostart", d->autostart);
			obs_data_set_string(dObj, "rtsp_port", d->rtsp_port.c_str());
			obs_data_set_string(dObj, "rtsp_mount", d->rtsp_mount.c_str());
			obs_data_set_string(dObj, "rtsp_pipeline", d->rtsp_pipeline.c_str());
			obs_data_set_string(dObj, "webrtc_port", d->webrtc_port.c_str());
			obs_data_set_string(dObj, "webrtc_stun", d->webrtc_stun.c_str());
			obs_data_set_string(dObj, "webrtc_web_root", d->webrtc_web_root.c_str());
			obs_data_set_string(dObj, "custom_pipeline", d->custom_pipeline.c_str());
			obs_data_array_push_back(destsArray, dObj);
			obs_data_release(dObj);
		}
		obs_data_set_array(hubObj, "destinations", destsArray);
		obs_data_array_push_back(hubsArray, hubObj);
		obs_data_array_release(destsArray);
		obs_data_release(hubObj);
	}

	obs_data_t *dock_data = obs_data_create();
	obs_data_set_array(dock_data, "master_hubs", hubsArray);
	obs_data_set_obj(save_data, "gstreamer-master-pipeline-dock", dock_data);
	obs_data_release(dock_data);
	obs_data_array_release(hubsArray);
}

static void load_dock_config(obs_data_t *save_data)
{
	for (auto &hub : g_master_hubs) {
		for (auto &dest : hub->destinations)
			stop_destination(hub.get(), dest.get());
	}
	g_master_hubs.clear();

	obs_data_t *dock_data = obs_data_get_obj(save_data, "gstreamer-master-pipeline-dock");
	if (!dock_data)
		return;

	obs_data_array_t *hubsArray = obs_data_get_array(dock_data, "master_hubs");
	obs_data_release(dock_data);
	if (!hubsArray)
		return;

	const size_t hub_count = obs_data_array_count(hubsArray);
	for (size_t i = 0; i < hub_count; i++) {
		obs_data_t *hubObj = obs_data_array_item(hubsArray, i);
		if (!hubObj)
			continue;
		auto hub = std::make_shared<GstMasterHub>();
		hub->name = read_data_string(hubObj, "name", "Master Hub");
		if (hub->name.empty())
			hub->name = "Master Hub";
		hub->source_type = read_data_string(hubObj, "source_type", "Program Output");
		hub->source_name = read_data_string(hubObj, "source_name");
		hub->scene_name = read_data_string(hubObj, "scene_name");
		hub->capture_method = read_data_string(hubObj, "capture_method", "Render Callback (GPU Direct Texture)");
		hub->gpu_encoder_type = read_data_string(hubObj, "gpu_encoder_type", "auto");
		hub->bitrate_kbps = obs_data_has_user_value(hubObj, "bitrate_kbps")
			? (int)obs_data_get_int(hubObj, "bitrate_kbps")
			: 4000;
		hub->keyint_sec = obs_data_has_user_value(hubObj, "keyint_sec")
			? (int)obs_data_get_int(hubObj, "keyint_sec")
			: 1;
		hub->master_pipeline = read_data_string(hubObj, "master_pipeline");

		obs_data_array_t *destsArray = obs_data_get_array(hubObj, "destinations");
		if (destsArray) {
			const size_t dest_count = obs_data_array_count(destsArray);
			for (size_t j = 0; j < dest_count; j++) {
				obs_data_t *dObj = obs_data_array_item(destsArray, j);
				if (!dObj)
					continue;
				auto dest = std::make_shared<GstDestination>();
				dest->name = read_data_string(dObj, "name", "Destination");
				if (dest->name.empty())
					dest->name = "Destination";
				const int type = (int)obs_data_get_int(dObj, "type");
				dest->type = (type >= DEST_TYPE_RTSP && type <= DEST_TYPE_CUSTOM)
					? (DestinationType)type
					: DEST_TYPE_RTSP;
				dest->enabled = obs_data_has_user_value(dObj, "enabled")
					? obs_data_get_bool(dObj, "enabled")
					: true;
				dest->autostart = obs_data_get_bool(dObj, "autostart");
				dest->rtsp_port = read_data_string(dObj, "rtsp_port", "8554");
				dest->rtsp_mount = read_data_string(dObj, "rtsp_mount", "/live");
				dest->rtsp_pipeline = read_data_string(dObj, "rtsp_pipeline", DEFAULT_RTSP_PIPELINE);
				dest->webrtc_port = read_data_string(dObj, "webrtc_port", "8888");
				dest->webrtc_stun = read_data_string(dObj, "webrtc_stun");
				dest->webrtc_web_root = read_data_string(dObj, "webrtc_web_root");
				dest->custom_pipeline = read_data_string(dObj, "custom_pipeline", "autovideosink sync=false");

				hub->destinations.push_back(dest);
				obs_data_release(dObj);
			}
			obs_data_array_release(destsArray);
		}

		g_master_hubs.push_back(hub);
		obs_data_release(hubObj);
	}
	obs_data_array_release(hubsArray);
}

/* --------------------------------------------------------------------------
 * OBS Scene / Source Enumeration Helpers
 * -------------------------------------------------------------------------- */
static std::vector<std::string> get_obs_scene_names()
{
	std::vector<std::string> scenes;
	struct obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		obs_source_t *src = list.sources.array[i];
		if (src) {
			const char *name = obs_source_get_name(src);
			if (name && name[0])
				scenes.push_back(name);
		}
	}
	obs_frontend_source_list_free(&list);
	return scenes;
}

static bool enum_source_cb(void *param, obs_source_t *source)
{
	auto *sources = static_cast<std::vector<std::string> *>(param);
	if (source) {
		const char *name = obs_source_get_name(source);
		if (name && name[0])
			sources->push_back(name);
	}
	return true;
}

static std::vector<std::string> get_obs_source_names()
{
	std::vector<std::string> sources;
	obs_enum_sources(enum_source_cb, &sources);
	return sources;
}

/* --------------------------------------------------------------------------
 * Destination Control Helpers
 * -------------------------------------------------------------------------- */
static void cleanup_unused_master_pipeline(GstMasterHub *hub)
{
	if (!hub)
		return;

	bool any_active = false;
	for (const auto &d : hub->destinations) {
		if (d->is_active) {
			any_active = true;
			break;
		}
	}

	if (!any_active && is_hub_pipeline_active(hub)) {
		blog(LOG_INFO, "[obs-gstreamer-dock] All destinations stopped: tearing down master pipeline for '%s'",
			hub->name.c_str());
		if (hub->active_render_cb) {
			gst_render_callback_destroy(hub->active_render_cb);
			hub->active_render_cb = nullptr;
		}
		if (hub->active_encoder_cb) {
			gst_encoder_callback_destroy(hub->active_encoder_cb);
			hub->active_encoder_cb = nullptr;
		}
		if (hub->active_output_cb) {
			gst_output_callback_destroy(hub->active_output_cb);
			hub->active_output_cb = nullptr;
		}
		if (hub->active_filter_cb) {
			gst_filter_callback_destroy(hub->active_filter_cb);
			hub->active_filter_cb = nullptr;
		}
	}
}

static void stop_destination(GstMasterHub *hub, GstDestination *dest)
{
	if (!dest->is_active)
		return;

	if (dest->type == DEST_TYPE_RTSP && dest->rtsp_branch) {
		blog(LOG_INFO, "[obs-gstreamer-dock] Stopping RTSP branch (%s:%s)",
			dest->rtsp_port.c_str(), dest->rtsp_mount.c_str());
		gst_render_hub_stop_branch(dest->rtsp_branch);
		dest->rtsp_branch = nullptr;
	} else if (dest->type == DEST_TYPE_WEBRTC && dest->webrtc_branch) {
		blog(LOG_INFO, "[obs-gstreamer-dock] Stopping WebRTC branch (port: %s)",
			dest->webrtc_port.c_str());
		gst_webrtc_branch_stop(dest->webrtc_branch);
		dest->webrtc_branch = nullptr;
	} else if (dest->type == DEST_TYPE_CUSTOM && dest->custom_branch) {
		blog(LOG_INFO, "[obs-gstreamer-dock] Stopping Custom pipeline branch ('%s')",
			dest->name.c_str());
		gst_render_hub_stop_branch(dest->custom_branch);
		dest->custom_branch = nullptr;
	}

	dest->is_active = false;
	cleanup_unused_master_pipeline(hub);
	export_dock_status_json();
}

static gst_master_pipeline_t *ensure_master_pipeline(GstMasterHub *hub)
{
	if (hub->active_render_cb)
		return gst_render_callback_get_pipeline(hub->active_render_cb);
	if (hub->active_encoder_cb)
		return gst_encoder_callback_get_pipeline(hub->active_encoder_cb);
	if (hub->active_output_cb)
		return gst_output_callback_get_pipeline(hub->active_output_cb);
	if (hub->active_filter_cb)
		return gst_filter_callback_get_pipeline(hub->active_filter_cb);

	gst_source_type_t stype = GST_SOURCE_MAIN_OUTPUT;
	if (hub->source_type == "Scene")
		stype = GST_SOURCE_SCENE;
	else if (hub->source_type == "Source")
		stype = GST_SOURCE_SOURCE;

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	uint32_t fps_num = ovi.fps_num ? ovi.fps_num : 60;
	uint32_t fps_den = ovi.fps_den ? ovi.fps_den : 1;

	const char *target_scene = hub->scene_name.c_str();
	const char *target_source = hub->source_name.c_str();
	const char *custom_pipe = hub->master_pipeline.c_str();
	const char *encoder_type = hub->gpu_encoder_type.c_str();

	blog(LOG_INFO, "[obs-gstreamer-dock] Starting Master Pipeline for '%s' (method='%s', encoder=%s, bitrate=%d kbps)",
		hub->name.c_str(), hub->capture_method.c_str(), encoder_type, hub->bitrate_kbps);

	if (hub->capture_method == "Encoder Callback (GPU Hardware Texture Encoder)") {
		hub->active_encoder_cb = gst_encoder_callback_create(
			stype, target_scene, target_source, custom_pipe, encoder_type,
			hub->bitrate_kbps, hub->keyint_sec, fps_num, fps_den
		);
		if (hub->active_encoder_cb)
			return gst_encoder_callback_get_pipeline(hub->active_encoder_cb);
	} else if (hub->capture_method == "Filter Callback (Source Video Filter Tap)") {
		hub->active_filter_cb = gst_filter_callback_create(
			stype, target_scene, target_source, custom_pipe, encoder_type,
			hub->bitrate_kbps, hub->keyint_sec, fps_num, fps_den
		);
		if (hub->active_filter_cb)
			return gst_filter_callback_get_pipeline(hub->active_filter_cb);
	} else if (hub->capture_method == "Output Callback (Raw Video/Audio Frames)") {
		hub->active_output_cb = gst_output_callback_create(
			stype, target_scene, target_source, custom_pipe, encoder_type,
			hub->bitrate_kbps, hub->keyint_sec, fps_num, fps_den
		);
		if (hub->active_output_cb)
			return gst_output_callback_get_pipeline(hub->active_output_cb);
	} else {
		/* Default: Render Callback (GPU Direct Texture) */
		hub->active_render_cb = gst_render_callback_create(
			stype, target_scene, target_source, custom_pipe, encoder_type,
			hub->bitrate_kbps, hub->keyint_sec, fps_num, fps_den
		);
		if (hub->active_render_cb)
			return gst_render_callback_get_pipeline(hub->active_render_cb);
	}

	blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to create Master Pipeline callback for '%s'", hub->name.c_str());
	return nullptr;
}

static bool start_destination(GstMasterHub *hub, GstDestination *dest)
{
	stop_destination(hub, dest);

	gst_master_pipeline_t *master_pipe = ensure_master_pipeline(hub);
	if (!master_pipe) {
		blog(LOG_ERROR, "[obs-gstreamer-dock] Master pipeline object is NULL");
		return false;
	}

	if (dest->type == DEST_TYPE_RTSP) {
		blog(LOG_INFO, "[obs-gstreamer-dock] Starting RTSP destination on port %s%s linked to master pipeline '%s'...",
			dest->rtsp_port.c_str(), dest->rtsp_mount.c_str(), hub->name.c_str());

		dest->rtsp_branch = gst_rtsp_branch_start(
			master_pipe,
			dest->name.c_str(),
			dest->rtsp_port.c_str(),
			dest->rtsp_mount.c_str(),
			hub->gpu_encoder_type.c_str(),
			dest->rtsp_pipeline.c_str()
		);

		if (!dest->rtsp_branch) {
			blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to start RTSP branch");
			cleanup_unused_master_pipeline(hub);
			return false;
		}
		dest->is_active = true;
		export_dock_status_json();

		g_timeout_add(350, [](gpointer) -> gboolean {
			obs_enum_sources([](void *, obs_source_t *src) -> bool {
				if (!src) return true;
				const char *id = obs_source_get_id(src);
				if (id && (strcmp(id, "gstreamer-source") == 0 || strcmp(id, "gstreamer-source-video") == 0)) {
					obs_data_t *s = obs_source_get_settings(src);
					if (s) {
						const char *pipe = obs_data_get_string(s, "pipeline");
						if (pipe && strstr(pipe, "rtsp://") != nullptr) {
							obs_source_update(src, s);
						}
						obs_data_release(s);
					}
				}
				return true;
			}, nullptr);
			return G_SOURCE_REMOVE;
		}, nullptr);

		return true;
	} else if (dest->type == DEST_TYPE_WEBRTC) {
		blog(LOG_INFO, "[obs-gstreamer-dock] Starting WebRTC destination on port %s linked to master pipeline '%s'...",
			dest->webrtc_port.c_str(), hub->name.c_str());

		dest->webrtc_branch = gst_webrtc_branch_start(
			master_pipe,
			dest->name.c_str(),
			dest->webrtc_port.c_str(),
			dest->webrtc_stun.c_str(),
			dest->webrtc_web_root.c_str(),
			hub->gpu_encoder_type.c_str()
		);

		if (!dest->webrtc_branch) {
			blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to start WebRTC branch");
			cleanup_unused_master_pipeline(hub);
			return false;
		}
		dest->is_active = true;
		blog(LOG_INFO, "[obs-gstreamer-dock] WebRTC branch active on port %s", dest->webrtc_port.c_str());
		export_dock_status_json();
		return true;
	} else if (dest->type == DEST_TYPE_CUSTOM) {
		blog(LOG_INFO, "[obs-gstreamer-dock] Starting Custom pipeline destination '%s' linked to master pipeline '%s'...",
			dest->name.c_str(), hub->name.c_str());

		dest->custom_branch = gst_custom_branch_start(
			master_pipe,
			dest->name.c_str(),
			hub->gpu_encoder_type.c_str(),
			dest->custom_pipeline.empty() ? "autovideosink sync=false" : dest->custom_pipeline.c_str()
		);

		if (!dest->custom_branch) {
			blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to start Custom pipeline branch");
			cleanup_unused_master_pipeline(hub);
			return false;
		}
		dest->is_active = true;
		blog(LOG_INFO, "[obs-gstreamer-dock] Custom pipeline branch '%s' active", dest->name.c_str());
		export_dock_status_json();
		return true;
	}

	cleanup_unused_master_pipeline(hub);
	return false;
}

/* --------------------------------------------------------------------------
 * Helper Widget: Collapsible Section
 * -------------------------------------------------------------------------- */
class CollapsibleSection : public QWidget {
public:
	CollapsibleSection(const QString &title, bool startsExpanded = true, QWidget *parent = nullptr)
		: QWidget(parent), m_title(title), m_isExpanded(startsExpanded)
	{
		QVBoxLayout *root = new QVBoxLayout(this);
		root->setContentsMargins(0, 0, 0, 0);
		root->setSpacing(0);

		m_headerBtn = new QPushButton(this);
		m_headerBtn->setCheckable(true);
		m_headerBtn->setChecked(startsExpanded);
		m_headerBtn->setCursor(Qt::PointingHandCursor);
		m_headerBtn->setStyleSheet(
			"QPushButton {"
			"  text-align: left;"
			"  padding: 8px 12px;"
			"  font-weight: bold;"
			"  font-size: 12px;"
			"  color: #e2e8f0;"
			"  background-color: #20232c;"
			"  border: 1px solid #353a47;"
			"  border-radius: 4px;"
			"}"
			"QPushButton:hover {"
			"  background-color: #282c38;"
			"  border-color: #4f5669;"
			"  color: #ffffff;"
			"}"
			"QPushButton:checked {"
			"  border-bottom-left-radius: 0px;"
			"  border-bottom-right-radius: 0px;"
			"}"
		);
		updateHeader();

		m_contentWidget = new QWidget(this);
		m_contentWidget->setObjectName("CollapsibleContent");
		m_contentWidget->setStyleSheet(
			"QWidget#CollapsibleContent {"
			"  background-color: #161820;"
			"  border: 1px solid #353a47;"
			"  border-top: none;"
			"  border-bottom-left-radius: 4px;"
			"  border-bottom-right-radius: 4px;"
			"}"
		);
		m_contentWidget->setVisible(startsExpanded);

		root->addWidget(m_headerBtn);
		root->addWidget(m_contentWidget);

		connect(m_headerBtn, &QPushButton::clicked, [this](bool) {
			toggle();
		});
	}

	void setContentLayout(QLayout *layout)
	{
		m_contentWidget->setLayout(layout);
	}

	void toggle()
	{
		m_isExpanded = !m_isExpanded;
		m_contentWidget->setVisible(m_isExpanded);
		m_headerBtn->setChecked(m_isExpanded);
		updateHeader();
	}

	void setExpanded(bool exp)
	{
		m_isExpanded = exp;
		m_contentWidget->setVisible(exp);
		m_headerBtn->setChecked(exp);
		updateHeader();
	}

	bool isExpanded() const { return m_isExpanded; }
	QWidget *contentWidget() const { return m_contentWidget; }

private:
	void updateHeader()
	{
		QString arrow = m_isExpanded ? "▾  " : "▸  ";
		m_headerBtn->setText(arrow + m_title);
	}

	QString m_title;
	bool m_isExpanded;
	QPushButton *m_headerBtn;
	QWidget *m_contentWidget;
};

/* --------------------------------------------------------------------------
 * Dialog: Edit Individual Destination Properties
 * -------------------------------------------------------------------------- */
class GstDestinationDetailDialog : public QDialog {
public:
	GstDestinationDetailDialog(std::shared_ptr<GstDestination> dest, QWidget *parent = nullptr)
		: QDialog(parent), m_dest(dest)
	{
		setWindowTitle(QString("Edit Destination: %1").arg(QString::fromStdString(dest->name)));
		resize(500, 320);

		QVBoxLayout *layout = new QVBoxLayout(this);
		layout->setContentsMargins(14, 14, 14, 14);
		layout->setSpacing(10);

		QFormLayout *form = new QFormLayout();
		form->setSpacing(8);
		form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

		m_nameEdit = new QLineEdit(QString::fromStdString(dest->name), this);
		form->addRow("Destination Name:", m_nameEdit);

		m_typeCombo = new QComboBox(this);
		m_typeCombo->addItem("RTSP Stream", DEST_TYPE_RTSP);
		m_typeCombo->addItem("WebRTC (WHEP)", DEST_TYPE_WEBRTC);
		m_typeCombo->addItem("Custom Pipeline Sink", DEST_TYPE_CUSTOM);
		int typeIdx = m_typeCombo->findData((int)dest->type);
		if (typeIdx >= 0) m_typeCombo->setCurrentIndex(typeIdx);
		form->addRow("Type:", m_typeCombo);

		m_autostartCheck = new QCheckBox("Auto-start when OBS launches", this);
		m_autostartCheck->setChecked(dest->autostart);
		form->addRow("Autostart:", m_autostartCheck);

		/* Stacked Widget for Type-Specific Settings to eliminate empty space */
		m_typeStack = new QStackedWidget(this);

		/* RTSP Section */
		m_rtspGroup = new QGroupBox("RTSP Server Settings", this);
		QFormLayout *rtspForm = new QFormLayout(m_rtspGroup);
		rtspForm->setSpacing(8);
		rtspForm->setContentsMargins(10, 8, 10, 8);
		rtspForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		m_rtspPortEdit = new QLineEdit(QString::fromStdString(dest->rtsp_port), this);
		m_rtspMountEdit = new QLineEdit(QString::fromStdString(dest->rtsp_mount), this);
		m_rtspPipeEdit = new QLineEdit(QString::fromStdString(dest->rtsp_pipeline), this);
		rtspForm->addRow("RTSP Port:", m_rtspPortEdit);
		rtspForm->addRow("Mount Path:", m_rtspMountEdit);
		rtspForm->addRow("Payloader Pipeline:", m_rtspPipeEdit);
		m_typeStack->addWidget(m_rtspGroup);

		/* WebRTC Section */
		m_webrtcGroup = new QGroupBox("WebRTC WHEP Settings", this);
		QFormLayout *webrtcForm = new QFormLayout(m_webrtcGroup);
		webrtcForm->setSpacing(8);
		webrtcForm->setContentsMargins(10, 8, 10, 8);
		webrtcForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		m_webrtcPortEdit = new QLineEdit(QString::fromStdString(dest->webrtc_port), this);
		m_webrtcStunEdit = new QLineEdit(QString::fromStdString(dest->webrtc_stun), this);
		m_webrtcRootEdit = new QLineEdit(QString::fromStdString(dest->webrtc_web_root), this);
		webrtcForm->addRow("WHEP Port:", m_webrtcPortEdit);
		webrtcForm->addRow("STUN Server:", m_webrtcStunEdit);
		webrtcForm->addRow("Web Root Path:", m_webrtcRootEdit);
		m_typeStack->addWidget(m_webrtcGroup);

		/* Custom Pipeline Section - fills full width and height with no empty top/bottom space */
		m_customGroup = new QGroupBox("Custom GStreamer Sink Settings", this);
		QVBoxLayout *customLayout = new QVBoxLayout(m_customGroup);
		customLayout->setSpacing(6);
		customLayout->setContentsMargins(10, 8, 10, 8);
		QLabel *customPipeLabel = new QLabel("GStreamer Pipeline:", m_customGroup);
		m_customPipeEdit = new QPlainTextEdit(QString::fromStdString(dest->custom_pipeline), this);
		m_customPipeEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		customLayout->addWidget(customPipeLabel);
		customLayout->addWidget(m_customPipeEdit, 1);
		m_typeStack->addWidget(m_customGroup);

		layout->addLayout(form);
		layout->addWidget(m_typeStack, 1);

		connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &GstDestinationDetailDialog::onTypeChanged);
		onTypeChanged();

		QHBoxLayout *footer = new QHBoxLayout();
		QPushButton *btnSave = new QPushButton("Save", this);
		QPushButton *btnCancel = new QPushButton("Cancel", this);
		footer->addStretch();
		footer->addWidget(btnSave);
		footer->addWidget(btnCancel);
		layout->addLayout(footer);

		connect(btnSave, &QPushButton::clicked, this, &GstDestinationDetailDialog::saveAndClose);
		connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
	}

private:
	void onTypeChanged()
	{
		DestinationType t = (DestinationType)m_typeCombo->currentData().toInt();
		if (t == DEST_TYPE_RTSP)
			m_typeStack->setCurrentWidget(m_rtspGroup);
		else if (t == DEST_TYPE_WEBRTC)
			m_typeStack->setCurrentWidget(m_webrtcGroup);
		else if (t == DEST_TYPE_CUSTOM)
			m_typeStack->setCurrentWidget(m_customGroup);
	}

	void saveAndClose()
	{
		m_dest->name = m_nameEdit->text().toStdString();
		m_dest->type = (DestinationType)m_typeCombo->currentData().toInt();
		m_dest->autostart = m_autostartCheck->isChecked();

		m_dest->rtsp_port = m_rtspPortEdit->text().toStdString();
		m_dest->rtsp_mount = m_rtspMountEdit->text().toStdString();
		m_dest->rtsp_pipeline = m_rtspPipeEdit->text().toStdString();

		m_dest->webrtc_port = m_webrtcPortEdit->text().toStdString();
		m_dest->webrtc_stun = m_webrtcStunEdit->text().toStdString();
		m_dest->webrtc_web_root = m_webrtcRootEdit->text().toStdString();

		m_dest->custom_pipeline = m_customPipeEdit->toPlainText().toStdString();

		accept();
	}

	std::shared_ptr<GstDestination> m_dest;
	QLineEdit *m_nameEdit;
	QComboBox *m_typeCombo;
	QCheckBox *m_autostartCheck;
	QStackedWidget *m_typeStack;

	QGroupBox *m_rtspGroup;
	QLineEdit *m_rtspPortEdit;
	QLineEdit *m_rtspMountEdit;
	QLineEdit *m_rtspPipeEdit;

	QGroupBox *m_webrtcGroup;
	QLineEdit *m_webrtcPortEdit;
	QLineEdit *m_webrtcStunEdit;
	QLineEdit *m_webrtcRootEdit;

	QGroupBox *m_customGroup;
	QPlainTextEdit *m_customPipeEdit;
};

/* --------------------------------------------------------------------------
 * Child Window: Detailed Configuration Dialog (Opened on Double-Click)
 * -------------------------------------------------------------------------- */
class GstMasterDetailDialog : public QDialog {
public:
	GstMasterDetailDialog(std::shared_ptr<GstMasterHub> hub, QWidget *parent = nullptr)
		: QDialog(parent), m_hub(hub)
	{
		setWindowTitle(QString("Edit Master Hub: %1").arg(QString::fromStdString(hub->name)));
		resize(680, 640);

		QVBoxLayout *rootLayout = new QVBoxLayout(this);
		rootLayout->setContentsMargins(12, 12, 12, 12);
		rootLayout->setSpacing(8);

		/* 1. Master Name (On top, NOT included in any collapsible) */
		QHBoxLayout *topLayout = new QHBoxLayout();
		topLayout->setSpacing(10);
		topLayout->setContentsMargins(2, 2, 2, 4);

		QLabel *nameLabel = new QLabel("Master Name:", this);
		nameLabel->setStyleSheet("font-weight: bold; font-size: 13px; color: #ffffff;");
		m_nameEdit = new QLineEdit(QString::fromStdString(hub->name), this);
		m_nameEdit->setStyleSheet(
			"QLineEdit { background-color: #1a1c24; border: 1px solid #3c4150; border-radius: 4px; padding: 6px 10px; color: #ffffff; font-size: 13px; font-weight: bold; }"
			"QLineEdit:focus { border: 1px solid #588bf6; }"
		);
		topLayout->addWidget(nameLabel);
		topLayout->addWidget(m_nameEdit, 1);
		rootLayout->addLayout(topLayout);

		/* Scroll Area for 3 Collapsible Sections */
		QScrollArea *scrollArea = new QScrollArea(this);
		scrollArea->setWidgetResizable(true);
		scrollArea->setFrameShape(QFrame::NoFrame);
		scrollArea->setStyleSheet("QScrollArea { background: transparent; } QWidget#ScrollContainer { background: transparent; }");

		QWidget *scrollContainer = new QWidget();
		scrollContainer->setObjectName("ScrollContainer");
		QVBoxLayout *scrollLayout = new QVBoxLayout(scrollContainer);
		scrollLayout->setContentsMargins(0, 0, 4, 0);
		scrollLayout->setSpacing(10);

		/* 2. First Collapsible: Source (what to capture, capture method, etc.) */
		m_secSource = new CollapsibleSection("Source (Capture Settings)", true, this);
		QWidget *sourceWidget = new QWidget();
		QFormLayout *sourceForm = new QFormLayout(sourceWidget);
		sourceForm->setContentsMargins(12, 12, 12, 12);
		sourceForm->setSpacing(8);
		sourceForm->setLabelAlignment(Qt::AlignLeft);
		sourceForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

		m_sourceTypeCombo = new QComboBox(this);
		m_sourceTypeCombo->addItem("Program Output");
		m_sourceTypeCombo->addItem("Scene");
		m_sourceTypeCombo->addItem("Source");
		m_sourceTypeCombo->setCurrentText(QString::fromStdString(hub->source_type));
		sourceForm->addRow("Capture Source:", m_sourceTypeCombo);

		m_captureMethodCombo = new QComboBox(this);
		m_captureMethodCombo->addItem("Render Callback (GPU Direct Texture)");
		m_captureMethodCombo->addItem("Encoder Callback (GPU Hardware Texture Encoder)");
		m_captureMethodCombo->addItem("Output Callback (Raw Video/Audio Frames)");
		m_captureMethodCombo->addItem("Filter Callback (Source Video Filter Tap)");
		int methodIdx = m_captureMethodCombo->findText(QString::fromStdString(hub->capture_method));
		if (methodIdx >= 0)
			m_captureMethodCombo->setCurrentIndex(methodIdx);
		sourceForm->addRow("Capture Method:", m_captureMethodCombo);

		m_targetLabel = new QLabel("Target Name:", this);
		m_targetCombo = new QComboBox(this);
		m_targetCombo->setEditable(false);
		sourceForm->addRow(m_targetLabel, m_targetCombo);

		m_secSource->setContentLayout(sourceForm);
		scrollLayout->addWidget(m_secSource);

		connect(m_sourceTypeCombo, &QComboBox::currentTextChanged, this, &GstMasterDetailDialog::populateTargetCombo);
		populateTargetCombo(m_sourceTypeCombo->currentText());

		/* 3. Second Collapsible: Encoder (hardware encoder, pipeline, bitrate, etc.) */
		m_secEncoder = new CollapsibleSection("Encoder (Pipeline and Compression)", true, this);
		QWidget *encoderWidget = new QWidget();
		QFormLayout *encoderForm = new QFormLayout(encoderWidget);
		encoderForm->setContentsMargins(12, 12, 12, 12);
		encoderForm->setSpacing(8);
		encoderForm->setLabelAlignment(Qt::AlignLeft);
		encoderForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

		m_encoderCombo = new QComboBox(this);
		const char *autoEnc = gst_master_pipeline_select_encoder("auto", true);
		m_encoderCombo->addItem(
			QString("Auto Detect GPU (%1)").arg(autoEnc ? autoEnc : "auto"),
			"auto");
		if (gst_master_pipeline_element_available("nvd3d11h264enc") ||
		    gst_master_pipeline_element_available("nvh264enc") ||
		    gst_master_pipeline_element_available("nvcudah264enc"))
			m_encoderCombo->addItem("NVIDIA NVENC (Detected)", "nvh264enc");
		else
			m_encoderCombo->addItem("NVIDIA NVENC (nvh264enc)", "nvh264enc");

		if (gst_master_pipeline_element_available("qsvh264enc"))
			m_encoderCombo->addItem("Intel QuickSync (Detected)", "qsvh264enc");
		else
			m_encoderCombo->addItem("Intel QuickSync (qsvh264enc)", "qsvh264enc");

		if (gst_master_pipeline_element_available("amfh264enc"))
			m_encoderCombo->addItem("AMD AMF (Detected)", "amfh264enc");
		else
			m_encoderCombo->addItem("AMD AMF (amfh264enc)", "amfh264enc");

		if (gst_master_pipeline_element_available("vah264enc") ||
		    gst_master_pipeline_element_available("vaapih264enc"))
			m_encoderCombo->addItem("VAAPI GPU (Detected)", "vaapih264enc");
		else
			m_encoderCombo->addItem("VAAPI GPU (vaapih264enc)", "vaapih264enc");

		if (gst_master_pipeline_element_available("mfh264enc"))
			m_encoderCombo->addItem("MediaFoundation H.264 (Detected)", "mfh264enc");

		m_encoderCombo->addItem("Software (x264enc)", "x264enc");
		int encIdx = m_encoderCombo->findData(QString::fromStdString(hub->gpu_encoder_type));
		if (encIdx >= 0) m_encoderCombo->setCurrentIndex(encIdx);
		encoderForm->addRow("Hardware Encoder:", m_encoderCombo);

		m_bitrateSpin = new QSpinBox(this);
		m_bitrateSpin->setRange(500, 50000);
		m_bitrateSpin->setSingleStep(500);
		m_bitrateSpin->setValue(hub->bitrate_kbps);
		m_bitrateSpin->setSuffix(" kbps");
		encoderForm->addRow("Master Bitrate:", m_bitrateSpin);

		m_keyintSpin = new QSpinBox(this);
		m_keyintSpin->setRange(1, 10);
		m_keyintSpin->setValue(hub->keyint_sec);
		m_keyintSpin->setSuffix(" sec");
		encoderForm->addRow("Keyframe Interval:", m_keyintSpin);

		m_customPipeEdit = new QPlainTextEdit(QString::fromStdString(hub->master_pipeline), this);
		m_customPipeEdit->setPlaceholderText("Optional: Custom GStreamer Master Pipeline string (leave empty for auto GPU encoder)");
		m_customPipeEdit->setMaximumHeight(45);
		encoderForm->addRow("Custom Pipeline:", m_customPipeEdit);

		m_secEncoder->setContentLayout(encoderForm);
		scrollLayout->addWidget(m_secEncoder);

		/* 4. Third Collapsible: Output (destinations list & action buttons) */
		m_secOutput = new CollapsibleSection("Output (Destinations and Streaming Sinks)", true, this);
		QWidget *outputWidget = new QWidget();
		QVBoxLayout *outputLayout = new QVBoxLayout(outputWidget);
		outputLayout->setContentsMargins(8, 8, 8, 6);
		outputLayout->setSpacing(6);

		m_destList = new QListWidget(this);
		m_destList->setSelectionMode(QAbstractItemView::SingleSelection);
		m_destList->setMinimumHeight(150);
		m_destList->setStyleSheet(
			"QListWidget, QListWidget::viewport { background: transparent; border: none; outline: 0; padding: 2px; }"
			"QListWidget::item { padding: 3px 5px; margin: 1px; border-radius: 4px; border: 1px solid transparent; color: #d0d4e0; }"
			"QListWidget::item:hover:!selected { background-color: rgba(255, 255, 255, 0.06); }"
			"QListWidget::item:selected { background-color: #2b5bc7; border: 1px solid #588bf6; border-radius: 4px; }"
			"QListWidget::item:selected:focus { background-color: #2b5bc7; border: 1px solid rgba(255, 255, 255, 0.75); border-radius: 4px; }"
		);
		outputLayout->addWidget(m_destList);

		/* Native OBS ToolBar for Destinations matching Scenes & Sources docks */
		QToolBar *destToolbar = new QToolBar(this);
		destToolbar->setObjectName("destToolbar");
		destToolbar->setStyleSheet("QToolBar { background: transparent; border-top: 1px solid #2b313d; padding-top: 4px; margin-top: 2px; }");
		destToolbar->setIconSize(QSize(16, 16));

		QAction *actAdd = destToolbar->addAction(QIcon("theme:Dark/plus.svg"), "Add Destination");
		actAdd->setProperty("class", "icon-plus");

		QAction *actRemove = destToolbar->addAction(QIcon("theme:Dark/trash.svg"), "Remove Destination");
		actRemove->setProperty("class", "icon-trash");

		destToolbar->addSeparator();

		QAction *actProps = destToolbar->addAction(QIcon("theme:Dark/settings/general.svg"), "Destination Properties");
		actProps->setProperty("class", "icon-gear");

		destToolbar->addSeparator();

		QAction *actUp = destToolbar->addAction(QIcon("theme:Dark/up.svg"), "Move Up");
		actUp->setProperty("class", "icon-up");

		QAction *actDown = destToolbar->addAction(QIcon("theme:Dark/down.svg"), "Move Down");
		actDown->setProperty("class", "icon-down");

		outputLayout->addWidget(destToolbar);

		connect(actAdd, &QAction::triggered, this, &GstMasterDetailDialog::onAddDestinationClicked);
		connect(actRemove, &QAction::triggered, this, &GstMasterDetailDialog::removeSelectedDestination);
		connect(actProps, &QAction::triggered, this, &GstMasterDetailDialog::editSelectedDestination);
		connect(actUp, &QAction::triggered, this, &GstMasterDetailDialog::moveUpSelectedDestination);
		connect(actDown, &QAction::triggered, this, &GstMasterDetailDialog::moveDownSelectedDestination);

		connect(m_destList, &QListWidget::itemDoubleClicked, this, &GstMasterDetailDialog::onDestDoubleClicked);

		m_secOutput->setContentLayout(outputLayout);
		scrollLayout->addWidget(m_secOutput);
		scrollLayout->addStretch(1);

		scrollArea->setWidget(scrollContainer);
		rootLayout->addWidget(scrollArea, 1);

		/* Dialog Footer */
		QHBoxLayout *footerLayout = new QHBoxLayout();
		QPushButton *btnSave = new QPushButton("Save", this);
		QPushButton *btnClose = new QPushButton("Close", this);
		footerLayout->addStretch();
		footerLayout->addWidget(btnSave);
		footerLayout->addWidget(btnClose);
		rootLayout->addLayout(footerLayout);

		connect(btnSave, &QPushButton::clicked, this, &GstMasterDetailDialog::saveAndClose);
		connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);

		refreshDestList();
	}

private:
	void populateTargetCombo(const QString &sourceType)
	{
		m_targetCombo->clear();
		if (sourceType == "Scene") {
			m_targetLabel->setText("Target Scene:");
			m_targetLabel->show();
			m_targetCombo->show();
			auto scenes = get_obs_scene_names();
			for (const auto &s : scenes) {
				m_targetCombo->addItem(QString::fromStdString(s));
			}
			int idx = m_targetCombo->findText(QString::fromStdString(m_hub->scene_name));
			if (idx >= 0) m_targetCombo->setCurrentIndex(idx);
		} else if (sourceType == "Source") {
			m_targetLabel->setText("Target Source:");
			m_targetLabel->show();
			m_targetCombo->show();
			auto sources = get_obs_source_names();
			for (const auto &s : sources) {
				m_targetCombo->addItem(QString::fromStdString(s));
			}
			int idx = m_targetCombo->findText(QString::fromStdString(m_hub->source_name));
			if (idx >= 0) m_targetCombo->setCurrentIndex(idx);
		} else {
			/* Program Output: completely hide target scene/source row */
			m_targetLabel->hide();
			m_targetCombo->hide();
		}
	}

	void refreshDestList()
	{
		int selectedRow = m_destList->currentRow();
		m_destList->clear();

		for (size_t i = 0; i < m_hub->destinations.size(); i++) {
			const auto &dest = m_hub->destinations[i];

			QString typeBadge = (dest->type == DEST_TYPE_RTSP) ? "RTSP" :
			                    (dest->type == DEST_TYPE_WEBRTC) ? "WebRTC" : "Custom";

			QString targetStr;
			if (dest->type == DEST_TYPE_RTSP)
				targetStr = QString(":%1%2").arg(QString::fromStdString(dest->rtsp_port), QString::fromStdString(dest->rtsp_mount));
			else if (dest->type == DEST_TYPE_WEBRTC)
				targetStr = QString("Port %1").arg(QString::fromStdString(dest->webrtc_port));
			else
				targetStr = "Custom Pipeline";

			QString statusStr = dest->is_active ? "Running" : "Stopped";
			QString autoStr = dest->autostart ? " • Autostart" : "";

			QWidget *rowWidget = new QWidget();
			rowWidget->setObjectName("DestRowWidget");
			rowWidget->setStyleSheet("QWidget#DestRowWidget { background: transparent; }");
			QHBoxLayout *rowLayout = new QHBoxLayout(rowWidget);
			rowLayout->setContentsMargins(8, 4, 8, 4);
			rowLayout->setSpacing(10);

			/* Info Labels - standard neutral OBS colors, no bright tints */
			QVBoxLayout *infoLayout = new QVBoxLayout();
			infoLayout->setSpacing(2);
			infoLayout->setContentsMargins(0, 0, 0, 0);

			QHBoxLayout *titleLayout = new QHBoxLayout();
			titleLayout->setSpacing(6);
			titleLayout->setContentsMargins(0, 0, 0, 0);

			QLabel *badgeLabel = new QLabel(QString("[%1]").arg(typeBadge), rowWidget);
			badgeLabel->setStyleSheet("color: #cbd5e1; font-weight: bold; font-size: 11px;");
			titleLayout->addWidget(badgeLabel);

			QLabel *nameLabel = new QLabel(QString::fromStdString(dest->name), rowWidget);
			nameLabel->setStyleSheet("color: #ffffff; font-weight: 600; font-size: 12px;");
			titleLayout->addWidget(nameLabel);
			titleLayout->addStretch(1);
			infoLayout->addLayout(titleLayout);

			QLabel *descLabel = new QLabel(QString("Target: %1  |  Status: %2%3").arg(targetStr, statusStr, autoStr), rowWidget);
			descLabel->setStyleSheet("color: #94a3b8; font-size: 11px;");
			infoLayout->addWidget(descLabel);

			rowLayout->addLayout(infoLayout, 1);

			/* Separate Start and Stop buttons inside list item - solid OBS default button style (not transparent) */
			QHBoxLayout *actionLayout = new QHBoxLayout();
			actionLayout->setSpacing(6);
			actionLayout->setContentsMargins(0, 0, 0, 0);

			const QString obsBtnStyle =
				"QPushButton {"
				"  background-color: #2b313d;"
				"  border: 1px solid #3c4354;"
				"  border-radius: 4px;"
				"  color: #e2e8f0;"
				"  font-size: 11px;"
				"  font-weight: 500;"
				"  padding: 3px 10px;"
				"}"
				"QPushButton:hover:!disabled {"
				"  background-color: #383f4f;"
				"  border-color: #4f586d;"
				"  color: #ffffff;"
				"}"
				"QPushButton:pressed {"
				"  background-color: #1e222b;"
				"}"
				"QPushButton:disabled {"
				"  background-color: #21252e;"
				"  border: 1px solid #2b313d;"
				"  color: #636b7b;"
				"}";

			QPushButton *btnStart = new QPushButton("Start", rowWidget);
			btnStart->setFixedHeight(24);
			btnStart->setMinimumWidth(52);
			btnStart->setStyleSheet(obsBtnStyle);
			btnStart->setEnabled(!dest->is_active);

			QPushButton *btnStop = new QPushButton("Stop", rowWidget);
			btnStop->setFixedHeight(24);
			btnStop->setMinimumWidth(52);
			btnStop->setStyleSheet(obsBtnStyle);
			btnStop->setEnabled(dest->is_active);

			connect(btnStart, &QPushButton::clicked, [this, i]() {
				m_destList->setCurrentRow((int)i);
				if (i < m_hub->destinations.size()) {
					start_destination(m_hub.get(), m_hub->destinations[i].get());
					export_dock_status_json();
					refreshDestList();
				}
			});

			connect(btnStop, &QPushButton::clicked, [this, i]() {
				m_destList->setCurrentRow((int)i);
				if (i < m_hub->destinations.size()) {
					stop_destination(m_hub.get(), m_hub->destinations[i].get());
					export_dock_status_json();
					refreshDestList();
				}
			});

			actionLayout->addWidget(btnStart);
			actionLayout->addWidget(btnStop);
			rowLayout->addLayout(actionLayout);

			QListWidgetItem *item = new QListWidgetItem(m_destList);
			item->setSizeHint(QSize(0, 48));
			m_destList->setItemWidget(item, rowWidget);
		}

		if (selectedRow >= 0 && selectedRow < m_destList->count()) {
			m_destList->setCurrentRow(selectedRow);
		} else if (m_destList->count() > 0) {
			m_destList->setCurrentRow(0);
		}
	}

	void onDestDoubleClicked(QListWidgetItem *item)
	{
		int row = m_destList->row(item);
		editDestination(row);
	}

	void editSelectedDestination()
	{
		int row = m_destList->currentRow();
		if (row >= 0) editDestination(row);
	}

	void editDestination(int idx)
	{
		if (idx < 0 || idx >= (int)m_hub->destinations.size()) return;
		GstDestinationDetailDialog dlg(m_hub->destinations[idx], this);
		if (dlg.exec() == QDialog::Accepted) {
			export_dock_status_json();
			refreshDestList();
		}
	}

	void toggleSelectedDestination()
	{
		int row = m_destList->currentRow();
		if (row < 0 || row >= (int)m_hub->destinations.size()) return;
		auto &dest = m_hub->destinations[row];
		if (dest->is_active) {
			stop_destination(m_hub.get(), dest.get());
		} else {
			start_destination(m_hub.get(), dest.get());
		}
		export_dock_status_json();
		refreshDestList();
	}

	void onAddDestinationClicked()
	{
		auto dest = std::make_shared<GstDestination>();
		dest->type = DEST_TYPE_RTSP;
		dest->name = "RTSP Stream " + std::to_string(m_hub->destinations.size() + 1);
		dest->rtsp_port = std::to_string(8554 + m_hub->destinations.size());
		dest->rtsp_mount = "/live" + std::to_string(m_hub->destinations.size() + 1);
		dest->rtsp_pipeline = DEFAULT_RTSP_PIPELINE;
		m_hub->destinations.push_back(dest);
		export_dock_status_json();
		refreshDestList();

		int newRow = (int)m_hub->destinations.size() - 1;
		m_destList->setCurrentRow(newRow);

		GstDestinationDetailDialog dlg(dest, this);
		if (dlg.exec() == QDialog::Accepted) {
			export_dock_status_json();
			refreshDestList();
			m_destList->setCurrentRow(newRow);
		} else {
			m_hub->destinations.pop_back();
			export_dock_status_json();
			refreshDestList();
			if (!m_hub->destinations.empty()) {
				m_destList->setCurrentRow(std::min(newRow, (int)m_hub->destinations.size() - 1));
			}
		}
	}

	void moveUpSelectedDestination()
	{
		int row = m_destList->currentRow();
		if (row > 0 && row < (int)m_hub->destinations.size()) {
			std::swap(m_hub->destinations[row], m_hub->destinations[row - 1]);
			export_dock_status_json();
			refreshDestList();
			m_destList->setCurrentRow(row - 1);
		}
	}

	void moveDownSelectedDestination()
	{
		int row = m_destList->currentRow();
		if (row >= 0 && row < (int)m_hub->destinations.size() - 1) {
			std::swap(m_hub->destinations[row], m_hub->destinations[row + 1]);
			export_dock_status_json();
			refreshDestList();
			m_destList->setCurrentRow(row + 1);
		}
	}

	void removeSelectedDestination()
	{
		int row = m_destList->currentRow();
		if (row >= 0 && row < (int)m_hub->destinations.size()) {
			stop_destination(m_hub.get(), m_hub->destinations[row].get());
			m_hub->destinations.erase(m_hub->destinations.begin() + row);
			export_dock_status_json();
			refreshDestList();
		}
	}

	void saveAndClose()
	{
		m_hub->name = m_nameEdit->text().toStdString();
		m_hub->source_type = m_sourceTypeCombo->currentText().toStdString();
		m_hub->capture_method = m_captureMethodCombo->currentText().toStdString();
		if (m_hub->source_type == "Scene")
			m_hub->scene_name = m_targetCombo->currentText().toStdString();
		else if (m_hub->source_type == "Source")
			m_hub->source_name = m_targetCombo->currentText().toStdString();

		m_hub->gpu_encoder_type = m_encoderCombo->currentData().toString().toStdString();
		m_hub->bitrate_kbps = m_bitrateSpin->value();
		m_hub->keyint_sec = m_keyintSpin->value();
		m_hub->master_pipeline = m_customPipeEdit->toPlainText().toStdString();

		export_dock_status_json();
		accept();
	}

	std::shared_ptr<GstMasterHub> m_hub;
	QLineEdit *m_nameEdit;
	QComboBox *m_sourceTypeCombo;
	QComboBox *m_captureMethodCombo;
	QLabel *m_targetLabel;
	QComboBox *m_targetCombo;
	QComboBox *m_encoderCombo;
	QSpinBox *m_bitrateSpin;
	QSpinBox *m_keyintSpin;
	QPlainTextEdit *m_customPipeEdit;
	QListWidget *m_destList;

	CollapsibleSection *m_secSource;
	CollapsibleSection *m_secEncoder;
	CollapsibleSection *m_secOutput;
};

/* --------------------------------------------------------------------------
 * Main Window: Dock Widget registered via obs_frontend_add_dock_by_id
 * -------------------------------------------------------------------------- */
class GstMasterDockWidget : public QWidget {
public:
	GstMasterDockWidget(QWidget *parent = nullptr)
		: QWidget(parent)
	{
		QVBoxLayout *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 4, 4, 4);
		layout->setSpacing(4);

		/* Main List Box of Masters - transparent background matching dock surface */
		m_hubList = new QListWidget(this);
		m_hubList->setSelectionMode(QAbstractItemView::SingleSelection);
		m_hubList->setStyleSheet(
			"QListWidget, QListWidget::viewport { background: transparent; border: none; outline: 0; padding: 2px; }"
			"QListWidget::item { padding: 6px 8px; margin: 1px; border-radius: 4px; border: 1px solid transparent; color: #d0d4e0; }"
			"QListWidget::item:hover:!selected { background-color: rgba(255, 255, 255, 0.06); color: #ffffff; }"
			"QListWidget::item:selected { background-color: #2b5bc7; color: #ffffff; border: 1px solid #588bf6; border-radius: 4px; }"
			"QListWidget::item:selected:focus { background-color: #2b5bc7; color: #ffffff; border: 1px solid rgba(255, 255, 255, 0.75); border-radius: 4px; }"
		);
		layout->addWidget(m_hubList);

		/* Bottom ToolBar matching built-in Scenes and Sources docks */
		QToolBar *toolbar = new QToolBar(this);
		toolbar->setObjectName("hubToolbar");
		toolbar->setStyleSheet("QToolBar { background: transparent; border-top: 1px solid #2b313d; padding-top: 4px; margin-top: 2px; }");
		toolbar->setIconSize(QSize(16, 16));

		QAction *actAdd = toolbar->addAction(QIcon("theme:Dark/plus.svg"), "Add Master Hub");
		actAdd->setProperty("class", "icon-plus");

		QAction *actRemove = toolbar->addAction(QIcon("theme:Dark/trash.svg"), "Remove Master Hub");
		actRemove->setProperty("class", "icon-trash");

		toolbar->addSeparator();

		QAction *actConfig = toolbar->addAction(QIcon("theme:Dark/settings/general.svg"), "Configure Master Hub");
		actConfig->setProperty("class", "icon-gear");

		toolbar->addSeparator();

		QAction *actUp = toolbar->addAction(QIcon("theme:Dark/up.svg"), "Move Up");
		actUp->setProperty("class", "icon-up");

		QAction *actDown = toolbar->addAction(QIcon("theme:Dark/down.svg"), "Move Down");
		actDown->setProperty("class", "icon-down");

		connect(actAdd, &QAction::triggered, this, &GstMasterDockWidget::onAddMasterClicked);
		connect(actRemove, &QAction::triggered, this, &GstMasterDockWidget::onDeleteMasterClicked);
		connect(actConfig, &QAction::triggered, this, &GstMasterDockWidget::onConfigureClicked);
		connect(actUp, &QAction::triggered, this, &GstMasterDockWidget::onMoveUpClicked);
		connect(actDown, &QAction::triggered, this, &GstMasterDockWidget::onMoveDownClicked);

		layout->addWidget(toolbar);

		/* Double-Click on Master Item -> Opens Child Configuration Dialog */
		connect(m_hubList, &QListWidget::itemDoubleClicked, [this](QListWidgetItem *item) {
			int row = m_hubList->row(item);
			openDetailDialog(row);
		});

		refreshList();
	}

	void refreshList()
	{
		int selectedRow = m_hubList->currentRow();
		m_hubList->clear();

		for (size_t i = 0; i < g_master_hubs.size(); i++) {
			const auto &hub = g_master_hubs[i];

			bool anyActive = false;
			int runningCount = 0;
			for (const auto &d : hub->destinations) {
				if (d->is_active) {
					anyActive = true;
					runningCount++;
				}
			}

			QString title = QString::fromStdString(hub->name);

			QString srcStr = QString("Source: %1").arg(QString::fromStdString(hub->source_type));
			if (hub->source_type == "Scene" && !hub->scene_name.empty())
				srcStr += QString(" (%1)").arg(QString::fromStdString(hub->scene_name));
			else if (hub->source_type == "Source" && !hub->source_name.empty())
				srcStr += QString(" (%1)").arg(QString::fromStdString(hub->source_name));

			QString methodStr = QString::fromStdString(hub->capture_method);
			if (methodStr.contains("Render Callback")) methodStr = "Render CB";
			else if (methodStr.contains("Encoder Callback")) methodStr = "Encoder CB";
			else if (methodStr.contains("Output Callback")) methodStr = "Output CB";
			else if (methodStr.contains("Filter Callback")) methodStr = "Filter CB";

			QString line2 = QString("%1  |  %2  |  %3 (%4 kbps)")
			                    .arg(srcStr, methodStr, QString::fromStdString(hub->gpu_encoder_type), QString::number(hub->bitrate_kbps));
			QString line3 = QString("Destinations: %1 running / %2 total  (Double-click to configure)")
			                    .arg(QString::number(runningCount), QString::number(hub->destinations.size()));

			QListWidgetItem *item = new QListWidgetItem(title + "\n" + line2 + "\n" + line3);
			if (anyActive) {
				item->setForeground(QBrush(QColor(50, 205, 80)));
			} else {
				item->setForeground(QBrush(QColor(200, 205, 215)));
			}
			m_hubList->addItem(item);
		}

		if (selectedRow >= 0 && selectedRow < m_hubList->count()) {
			m_hubList->setCurrentRow(selectedRow);
		} else if (m_hubList->count() > 0) {
			m_hubList->setCurrentRow(0);
		}
	}

	void refreshTable()
	{
		refreshList();
	}

private:
	void onConfigureClicked()
	{
		int row = m_hubList->currentRow();
		if (row >= 0) openDetailDialog(row);
	}

	void openDetailDialog(int row)
	{
		if (row < 0 || row >= (int)g_master_hubs.size()) return;
		GstMasterDetailDialog dlg(g_master_hubs[row], this);
		dlg.exec();
		export_dock_status_json();
		refreshList();
	}

	void onAddMasterClicked()
	{
		auto hub = std::make_shared<GstMasterHub>();
		hub->name = "Master Hub " + std::to_string(g_master_hubs.size() + 1);

		/* Add default RTSP destination */
		auto defaultRtsp = std::make_shared<GstDestination>();
		defaultRtsp->name = "RTSP Main";
		hub->destinations.push_back(defaultRtsp);

		g_master_hubs.push_back(hub);
		export_dock_status_json();
		refreshList();
		m_hubList->setCurrentRow((int)g_master_hubs.size() - 1);
	}

	void onDeleteMasterClicked()
	{
		int row = m_hubList->currentRow();
		if (row >= 0 && row < (int)g_master_hubs.size()) {
			/* Stop all destinations on this hub */
			for (auto &dest : g_master_hubs[row]->destinations) {
				stop_destination(g_master_hubs[row].get(), dest.get());
			}
			g_master_hubs.erase(g_master_hubs.begin() + row);
			export_dock_status_json();
			refreshList();
		}
	}

	void onMoveUpClicked()
	{
		int row = m_hubList->currentRow();
		if (row > 0 && row < (int)g_master_hubs.size()) {
			std::swap(g_master_hubs[row], g_master_hubs[row - 1]);
			export_dock_status_json();
			refreshList();
			m_hubList->setCurrentRow(row - 1);
		}
	}

	void onMoveDownClicked()
	{
		int row = m_hubList->currentRow();
		if (row >= 0 && row < (int)g_master_hubs.size() - 1) {
			std::swap(g_master_hubs[row], g_master_hubs[row + 1]);
			export_dock_status_json();
			refreshList();
			m_hubList->setCurrentRow(row + 1);
		}
	}

	QListWidget *m_hubList;
};

/* --------------------------------------------------------------------------
 * Module Lifecycle Entry Points
 * -------------------------------------------------------------------------- */
extern "C" {

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	(void)private_data;
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		if (g_master_hubs.empty()) {
			auto defaultHub = std::make_shared<GstMasterHub>();
			defaultHub->name = "Program Master Hub";
			auto defaultRtsp = std::make_shared<GstDestination>();
			defaultRtsp->name = "Default RTSP";
			defaultHub->destinations.push_back(defaultRtsp);

			auto defaultWebRTC = std::make_shared<GstDestination>();
			defaultWebRTC->type = DEST_TYPE_WEBRTC;
			defaultWebRTC->name = "Default WebRTC";
			defaultWebRTC->webrtc_port = "8888";
			defaultHub->destinations.push_back(defaultWebRTC);

			g_master_hubs.push_back(defaultHub);
		}

		/* Check for autostart destinations once frontend finished loading */
		for (auto &hub : g_master_hubs) {
			for (auto &dest : hub->destinations) {
				if (dest->enabled && dest->autostart && !dest->is_active) {
					blog(LOG_INFO, "[obs-gstreamer-dock] Autostarting destination '%s' for hub '%s'",
						dest->name.c_str(), hub->name.c_str());
					start_destination(hub.get(), dest.get());
				}
			}
		}

		if (g_dock_widget) {
			g_dock_widget->refreshTable();

			/* Strip close button ('X') from QDockWidget wrapper so it behaves like built-in OBS docks */
			QWidget *p = g_dock_widget->parentWidget();
			while (p) {
				if (auto *dw = qobject_cast<QDockWidget*>(p)) {
					dw->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
					break;
				}
				p = p->parentWidget();
			}
		}

		export_dock_status_json();
	}
}

static void on_frontend_save_load(obs_data_t *save_data, bool saving, void *private_data)
{
	(void)private_data;
	if (saving) {
		save_dock_config(save_data);
		blog(LOG_INFO, "[obs-gstreamer-dock] Saved %zu Master Hub(s) to OBS scene collection",
			g_master_hubs.size());
	} else {
		load_dock_config(save_data);
		blog(LOG_INFO, "[obs-gstreamer-dock] Restored %zu Master Hub(s) from OBS scene collection",
			g_master_hubs.size());
		export_dock_status_json();
		if (g_dock_widget)
			g_dock_widget->refreshTable();
	}
}

void gstreamer_dock_register(void)
{
	/* Register dock into OBS Studio early during obs_module_load so OBS UI layout restoration knows about it */
	if (!g_dock_widget && obs_frontend_add_dock_by_id) {
		g_dock_widget = new GstMasterDockWidget();
		g_dock_widget->refreshTable();

		obs_frontend_add_dock_by_id(
			"gstreamer_master_pipeline_dock",
			"GStreamer Master Hub",
			(void *)g_dock_widget
		);

		QWidget *p = g_dock_widget->parentWidget();
		while (p) {
			if (auto *dw = qobject_cast<QDockWidget*>(p)) {
				dw->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
				break;
			}
			p = p->parentWidget();
		}

		blog(LOG_INFO, "[obs-gstreamer-dock] Registered GStreamer Master Hub dock early during module load");
	}

	obs_frontend_add_save_callback(on_frontend_save_load, nullptr);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
}

void gstreamer_dock_unregister(void)
{
	/* Stop all active destinations on all master hubs */
	for (auto &hub : g_master_hubs) {
		for (auto &dest : hub->destinations) {
			stop_destination(hub.get(), dest.get());
		}
	}

	g_master_hubs.clear();
	obs_frontend_remove_save_callback(on_frontend_save_load, nullptr);
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	if (g_dock_widget) {
		g_dock_widget = nullptr;
	}

	blog(LOG_INFO, "[obs-gstreamer-dock] Unregistered GStreamer Master Hub dock");
}

}
