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

#pragma once

#include <obs-module.h>
#include <gst/gst.h>

struct gstreamer_webrtc;

struct gstreamer_webrtc *gstreamer_webrtc_create(obs_output_t *output, obs_data_t *settings,
						 struct obs_video_info *ovi, char **error);
void gstreamer_webrtc_destroy(struct gstreamer_webrtc *webrtc);

GstElement *gstreamer_webrtc_appsrc(struct gstreamer_webrtc *webrtc);
