#
# obs-gstreamer. OBS Studio plugin.
# Copyright (C) 2018-2021 Florian Zwoch <fzwoch@gmail.com>
#
# This file is part of obs-gstreamer.
#
# obs-gstreamer is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# obs-gstreamer is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with obs-gstreamer. If not, see <http://www.gnu.org/licenses/>.
#

project('obs-gstreamer', ['c', 'cpp'],
  default_options : [
    'prefix=~/.config/obs-studio/plugins/obs-gstreamer/bin/64bit',
    'libdir='
  ],
)

obs_dep = dependency('libobs', required : false)
if not obs_dep.found()
  obs_dep = meson.get_compiler('c').find_library('obs', has_headers : 'obs/obs.h')
endif

obs_frontend_dep = meson.get_compiler('cpp').find_library('obs-frontend-api', has_headers : 'obs/obs-frontend-api.h')

name_suffix = []
if host_machine.system() == 'darwin'
  name_suffix = 'so'
endif

qt6 = import('qt6', required : false)
qt_dep = dependency('qt6', modules : ['Widgets'], required : false)
if not qt_dep.found()
  qt5 = import('qt5')
  qt_dep = dependency('qt5', modules : ['Widgets'])
endif

shared_library('obs-gstreamer',
  'gstreamer.c',
  'gstreamer-source.c',
  'gstreamer-encoder.c',
  'gstreamer-filter.c',
  'gstreamer-output.c',
  'gstreamer-dock.cpp',
  vcs_tag(
    command : ['git', 'rev-parse', '--short', 'HEAD'],
    input : 'version.c.in',
    output : 'version.c',
  ),
  name_prefix : '',
  name_suffix : name_suffix,
  dependencies : [
    obs_dep,
    obs_frontend_dep,
    qt_dep,
    dependency('gstreamer-1.0', version : '>=1.16.0'),
    dependency('gstreamer-video-1.0'),
    dependency('gstreamer-audio-1.0'),
    dependency('gstreamer-app-1.0'),
    dependency('gstreamer-net-1.0'),
    dependency('gstreamer-rtsp-server-1.0'),
    dependency('gstreamer-webrtc-1.0'),
    dependency('gstreamer-sdp-1.0'),
  ],
  install : true,
)
