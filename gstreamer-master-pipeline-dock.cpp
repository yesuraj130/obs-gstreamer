/*
 * obs-gstreamer: Master Pipeline Hub Dock (OBS 32+ / Qt 6)
 *
 * Implements a native OBS Dock using obs_frontend_add_dock_by_id:
 * - Main Window: Listed in OBS "Docks" menu, displays all configured Master Hubs.
 * - Child Window (Double-click / Configure): Opens detailed editor for master encoder & destinations.
 * - Destination Editor Dialog: Edit individual RTSP, WebRTC, or Custom pipeline properties.
 * - JSON Persistence: Automatically saves & restores Master Hub settings across OBS sessions.
 * - Live Status Export: Writes JSON telemetry to /tmp/obs_gstreamer_hubs.json for Web UI monitoring.
 */

#include <obs.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <gst/gst.h>

#include "gstreamer-master-pipeline.h"
#include "gstreamer-rtspserver.h"
#include "gstreamer-render-callback.h"
#include "gstreamer-helper.h"

#include <QtCore/qglobal.h>
#if QT_VERSION < QT_VERSION_CHECK(6, 4, 0)
#error "obs-gstreamer requires Qt 6.4.0 or newer"
#endif

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QDateTime>

#include <QtGui/QColor>
#include <QtGui/QBrush>
#include <QtWidgets/QWidget>
#include <QtWidgets/QDialog>
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

#include <vector>
#include <string>
#include <memory>

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
	obs_output_t *webrtc_output;

	/* Custom Pipeline specifics */
	std::string custom_pipeline;
	obs_output_t *custom_output;

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
		  webrtc_output(nullptr),
		  custom_pipeline("autovideosink sync=false"),
		  custom_output(nullptr)
	{
	}
};

struct GstMasterHub {
	std::string name;
	std::string source_type;      /* "Program Output", "Scene", "Source" */
	std::string source_name;
	std::string scene_name;
	std::string render_mode;      /* Capture Callback */
	std::string gpu_encoder_type; /* "auto", "nvenc", "vaapi", "vtenc", "x264enc" */
	int bitrate_kbps;
	int keyint_sec;
	std::string master_pipeline;

	bool is_active;
	gst_render_callback_t *active_render_cb;

	std::vector<std::shared_ptr<GstDestination>> destinations;

	GstMasterHub()
		: name("Master Hub"),
		  source_type("Program Output"),
		  source_name(""),
		  scene_name(""),
		  render_mode("Render Callback (GPU Direct Texture)"),
		  gpu_encoder_type("auto"),
		  bitrate_kbps(4000),
		  keyint_sec(1),
		  master_pipeline(""),
		  is_active(false),
		  active_render_cb(nullptr)
	{
	}
};

/* Global Master Hubs managed by the Dock */
static std::vector<std::shared_ptr<GstMasterHub>> g_master_hubs;

class GstMasterDockWidget;
static GstMasterDockWidget *g_dock_widget = nullptr;

/* --------------------------------------------------------------------------
 * Persistence & Telemetry Helpers
 * -------------------------------------------------------------------------- */
static QString get_config_file_path()
{
	QString dirPath = "/root/.config/obs-studio/plugin_config/obs-gstreamer";
	QDir().mkpath(dirPath);
	return dirPath + "/master_hubs.json";
}

static void export_dock_status_json()
{
	QJsonArray hubsArray;
	for (const auto &hub : g_master_hubs) {
		QJsonObject hubObj;
		hubObj["name"] = QString::fromStdString(hub->name);
		hubObj["source_type"] = QString::fromStdString(hub->source_type);
		hubObj["source_name"] = QString::fromStdString(hub->source_name);
		hubObj["scene_name"] = QString::fromStdString(hub->scene_name);
		hubObj["gpu_encoder_type"] = QString::fromStdString(hub->gpu_encoder_type);
		hubObj["bitrate_kbps"] = hub->bitrate_kbps;
		hubObj["is_active"] = hub->is_active || (hub->active_render_cb != nullptr);

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

static void save_dock_config()
{
	QJsonArray hubsArray;
	for (const auto &hub : g_master_hubs) {
		QJsonObject hubObj;
		hubObj["name"] = QString::fromStdString(hub->name);
		hubObj["source_type"] = QString::fromStdString(hub->source_type);
		hubObj["source_name"] = QString::fromStdString(hub->source_name);
		hubObj["scene_name"] = QString::fromStdString(hub->scene_name);
		hubObj["gpu_encoder_type"] = QString::fromStdString(hub->gpu_encoder_type);
		hubObj["bitrate_kbps"] = hub->bitrate_kbps;
		hubObj["keyint_sec"] = hub->keyint_sec;
		hubObj["master_pipeline"] = QString::fromStdString(hub->master_pipeline);

		QJsonArray destsArray;
		for (const auto &d : hub->destinations) {
			QJsonObject dObj;
			dObj["name"] = QString::fromStdString(d->name);
			dObj["type"] = (int)d->type;
			dObj["enabled"] = d->enabled;
			dObj["autostart"] = d->autostart;
			dObj["rtsp_port"] = QString::fromStdString(d->rtsp_port);
			dObj["rtsp_mount"] = QString::fromStdString(d->rtsp_mount);
			dObj["rtsp_pipeline"] = QString::fromStdString(d->rtsp_pipeline);
			dObj["webrtc_port"] = QString::fromStdString(d->webrtc_port);
			dObj["webrtc_stun"] = QString::fromStdString(d->webrtc_stun);
			dObj["webrtc_web_root"] = QString::fromStdString(d->webrtc_web_root);
			dObj["custom_pipeline"] = QString::fromStdString(d->custom_pipeline);
			destsArray.append(dObj);
		}
		hubObj["destinations"] = destsArray;
		hubsArray.append(hubObj);
	}

	QJsonObject rootObj;
	rootObj["master_hubs"] = hubsArray;

	QFile file(get_config_file_path());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(rootObj).toJson(QJsonDocument::Indented));
		file.close();
	}

	export_dock_status_json();
}

static void load_dock_config()
{
	QFile file(get_config_file_path());
	if (!file.open(QIODevice::ReadOnly))
		return;

	QByteArray data = file.readAll();
	file.close();

	QJsonDocument doc = QJsonDocument::fromJson(data);
	if (!doc.isObject())
		return;

	QJsonObject rootObj = doc.object();
	QJsonArray hubsArray = rootObj["master_hubs"].toArray();
	if (hubsArray.isEmpty())
		return;

	g_master_hubs.clear();

	for (const auto &val : hubsArray) {
		QJsonObject hubObj = val.toObject();
		auto hub = std::make_shared<GstMasterHub>();
		hub->name = hubObj["name"].toString("Master Hub").toStdString();
		hub->source_type = hubObj["source_type"].toString("Program Output").toStdString();
		hub->source_name = hubObj["source_name"].toString("").toStdString();
		hub->scene_name = hubObj["scene_name"].toString("").toStdString();
		hub->gpu_encoder_type = hubObj["gpu_encoder_type"].toString("auto").toStdString();
		hub->bitrate_kbps = hubObj["bitrate_kbps"].toInt(4000);
		hub->keyint_sec = hubObj["keyint_sec"].toInt(1);
		hub->master_pipeline = hubObj["master_pipeline"].toString("").toStdString();

		QJsonArray destsArray = hubObj["destinations"].toArray();
		for (const auto &dVal : destsArray) {
			QJsonObject dObj = dVal.toObject();
			auto dest = std::make_shared<GstDestination>();
			dest->name = dObj["name"].toString("Destination").toStdString();
			dest->type = (DestinationType)dObj["type"].toInt(0);
			dest->enabled = dObj["enabled"].toBool(true);
			dest->autostart = dObj["autostart"].toBool(false);
			dest->rtsp_port = dObj["rtsp_port"].toString("8554").toStdString();
			dest->rtsp_mount = dObj["rtsp_mount"].toString("/live").toStdString();
			dest->rtsp_pipeline = dObj["rtsp_pipeline"].toString(DEFAULT_RTSP_PIPELINE).toStdString();
			dest->webrtc_port = dObj["webrtc_port"].toString("8888").toStdString();
			dest->webrtc_stun = dObj["webrtc_stun"].toString("").toStdString();
			dest->webrtc_web_root = dObj["webrtc_web_root"].toString("").toStdString();
			dest->custom_pipeline = dObj["custom_pipeline"].toString("autovideosink sync=false").toStdString();

			hub->destinations.push_back(dest);
		}

		g_master_hubs.push_back(hub);
	}

	export_dock_status_json();
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
static void stop_destination(GstMasterHub *hub, GstDestination *dest)
{
	if (!dest->is_active)
		return;

	if (dest->type == DEST_TYPE_RTSP && dest->rtsp_branch) {
		blog(LOG_INFO, "[obs-gstreamer-dock] Stopping RTSP branch (%s:%s)",
			dest->rtsp_port.c_str(), dest->rtsp_mount.c_str());
		gst_render_hub_stop_branch(dest->rtsp_branch);
		dest->rtsp_branch = nullptr;
	} else if (dest->type == DEST_TYPE_WEBRTC && dest->webrtc_output) {
		if (obs_output_active(dest->webrtc_output)) {
			obs_output_stop(dest->webrtc_output);
		}
		obs_output_release(dest->webrtc_output);
		dest->webrtc_output = nullptr;
		blog(LOG_INFO, "[obs-gstreamer-dock] Stopped WebRTC output (port: %s)", dest->webrtc_port.c_str());
	} else if (dest->type == DEST_TYPE_CUSTOM && dest->custom_output) {
		if (obs_output_active(dest->custom_output)) {
			obs_output_stop(dest->custom_output);
		}
		obs_output_release(dest->custom_output);
		dest->custom_output = nullptr;
		blog(LOG_INFO, "[obs-gstreamer-dock] Stopped Custom pipeline output");
	}

	dest->is_active = false;

	/* If no active destinations remain on this master hub, detach and destroy GPU render callback */
	bool any_active = false;
	for (const auto &d : hub->destinations) {
		if (d->is_active) {
			any_active = true;
			break;
		}
	}

	if (!any_active && hub->active_render_cb) {
		blog(LOG_INFO, "[obs-gstreamer-dock] All destinations stopped: tearing down master pipeline for '%s'",
			hub->name.c_str());
		gst_render_callback_destroy(hub->active_render_cb);
		hub->active_render_cb = nullptr;
	}

	export_dock_status_json();
}

static bool ensure_master_pipeline(GstMasterHub *hub)
{
	if (hub->active_render_cb)
		return true;

	gst_source_type_t stype = GST_SOURCE_MAIN_OUTPUT;
	if (hub->source_type == "Scene")
		stype = GST_SOURCE_SCENE;
	else if (hub->source_type == "Source")
		stype = GST_SOURCE_SOURCE;

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	uint32_t fps_num = ovi.fps_num ? ovi.fps_num : 60;
	uint32_t fps_den = ovi.fps_den ? ovi.fps_den : 1;

	blog(LOG_INFO, "[obs-gstreamer-dock] Starting Master Hardware Pipeline for '%s' (encoder=%s, bitrate=%d kbps)",
		hub->name.c_str(), hub->gpu_encoder_type.c_str(), hub->bitrate_kbps);

	hub->active_render_cb = gst_render_callback_create(
		stype,
		hub->scene_name.c_str(),
		hub->source_name.c_str(),
		hub->master_pipeline.c_str(),
		hub->gpu_encoder_type.c_str(),
		hub->bitrate_kbps,
		hub->keyint_sec,
		fps_num,
		fps_den
	);

	if (!hub->active_render_cb) {
		blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to create Master Render Callback for '%s'", hub->name.c_str());
		return false;
	}

	return true;
}

static bool start_destination(GstMasterHub *hub, GstDestination *dest)
{
	stop_destination(hub, dest);

	if (dest->type == DEST_TYPE_RTSP) {
		if (!ensure_master_pipeline(hub))
			return false;

		gst_master_pipeline_t *master_pipe = gst_render_callback_get_pipeline(hub->active_render_cb);
		if (!master_pipe) {
			blog(LOG_ERROR, "[obs-gstreamer-dock] Master pipeline object is NULL");
			return false;
		}

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
			return false;
		}
		dest->is_active = true;
		export_dock_status_json();
		return true;
	} else if (dest->type == DEST_TYPE_WEBRTC) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_bool(settings, "webrtc_output", true);
		obs_data_set_string(settings, "webrtc_http_port", dest->webrtc_port.c_str());
		obs_data_set_string(settings, "webrtc_stun_server", dest->webrtc_stun.c_str());
		obs_data_set_string(settings, "webrtc_web_root", dest->webrtc_web_root.c_str());
		obs_data_set_string(settings, "pipeline", "autovideosink sync=false");

		dest->webrtc_output = obs_output_create("hjm-gstreamer-output", dest->name.c_str(), settings, nullptr);
		obs_data_release(settings);

		if (!dest->webrtc_output) {
			blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to create WebRTC output");
			return false;
		}

		obs_output_set_media(dest->webrtc_output, obs_get_video(), obs_get_audio());
		if (!obs_output_start(dest->webrtc_output)) {
			blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to start WebRTC output: %s",
				obs_output_get_last_error(dest->webrtc_output));
			stop_destination(hub, dest);
			return false;
		}
		dest->is_active = true;
		blog(LOG_INFO, "[obs-gstreamer-dock] WebRTC output active on port %s", dest->webrtc_port.c_str());
		export_dock_status_json();
		return true;
	} else if (dest->type == DEST_TYPE_CUSTOM) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "pipeline",
			dest->custom_pipeline.empty() ? "autovideosink sync=false" : dest->custom_pipeline.c_str());

		dest->custom_output = obs_output_create("hjm-gstreamer-output", dest->name.c_str(), settings, nullptr);
		obs_data_release(settings);

		if (!dest->custom_output) {
			blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to create custom pipeline output");
			return false;
		}

		obs_output_set_media(dest->custom_output, obs_get_video(), obs_get_audio());
		if (!obs_output_start(dest->custom_output)) {
			blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to start custom output: %s",
				obs_output_get_last_error(dest->custom_output));
			stop_destination(hub, dest);
			return false;
		}
		dest->is_active = true;
		blog(LOG_INFO, "[obs-gstreamer-dock] Custom pipeline output active");
		export_dock_status_json();
		return true;
	}

	return false;
}

/* --------------------------------------------------------------------------
 * Dialog: Edit Individual Destination Properties
 * -------------------------------------------------------------------------- */
class GstDestinationDetailDialog : public QDialog {
public:
	GstDestinationDetailDialog(std::shared_ptr<GstDestination> dest, QWidget *parent = nullptr)
		: QDialog(parent), m_dest(dest)
	{
		setWindowTitle(QString("Edit Destination: %1").arg(QString::fromStdString(dest->name)));
		resize(520, 420);

		QVBoxLayout *layout = new QVBoxLayout(this);
		QFormLayout *form = new QFormLayout();

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

		/* RTSP Section */
		m_rtspGroup = new QGroupBox("RTSP Server Settings", this);
		QFormLayout *rtspForm = new QFormLayout(m_rtspGroup);
		m_rtspPortEdit = new QLineEdit(QString::fromStdString(dest->rtsp_port), this);
		m_rtspMountEdit = new QLineEdit(QString::fromStdString(dest->rtsp_mount), this);
		m_rtspPipeEdit = new QLineEdit(QString::fromStdString(dest->rtsp_pipeline), this);
		rtspForm->addRow("RTSP Port:", m_rtspPortEdit);
		rtspForm->addRow("Mount Path:", m_rtspMountEdit);
		rtspForm->addRow("Payloader Pipeline:", m_rtspPipeEdit);

		/* WebRTC Section */
		m_webrtcGroup = new QGroupBox("WebRTC WHEP Settings", this);
		QFormLayout *webrtcForm = new QFormLayout(m_webrtcGroup);
		m_webrtcPortEdit = new QLineEdit(QString::fromStdString(dest->webrtc_port), this);
		m_webrtcStunEdit = new QLineEdit(QString::fromStdString(dest->webrtc_stun), this);
		m_webrtcRootEdit = new QLineEdit(QString::fromStdString(dest->webrtc_web_root), this);
		webrtcForm->addRow("WHEP Port:", m_webrtcPortEdit);
		webrtcForm->addRow("STUN Server:", m_webrtcStunEdit);
		webrtcForm->addRow("Web Root Path:", m_webrtcRootEdit);

		/* Custom Pipeline Section */
		m_customGroup = new QGroupBox("Custom GStreamer Sink Settings", this);
		QFormLayout *customForm = new QFormLayout(m_customGroup);
		m_customPipeEdit = new QPlainTextEdit(QString::fromStdString(dest->custom_pipeline), this);
		m_customPipeEdit->setMaximumHeight(80);
		customForm->addRow("GStreamer Pipeline:", m_customPipeEdit);

		layout->addLayout(form);
		layout->addWidget(m_rtspGroup);
		layout->addWidget(m_webrtcGroup);
		layout->addWidget(m_customGroup);

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
		m_rtspGroup->setVisible(t == DEST_TYPE_RTSP);
		m_webrtcGroup->setVisible(t == DEST_TYPE_WEBRTC);
		m_customGroup->setVisible(t == DEST_TYPE_CUSTOM);
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
		resize(720, 620);

		QVBoxLayout *rootLayout = new QVBoxLayout(this);

		/* Master Hardware Encoder Settings */
		QGroupBox *masterGroup = new QGroupBox("Master Video & Hardware Encoder", this);
		QFormLayout *formLayout = new QFormLayout(masterGroup);

		m_nameEdit = new QLineEdit(QString::fromStdString(hub->name), this);
		formLayout->addRow("Master Name:", m_nameEdit);

		m_sourceTypeCombo = new QComboBox(this);
		m_sourceTypeCombo->addItem("Program Output");
		m_sourceTypeCombo->addItem("Scene");
		m_sourceTypeCombo->addItem("Source");
		m_sourceTypeCombo->setCurrentText(QString::fromStdString(hub->source_type));
		formLayout->addRow("Capture Source:", m_sourceTypeCombo);

		m_targetCombo = new QComboBox(this);
		m_targetCombo->setEditable(true);
		formLayout->addRow("Target Scene/Source:", m_targetCombo);

		m_encoderCombo = new QComboBox(this);
		m_encoderCombo->addItem("Auto Detect", "auto");
		m_encoderCombo->addItem("NVIDIA NVENC (nvh264enc)", "nvh264enc");
		m_encoderCombo->addItem("Intel QuickSync (qsvh264enc)", "qsvh264enc");
		m_encoderCombo->addItem("VAAPI GPU (vaapih264enc)", "vaapih264enc");
		m_encoderCombo->addItem("Software (x264enc)", "x264enc");
		int encIdx = m_encoderCombo->findData(QString::fromStdString(hub->gpu_encoder_type));
		if (encIdx >= 0) m_encoderCombo->setCurrentIndex(encIdx);
		formLayout->addRow("Hardware Encoder:", m_encoderCombo);

		m_bitrateSpin = new QSpinBox(this);
		m_bitrateSpin->setRange(500, 50000);
		m_bitrateSpin->setSingleStep(500);
		m_bitrateSpin->setValue(hub->bitrate_kbps);
		m_bitrateSpin->setSuffix(" kbps");
		formLayout->addRow("Master Bitrate:", m_bitrateSpin);

		m_keyintSpin = new QSpinBox(this);
		m_keyintSpin->setRange(1, 10);
		m_keyintSpin->setValue(hub->keyint_sec);
		m_keyintSpin->setSuffix(" sec");
		formLayout->addRow("Keyframe Interval:", m_keyintSpin);

		m_customPipeEdit = new QPlainTextEdit(QString::fromStdString(hub->master_pipeline), this);
		m_customPipeEdit->setPlaceholderText("Optional: Custom GStreamer Master Pipeline string (leave empty for auto hardware encoder)");
		m_customPipeEdit->setMaximumHeight(55);
		formLayout->addRow("Custom Master Pipeline:", m_customPipeEdit);

		rootLayout->addWidget(masterGroup);

		connect(m_sourceTypeCombo, &QComboBox::currentTextChanged, this, &GstMasterDetailDialog::populateTargetCombo);
		populateTargetCombo(m_sourceTypeCombo->currentText());

		/* Multiple Destinations Manager */
		QGroupBox *destGroup = new QGroupBox("Destinations (Unlimited RTSP, WebRTC, Custom Sinks)", this);
		QVBoxLayout *destLayout = new QVBoxLayout(destGroup);

		m_destTable = new QTableWidget(this);
		m_destTable->setColumnCount(6);
		m_destTable->setHorizontalHeaderLabels({"Type", "Name", "Target / Port", "Status", "Control", "Edit"});
		m_destTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
		m_destTable->setSelectionBehavior(QAbstractItemView::SelectRows);
		destLayout->addWidget(m_destTable);

		QHBoxLayout *btnLayout = new QHBoxLayout();
		QPushButton *btnAddRtsp = new QPushButton("+ Add RTSP", this);
		QPushButton *btnAddWebRTC = new QPushButton("+ Add WebRTC", this);
		QPushButton *btnAddCustom = new QPushButton("+ Add Custom Sink", this);
		QPushButton *btnRemove = new QPushButton("- Remove Selected", this);

		btnLayout->addWidget(btnAddRtsp);
		btnLayout->addWidget(btnAddWebRTC);
		btnLayout->addWidget(btnAddCustom);
		btnLayout->addWidget(btnRemove);
		destLayout->addLayout(btnLayout);

		connect(btnAddRtsp, &QPushButton::clicked, this, &GstMasterDetailDialog::addRtspDestination);
		connect(btnAddWebRTC, &QPushButton::clicked, this, &GstMasterDetailDialog::addWebRTCDestination);
		connect(btnAddCustom, &QPushButton::clicked, this, &GstMasterDetailDialog::addCustomDestination);
		connect(btnRemove, &QPushButton::clicked, this, &GstMasterDetailDialog::removeSelectedDestination);
		connect(m_destTable, &QTableWidget::cellDoubleClicked, this, &GstMasterDetailDialog::onCellDoubleClicked);

		rootLayout->addWidget(destGroup);

		/* Dialog Buttons */
		QHBoxLayout *footerLayout = new QHBoxLayout();
		QPushButton *btnSave = new QPushButton("Save Changes", this);
		QPushButton *btnClose = new QPushButton("Close", this);
		footerLayout->addStretch();
		footerLayout->addWidget(btnSave);
		footerLayout->addWidget(btnClose);
		rootLayout->addLayout(footerLayout);

		connect(btnSave, &QPushButton::clicked, this, &GstMasterDetailDialog::saveAndClose);
		connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);

		refreshDestTable();
	}

private:
	void populateTargetCombo(const QString &sourceType)
	{
		m_targetCombo->clear();
		if (sourceType == "Scene") {
			m_targetCombo->setEnabled(true);
			auto scenes = get_obs_scene_names();
			for (const auto &s : scenes) {
				m_targetCombo->addItem(QString::fromStdString(s));
			}
			m_targetCombo->setCurrentText(QString::fromStdString(m_hub->scene_name));
		} else if (sourceType == "Source") {
			m_targetCombo->setEnabled(true);
			auto sources = get_obs_source_names();
			for (const auto &s : sources) {
				m_targetCombo->addItem(QString::fromStdString(s));
			}
			m_targetCombo->setCurrentText(QString::fromStdString(m_hub->source_name));
		} else {
			m_targetCombo->setEnabled(false);
			m_targetCombo->setCurrentText("");
		}
	}

	void refreshDestTable()
	{
		m_destTable->setRowCount((int)m_hub->destinations.size());
		for (int i = 0; i < (int)m_hub->destinations.size(); i++) {
			auto &dest = m_hub->destinations[i];

			QString typeStr = (dest->type == DEST_TYPE_RTSP) ? "RTSP" :
			                  (dest->type == DEST_TYPE_WEBRTC) ? "WebRTC" : "Custom";
			m_destTable->setItem(i, 0, new QTableWidgetItem(typeStr));
			m_destTable->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(dest->name)));

			QString targetStr;
			if (dest->type == DEST_TYPE_RTSP)
				targetStr = QString(":%1%2").arg(QString::fromStdString(dest->rtsp_port), QString::fromStdString(dest->rtsp_mount));
			else if (dest->type == DEST_TYPE_WEBRTC)
				targetStr = QString("Port %1").arg(QString::fromStdString(dest->webrtc_port));
			else
				targetStr = "Custom Pipeline";
			m_destTable->setItem(i, 2, new QTableWidgetItem(targetStr));

			QString statusStr = dest->is_active ? "● Running" : "○ Stopped";
			QTableWidgetItem *statusItem = new QTableWidgetItem(statusStr);
			if (dest->is_active)
				statusItem->setForeground(QBrush(QColor(40, 167, 69)));
			m_destTable->setItem(i, 3, statusItem);

			QPushButton *actionBtn = new QPushButton(dest->is_active ? "Stop" : "Start", this);
			int rowIdx = i;
			connect(actionBtn, &QPushButton::clicked, [this, rowIdx]() {
				toggleDestination(rowIdx);
			});
			m_destTable->setCellWidget(i, 4, actionBtn);

			QPushButton *editBtn = new QPushButton("Edit", this);
			connect(editBtn, &QPushButton::clicked, [this, rowIdx]() {
				editDestination(rowIdx);
			});
			m_destTable->setCellWidget(i, 5, editBtn);
		}
	}

	void editDestination(int idx)
	{
		if (idx < 0 || idx >= (int)m_hub->destinations.size()) return;
		GstDestinationDetailDialog dlg(m_hub->destinations[idx], this);
		if (dlg.exec() == QDialog::Accepted) {
			save_dock_config();
			refreshDestTable();
		}
	}

	void onCellDoubleClicked(int row, int col)
	{
		Q_UNUSED(col);
		editDestination(row);
	}

	void toggleDestination(int idx)
	{
		if (idx < 0 || idx >= (int)m_hub->destinations.size()) return;
		auto &dest = m_hub->destinations[idx];
		if (dest->is_active) {
			stop_destination(m_hub.get(), dest.get());
		} else {
			start_destination(m_hub.get(), dest.get());
		}
		save_dock_config();
		refreshDestTable();
	}

	void addRtspDestination()
	{
		auto dest = std::make_shared<GstDestination>();
		dest->type = DEST_TYPE_RTSP;
		dest->name = "RTSP Stream " + std::to_string(m_hub->destinations.size() + 1);
		dest->rtsp_port = std::to_string(8554 + m_hub->destinations.size());
		dest->rtsp_mount = "/live" + std::to_string(m_hub->destinations.size() + 1);
		m_hub->destinations.push_back(dest);
		save_dock_config();
		refreshDestTable();
	}

	void addWebRTCDestination()
	{
		auto dest = std::make_shared<GstDestination>();
		dest->type = DEST_TYPE_WEBRTC;
		dest->name = "WebRTC Stream " + std::to_string(m_hub->destinations.size() + 1);
		dest->webrtc_port = std::to_string(8888 + m_hub->destinations.size());
		m_hub->destinations.push_back(dest);
		save_dock_config();
		refreshDestTable();
	}

	void addCustomDestination()
	{
		auto dest = std::make_shared<GstDestination>();
		dest->type = DEST_TYPE_CUSTOM;
		dest->name = "Custom Sink " + std::to_string(m_hub->destinations.size() + 1);
		dest->custom_pipeline = "autovideosink sync=false";
		m_hub->destinations.push_back(dest);
		save_dock_config();
		refreshDestTable();
	}

	void removeSelectedDestination()
	{
		int row = m_destTable->currentRow();
		if (row >= 0 && row < (int)m_hub->destinations.size()) {
			stop_destination(m_hub.get(), m_hub->destinations[row].get());
			m_hub->destinations.erase(m_hub->destinations.begin() + row);
			save_dock_config();
			refreshDestTable();
		}
	}

	void saveAndClose()
	{
		m_hub->name = m_nameEdit->text().toStdString();
		m_hub->source_type = m_sourceTypeCombo->currentText().toStdString();
		if (m_hub->source_type == "Scene")
			m_hub->scene_name = m_targetCombo->currentText().toStdString();
		else if (m_hub->source_type == "Source")
			m_hub->source_name = m_targetCombo->currentText().toStdString();

		m_hub->gpu_encoder_type = m_encoderCombo->currentData().toString().toStdString();
		m_hub->bitrate_kbps = m_bitrateSpin->value();
		m_hub->keyint_sec = m_keyintSpin->value();
		m_hub->master_pipeline = m_customPipeEdit->toPlainText().toStdString();

		save_dock_config();
		accept();
	}

	std::shared_ptr<GstMasterHub> m_hub;
	QLineEdit *m_nameEdit;
	QComboBox *m_sourceTypeCombo;
	QComboBox *m_targetCombo;
	QComboBox *m_encoderCombo;
	QSpinBox *m_bitrateSpin;
	QSpinBox *m_keyintSpin;
	QPlainTextEdit *m_customPipeEdit;
	QTableWidget *m_destTable;
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

		/* Header Controls */
		QHBoxLayout *topBar = new QHBoxLayout();
		QPushButton *btnAddMaster = new QPushButton("+ Add Master Hub", this);
		QPushButton *btnDeleteMaster = new QPushButton("- Delete Master", this);
		QPushButton *btnSaveConfig = new QPushButton("Save Config", this);

		topBar->addWidget(btnAddMaster);
		topBar->addWidget(btnDeleteMaster);
		topBar->addWidget(btnSaveConfig);
		topBar->addStretch();
		layout->addLayout(topBar);

		connect(btnAddMaster, &QPushButton::clicked, this, &GstMasterDockWidget::onAddMasterClicked);
		connect(btnDeleteMaster, &QPushButton::clicked, this, &GstMasterDockWidget::onDeleteMasterClicked);
		connect(btnSaveConfig, &QPushButton::clicked, [this]() {
			save_dock_config();
			QMessageBox::information(this, "Master Hubs", "Master Hub configuration saved successfully.");
		});

		/* Main Table of Masters */
		m_table = new QTableWidget(this);
		m_table->setColumnCount(5);
		m_table->setHorizontalHeaderLabels({"Master Hub Name", "Source", "Encoder", "Destinations", "Action"});
		m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
		m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
		m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		layout->addWidget(m_table);

		/* Double-Click Row -> Opens Child Detail Dialog */
		connect(m_table, &QTableWidget::cellDoubleClicked, this, &GstMasterDockWidget::onCellDoubleClicked);

		refreshTable();
	}

	void refreshTable()
	{
		m_table->setRowCount((int)g_master_hubs.size());
		for (int i = 0; i < (int)g_master_hubs.size(); i++) {
			auto &hub = g_master_hubs[i];

			m_table->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(hub->name)));
			m_table->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(hub->source_type)));

			QString encStr = QString("%1 (%2k)").arg(QString::fromStdString(hub->gpu_encoder_type), QString::number(hub->bitrate_kbps));
			m_table->setItem(i, 2, new QTableWidgetItem(encStr));

			int runningCount = 0;
			for (const auto &d : hub->destinations) {
				if (d->is_active) runningCount++;
			}
			QString destSummary = QString("%1 active / %2 total")
			                          .arg(QString::number(runningCount), QString::number(hub->destinations.size()));
			m_table->setItem(i, 3, new QTableWidgetItem(destSummary));

			QPushButton *editBtn = new QPushButton("Configure", this);
			int rowIdx = i;
			connect(editBtn, &QPushButton::clicked, [this, rowIdx]() {
				openDetailDialog(rowIdx);
			});
			m_table->setCellWidget(i, 4, editBtn);
		}
	}

private:
	void onCellDoubleClicked(int row, int column)
	{
		Q_UNUSED(column);
		openDetailDialog(row);
	}

	void openDetailDialog(int row)
	{
		if (row < 0 || row >= (int)g_master_hubs.size()) return;
		GstMasterDetailDialog dlg(g_master_hubs[row], this);
		dlg.exec();
		save_dock_config();
		refreshTable();
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
		save_dock_config();
		refreshTable();
	}

	void onDeleteMasterClicked()
	{
		int row = m_table->currentRow();
		if (row >= 0 && row < (int)g_master_hubs.size()) {
			/* Stop all destinations on this hub */
			for (auto &dest : g_master_hubs[row]->destinations) {
				stop_destination(g_master_hubs[row].get(), dest.get());
			}
			g_master_hubs.erase(g_master_hubs.begin() + row);
			save_dock_config();
			refreshTable();
		}
	}

	QTableWidget *m_table;
};

/* --------------------------------------------------------------------------
 * Module Lifecycle Entry Points
 * -------------------------------------------------------------------------- */
extern "C" {

void gstreamer_dock_register(void)
{
	/* Load persistent Master Hub configuration */
	load_dock_config();

	/* Create the Main Dock Widget */
	g_dock_widget = new GstMasterDockWidget();

	/* Add default Master Hub if empty */
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
		save_dock_config();
	}

	g_dock_widget->refreshTable();

	/* Register dock into OBS Studio Docks menu using obs_frontend_add_dock_by_id */
	obs_frontend_add_dock_by_id(
		"gstreamer_master_pipeline_dock",
		"GStreamer Master Hub",
		(void *)g_dock_widget
	);

	/* Check for autostart destinations */
	for (auto &hub : g_master_hubs) {
		for (auto &dest : hub->destinations) {
			if (dest->enabled && dest->autostart && !dest->is_active) {
				blog(LOG_INFO, "[obs-gstreamer-dock] Autostarting destination '%s' for hub '%s'",
					dest->name.c_str(), hub->name.c_str());
				start_destination(hub.get(), dest.get());
			}
		}
	}

	export_dock_status_json();
	blog(LOG_INFO, "[obs-gstreamer-dock] Registered GStreamer Master Hub dock via obs_frontend_add_dock_by_id (OBS 32+ / Qt 6)");
}

void gstreamer_dock_unregister(void)
{
	/* Stop all active destinations on all master hubs */
	for (auto &hub : g_master_hubs) {
		for (auto &dest : hub->destinations) {
			stop_destination(hub.get(), dest.get());
		}
	}

	save_dock_config();
	g_master_hubs.clear();

	if (g_dock_widget) {
		g_dock_widget = nullptr;
	}

	blog(LOG_INFO, "[obs-gstreamer-dock] Unregistered GStreamer Master Hub dock");
}

}
