#!/bin/bash -e
#
# Builds the release AppImage inside an Ubuntu 22.04 (jammy) container, so it
# runs on 22.04 and anything newer. An AppImage uses the C library of the
# system it runs on, so it needs a glibc no newer than the one it was built
# against: built on 24.04 it asks for glibc 2.38 or later, and the AppImage
# catalog (which tests on the oldest supported Ubuntu LTS) rejects it.
#
# Needs Docker. Uses the repository this script is in, builds in
# app/build-appimage-jammy, and leaves Glissando-<version>-x86_64.AppImage in
# app/appimage. wxWidgets 3.2 is built from source, because 22.04 ships 3.0.
# HTTPS_PROXY, HTTP_PROXY and NO_PROXY pass through, and SSL_CERT_FILE, if
# set, is mounted as the container's extra CA bundle.

cd "$(dirname "$(realpath "$0")")"
REPO="$(git rev-parse --show-toplevel)"
MACH_ARCH=`uname -m`

# Fetch the tools here, where any proxy is already set up.
if [ ! -f linuxdeploy-${MACH_ARCH}.AppImage ]; then
    curl -sSLf -o linuxdeploy-${MACH_ARCH}.AppImage \
        https://github.com/linuxdeploy/linuxdeploy/releases/latest/download/linuxdeploy-${MACH_ARCH}.AppImage
    chmod +x linuxdeploy-${MACH_ARCH}.AppImage
fi
if [ ! -f linuxdeploy-plugin-gtk.sh ]; then
    curl -sSLf -o linuxdeploy-plugin-gtk.sh \
        https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh
    chmod +x linuxdeploy-plugin-gtk.sh
fi
if [ ! -f runtime-${MACH_ARCH} ]; then
    curl -sSLf -o runtime-${MACH_ARCH} \
        https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-${MACH_ARCH}
fi

CA_ARGS=()
if [ -n "$SSL_CERT_FILE" ] && [ -f "$SSL_CERT_FILE" ]; then
    CA_ARGS=(-v "$SSL_CERT_FILE:/extra-ca.crt:ro" -e SSL_CERT_FILE=/extra-ca.crt
             -e CURL_CA_BUNDLE=/extra-ca.crt -e GIT_SSL_CAINFO=/extra-ca.crt)
fi

docker run --rm --network host \
    -e HTTPS_PROXY -e HTTP_PROXY -e NO_PROXY -e https_proxy -e http_proxy -e no_proxy \
    "${CA_ARGS[@]}" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -v "$REPO:/src" -w /src/app/appimage \
    ubuntu:22.04 bash -ec '
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q build-essential cmake git autoconf automake libtool pkg-config file \
    ca-certificates dpkg-dev librsvg2-common libgtk-3-dev libpulse-dev libspeexdsp-dev \
    libsndfile1-dev libhamlib-dev libasound2-dev libao-dev libgsm1-dev libssl-dev >/dev/null
git config --global --add safe.directory "*"
B=/src/app/build-appimage-jammy
cmake -S /src/app -B $B -DCMAKE_BUILD_TYPE=Release -DUSE_NATIVE_AUDIO=1 \
    -DBOOTSTRAP_WXWIDGETS=TRUE -DwxUSE_WEBVIEW=OFF
cmake --build $B -j$(nproc)
strip --strip-unneeded $B/src/glissando
APPIMAGE_EXTRACT_AND_RUN=1 LDAI_RUNTIME_FILE=/src/app/appimage/runtime-'"$MACH_ARCH"' \
    BUILD_DIR=$B ./make-appimage.sh
chown -R $HOST_UID:$HOST_GID $B /src/app/appimage
'
