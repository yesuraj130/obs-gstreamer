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
    'libdir=',
    'cpp_std=c++17'
  ],
)

obs_dep = dependency('libobs', required : false)
if not obs_dep.found()
  obs_dep = meson.get_compiler('c').find_library('obs', has_headers : 'obs/obs.h', required : false)
endif
if not obs_dep.found()
  obs_dep = meson.get_compiler('c').find_library('obs', has_headers : 'obs.h')
endif

obs_frontend_dep = dependency('obs-frontend-api', required : false)
if not obs_frontend_dep.found()
  obs_frontend_dep = meson.get_compiler('cpp').find_library('obs-frontend-api', has_headers : 'obs/obs-frontend-api.h', required : false)
endif
if not obs_frontend_dep.found()
  obs_frontend_dep = meson.get_compiler('cpp').find_library('obs-frontend-api', has_headers : 'obs-frontend-api.h', required : false)
endif

name_suffix = []
if host_machine.system() == 'darwin'
  name_suffix = 'so'
endif

qt6 = import('qt6', required : false)
qt_dep = dependency('qt6', modules : ['Core', 'Gui', 'Widgets'], required : false)
if not qt_dep.found()
  qt5 = import('qt5', required : false)
  qt_dep = dependency('qt5', modules : ['Core', 'Gui', 'Widgets'], required : false)
endif

webrtc_opt = get_option('webrtc')
webrtc_deps = []
enable_webrtc = false

if not webrtc_opt.disabled()
  gst_webrtc_dep = dependency('gstreamer-webrtc-1.0', required : webrtc_opt)
  gst_sdp_dep = dependency('gstreamer-sdp-1.0', required : webrtc_opt)
  libsoup_dep = dependency('libsoup-3.0', required : webrtc_opt)

  if gst_webrtc_dep.found() and gst_sdp_dep.found() and libsoup_dep.found()
    enable_webrtc = true
    webrtc_deps = [gst_webrtc_dep, gst_sdp_dep, libsoup_dep]
  endif
endif

extra_c_args = []
if enable_webrtc
  extra_c_args += ['-DENABLE_WEBRTC=1']
else
  extra_c_args += ['-DENABLE_WEBRTC=0']
endif

sources = [
  'gstreamer.c',
  'gstreamer-source.c',
  'gstreamer-encoder.c',
  'gstreamer-encoder-tex.c',
  'gstreamer-render-hub.c',
  'gstreamer-filter.c',
  'gstreamer-output.c',
  'gstreamer-webrtc.c',
  vcs_tag(
    command : ['git', 'rev-parse', '--short', 'HEAD'],
    input : 'version.c.in',
    output : 'version.c',
    fallback : 'custom',
  ),
]

deps = [
  obs_dep,
  dependency('gstreamer-1.0', version : '>=1.16.0'),
  dependency('gstreamer-video-1.0'),
  dependency('gstreamer-audio-1.0'),
  dependency('gstreamer-app-1.0'),
  dependency('gstreamer-net-1.0'),
  dependency('gstreamer-rtsp-server-1.0'),
]

if qt_dep.found() and obs_frontend_dep.found()
  sources += ['gstreamer-dock.cpp']
  deps += [qt_dep, obs_frontend_dep]
  extra_c_args += ['-DENABLE_DOCK=1']
else
  extra_c_args += ['-DENABLE_DOCK=0']
endif

deps += webrtc_deps

cc = meson.get_compiler('c')
obs_inc = []
if not cc.has_header('obs-module.h', dependencies : obs_dep)
  foreach dir : ['/usr/include/obs', '/usr/local/include/obs']
    if cc.has_header('obs-module.h', args : ['-I' + dir])
      obs_inc += include_directories(dir)
      extra_c_args += ['-I' + dir]
      break
    endif
  endforeach
endif

shlib_kwargs = {}
if host_machine.system() == 'darwin'
  shlib_kwargs += {'name_suffix': 'so'}
endif

shared_library('obs-gstreamer',
  sources,
  name_prefix : '',
  c_args : extra_c_args,
  cpp_args : extra_c_args,
  include_directories : obs_inc,
  dependencies : deps,
  install : true,
  kwargs : shlib_kwargs,
)
