#include <obs.h>
#include <obs-frontend-api.h>
#include "gstreamer-render-hub.h"

#include <QApplication>
#include <QPalette>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStringList>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QDockWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QStandardItemModel>

#include <vector>

#ifndef MAKE_SEMANTIC_VERSION
#define MAKE_SEMANTIC_VERSION(maj, min, patch) \
	((((maj) & 0xFF) << 24) | (((min) & 0xFF) << 16) | ((patch) & 0xFFFF))
#endif

#ifndef LIBOBS_API_VER
#if defined(LIBOBS_API_MAJOR_VER) && defined(LIBOBS_API_MINOR_VER) && defined(LIBOBS_API_PATCH_VER)
#define LIBOBS_API_VER MAKE_SEMANTIC_VERSION(LIBOBS_API_MAJOR_VER, LIBOBS_API_MINOR_VER, LIBOBS_API_PATCH_VER)
#else
#define LIBOBS_API_VER 0
#endif
#endif

static const char *RENDER_MODE_CALLBACK = "Render Callback (GPU Texture)";
static const char *RENDER_MODE_GPU_TEX  = "OBS Encoder (GPU Texture)";
static const char *RENDER_MODE_CPU_RAM  = "OBS Output (CPU RAM)";

static const char *DEFAULT_MASTER_PIPELINE =
	"appsrc name=hub_appsrc is-live=true format=GST_FORMAT_TIME do-timestamp=true "
	"caps=\"video/x-raw, format=BGRA, width=%u, height=%u, framerate=%u/%u\" ! "
	"videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast bframes=0 key-int-max=60 bitrate=4000 ! "
	"h264parse config-interval=-1 ! appsink name=hub_appsink sync=false drop=true max-buffers=1";

static const char *DEFAULT_HUB_RTSP_PIPELINE =
	"( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true "
	"caps=\"video/x-h264, stream-format=byte-stream, alignment=au\" ! "
	"h264parse config-interval=-1 ! rtph264pay name=pay0 pt=96 config-interval=1 )";

struct gstreamer_output_config {
	QString name = "Output";
	QString source_type = "Program Output";
	QString source_name;
	QString scene_name;
	QString render_mode = RENDER_MODE_CALLBACK;
	QString mode = "RTSP";
	QString rtsp_mount = "/live";
	QString rtsp_service = "8554";
	QString rtsp_pipeline = "( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true "
	                        "caps=\"video/x-h264, stream-format=byte-stream, alignment=au\" ! "
	                        "h264parse config-interval=-1 ! rtph264pay name=pay0 pt=96 config-interval=1 )";
	QString signaling_url = "ws://127.0.0.1:8443";
	QString webrtc_http_port = "8888";
	QString webrtc_web_root;
	QString pipeline = "autovideosink sync=false";
	bool auto_start = true;
	bool use_gpu_encoder = false;
	bool use_render_hub = true;
	QString gpu_encoder_type = "auto";
	QString color_space_mode = "auto";
	int bitrate_kbps = 4000;
	obs_output_t *output = nullptr;
	obs_encoder_t *encoder = nullptr;
	obs_view_t *view = nullptr;
	video_t *video  = nullptr;
	gst_hub_branch_t *hub_branch = nullptr;
};

struct gstreamer_dock_state {
	QWidget *widget = nullptr;
	QListWidget *outputs = nullptr;
	QToolButton *add = nullptr;
	QToolButton *edit = nullptr;
	QToolButton *remove = nullptr;
	QToolButton *move_up = nullptr;
	QToolButton *move_down = nullptr;
	QToolButton *hub_settings = nullptr;
	QString master_pipeline = DEFAULT_MASTER_PIPELINE;
	std::vector<gstreamer_output_config> configurations;
};

static QString profile_settings_path(void)
{
	char *profile_path = obs_frontend_get_current_profile_path();
	QString path;
	if (profile_path && *profile_path) {
		path = QDir::fromNativeSeparators(QString::fromUtf8(profile_path)) + "/obs-gstreamer.ini";
	}
	bfree(profile_path);
	return path;
}

static QStringList source_names(void)
{
	QStringList names;
	obs_enum_sources([](void *data, obs_source_t *source) -> bool {
		QStringList *result = static_cast<QStringList *>(data);
		const char *name = obs_source_get_name(source);
		if (name && *name)
			result->append(QString::fromUtf8(name));
		return true;
	}, &names);
	return names;
}

static QStringList scene_names(void)
{
	QStringList names;
		obs_enum_scenes([](void *data, obs_source_t *scene) -> bool {
		QStringList *result = static_cast<QStringList *>(data);
		const char *name = obs_source_get_name(scene);
		if (name && *name)
			result->append(QString::fromUtf8(name));
		return true;
	}, &names);
	return names;
}

static QIcon obs_theme_icon(const QString &name)
{
#if defined(LIBOBS_API_VER) && LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(28, 0, 0)
	const char *theme = obs_frontend_is_theme_dark() ? "Dark" : "Light";
#else
	bool is_dark = true;
	if (qApp && qApp->palette().color(QPalette::Window).lightness() > 128)
		is_dark = false;
	const char *theme = is_dark ? "Dark" : "Light";
#endif
	const QString rel = QString("themes/%1/%2.svg").arg(theme, name);
	char *path = obs_find_data_file(rel.toUtf8().constData());
	if (path) {
		QIcon icon(QString::fromUtf8(path));
		bfree(path);
		return icon;
	}
	const QStringList roots = {
		QString::fromUtf8(qgetenv("OBS_DATA_PATH")),
		"/usr/share/obs/obs-studio",
		"/usr/local/share/obs/obs-studio",
		"/app/share/obs/obs-studio",
		"/opt/obs-studio/share/obs/obs-studio",
	};
	for (const QString &root : roots) {
		if (root.isEmpty())
			continue;
		const QString file = root + "/" + rel;
		if (QFile::exists(file))
			return QIcon(file);
	}
	return QIcon(QString(":/res/images/%1.svg").arg(name));
}

static void select_source(gstreamer_output_config &config)
{
	if (config.view) 
	{
#if defined(LIBOBS_API_VER) && LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(28, 0, 0)
		obs_view_remove(config.view);
#endif
		obs_view_destroy(config.view);
		config.view = nullptr;
		config.video = nullptr;
	}
	if (config.source_type == "Program Output") 
	{	
		obs_output_set_media(config.output, obs_get_video(), obs_get_audio());
	}
	else if (config.source_type == "Source")
	{
		obs_source_t *source = obs_get_source_by_name(config.source_name.toUtf8().constData());
		if (!source) return;

		config.view = obs_view_create();
		obs_view_set_source(config.view, 0, source);
#if defined(LIBOBS_API_VER) && LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(28, 0, 0)
		config.video = obs_view_add(config.view);
#else
		config.video = obs_get_video();
#endif
		obs_output_set_media(config.output, config.video, obs_get_audio());

		obs_source_release(source);
	}
	else if (config.source_type == "Scene") 
	{
		obs_scene_t *scene = obs_get_scene_by_name(config.scene_name.toUtf8().constData());
		if (!scene) return;
		obs_source_t *source = obs_scene_get_source(scene);
		if (!source) 
		{
			obs_scene_release(scene);
			return;
		}

		config.view = obs_view_create();
		obs_view_set_source(config.view, 0, source);
#if defined(LIBOBS_API_VER) && LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(28, 0, 0)
		config.video = obs_view_add(config.view);
#else
		config.video = obs_get_video();
#endif
		obs_output_set_media(config.output, config.video, obs_get_audio());

		obs_source_release(source);
		obs_scene_release(scene);
	}
}

static bool is_config_active(const gstreamer_output_config &config)
{
	if (config.use_render_hub && config.hub_branch)
		return gst_render_hub_is_branch_active(config.hub_branch);
	return config.output && obs_output_active(config.output);
}

static void stop_output(gstreamer_output_config &config)
{
	if (config.hub_branch) {
		gst_render_hub_stop_branch(config.hub_branch);
		config.hub_branch = nullptr;
	}
	if (config.output) {
		if (obs_output_active(config.output))
			obs_output_stop(config.output);
		obs_output_release(config.output);
		config.output = nullptr;
	}
	if (config.encoder) {
		obs_encoder_release(config.encoder);
		config.encoder = nullptr;
	}
	if (config.view) {
#if defined(LIBOBS_API_VER) && LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(28, 0, 0)
		obs_view_remove(config.view);
#endif
		obs_view_destroy(config.view);
		config.view = nullptr;
		config.video = nullptr;
	}
}

static obs_data_t *output_settings(const gstreamer_output_config &config)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "rtsp_server", config.mode == "RTSP");
	obs_data_set_string(settings, "rtsp_mount", config.rtsp_mount.toUtf8().constData());
	obs_data_set_string(settings, "rtsp_service", config.rtsp_service.toUtf8().constData());
	obs_data_set_string(settings, "rtsp_pipeline", config.rtsp_pipeline.toUtf8().constData());
	obs_data_set_bool(settings, "webrtc_output", config.mode == "WebRTC");
	obs_data_set_bool(settings, "webrtc_auto_start", config.auto_start);
	obs_data_set_string(settings, "webrtc_http_port", config.webrtc_http_port.toUtf8().constData());
	obs_data_set_string(settings, "webrtc_web_root", config.webrtc_web_root.toUtf8().constData());
	obs_data_set_string(settings, "webrtc_signaling_url", config.signaling_url.toUtf8().constData());
	obs_data_set_string(settings, "pipeline", config.pipeline.toUtf8().constData());
	return settings;
}

static void save_configurations(const gstreamer_dock_state *state)
{
	QSettings settings(profile_settings_path(), QSettings::IniFormat);
	settings.beginGroup("obs-gstreamer/outputs");
	settings.remove("");
	settings.setValue("master_pipeline", state->master_pipeline);
	settings.setValue("count", static_cast<int>(state->configurations.size()));
	for (size_t index = 0; index < state->configurations.size(); ++index) {
		const auto &config = state->configurations[index];
		settings.beginGroup(QString::number(index));
		settings.setValue("name", config.name);
		settings.setValue("source_type", config.source_type);
		settings.setValue("source_name", config.source_name);
		settings.setValue("scene_name", config.scene_name);
		settings.setValue("render_mode", config.render_mode);
		settings.setValue("mode", config.mode);
		settings.setValue("rtsp_mount", config.rtsp_mount);
		settings.setValue("rtsp_service", config.rtsp_service);
		settings.setValue("rtsp_pipeline", config.rtsp_pipeline);
		settings.setValue("signaling_url", config.signaling_url);
		settings.setValue("webrtc_http_port", config.webrtc_http_port);
		settings.setValue("webrtc_web_root", config.webrtc_web_root);
		settings.setValue("auto_start", config.auto_start);
		settings.setValue("pipeline", config.pipeline);
		settings.setValue("use_gpu_encoder", config.use_gpu_encoder);
		settings.setValue("use_render_hub", config.use_render_hub);
		settings.setValue("gpu_encoder_type", config.gpu_encoder_type);
		settings.setValue("color_space_mode", "auto");
		settings.setValue("bitrate_kbps", config.bitrate_kbps);
		settings.endGroup();
	}
	settings.endGroup();
}

static void load_configurations(gstreamer_dock_state *state)
{
	QSettings settings(profile_settings_path(), QSettings::IniFormat);
	settings.beginGroup("obs-gstreamer/outputs");
	state->master_pipeline = settings.value("master_pipeline", DEFAULT_MASTER_PIPELINE).toString();
	const int count = settings.value("count", 0).toInt();
	for (int index = 0; index < count; ++index) {
		settings.beginGroup(QString::number(index));
		gstreamer_output_config config;
		config.name = settings.value("name", QString("Output %1").arg(index + 1)).toString();
		config.source_type = settings.value("source_type", config.source_type).toString();
		config.source_name = settings.value("source_name").toString();
		config.scene_name = settings.value("scene_name").toString();
		config.mode = settings.value("mode", config.mode).toString();
		config.rtsp_mount = settings.value("rtsp_mount", config.rtsp_mount).toString();
		config.rtsp_service = settings.value("rtsp_service", config.rtsp_service).toString();
		config.rtsp_pipeline = settings.value("rtsp_pipeline", config.rtsp_pipeline).toString();
		config.signaling_url = settings.value("signaling_url", config.signaling_url).toString();
		config.webrtc_http_port = settings.value("webrtc_http_port", config.webrtc_http_port).toString();
		config.webrtc_web_root = settings.value("webrtc_web_root", config.webrtc_web_root).toString();
		config.auto_start = settings.value("auto_start", config.auto_start).toBool();
		config.pipeline = settings.value("pipeline", config.pipeline).toString();
		config.use_gpu_encoder = settings.value("use_gpu_encoder", config.use_gpu_encoder).toBool();
		config.use_render_hub = settings.value("use_render_hub", config.use_render_hub).toBool();
		config.render_mode = settings.value("render_mode").toString();
		if (config.render_mode.isEmpty()) {
			if (config.use_render_hub)
				config.render_mode = RENDER_MODE_CALLBACK;
			else if (config.use_gpu_encoder)
				config.render_mode = RENDER_MODE_GPU_TEX;
			else
				config.render_mode = RENDER_MODE_CPU_RAM;
		} else {
			config.use_render_hub = (config.render_mode == RENDER_MODE_CALLBACK);
			config.use_gpu_encoder = (config.render_mode == RENDER_MODE_GPU_TEX);
		}
		config.gpu_encoder_type = settings.value("gpu_encoder_type", "auto").toString();
		config.color_space_mode = "auto";
		config.bitrate_kbps = settings.value("bitrate_kbps", config.bitrate_kbps).toInt();
		state->configurations.push_back(config);
		settings.endGroup();
	}
	settings.endGroup();
}

static bool edit_configuration(QWidget *parent, gstreamer_output_config *config)
{
	QDialog dialog(parent);
	dialog.setWindowTitle("GStreamer Output");
	dialog.resize(760, 380);
	QHBoxLayout *columns = new QHBoxLayout(&dialog);
	QWidget *left_panel = new QWidget(&dialog);
	QWidget *right_panel = new QWidget(&dialog);
	QFormLayout *left_form = new QFormLayout(left_panel);
	QFormLayout *right_form = new QFormLayout(right_panel);

	QLineEdit *name = new QLineEdit(config->name);

	QComboBox *source_type = new QComboBox();
	source_type->addItems({"Program Output", "Scene", "Source"});
	source_type->setCurrentText(config->source_type);

	QComboBox *source = new QComboBox();
	source->addItems(source_names());
	source->setCurrentText(config->source_name);

	QComboBox *scene = new QComboBox();
	scene->addItems(scene_names());
	scene->setCurrentText(config->scene_name);

	QComboBox *render_mode = new QComboBox();
	render_mode->addItems({RENDER_MODE_CALLBACK, RENDER_MODE_GPU_TEX, RENDER_MODE_CPU_RAM});
	if (config->render_mode.isEmpty()) {
		if (config->use_render_hub)
			render_mode->setCurrentText(RENDER_MODE_CALLBACK);
		else if (config->use_gpu_encoder)
			render_mode->setCurrentText(RENDER_MODE_GPU_TEX);
		else
			render_mode->setCurrentText(RENDER_MODE_CPU_RAM);
	} else {
		render_mode->setCurrentText(config->render_mode);
	}

	QComboBox *mode = new QComboBox();
	mode->addItems({"RTSP", "WebRTC", "Pipeline"});
	mode->setCurrentText(config->mode);

	QLineEdit *mount = new QLineEdit(config->rtsp_mount);
	QLineEdit *service = new QLineEdit(config->rtsp_service);

	QPlainTextEdit *rtsp_pipeline = new QPlainTextEdit(config->rtsp_pipeline);
	rtsp_pipeline->setMinimumHeight(100);
	rtsp_pipeline->setMinimumWidth(430);

	QLineEdit *http_port = new QLineEdit(config->webrtc_http_port);
	QLineEdit *web_root = new QLineEdit(config->webrtc_web_root);
	web_root->setPlaceholderText("~/.local/share/obs-gstreamer/webrtc");

	QCheckBox *auto_start = new QCheckBox("Start output automatically when OBS launches");
	auto_start->setChecked(config->auto_start);

	QLineEdit *pipeline = new QLineEdit(config->pipeline);

	left_form->addRow("Name", name);
	left_form->addRow("Source type", source_type);
	left_form->addRow("Source", source);
	left_form->addRow("Scene", scene);
	left_form->addRow("Render mode", render_mode);
	left_form->addRow("Output mode", mode);
	left_form->addRow("RTSP mount", mount);
	left_form->addRow("RTSP service", service);
	left_form->addRow("Auto-start", auto_start);

	right_form->addRow("RTSP pipeline", rtsp_pipeline);
	right_form->addRow("WebRTC HTTP port", http_port);
	right_form->addRow("WebRTC web root", web_root);
	right_form->addRow("Pipeline", pipeline);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	right_form->addRow(buttons);

	columns->addWidget(left_panel, 1);
	columns->addWidget(right_panel, 2);

	auto update_visibility = [&dialog, left_form, right_form, source_type, source, scene, render_mode, mode, mount, service, rtsp_pipeline, http_port, web_root, pipeline]() {
		const QString cur_source_type = source_type->currentText();
		left_form->setRowVisible(source, cur_source_type == "Source");
		left_form->setRowVisible(scene, cur_source_type == "Scene");

		const QString cur_render_mode = render_mode->currentText();
		const bool is_callback = (cur_render_mode == RENDER_MODE_CALLBACK);

		// If Render Callback is active, WebRTC is not supported on direct GPU hub branches.
		// If WebRTC is currently selected, switch to RTSP.
		if (is_callback && mode->currentText() == "WebRTC") {
			mode->setCurrentText("RTSP");
		}
		QStandardItemModel *mode_model = qobject_cast<QStandardItemModel *>(mode->model());
		if (mode_model) {
			for (int i = 0; i < mode_model->rowCount(); ++i) {
				QStandardItem *item = mode_model->item(i);
				if (item && item->text() == "WebRTC") {
					item->setEnabled(!is_callback);
					item->setSelectable(!is_callback);
				}
			}
		}

		const QString cur_mode = mode->currentText();
		const bool is_rtsp = (cur_mode == "RTSP");
		const bool is_webrtc = (cur_mode == "WebRTC");
		const bool is_pipe = (cur_mode == "Pipeline");

		left_form->setRowVisible(mount, is_rtsp);
		left_form->setRowVisible(service, is_rtsp);
		right_form->setRowVisible(rtsp_pipeline, is_rtsp);

		right_form->setRowVisible(http_port, is_webrtc);
		right_form->setRowVisible(web_root, is_webrtc);

		right_form->setRowVisible(pipeline, is_pipe);

		dialog.adjustSize();
	};

	QObject::connect(source_type, &QComboBox::currentTextChanged, update_visibility);
	QObject::connect(render_mode, &QComboBox::currentTextChanged, update_visibility);
	QObject::connect(render_mode, &QComboBox::currentTextChanged, [rtsp_pipeline](const QString &cur_rm) {
		if (cur_rm == RENDER_MODE_CALLBACK &&
		    (rtsp_pipeline->toPlainText().trimmed().isEmpty() || rtsp_pipeline->toPlainText().contains("x264enc"))) {
			rtsp_pipeline->setPlainText(DEFAULT_HUB_RTSP_PIPELINE);
		}
	});
	QObject::connect(mode, &QComboBox::currentTextChanged, update_visibility);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	update_visibility();

	if (dialog.exec() != QDialog::Accepted)
		return false;

	config->name = name->text().trimmed();
	config->source_type = source_type->currentText();
	config->source_name = source->currentText();
	config->scene_name = scene->currentText();
	config->render_mode = render_mode->currentText();
	config->use_render_hub = (config->render_mode == RENDER_MODE_CALLBACK);
	config->use_gpu_encoder = (config->render_mode == RENDER_MODE_GPU_TEX);
	config->mode = mode->currentText();
	config->rtsp_mount = mount->text();
	config->rtsp_service = service->text();
	config->rtsp_pipeline = rtsp_pipeline->toPlainText();
	config->webrtc_http_port = http_port->text();
	config->webrtc_web_root = web_root->text();
	config->auto_start = auto_start->isChecked();
	config->pipeline = pipeline->text();
	config->color_space_mode = "auto";
	if (config->name.isEmpty())
		config->name = "Output";
	return true;
}

static void start_selected(gstreamer_dock_state *state, int row);
static void stop_selected(gstreamer_dock_state *state, int row);

static void refresh_rows(gstreamer_dock_state *state)
{
	const int selected = state->outputs->currentRow();
	state->outputs->clear();
	for (size_t index = 0; index < state->configurations.size(); ++index) {
		const auto &config = state->configurations[index];
		const bool running = is_config_active(config);
		const QString status = running ? "Running" : "Stopped";
		QListWidgetItem *item = new QListWidgetItem(state->outputs);
		QWidget *row_widget = new QWidget(state->outputs);
		row_widget->setStyleSheet("QLabel { color: palette(WindowText); }");
		QHBoxLayout *row_layout = new QHBoxLayout(row_widget);
		row_layout->setContentsMargins(8, 2, 8, 2);
		row_layout->setSpacing(6);
		QString label_text;
		if (config.render_mode == RENDER_MODE_CALLBACK || config.use_render_hub) {
			label_text = QString("%1 [Render Callback: %2]  [%3]").arg(config.name, config.mode, status);
		} else if (config.render_mode == RENDER_MODE_GPU_TEX || config.use_gpu_encoder) {
			label_text = QString("%1 [GPU Texture: %2]  [%3]").arg(config.name, config.mode, status);
		} else {
			label_text = QString("%1 [CPU RAM: %2]  [%3]").arg(config.name, config.mode, status);
		}
		QLabel *label = new QLabel(label_text);
		QPushButton *start = new QPushButton("Start");
		QPushButton *stop = new QPushButton("Stop");
		start->setFixedSize(64, 24);
		stop->setFixedSize(64, 24);
		label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		start->setToolTip("Start output");
		stop->setToolTip("Stop output");
		start->setEnabled(!running);
		stop->setEnabled(running);
		label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
		row_layout->addWidget(label, 1);
		row_layout->addWidget(start);
		row_layout->addWidget(stop);
		item->setSizeHint(row_widget->sizeHint());
		state->outputs->setItemWidget(item, row_widget);
		QObject::connect(start, &QPushButton::clicked, [state, index]() { start_selected(state, static_cast<int>(index)); });
		QObject::connect(stop, &QPushButton::clicked, [state, index]() { stop_selected(state, static_cast<int>(index)); });
	}
	if (!state->configurations.empty())
		state->outputs->setCurrentRow(qBound(0, selected, state->outputs->count() - 1));

	const int cur = state->outputs->currentRow();
	const bool cur_running = (cur >= 0 && cur < static_cast<int>(state->configurations.size())) ?
		is_config_active(state->configurations[cur]) : false;
	state->edit->setEnabled(!cur_running);
	state->remove->setEnabled(!cur_running);
}

static void update_row_states(gstreamer_dock_state *state)
{
	if (state->outputs->count() != static_cast<int>(state->configurations.size())) {
		refresh_rows(state);
		return;
	}

	for (size_t index = 0; index < state->configurations.size(); ++index) {
		const auto &config = state->configurations[index];
		const bool running = is_config_active(config);
		const QString status = running ? "Running" : "Stopped";

		QListWidgetItem *item = state->outputs->item(static_cast<int>(index));
		if (!item) continue;
		QWidget *row_widget = state->outputs->itemWidget(item);
		if (!row_widget) continue;

		QLabel *label = row_widget->findChild<QLabel *>();
		auto buttons = row_widget->findChildren<QPushButton *>();

		QString label_text;
		if (config.render_mode == RENDER_MODE_CALLBACK || config.use_render_hub) {
			label_text = QString("%1 [Render Callback: %2]  [%3]").arg(config.name, config.mode, status);
		} else if (config.render_mode == RENDER_MODE_GPU_TEX || config.use_gpu_encoder) {
			label_text = QString("%1 [GPU Texture: %2]  [%3]").arg(config.name, config.mode, status);
		} else {
			label_text = QString("%1 [CPU RAM: %2]  [%3]").arg(config.name, config.mode, status);
		}

		if (label && label->text() != label_text) {
			label->setText(label_text);
		}

		if (buttons.size() >= 2) {
			QPushButton *start_btn = buttons[0];
			QPushButton *stop_btn = buttons[1];
			if (start_btn->isEnabled() == running) start_btn->setEnabled(!running);
			if (stop_btn->isEnabled() != running) stop_btn->setEnabled(running);
		}
	}

	const int cur = state->outputs->currentRow();
	const bool cur_running = (cur >= 0 && cur < static_cast<int>(state->configurations.size())) ?
		is_config_active(state->configurations[cur]) : false;
	state->edit->setEnabled(!cur_running);
	state->remove->setEnabled(!cur_running);
}

static void start_selected(gstreamer_dock_state *state, int row)
{
	if (row < 0 || row >= static_cast<int>(state->configurations.size())) return;
	gstreamer_output_config &config = state->configurations[row];
	blog(LOG_INFO, "[obs-gstreamer-dock] Starting output #%d: '%s' (mode=%s, source_type=%s, source_name=%s)",
		row, config.name.toUtf8().constData(), config.mode.toUtf8().constData(),
		config.source_type.toUtf8().constData(),
		(config.source_type == "Scene" ? config.scene_name : config.source_name).toUtf8().constData());

	// Port Conflict Detection: If starting an RTSP server, check if port is already taken by another active output
	const bool is_rtsp = config.use_render_hub || (config.mode == "RTSP");
	if (is_rtsp) {
		const QString target_port = config.rtsp_service.trimmed().isEmpty() ? "8554" : config.rtsp_service.trimmed();
		for (size_t i = 0; i < state->configurations.size(); ++i) {
			if (static_cast<int>(i) == row) continue;
			const auto &other = state->configurations[i];
			if (is_config_active(other)) {
				const bool other_is_rtsp = other.use_render_hub || (other.mode == "RTSP");
				const QString other_port = other.rtsp_service.trimmed().isEmpty() ? "8554" : other.rtsp_service.trimmed();
				if (other_is_rtsp && other_port == target_port) {
					blog(LOG_ERROR, "[obs-gstreamer-dock] Port conflict: '%s' requested port %s which is in use by '%s'",
						config.name.toUtf8().constData(), target_port.toUtf8().constData(), other.name.toUtf8().constData());
					QMessageBox::critical(
						state->widget,
						"RTSP Port Conflict",
						QString("Cannot start '%1': Port %2 is already in use by active output '%3'.\n\n"
						        "Starting a second RTSP server on the same port is not allowed. "
						        "Please configure a different port before starting this output.")
							.arg(config.name, target_port, other.name));
					return;
				}
			}
		}
	}

	stop_output(config);

	if (config.use_render_hub) {
		gst_hub_output_params_t params = {};
		params.name = config.name.toUtf8().constData();
		params.source_type = config.source_type.toUtf8().constData();
		params.source_name = (config.source_type == "Scene") ? config.scene_name.toUtf8().constData() : config.source_name.toUtf8().constData();
		params.rtsp_service = config.rtsp_service.toUtf8().constData();
		params.rtsp_mount = config.rtsp_mount.toUtf8().constData();
		params.encoder_type = config.gpu_encoder_type.toUtf8().constData();
		params.bitrate_kbps = config.bitrate_kbps > 0 ? config.bitrate_kbps : 4000;
		params.keyint_sec = 1;
		params.master_pipeline = state->master_pipeline.toUtf8().constData();
		params.rtsp_pipeline = config.rtsp_pipeline.toUtf8().constData();

		config.hub_branch = gst_render_hub_start_branch(&params);
		if (!config.hub_branch) {
			const QString display_port = config.rtsp_service.trimmed().isEmpty() ? "8554" : config.rtsp_service.trimmed();
			blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to start Direct GPU RTSP server for '%s' on port %s",
				config.name.toUtf8().constData(), display_port.toUtf8().constData());
			QMessageBox::critical(
				state->widget,
				"Failed to Start RTSP Server",
				QString("Could not start Direct GPU RTSP server for '%1' on port %2.\n\n"
				        "The port may already be in use by another application or server.")
					.arg(config.name, display_port));
		}
		refresh_rows(state);
		save_configurations(state);
		return;
	}

	obs_data_t *settings = output_settings(config);
	config.output = obs_output_create("hjm-gstreamer-output", config.name.toUtf8().constData(), settings, nullptr);
	obs_data_release(settings);
	if (!config.output) {
		blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to create output object 'hjm-gstreamer-output'");
		return;
	}
	select_source(config);

	if (config.use_gpu_encoder) {
		const char *enc_id = config.gpu_encoder_type.contains("h264") ?
			"hjm-gstreamer-encoder-tex-h264" : "hjm-gstreamer-encoder-tex-h265";
		obs_data_t *enc_settings = obs_data_create();
		obs_data_set_string(enc_settings, "encoder_type", config.gpu_encoder_type.toUtf8().constData());
		obs_data_set_string(enc_settings, "color_space_mode", config.color_space_mode.toUtf8().constData());
		obs_data_set_int(enc_settings, "bitrate", config.bitrate_kbps > 0 ? config.bitrate_kbps : 4000);
		config.encoder = obs_video_encoder_create(enc_id, "gst_hw_tex_encoder", enc_settings, nullptr);
		obs_data_release(enc_settings);
		if (config.encoder) {
			obs_output_set_video_encoder(config.output, config.encoder);
			blog(LOG_INFO, "[obs-gstreamer-dock] Bound hardware texture encoder '%s' to output", enc_id);
		} else {
			blog(LOG_ERROR, "[obs-gstreamer-dock] Failed to create hardware texture encoder '%s'", enc_id);
		}
	}

	if (!obs_output_start(config.output)) {
		const char *err = obs_output_get_last_error(config.output);
		blog(LOG_ERROR, "[obs-gstreamer-dock] obs_output_start failed for '%s': %s",
			config.name.toUtf8().constData(), (err && *err) ? err : "unknown error");
		stop_output(config);
		QMessageBox::critical(
			state->widget,
			"Failed to Start Output",
			QString("Failed to start output '%1': %2")
				.arg(config.name, (err && *err) ? QString::fromUtf8(err) : "Port already in use or pipeline initialization failed."));
	} else {
		blog(LOG_INFO, "[obs-gstreamer-dock] Output '%s' started successfully", config.name.toUtf8().constData());
	}
	refresh_rows(state);
	save_configurations(state);
}

static void stop_selected(gstreamer_dock_state *state, int row)
{
	if (row >= 0 && row < static_cast<int>(state->configurations.size())) {
		blog(LOG_INFO, "[obs-gstreamer-dock] Stopping output #%d: '%s'",
			row, state->configurations[row].name.toUtf8().constData());
		stop_output(state->configurations[row]);
	}
	refresh_rows(state);
}

static void edit_master_pipeline(QWidget *parent, gstreamer_dock_state *state)
{
	QDialog dialog(parent);
	dialog.setWindowTitle("Direct GPU Hub - Master Pipeline Configuration");
	dialog.resize(720, 380);
	QVBoxLayout *layout = new QVBoxLayout(&dialog);

	QLabel *info = new QLabel(
		"<b>Master GPU Encoder Pipeline (Runs once in VRAM, feeds all RTSP branches):</b><br>"
		"• Ingestion element: <code>appsrc name=hub_appsrc</code> (Format: BGRA)<br>"
		"• Codec output element: <code>appsink name=hub_appsink</code> (Format: H.264 byte-stream)<br>"
		"• Placeholders: <code>%u</code> for width, height, fps_num, fps_den.",
		&dialog);
	info->setWordWrap(true);
	layout->addWidget(info);

	QPlainTextEdit *editor = new QPlainTextEdit(&dialog);
	editor->setPlainText(state->master_pipeline);
	layout->addWidget(editor, 1);

	QHBoxLayout *btn_row = new QHBoxLayout();
	QPushButton *reset_btn = new QPushButton("Reset to Default", &dialog);
	QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);

	btn_row->addWidget(reset_btn);
	btn_row->addStretch();
	btn_row->addWidget(box);
	layout->addLayout(btn_row);

	QObject::connect(reset_btn, &QPushButton::clicked, [editor]() {
		editor->setPlainText(DEFAULT_MASTER_PIPELINE);
	});
	QObject::connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	if (dialog.exec() == QDialog::Accepted) {
		state->master_pipeline = editor->toPlainText().trimmed();
		save_configurations(state);
	}
}

static bool g_dock_registered = false;
static QDockWidget *g_dock_widget = nullptr;
static const char *DOCK_ID = "obs-gstreamer-dock";

static void dock_frontend_event(enum obs_frontend_event event, void *private_data)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		auto *st = static_cast<gstreamer_dock_state *>(private_data);
		if (!st) return;
		for (int index = 0; index < static_cast<int>(st->configurations.size()); ++index) {
			if (st->configurations[index].auto_start && !is_config_active(st->configurations[index])) {
				start_selected(st, index);
			}
		}
	}
}

static QWidget *create_gstreamer_dock_widget(void)
{
	auto *state = new gstreamer_dock_state();
	QWidget *widget = new QWidget();
	state->widget = widget;
	QVBoxLayout *layout = new QVBoxLayout(widget);
	state->outputs = new QListWidget();
	state->outputs->setContextMenuPolicy(Qt::CustomContextMenu);
	state->outputs->setStyleSheet(
		"QListWidget::item { margin: 0; padding: 0; }"
		"QListWidget::item:selected { background: palette(highlight); }"
		"QListWidget::item:selected QLabel { color: palette(HighlightedText); }");
	layout->addWidget(state->outputs);
	QHBoxLayout *manage = new QHBoxLayout();
	manage->setContentsMargins(0, 0, 0, 0);
	manage->setSpacing(2);
	state->add = new QToolButton();
	state->add->setAutoRaise(true);
	state->add->setIcon(obs_theme_icon("plus"));
	state->add->setIconSize(QSize(16, 16));
	state->add->setToolTip("Add output");
	state->edit = new QToolButton();
	state->edit->setAutoRaise(true);
	state->edit->setIcon(obs_theme_icon("cogs"));
	state->edit->setIconSize(QSize(16, 16));
	state->edit->setToolTip("Edit selected output");
	state->remove = new QToolButton();
	state->remove->setAutoRaise(true);
	state->remove->setIcon(obs_theme_icon("trash"));
	state->remove->setIconSize(QSize(16, 16));
	state->remove->setToolTip("Remove selected output");
	state->move_up = new QToolButton();
	state->move_up->setAutoRaise(true);
	state->move_up->setIcon(obs_theme_icon("up"));
	state->move_up->setIconSize(QSize(16, 16));
	state->move_up->setToolTip("Move output up");
	state->move_down = new QToolButton();
	state->move_down->setAutoRaise(true);
	state->move_down->setIcon(obs_theme_icon("down"));
	state->move_down->setIconSize(QSize(16, 16));
	state->move_down->setToolTip("Move output down");
	state->hub_settings = new QToolButton();
	state->hub_settings->setAutoRaise(true);
	state->hub_settings->setIcon(obs_theme_icon("settings"));
	state->hub_settings->setIconSize(QSize(16, 16));
	state->hub_settings->setToolTip("Direct GPU Hub: Configure Master Pipeline");
	for (QToolButton *button : {state->add, state->edit, state->remove, state->move_up, state->move_down, state->hub_settings})
		button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	manage->addWidget(state->add);
	manage->addWidget(state->edit);
	manage->addWidget(state->remove);
	manage->addWidget(state->move_up);
	manage->addWidget(state->move_down);
	manage->addWidget(state->hub_settings);
	manage->addStretch();
	layout->addLayout(manage);
	load_configurations(state);
	if (state->configurations.empty())
		state->configurations.emplace_back();
	refresh_rows(state);
	obs_frontend_add_event_callback(dock_frontend_event, state);
	QTimer::singleShot(200, widget, [state]() {
		for (int index = 0; index < static_cast<int>(state->configurations.size()); ++index) {
			if (state->configurations[index].auto_start && !is_config_active(state->configurations[index]))
				start_selected(state, index);
		}
	});
	QObject::connect(widget, &QObject::destroyed, [state]() {
		obs_frontend_remove_event_callback(dock_frontend_event, state);
		for (auto &config : state->configurations)
			stop_output(config);
		save_configurations(state);
		delete state;
	});
	QObject::connect(state->add, &QPushButton::clicked, [state]() {
		gstreamer_output_config config;
		config.name = QString("Output %1").arg(state->configurations.size() + 1);
		if (edit_configuration(state->widget, &config)) {
			state->configurations.push_back(config);
			refresh_rows(state);
			save_configurations(state);
		}
	});
	QObject::connect(state->edit, &QPushButton::clicked, [state]() {
		const int row = state->outputs->currentRow();
		if (row >= 0) {
			if (is_config_active(state->configurations[row]))
				return;
			if (edit_configuration(state->widget, &state->configurations[row])) {
				refresh_rows(state);
				save_configurations(state);
			}
		}
	});
	QObject::connect(state->hub_settings, &QPushButton::clicked, [state]() {
		edit_master_pipeline(state->widget, state);
	});
	QObject::connect(state->outputs, &QListWidget::currentRowChanged, [state](int row) {
		const bool cur_running = (row >= 0 && row < static_cast<int>(state->configurations.size())) ?
			is_config_active(state->configurations[row]) : false;
		state->edit->setEnabled(!cur_running);
		state->remove->setEnabled(!cur_running);
	});
	QObject::connect(state->remove, &QPushButton::clicked, [state]() {
		const int row = state->outputs->currentRow();
		if (row >= 0) {
			stop_output(state->configurations[row]);
			state->configurations.erase(state->configurations.begin() + row);
			refresh_rows(state);
			save_configurations(state);
		}
	});
	QObject::connect(state->move_up, &QPushButton::clicked, [state]() {
		const int row = state->outputs->currentRow();
		if (row > 0) {
			std::swap(state->configurations[row], state->configurations[row - 1]);
			refresh_rows(state);
			state->outputs->setCurrentRow(row - 1);
			save_configurations(state);
		}
	});
	QObject::connect(state->move_down, &QPushButton::clicked, [state]() {
		const int row = state->outputs->currentRow();
		if (row >= 0 && row + 1 < static_cast<int>(state->configurations.size())) {
			std::swap(state->configurations[row], state->configurations[row + 1]);
			refresh_rows(state);
			state->outputs->setCurrentRow(row + 1);
			save_configurations(state);
		}
	});
	QObject::connect(state->outputs, &QListWidget::itemDoubleClicked, [state](QListWidgetItem *) { state->edit->click(); });
	QObject::connect(state->outputs, &QListWidget::customContextMenuRequested, [state](const QPoint &position) {
		QListWidgetItem *item = state->outputs->itemAt(position);
		if (!item)
			return;
		state->outputs->setCurrentItem(item);
		QMenu menu(state->outputs);
		QAction *start = menu.addAction("Start");
		QAction *stop = menu.addAction("Stop");
		menu.addSeparator();
		QAction *edit = menu.addAction("Edit...");
		QAction *remove = menu.addAction("Remove");
		const int row = state->outputs->row(item);
		const bool running = is_config_active(state->configurations[row]);
		start->setEnabled(!running);
		stop->setEnabled(running);
		remove->setEnabled(!running);
		QAction *chosen = menu.exec(state->outputs->viewport()->mapToGlobal(position));
		if (chosen == start)
			start_selected(state, row);
		else if (chosen == stop)
			stop_selected(state, row);
		else if (chosen == edit)
			state->edit->click();
		else if (chosen == remove)
			state->remove->click();
	});
	QTimer *timer = new QTimer(widget);
	QObject::connect(timer, &QTimer::timeout, [state]() {
		update_row_states(state);
	});
	timer->setInterval(1000);
	timer->start();
	return widget;
}

extern "C" void gstreamer_dock_register(void)
{
	if (!g_dock_registered) {
		blog(LOG_INFO, "[obs-gstreamer-dock] Registering 'GStreamer Output' dock with OBS frontend (ID: %s)", DOCK_ID);
#if (defined(LIBOBS_API_MAJOR_VER) && (LIBOBS_API_MAJOR_VER >= 30)) || (defined(LIBOBS_API_VER) && (LIBOBS_API_VER >= 0x1E000000))
		QWidget *widget = create_gstreamer_dock_widget();
		obs_frontend_add_dock_by_id(DOCK_ID, "GStreamer Output", widget);
#else
		QWidget *main_win = (QWidget *)obs_frontend_get_main_window();
		g_dock_widget = new QDockWidget("GStreamer Output", main_win);
		g_dock_widget->setObjectName(DOCK_ID);
		g_dock_widget->setWidget(create_gstreamer_dock_widget());
		obs_frontend_add_dock(g_dock_widget);
#endif
		g_dock_registered = true;
		blog(LOG_INFO, "[obs-gstreamer-dock] 'GStreamer Output' dock registered successfully");
	}
}

extern "C" void gstreamer_dock_unregister(void)
{
	if (g_dock_registered) {
#if (defined(LIBOBS_API_MAJOR_VER) && (LIBOBS_API_MAJOR_VER >= 30)) || (defined(LIBOBS_API_VER) && (LIBOBS_API_VER >= 0x1E000000))
		obs_frontend_remove_dock(DOCK_ID);
#else
		if (g_dock_widget) {
			delete g_dock_widget;
			g_dock_widget = nullptr;
		}
#endif
		g_dock_registered = false;
	}
}
