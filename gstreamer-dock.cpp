#include <obs.h>
#include <obs-frontend-api.h>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

#include <vector>

struct gstreamer_output_config {
	QString name = "Output";
	QString source_type = "Program Output";
	QString source_name;
	QString scene_name;
	QString mode = "Pipeline";
	QString rtsp_mount = "/live";
	QString rtsp_service = "8554";
	QString rtsp_pipeline = "( appsrc name=appsrc_video is-live=true format=GST_FORMAT_TIME do-timestamp=true block=true ! queue ! video/x-raw, format=%s, width=%d, height=%d, framerate=%d/%d ! videoconvert ! x264enc tune=zerolatency speed-preset=veryfast bitrate=3000 key-int-max=30 ! video/x-h264, stream-format=byte-stream, alignment=au ! h264parse ! rtph264pay name=pay0 pt=96 )";
	QString signaling_url = "ws://127.0.0.1:8443";
	QString pipeline = "autovideosink sync=false";
	obs_output_t *output = nullptr;
	obs_view_t *view = nullptr;
	video_t *video  = nullptr;
};

struct gstreamer_dock_state {
	QWidget *widget = nullptr;
	QListWidget *outputs = nullptr;
	QToolButton *add = nullptr;
	QToolButton *edit = nullptr;
	QToolButton *remove = nullptr;
	QToolButton *move_up = nullptr;
	QToolButton *move_down = nullptr;
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
	const char *theme = obs_frontend_is_theme_dark() ? "Dark" : "Light";
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
		obs_view_remove(config.view);
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
		config.video = obs_view_add(config.view);
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
		config.video = obs_view_add(config.view);
		obs_output_set_media(config.output, config.video, obs_get_audio());

		obs_source_release(source);
		obs_scene_release(scene);
	}
}

static void stop_output(gstreamer_output_config &config)
{
	if (!config.output)
		return;
	if (obs_output_active(config.output))
		obs_output_stop(config.output);
	obs_output_release(config.output);
	config.output = nullptr;
}

static obs_data_t *output_settings(const gstreamer_output_config &config)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "rtsp_server", config.mode == "RTSP");
	obs_data_set_string(settings, "rtsp_mount", config.rtsp_mount.toUtf8().constData());
	obs_data_set_string(settings, "rtsp_service", config.rtsp_service.toUtf8().constData());
	obs_data_set_string(settings, "rtsp_pipeline", config.rtsp_pipeline.toUtf8().constData());
	obs_data_set_bool(settings, "webrtc_output", config.mode == "WebRTC");
	obs_data_set_string(settings, "webrtc_signaling_url", config.signaling_url.toUtf8().constData());
	obs_data_set_string(settings, "pipeline", config.pipeline.toUtf8().constData());
	return settings;
}

static void save_configurations(const gstreamer_dock_state *state)
{
	QSettings settings(profile_settings_path(), QSettings::IniFormat);
	settings.beginGroup("obs-gstreamer/outputs");
	settings.remove("");
	settings.setValue("count", static_cast<int>(state->configurations.size()));
	for (size_t index = 0; index < state->configurations.size(); ++index) {
		const auto &config = state->configurations[index];
		settings.beginGroup(QString::number(index));
		settings.setValue("name", config.name);
		settings.setValue("source_type", config.source_type);
		settings.setValue("source_name", config.source_name);
		settings.setValue("scene_name", config.scene_name);
		settings.setValue("mode", config.mode);
		settings.setValue("rtsp_mount", config.rtsp_mount);
		settings.setValue("rtsp_service", config.rtsp_service);
		settings.setValue("rtsp_pipeline", config.rtsp_pipeline);
		settings.setValue("signaling_url", config.signaling_url);
		settings.setValue("pipeline", config.pipeline);
		settings.endGroup();
	}
	settings.endGroup();
}

static void load_configurations(gstreamer_dock_state *state)
{
	QSettings settings(profile_settings_path(), QSettings::IniFormat);
	settings.beginGroup("obs-gstreamer/outputs");
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
		config.pipeline = settings.value("pipeline", config.pipeline).toString();
		state->configurations.push_back(config);
		settings.endGroup();
	}
	settings.endGroup();
}

static bool edit_configuration(QWidget *parent, gstreamer_output_config *config)
{
	QDialog dialog(parent);
	dialog.setWindowTitle("GStreamer Output");
	dialog.resize(760, 430);
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
	QComboBox *mode = new QComboBox();
	mode->addItems({"RTSP", "WebRTC", "Pipeline"});
	mode->setCurrentText(config->mode);
	QLineEdit *mount = new QLineEdit(config->rtsp_mount);
	QLineEdit *service = new QLineEdit(config->rtsp_service);
	QPlainTextEdit *rtsp_pipeline = new QPlainTextEdit(config->rtsp_pipeline);
	rtsp_pipeline->setMinimumHeight(100);
	QLineEdit *signaling = new QLineEdit(config->signaling_url);
	QLineEdit *pipeline = new QLineEdit(config->pipeline);
	rtsp_pipeline->setMinimumWidth(430);
	left_form->addRow("Name", name);
	left_form->addRow("Source type", source_type);
	left_form->addRow("Source", source);
	left_form->addRow("Scene", scene);
	left_form->addRow("Output mode", mode);
	left_form->addRow("RTSP mount", mount);
	left_form->addRow("RTSP service", service);
	right_form->addRow("RTSP pipeline", rtsp_pipeline);
	right_form->addRow("WebRTC signaling", signaling);
	right_form->addRow("Pipeline", pipeline);
	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	right_form->addRow(buttons);
	columns->addWidget(left_panel, 1);
	columns->addWidget(right_panel, 2);
	auto update_enabled = [source_type, source, scene, mode, mount, service, rtsp_pipeline, signaling, pipeline]() {
		source->setEnabled(source_type->currentText() == "Source");
		scene->setEnabled(source_type->currentText() == "Scene");
		mount->setEnabled(mode->currentText() == "RTSP");
		service->setEnabled(mode->currentText() == "RTSP");
		rtsp_pipeline->setEnabled(mode->currentText() == "RTSP");
		signaling->setEnabled(mode->currentText() == "WebRTC");
		pipeline->setEnabled(mode->currentText() == "Pipeline");
	};
	QObject::connect(source_type, &QComboBox::currentTextChanged, update_enabled);
	QObject::connect(mode, &QComboBox::currentTextChanged, update_enabled);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	update_enabled();
	if (dialog.exec() != QDialog::Accepted)
		return false;
	config->name = name->text().trimmed();
	config->source_type = source_type->currentText();
	config->source_name = source->currentText();
	config->scene_name = scene->currentText();
	config->mode = mode->currentText();
	config->rtsp_mount = mount->text();
	config->rtsp_service = service->text();
	config->rtsp_pipeline = rtsp_pipeline->toPlainText();
	config->signaling_url = signaling->text();
	config->pipeline = pipeline->text();
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
		const QString status = config.output && obs_output_active(config.output) ? "Running" : "Stopped";
		QListWidgetItem *item = new QListWidgetItem(state->outputs);
		QWidget *row_widget = new QWidget(state->outputs);
		row_widget->setStyleSheet("QLabel { color: palette(WindowText); }");
		QHBoxLayout *row_layout = new QHBoxLayout(row_widget);
		row_layout->setContentsMargins(8, 2, 8, 2);
		row_layout->setSpacing(6);
		QLabel *label = new QLabel(QString("%1  [%2]").arg(config.name, status));
		QPushButton *start = new QPushButton("Start");
		QPushButton *stop = new QPushButton("Stop");
		start->setFixedSize(64, 24);
		stop->setFixedSize(64, 24);
		label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		start->setToolTip("Start output");
		stop->setToolTip("Stop output");
		const bool running = config.output && obs_output_active(config.output);
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
}

static void start_selected(gstreamer_dock_state *state, int row)
{
	if (row < 0 || row >= static_cast<int>(state->configurations.size())) return;
	gstreamer_output_config &config = state->configurations[row];
	stop_output(config);
	obs_data_t *settings = output_settings(config);
	config.output = obs_output_create("hjm-gstreamer-output", config.name.toUtf8().constData(), settings, nullptr);
	obs_data_release(settings);
	if (!config.output)	return;
	select_source(config);

	if (!obs_output_start(config.output)) stop_output(config);
	refresh_rows(state);
	save_configurations(state);
}

static void stop_selected(gstreamer_dock_state *state, int row)
{
	if (row >= 0 && row < static_cast<int>(state->configurations.size()))
		stop_output(state->configurations[row]);
	refresh_rows(state);
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
	for (QToolButton *button : {state->add, state->edit, state->remove, state->move_up, state->move_down})
		button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	manage->addWidget(state->add);
	manage->addWidget(state->edit);
	manage->addWidget(state->remove);
	manage->addWidget(state->move_up);
	manage->addWidget(state->move_down);
	manage->addStretch();
	layout->addLayout(manage);
	load_configurations(state);
	if (state->configurations.empty())
		state->configurations.emplace_back();
	refresh_rows(state);
	QObject::connect(widget, &QObject::destroyed, [state]() {
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
		if (row >= 0 && edit_configuration(state->widget, &state->configurations[row])) {
			refresh_rows(state);
			save_configurations(state);
		}
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
		const bool running = state->configurations[row].output && obs_output_active(state->configurations[row].output);
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
		refresh_rows(state);
	});
	timer->setInterval(1000);
	timer->start();
	return widget;
}

extern "C" void gstreamer_dock_register(void)
{
	obs_frontend_add_dock_by_id("obs-gstreamer-dock", "GStreamer Output", create_gstreamer_dock_widget());
}

extern "C" void gstreamer_dock_unregister(void)
{
	obs_frontend_remove_dock("obs-gstreamer-dock");
}
