#include <obs.h>
#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

struct gstreamer_dock_state {
	QWidget *widget = nullptr;
	QComboBox *source_type = nullptr;
	QComboBox *source_list = nullptr;
	QComboBox *scene_list = nullptr;
	QPushButton *refresh = nullptr;
	QCheckBox *rtsp_server = nullptr;
	QLineEdit *rtsp_mount = nullptr;
	QLineEdit *rtsp_service = nullptr;
	QCheckBox *webrtc_output = nullptr;
	QLineEdit *webrtc_signaling_url = nullptr;
	QLineEdit *pipeline = nullptr;
	QPushButton *start = nullptr;
	QPushButton *stop = nullptr;
	obs_output_t *output = nullptr;
};

static QStringList find_source_names(void)
{
	QStringList names;
	obs_enum_sources(
		[](void *data, obs_source_t *source) -> bool {
			QStringList *dst = static_cast<QStringList *>(data);
			const char *name = obs_source_get_name(source);
			if (name && *name)
				dst->append(QString::fromUtf8(name));
			return true;
		},
		&names);
	return names;
}

static QStringList find_scene_names(void)
{
	QStringList scenes;
	obs_enum_scenes(
		[](void *data, obs_source_t *scene) -> bool {
			QStringList *dst = static_cast<QStringList *>(data);
			const char *name = obs_source_get_name(scene);
			if (name && *name)
				dst->append(QString::fromUtf8(name));
			return true;
		},
		&scenes);
	return scenes;
}

static obs_source_t *select_source_for_target(const char *type_string,
								   const char *source_name,
								   const char *scene_name)
{
	obs_source_t *selected = nullptr;
	if (strcmp(type_string, "Program Output") == 0) {
		selected = obs_get_output_source(0);
	} else if (strcmp(type_string, "Program Preview") == 0) {
		selected = obs_frontend_get_current_preview_scene();
	} else if (strcmp(type_string, "Source") == 0 && source_name && *source_name) {
		selected = obs_get_source_by_name(source_name);
	} else if (strcmp(type_string, "Scene") == 0 && scene_name && *scene_name) {
		obs_scene_t *scene = obs_get_scene_by_name(scene_name);
		if (scene) {
			selected = obs_scene_get_source(scene);
			obs_scene_release(scene);
		}
	}
	return selected;
}

static void populate_targets(gstreamer_dock_state *state)
{
	const QString current_source = state->source_list->currentText();
	const QString current_scene = state->scene_list->currentText();

	state->source_list->clear();
	QStringList sources = find_source_names();
	for (const QString &source : sources)
		state->source_list->addItem(source);
	if (!current_source.isEmpty())
		state->source_list->setCurrentText(current_source);
	if (state->source_list->count() == 0)
		state->source_list->addItem("No sources");

	state->scene_list->clear();
	QStringList scenes = find_scene_names();
	for (const QString &scene : scenes)
		state->scene_list->addItem(scene);
	if (!current_scene.isEmpty())
		state->scene_list->setCurrentText(current_scene);
	if (state->scene_list->count() == 0)
		state->scene_list->addItem("No scenes");

	const QString type = state->source_type->currentText();
	const bool use_source = (type == "Source");
	const bool use_scene = (type == "Scene");
	state->source_list->setEnabled(use_source && state->source_list->count() > 0);
	state->scene_list->setEnabled(use_scene && state->scene_list->count() > 0);
}

static void reset_output(gstreamer_dock_state *state)
{
	if (state->output) {
		if (obs_output_active(state->output))
			obs_output_stop(state->output);
		obs_output_release(state->output);
		state->output = nullptr;
	}
}

static void start_button_clicked(gstreamer_dock_state *state)
{
	reset_output(state);

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "rtsp_server", state->rtsp_server->isChecked());
	obs_data_set_string(settings, "rtsp_mount",
		state->rtsp_mount->text().toUtf8().constData());
	obs_data_set_string(settings, "rtsp_service",
		state->rtsp_service->text().toUtf8().constData());
	obs_data_set_bool(settings, "webrtc_output", state->webrtc_output->isChecked());
	obs_data_set_string(settings, "webrtc_signaling_url",
		state->webrtc_signaling_url->text().toUtf8().constData());
	obs_data_set_string(settings, "pipeline",
		state->pipeline->text().toUtf8().constData());

	state->output = obs_output_create("hjm-gstreamer-output",
		"obs-gstreamer-dock-output", settings, nullptr);
	obs_data_release(settings);
	if (!state->output) {
		blog(LOG_ERROR, "[obs-gstreamer] Failed to create output");
		return;
	}

	obs_source_t *target = select_source_for_target(
		state->source_type->currentText().toUtf8().constData(),
		state->source_list->currentText().toUtf8().constData(),
		state->scene_list->currentText().toUtf8().constData());
	if (target) {
		obs_set_output_source(0, target);
		obs_source_release(target);
	}

	if (!obs_output_start(state->output)) {
		blog(LOG_ERROR, "[obs-gstreamer] Failed to start output");
		reset_output(state);
		return;
	}

	state->start->setEnabled(false);
	state->stop->setEnabled(true);
}

static void stop_button_clicked(gstreamer_dock_state *state)
{
	reset_output(state);
	state->start->setEnabled(true);
	state->stop->setEnabled(false);
}

static void source_type_changed(gstreamer_dock_state *state)
{
	populate_targets(state);
}

static QWidget *create_gstreamer_dock_widget(void)
{
	gstreamer_dock_state *state = new gstreamer_dock_state();
	QWidget *widget = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(widget);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	state->widget = widget;

	QComboBox *source_type = new QComboBox();
	source_type->addItem("Program Output");
	source_type->addItem("Program Preview");
	source_type->addItem("Scene");
	source_type->addItem("Source");
	state->source_type = source_type;
	layout->addWidget(new QLabel("Source Type"));
	layout->addWidget(source_type);

	state->source_list = new QComboBox();
	state->source_list->setEnabled(false);
	layout->addWidget(new QLabel("Source"));
	layout->addWidget(state->source_list);

	state->scene_list = new QComboBox();
	state->scene_list->setEnabled(false);
	layout->addWidget(new QLabel("Scene"));
	layout->addWidget(state->scene_list);

	state->refresh = new QPushButton("Refresh Lists");
	layout->addWidget(state->refresh);

	state->rtsp_server = new QCheckBox("RTSP server");
	layout->addWidget(state->rtsp_server);

	QFormLayout *form = new QFormLayout();
	form->setContentsMargins(0, 0, 0, 0);
	state->rtsp_mount = new QLineEdit("/live");
	state->rtsp_service = new QLineEdit("8554");
	state->webrtc_output = new QCheckBox("Start WebRTC output");
	state->webrtc_signaling_url = new QLineEdit("ws://127.0.0.1:8443");
	state->pipeline = new QLineEdit("autovideosink sync=false");
	form->addRow("RTSP mount", state->rtsp_mount);
	form->addRow("RTSP service", state->rtsp_service);
	layout->addWidget(state->webrtc_output);
	form->addRow("WebRTC Signaling URL", state->webrtc_signaling_url);
	form->addRow("Pipeline", state->pipeline);
	layout->addLayout(form);

	QHBoxLayout *button_row = new QHBoxLayout();
	state->start = new QPushButton("Start");
	state->stop = new QPushButton("Stop");
	state->stop->setEnabled(false);
	button_row->addWidget(state->start);
	button_row->addWidget(state->stop);
	layout->addLayout(button_row);

	QObject::connect(state->source_type, QOverload<int>::of(&QComboBox::currentIndexChanged),
		[state]() { source_type_changed(state); });
	QObject::connect(state->refresh, &QPushButton::clicked, [state]() { populate_targets(state); });
	QObject::connect(state->start, &QPushButton::clicked, [state]() { start_button_clicked(state); });
	QObject::connect(state->stop, &QPushButton::clicked, [state]() { stop_button_clicked(state); });

	QTimer *refresh_timer = new QTimer(widget);
	QObject::connect(refresh_timer, &QTimer::timeout, [state]() { populate_targets(state); });
	refresh_timer->setInterval(1500);
	refresh_timer->start();

	populate_targets(state);
	state->source_type->setCurrentText("Program Output");
	return widget;
}

extern "C" void gstreamer_dock_register(void)
{
	QWidget *dock_widget = create_gstreamer_dock_widget();
	obs_frontend_add_dock_by_id("obs-gstreamer-dock", "GStreamer Output", dock_widget);
}

extern "C" void gstreamer_dock_unregister(void)
{
	obs_frontend_remove_dock("obs-gstreamer-dock");
}
