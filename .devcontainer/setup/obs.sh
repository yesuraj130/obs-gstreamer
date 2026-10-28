#!/usr/bin/env bash
set -Eeuo pipefail

export DEBIAN_FRONTEND=noninteractive
export DEBIAN_PRIORITY=critical
export LANGUAGE=C.UTF-8
export LC_ALL=C.UTF-8

# Add official OBS Studio PPA to install latest OBS 32.X (upgraded from 30.X)
if ! grep -rq "obsproject/obs-studio" /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
    sudo DEBIAN_FRONTEND=noninteractive apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends software-properties-common
    sudo add-apt-repository -y ppa:obsproject/obs-studio
    sudo DEBIAN_FRONTEND=noninteractive apt-get update
fi

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    gcc \
    g++ \
    gdb \
    valgrind \
    git \
    pkg-config \
    meson \
    ninja-build \
    libglib2.0-dev \
    libobs-dev \
    obs-studio \
    qt6-base-dev
