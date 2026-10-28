/*
 * obs-gstreamer: Master Pipeline Hub Dock (OBS 32+ / Qt 6)
 *
 * Implements a native OBS Dock using obs_frontend_add_dock_by_id:
 * - Main Window: Listed in OBS "Docks" menu, displays all configured Master Hubs.
 * - Child Window (Double-click): Opens detailed editor for master encoder & destinations.
 * - Multi-destination support: Unlimited RTSP, WebRTC, and Custom pipeline destinations
 *   attached to a master hub (e.g. 2 RTSP, 3 WebRTC concurrently).
 * - No scenes or filters cluttered.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <gst/gst.h>

#include "gstreamer-master-pipeline.h"
#include "gstreamer-rtspserver.h"
#include "gstreamer-render-callback.h"
#include "gstreamer-helper.h"

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
 * Destination Data Structures (Supports Multiple Destinations of Any Type)
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
		  master_pipeline(DEFAULT_MASTER_PIPELINE),
		  is_active(false),
		  active_render_cb(nullptr)
	{
	}
};

/* Global list of all Master Hubs managed by the Dock */
static std::vector<std::shared_ptr<GstMasterHub>> g_master_hubs;

/* Forward declaration */
class GstMasterDockWidget;
static GstMasterDockWidget *g_dock_widget = nullptr;

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
}

static bool start_destination(GstMasterHub *hub, GstDestination *dest)
{
	stop_destination(hub, dest);

	if (dest->type == DEST_TYPE_RTSP) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "name", hub->name.c_str());
		obs_data_set_string(settings, "source_type", hub->source_type.c_str());

		const char *src_name = "";
		if (hub->source_type == "Scene")
			src_name = hub->scene_name.c_str();
		else if (hub->source_type == "Source")
			src_name = hub->source_name.c_str();

		obs_data_set_string(settings, "source_name", src_name);
		obs_data_set_string(settings, "render_mode", hub->render_mode.c_str());

		int port = atoi(dest->rtsp_port.c_str());
		if (port <= 0) port = 8554;
		obs_data_set_int(settings, "rtsp_port", port);
		obs_data_set_string(settings, "rtsp_service", dest->rtsp_port.c_str());
		obs_data_set_string(settings, "rtsp_mount", dest->rtsp_mount.c_str());

		obs_data_set_string(settings, "encoder_type", hub->gpu_encoder_type.c_str());
		obs_data_set_int(settings, "bitrate_kbps", hub->bitrate_kbps);
		obs_data_set_int(settings, "keyint_sec", hub->keyint_sec);
		obs_data_set_string(settings, "master_pipeline", hub->master_pipeline.c_str());
		obs_data_set_string(settings, "rtsp_pipeline", dest->rtsp_pipeline.c_str());

		blog(LOG_INFO, "[obs-gstreamer-dock] Starting RTSP destination on port %s%s...",
			dest->rtsp_port.c_str(), dest->rtsp_mount.c_str());

		dest->rtsp_branch = gst_render_hub_start_branch_data(settings);
		obs_data_release(settings);

		if (!dest->rtsp_branch) {
			blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to start RTSP branch");
			return false;
		}
		dest->is_active = true;
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
		return true;
	}

	return false;
}

/* --------------------------------------------------------------------------
 * Child Window: Detailed Configuration Dialog (Opened on Double-Click)
 * -------------------------------------------------------------------------- */
class GstMasterDetailDialog : public QDialog {
public:
	GstMasterDetailDialog(std::shared_ptr<GstMasterHub> hub, QWidget *parent = nullptr)
		: QDialog(parent), m_hub(hub)
	{
		setWindowTitle(QString("Edit Master: %1").arg(QString::fromStdString(hub->name)));
		resize(680, 560);

		QVBoxLayout *rootLayout = new QVBoxLayout(this);

		/* Tab 1: Master Hardware Encoder */
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

		m_targetNameEdit = new QLineEdit(this);
		if (hub->source_type == "Scene")
			m_targetNameEdit->setText(QString::fromStdString(hub->scene_name));
		else if (hub->source_type == "Source")
			m_targetNameEdit->setText(QString::fromStdString(hub->source_name));
		formLayout->addRow("Target Name (Scene/Source):", m_targetNameEdit);

		m_encoderCombo = new QComboBox(this);
		m_encoderCombo->addItem("Auto Detect", "auto");
		m_encoderCombo->addItem("NVIDIA NVENC", "nvenc");
		m_encoderCombo->addItem("Intel / AMD QuickSync (VAAPI)", "vaapi");
		m_encoderCombo->addItem("Apple VideoToolbox", "vtenc");
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

		rootLayout->addWidget(masterGroup);

		/* Tab 2: Multiple Destinations Manager */
		QGroupBox *destGroup = new QGroupBox("Destinations (Unlimited RTSP, WebRTC, Custom Pipelines)", this);
		QVBoxLayout *destLayout = new QVBoxLayout(destGroup);

		m_destTable = new QTableWidget(this);
		m_destTable->setColumnCount(5);
		m_destTable->setHorizontalHeaderLabels({"Type", "Name", "Target / Port", "Status", "Action"});
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
		}
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
		refreshDestTable();
	}

	void addWebRTCDestination()
	{
		auto dest = std::make_shared<GstDestination>();
		dest->type = DEST_TYPE_WEBRTC;
		dest->name = "WebRTC Stream " + std::to_string(m_hub->destinations.size() + 1);
		dest->webrtc_port = std::to_string(8888 + m_hub->destinations.size());
		m_hub->destinations.push_back(dest);
		refreshDestTable();
	}

	void addCustomDestination()
	{
		auto dest = std::make_shared<GstDestination>();
		dest->type = DEST_TYPE_CUSTOM;
		dest->name = "Custom Sink " + std::to_string(m_hub->destinations.size() + 1);
		dest->custom_pipeline = "autovideosink sync=false";
		m_hub->destinations.push_back(dest);
		refreshDestTable();
	}

	void removeSelectedDestination()
	{
		int row = m_destTable->currentRow();
		if (row >= 0 && row < (int)m_hub->destinations.size()) {
			stop_destination(m_hub.get(), m_hub->destinations[row].get());
			m_hub->destinations.erase(m_hub->destinations.begin() + row);
			refreshDestTable();
		}
	}

	void saveAndClose()
	{
		m_hub->name = m_nameEdit->text().toStdString();
		m_hub->source_type = m_sourceTypeCombo->currentText().toStdString();
		if (m_hub->source_type == "Scene")
			m_hub->scene_name = m_targetNameEdit->text().toStdString();
		else if (m_hub->source_type == "Source")
			m_hub->source_name = m_targetNameEdit->text().toStdString();

		m_hub->gpu_encoder_type = m_encoderCombo->currentData().toString().toStdString();
		m_hub->bitrate_kbps = m_bitrateSpin->value();
		m_hub->keyint_sec = m_keyintSpin->value();
		accept();
	}

	std::shared_ptr<GstMasterHub> m_hub;
	QLineEdit *m_nameEdit;
	QComboBox *m_sourceTypeCombo;
	QLineEdit *m_targetNameEdit;
	QComboBox *m_encoderCombo;
	QSpinBox *m_bitrateSpin;
	QSpinBox *m_keyintSpin;
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
		topBar->addWidget(btnAddMaster);
		topBar->addWidget(btnDeleteMaster);
		topBar->addStretch();
		layout->addLayout(topBar);

		connect(btnAddMaster, &QPushButton::clicked, this, &GstMasterDockWidget::onAddMasterClicked);
		connect(btnDeleteMaster, &QPushButton::clicked, this, &GstMasterDockWidget::onDeleteMasterClicked);

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
		refreshTable();
	}

	void onAddMasterClicked()
	{
		auto hub = std::make_shared<GstMasterHub>();
		hub->name = "Master Hub " + std::to_string(g_master_hubs.size() + 1);

		/* Add a default RTSP destination */
		auto defaultRtsp = std::make_shared<GstDestination>();
		defaultRtsp->name = "RTSP Main";
		hub->destinations.push_back(defaultRtsp);

		g_master_hubs.push_back(hub);
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
	/* Create the Main Dock Widget */
	g_dock_widget = new GstMasterDockWidget();

	/* Add default Master Hub if empty */
	if (g_master_hubs.empty()) {
		auto defaultHub = std::make_shared<GstMasterHub>();
		defaultHub->name = "Program Master Hub";
		auto defaultRtsp = std::make_shared<GstDestination>();
		defaultRtsp->name = "Default RTSP";
		defaultHub->destinations.push_back(defaultRtsp);
		g_master_hubs.push_back(defaultHub);
		g_dock_widget->refreshTable();
	}

	/* Register dock into OBS Studio Docks menu using obs_frontend_add_dock_by_id */
	obs_frontend_add_dock_by_id(
		"gstreamer_master_pipeline_dock",
		"GStreamer Master Hub",
		(void *)g_dock_widget
	);

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

	g_master_hubs.clear();

	if (g_dock_widget) {
		g_dock_widget = nullptr;
	}

	blog(LOG_INFO, "[obs-gstreamer-dock] Unregistered GStreamer Master Hub dock");
}

}
