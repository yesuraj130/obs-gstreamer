#!/usr/bin/env bash
set -Eeuo pipefail

export DEBIAN_FRONTEND=noninteractive
export DEBIAN_PRIORITY=critical
export LANGUAGE=C.UTF-8
export LC_ALL=C.UTF-8

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    gcc \
    gdb \
    valgrind \
    git \
    pkg-config \
    meson \
    ninja-build \
    libglib2.0-dev \
    libobs-dev \
    obs-studio
