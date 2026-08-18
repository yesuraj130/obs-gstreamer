#!/usr/bin/env bash
set -Eeuo pipefail

export DEBIAN_FRONTEND=noninteractive
export DEBIAN_PRIORITY=critical
export LANGUAGE=C.UTF-8
export LC_ALL=C.UTF-8

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    xfce4 \
    xfce4-terminal \
    dbus-x11 \
    dbus-user-session \
    tigervnc-standalone-server \
    tigervnc-tools
