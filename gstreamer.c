/*
 * obs-gstreamer. OBS Studio plugin.
 * Copyright (C) 2018-2021 Florian Zwoch <fzwoch@gmail.com>
 *
 * This file is part of obs-gstreamer.
 *
 * obs-gstreamer is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * obs-gstreamer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with obs-gstreamer. If not, see <http://www.gnu.org/licenses/>.
 */

#include <obs-module.h>
#include <plugin-support.h>
#include <gst/gst.h>

#include "gstreamer-encoder-tex.h"
#include "gstreamer-render-hub.h"

OBS_DECLARE_MODULE()

// gstreamer-output.c
extern const char *gstreamer_output_get_name(void *type_data);
extern void *gstreamer_output_create(obs_data_t *settings, obs_output_t *output);
extern void gstreamer_output_destroy(void *data);
extern bool gstreamer_output_start(void *data);
extern void gstreamer_output_stop(void *data, uint64_t ts);
extern void gstreamer_output_encoded_packet(void *data, struct encoder_packet *packet);
extern void gstreamer_output_raw_video(void *data, struct video_data *frame);
extern void gstreamer_output_raw_audio(void *data, struct audio_data *frame);
extern void gstreamer_output_get_defaults(obs_data_t *settings);
extern obs_properties_t *gstreamer_output_get_properties(void *data);

// gstreamer-source.c
extern struct obs_source_info gstreamer_source_info;
extern struct obs_source_info gstreamer_source_video_info;

// gstreamer-filter.c
extern struct obs_source_info gstreamer_filter_video_info;
extern struct obs_source_info gstreamer_filter_video_gpu_info;
extern struct obs_source_info gstreamer_filter_audio_info;

#ifndef ENABLE_DOCK
#define ENABLE_DOCK 1
#endif

#if ENABLE_DOCK
extern void gstreamer_dock_register(void);
extern void gstreamer_dock_unregister(void);
#else
static inline void gstreamer_dock_register(void)
{
	blog(LOG_WARNING, "[obs-gstreamer] Dock UI is DISABLED in this build (compiled with ENABLE_DOCK=0)");
}
static inline void gstreamer_dock_unregister(void) {}
#endif

bool obs_module_load(void)
{
	blog(LOG_INFO, "[obs-gstreamer] Loading plugin (version: %s)", PLUGIN_VERSION);

	gst_init(NULL, NULL);
	blog(LOG_INFO, "[obs-gstreamer] GStreamer runtime initialized: %s", gst_version_string());

	struct obs_output_info output_info = {
		.id = "hjm-gstreamer-output",
		.flags = OBS_OUTPUT_VIDEO,
		.get_name = gstreamer_output_get_name,
		.create = gstreamer_output_create,
		.destroy = gstreamer_output_destroy,
		.start = gstreamer_output_start,
		.stop = gstreamer_output_stop,

		.encoded_packet = gstreamer_output_encoded_packet,
		.raw_video = gstreamer_output_raw_video,
		.raw_audio = gstreamer_output_raw_audio,

		.get_defaults = gstreamer_output_get_defaults,
		.get_properties = gstreamer_output_get_properties,
	};
	obs_register_output(&output_info);
	blog(LOG_INFO, "[obs-gstreamer] Registered raw output module: hjm-gstreamer-output");

	struct obs_output_info output_encoded_info = {
		.id = "hjm-gstreamer-output-encoded",
		.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED,
		.get_name = gstreamer_output_get_name,
		.create = gstreamer_output_create,
		.destroy = gstreamer_output_destroy,
		.start = gstreamer_output_start,
		.stop = gstreamer_output_stop,

		.encoded_packet = gstreamer_output_encoded_packet,
		.raw_video = gstreamer_output_raw_video,
		.raw_audio = gstreamer_output_raw_audio,

		.get_defaults = gstreamer_output_get_defaults,
		.get_properties = gstreamer_output_get_properties,
	};
	obs_register_output(&output_encoded_info);
	blog(LOG_INFO, "[obs-gstreamer] Registered encoded output module: hjm-gstreamer-output-encoded");

	obs_register_source(&gstreamer_source_info);
	blog(LOG_INFO, "[obs-gstreamer] Registered source module: gstreamer-source");

	obs_register_source(&gstreamer_source_video_info);
	blog(LOG_INFO, "[obs-gstreamer] Registered source module: gstreamer-source-video");

	obs_register_source(&gstreamer_filter_video_info);
	blog(LOG_INFO, "[obs-gstreamer] Registered filter module: gstreamer-filter-video (CPU)");

	obs_register_source(&gstreamer_filter_video_gpu_info);
	blog(LOG_INFO, "[obs-gstreamer] Registered filter module: gstreamer-filter-video-gpu (GPU)");

	obs_register_source(&gstreamer_filter_audio_info);
	blog(LOG_INFO, "[obs-gstreamer] Registered filter module: gstreamer-filter-audio");

	gstreamer_encoder_tex_register();
	gstreamer_dock_register();
	blog(LOG_INFO, "[obs-gstreamer] Plugin loaded successfully");

	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[obs-gstreamer] Unloading plugin...");
	gst_render_hub_shutdown();
	gstreamer_dock_unregister();
	gstreamer_encoder_tex_unregister();
	blog(LOG_INFO, "[obs-gstreamer] Plugin unloaded successfully");
}
