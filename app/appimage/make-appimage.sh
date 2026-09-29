#!/bin/bash -e
#
# Builds Glissando-<version>-<arch>.AppImage in this directory.
#
# By default it configures and builds a Release tree in ../build-appimage.
# Set BUILD_DIR to package an existing build instead (it must contain
# src/glissando and freedv-version.txt).

export APPNAME="Glissando"
MACH_ARCH=`uname -m`

export NO_STRIP=1

# Change to the directory where this script is located
cd "$(dirname "$(realpath "$0")")"

DESKTOP_FILE="$APPNAME.desktop"
APPDIR="$APPNAME.AppDir"

if [ -z "$BUILD_DIR" ]; then
    BUILD_DIR="$PWD/../build-appimage"
    cmake -S .. -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DUSE_NATIVE_AUDIO=1
    cmake --build "$BUILD_DIR" -j`nproc`
fi
APPEXEC="$BUILD_DIR/src/glissando"

if [ -d "$APPDIR" ]; then
    echo "Deleting $APPDIR..."
    rm -rf "$APPDIR"
fi

echo "Bundle dependencies..."
if test -f linuxdeploy-${MACH_ARCH}.AppImage; then
  echo "linuxdeploy exists"
else
    wget -c "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
    wget https://github.com/linuxdeploy/linuxdeploy/releases/latest/download/linuxdeploy-${MACH_ARCH}.AppImage
    chmod +x linuxdeploy-${MACH_ARCH}.AppImage linuxdeploy-plugin-gtk.sh
fi

# The icon's file name must match Icon= in the desktop file.
ICON_DIR=`mktemp -d`
trap 'rm -rf "$ICON_DIR"' EXIT
cp ../contrib/glissando256x256.png "$ICON_DIR/glissando.png"

./linuxdeploy-${MACH_ARCH}.AppImage \
  --executable "$APPEXEC" \
  --appdir "$APPDIR" \
  --icon-file "$ICON_DIR/glissando.png" \
  --custom-apprun "AppRun.sh" \
  --desktop-file $DESKTOP_FILE

# Manually copy over /etc/ssl to APPDIR. Needed for OpenSSL to behave properly on non-Ubuntu
# distros.
mkdir -p "$APPDIR/etc/ssl/certs"
cp -aL /etc/ssl/certs/* "$APPDIR/etc/ssl/certs"

# Create the output
./linuxdeploy-${MACH_ARCH}.AppImage \
  --appdir "$APPDIR" \
  --plugin gtk \
  --output appimage

# Include version number in AppImage filename
VERSION=`cat "$BUILD_DIR/freedv-version.txt"`
mv ${APPNAME}-${MACH_ARCH}.AppImage ${APPNAME}-$VERSION-${MACH_ARCH}.AppImage

echo "Done: ${APPNAME}-$VERSION-${MACH_ARCH}.AppImage"
