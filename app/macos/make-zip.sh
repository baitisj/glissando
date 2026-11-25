#!/bin/bash
set -euo pipefail
#
# Builds the macOS zip, Glissando-<version>-macos-<arch>.zip, on the Mac it
# runs on: arm64 on Apple Silicon, x86_64 on Intel. Needs the Xcode command
# line tools, CMake and, from Homebrew,
#
#     brew install autoconf automake libtool
#
# It builds in app/build-macos (BUILD_DIR to use another folder), fetching
# and compiling wxWidgets, Hamlib, libsndfile and libsamplerate statically
# the first time, runs the app's tests, and leaves the zip in app/macos. The
# zip holds Glissando.app, which needs nothing outside macOS, with the
# licences of the libraries built into it, and README.txt.
#
# The app is signed ad hoc unless MACOS_SIGN_IDENTITY names a Developer ID
# Application identity; MACOS_NOTARY_PROFILE (a notarytool keychain
# profile) then has Apple notarize it too.
#
# Adapted from Tim Vetter's macOS recipe for Ragtime
# (github.com/baitisj/ragtime, packaging/macos/make-zip.sh).

cd "$(dirname "$0")"
MACOS="$(pwd)"
APP_SRC="$(cd .. && pwd)"
ARCH="$(uname -m)"
if [ "$(uname -s)" != Darwin ] || { [ "$ARCH" != arm64 ] && [ "$ARCH" != x86_64 ]; }; then
    echo "Build this on a Mac (Apple Silicon or Intel), in a native shell." >&2
    exit 1
fi
for tool in cmake ctest git python3 autoconf automake glibtoolize; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
BUILD_DIR="${BUILD_DIR:-$APP_SRC/build-macos}"
mkdir -p "$BUILD_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu)}"
# The oldest macOS the bundled libraries are built for (cmake/BuildLame.cmake
# and the others say 11.0 too).
export MACOSX_DEPLOYMENT_TARGET=11.0
IDENTITY="${MACOS_SIGN_IDENTITY:--}"
if [ -n "${MACOS_NOTARY_PROFILE:-}" ] && [ "$IDENTITY" = - ]; then
    echo "Notarizing needs MACOS_SIGN_IDENTITY (a Developer ID Application identity)." >&2
    exit 1
fi

# Homebrew's libraries are left out, so everything but macOS's own is built
# here and linked in statically.
cmake -S "$APP_SRC" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
    -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
    -DUSE_NATIVE_AUDIO=1 -DBOOTSTRAP_WXWIDGETS=TRUE -DwxUSE_WEBVIEW=OFF \
    -DUSE_STATIC_SNDFILE=TRUE -DUSE_STATIC_SAMPLERATE=TRUE -DUNITTEST=ON
cmake --build "$BUILD_DIR" -j"$JOBS"
ctest --test-dir "$BUILD_DIR" -R "glissando|text_messaging" --output-on-failure

VERSION="$(cat "$BUILD_DIR/glissando-version.txt")"
HASH="$(git -C "$APP_SRC" rev-parse --short HEAD)"
NAME="Glissando-$VERSION-macos-$ARCH"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
OUT="$STAGE/$NAME"
mkdir "$OUT"
APP="$OUT/Glissando.app"
ditto "$BUILD_DIR/src/Glissando.app" "$APP"

# The licences of what is built in: the first licence file found in each
# library's source tree.
LICENCES="$APP/Contents/Resources/licences"
mkdir -p "$LICENCES"
cp "$APP_SRC/COPYING" "$APP/Contents/Resources/COPYING.txt"
licence() {
    local name="$1" dir="$2" file
    for file in COPYING.LIB COPYING LICENSE LICENSE.txt docs/licence.txt; do
        if [ -f "$dir/$file" ]; then
            cp "$dir/$file" "$LICENCES/$name.txt"
            return
        fi
    done
    echo "No licence file for $name in $dir" >&2
    exit 1
}
licence wxWidgets "$BUILD_DIR/_deps/wxwidgets-src"
licence Hamlib "$BUILD_DIR/build_hamlib-prefix/src/build_hamlib"
licence libsndfile "$BUILD_DIR/sndfile-prefix/src/sndfile"
licence mpg123 "$BUILD_DIR/build_mpg123-prefix/src/build_mpg123"
licence LAME "$BUILD_DIR/build_mp3lame-prefix/src/build_mp3lame"
licence libsamplerate "$BUILD_DIR/_deps/samplerate-src"
licence codec2 "$APP_SRC/codec2-1.2.0"

# Sign what is nested first; --deep is for checking, not signing.
sign_args=(--force --sign "$IDENTITY")
if [ "$IDENTITY" != - ]; then sign_args+=(--options runtime --timestamp); fi
find "$APP/Contents" -name '*.dylib' -print0 | while IFS= read -r -d '' library; do
    codesign "${sign_args[@]}" "$library"
done
codesign "${sign_args[@]}" --entitlements "$MACOS/entitlements.plist" "$APP"
python3 "$MACOS/check-bundle.py" "$APP"

if [ -n "${MACOS_NOTARY_PROFILE:-}" ]; then
    ditto -c -k --keepParent "$APP" "$STAGE/notarize.zip"
    xcrun notarytool submit "$STAGE/notarize.zip" \
        --keychain-profile "$MACOS_NOTARY_PROFILE" --wait
    xcrun stapler staple "$APP"
    xcrun stapler validate "$APP"
    rm "$STAGE/notarize.zip"
fi

sed -e "s/@VERSION@/$VERSION/" -e "s/@HASH@/$HASH/" -e "s/@ARCH@/$ARCH/" \
    "$MACOS/README.txt" > "$OUT/README.txt"
cp "$APP_SRC/COPYING" "$OUT/COPYING.txt"

rm -f "$NAME.zip"
ditto -c -k --keepParent "$OUT" "$STAGE/$NAME.zip"
mv "$STAGE/$NAME.zip" "$MACOS/"
if [ "$IDENTITY" = - ]; then
    echo "Signed ad hoc. Use a Developer ID and notarization to pass Gatekeeper without a warning."
fi
echo "Done: app/macos/$NAME.zip"
