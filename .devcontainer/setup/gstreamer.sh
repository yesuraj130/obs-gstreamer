#!/usr/bin/env bash
set -Eeuo pipefail

export DEBIAN_FRONTEND=noninteractive
export DEBIAN_PRIORITY=critical
export LANGUAGE=C.UTF-8
export LC_ALL=C.UTF-8

GST_TARGET_VERSION="1.28.9"
LIBNICE_VERSION="0.1.24"

if ! command -v sudo >/dev/null 2>&1; then
    sudo() { "$@"; }
fi

echo "=== GStreamer $GST_TARGET_VERSION & Dependencies Installation ==="

# 1. Install prerequisites and build tools
sudo apt-get update -y
sudo apt-get install -y --no-install-recommends \
    build-essential \
    pkg-config \
    ninja-build \
    python3-pip \
    flex \
    bison \
    libssl-dev \
    libsrtp2-dev \
    libglib2.0-dev \
    libsoup-3.0-dev \
    libmount-dev \
    libseccomp-dev \
    ca-certificates \
    curl \
    xz-utils

# Ensure modern Meson (>=1.4 needed for GStreamer 1.28)
pip3 install --upgrade --break-system-packages meson

# Set up local pkg-config and library search paths
mkdir -p /usr/local/lib/pkgconfig /usr/local/lib/x86_64-linux-gnu/pkgconfig
echo "/usr/local/lib/x86_64-linux-gnu" | sudo tee /etc/ld.so.conf.d/gstreamer-local.conf >/dev/null
echo "/usr/local/lib" | sudo tee -a /etc/ld.so.conf.d/gstreamer-local.conf >/dev/null
sudo ldconfig

export PKG_CONFIG_PATH="/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/pkgconfig}"
export LD_LIBRARY_PATH="/usr/local/lib/x86_64-linux-gnu:/usr/local/lib:${LD_LIBRARY_PATH:-}"
export GST_PLUGIN_SYSTEM_PATH_1_0="/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0:/usr/lib/x86_64-linux-gnu/gstreamer-1.0"
export GST_PLUGIN_PATH="/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0:/usr/lib/x86_64-linux-gnu/gstreamer-1.0"

# Persist environment variables for interactive login shells
sudo tee /etc/profile.d/gstreamer.sh >/dev/null << 'EOF'
export PKG_CONFIG_PATH="/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/pkgconfig}"
export LD_LIBRARY_PATH="/usr/local/lib/x86_64-linux-gnu:/usr/local/lib:${LD_LIBRARY_PATH:-}"
export GST_PLUGIN_SYSTEM_PATH_1_0="/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0:/usr/lib/x86_64-linux-gnu/gstreamer-1.0"
export GST_PLUGIN_PATH="/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0:/usr/lib/x86_64-linux-gnu/gstreamer-1.0"
EOF
sudo chmod +x /etc/profile.d/gstreamer.sh

BUILD_DIR="/tmp/gst-build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# 2. Build core gstreamer-1.28
if ! pkg-config --exact-version="$GST_TARGET_VERSION" gstreamer-1.0 2>/dev/null; then
    echo "--- Building core GStreamer $GST_TARGET_VERSION ---"
    curl -L -s --retry 3 -o "gstreamer-$GST_TARGET_VERSION.tar.xz" "https://gstreamer.freedesktop.org/src/gstreamer/gstreamer-$GST_TARGET_VERSION.tar.xz"
    tar -xf "gstreamer-$GST_TARGET_VERSION.tar.xz"
    cd "gstreamer-$GST_TARGET_VERSION"
    rm -rf bld
    meson setup bld --prefix=/usr/local -Dauto_features=disabled -Dtools=enabled -Dtests=disabled -Dexamples=disabled -Ddoc=disabled -Dintrospection=disabled
    ninja -C bld -j$(nproc)
    sudo ninja -C bld install
    sudo ldconfig
    cd "$BUILD_DIR"
fi

# 3. Build gst-plugins-base-1.28 (video, audio, app, gl, x11)
if ! pkg-config --exact-version="$GST_TARGET_VERSION" gstreamer-gl-x11-1.0 2>/dev/null; then
    echo "--- Building gst-plugins-base $GST_TARGET_VERSION ---"
    curl -L -s --retry 3 -o "gst-plugins-base-$GST_TARGET_VERSION.tar.xz" "https://gstreamer.freedesktop.org/src/gst-plugins-base/gst-plugins-base-$GST_TARGET_VERSION.tar.xz"
    tar -xf "gst-plugins-base-$GST_TARGET_VERSION.tar.xz"
    cd "gst-plugins-base-$GST_TARGET_VERSION"
    rm -rf bld
    meson setup bld --prefix=/usr/local -Dauto_features=disabled -Dapp=enabled -Dgl=enabled -Dx11=enabled -Dgl_winsys=x11,egl -Dvideoconvertscale=enabled -Dplayback=enabled -Daudioconvert=enabled -Daudioresample=enabled -Dtypefind=enabled -Dcompositor=enabled -Dtests=disabled -Dexamples=disabled -Ddoc=disabled -Dintrospection=disabled
    ninja -C bld -j$(nproc)
    sudo ninja -C bld install
    sudo ldconfig
    cd "$BUILD_DIR"
fi

# 4. Build libnice (>= 0.1.23 required for webrtc)
if ! pkg-config --atleast-version="0.1.23" nice 2>/dev/null; then
    echo "--- Building libnice $LIBNICE_VERSION ---"
    curl -L -s --retry 3 -o "libnice-$LIBNICE_VERSION.tar.gz" "https://libnice.freedesktop.org/releases/libnice-$LIBNICE_VERSION.tar.gz"
    tar -xf "libnice-$LIBNICE_VERSION.tar.gz"
    cd "libnice-$LIBNICE_VERSION"
    rm -rf bld
    meson setup bld --prefix=/usr/local -Dauto_features=disabled -Dtests=disabled -Dexamples=disabled -Dgtk_doc=disabled -Dintrospection=disabled
    ninja -C bld -j$(nproc)
    sudo ninja -C bld install
    sudo ldconfig
    cd "$BUILD_DIR"
fi

# 5. Build gst-plugins-bad-1.28 (webrtc, dtls, sdp, sctp, srtp)
if ! pkg-config --exact-version="$GST_TARGET_VERSION" gstreamer-webrtc-1.0 2>/dev/null; then
    echo "--- Building gst-plugins-bad $GST_TARGET_VERSION (webrtc, dtls, sdp) ---"
    curl -L -s --retry 3 -o "gst-plugins-bad-$GST_TARGET_VERSION.tar.xz" "https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-$GST_TARGET_VERSION.tar.xz"
    tar -xf "gst-plugins-bad-$GST_TARGET_VERSION.tar.xz"
    cd "gst-plugins-bad-$GST_TARGET_VERSION"
    rm -rf bld
    meson setup bld --prefix=/usr/local -Dauto_features=disabled -Dwebrtc=enabled -Ddtls=enabled -Dsctp=enabled -Dsrtp=enabled -Dtests=disabled -Dexamples=disabled -Ddoc=disabled -Dintrospection=disabled
    ninja -C bld -j$(nproc)
    sudo ninja -C bld install
    sudo ldconfig
    cd "$BUILD_DIR"
fi

# 6. Build gst-rtsp-server-1.28
if ! pkg-config --exact-version="$GST_TARGET_VERSION" gstreamer-rtsp-server-1.0 2>/dev/null; then
    echo "--- Building gst-rtsp-server $GST_TARGET_VERSION ---"
    curl -L -s --retry 3 -o "gst-rtsp-server-$GST_TARGET_VERSION.tar.xz" "https://gstreamer.freedesktop.org/src/gst-rtsp-server/gst-rtsp-server-$GST_TARGET_VERSION.tar.xz"
    tar -xf "gst-rtsp-server-$GST_TARGET_VERSION.tar.xz"
    cd "gst-rtsp-server-$GST_TARGET_VERSION"
    rm -rf bld
    meson setup bld --prefix=/usr/local -Dauto_features=disabled -Dtests=disabled -Dexamples=disabled -Ddoc=disabled -Dintrospection=disabled
    ninja -C bld -j$(nproc)
    sudo ninja -C bld install
    sudo ldconfig
    cd "$BUILD_DIR"
fi

# Clean up build artifacts in /tmp
rm -rf "$BUILD_DIR"

echo "=== GStreamer $GST_TARGET_VERSION installation finished successfully ==="
