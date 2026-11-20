#!/bin/bash -e
#
# Builds the portable 64-bit Windows zip, Glissando-<version>-windows-x86_64.zip,
# by cross-compiling on Ubuntu 24.04 with MinGW-w64. Needs
#
#     sudo apt-get install g++-mingw-w64-x86-64-posix libltdl-dev autoconf automake libtool zip
#
# It builds in app/build-win (BUILD_DIR to use another folder) and leaves the
# zip in app/windows. The build fetches and compiles wxWidgets, Hamlib and
# libsndfile the first time. The zip holds glissando.exe, the DLLs it needs
# that Windows doesn't have (Hamlib's and the MinGW-w64 runtime), all
# stripped, and README.txt. There is no installer.

cd "$(dirname "$(realpath "$0")")"
APP="$(realpath ..)"
BUILD_DIR="${BUILD_DIR:-$APP/build-win}"
PREFIX=x86_64-w64-mingw32

cmake -S "$APP" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$APP/cmake/Toolchain-Ubuntu-mingw64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DUSE_NATIVE_AUDIO=1 \
    -DBOOTSTRAP_WXWIDGETS=TRUE -DwxUSE_WEBVIEW=OFF
cmake --build "$BUILD_DIR" -j"$(nproc)"

VERSION="$(cat "$BUILD_DIR/glissando-version.txt")"
HASH="$(git -C "$APP" rev-parse --short HEAD)"
NAME="Glissando-$VERSION-windows-x86_64"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/$NAME"
OUT="$STAGE/$NAME"

cp "$BUILD_DIR/src/glissando.exe" "$OUT/"

# Where the DLLs it names can come from. A DLL found in none of these is
# one of Windows' own.
SEARCH=("$BUILD_DIR/external/dist/bin")
for dll in libstdc++-6.dll libgcc_s_seh-1.dll libssp-0.dll libwinpthread-1.dll; do
    found="$($PREFIX-g++-posix -print-file-name="$dll")"
    [ -f "$found" ] && SEARCH+=("$(dirname "$found")")
done
SEARCH+=("/usr/$PREFIX/lib" "/usr/$PREFIX/bin")

# Copy every DLL the exe needs, and every DLL those need, until none is new.
todo=("$OUT/glissando.exe")
while [ ${#todo[@]} -gt 0 ]; do
    file="${todo[0]}"
    todo=("${todo[@]:1}")
    for dll in $($PREFIX-objdump -p "$file" | sed -n 's/^\s*DLL Name: //p'); do
        [ -f "$OUT/$dll" ] && continue
        for dir in "${SEARCH[@]}"; do
            if [ -f "$dir/$dll" ]; then
                cp "$dir/$dll" "$OUT/"
                todo+=("$OUT/$dll")
                break
            fi
        done
    done
done

# Unstripped, the zip is about 120 MB instead of 10.
$PREFIX-strip "$OUT"/*.exe "$OUT"/*.dll

cat > "$OUT/README.txt" <<EOF
Glissando $VERSION ($HASH) for 64-bit Windows, portable build.

Unzip anywhere and run glissando.exe. Nothing is installed.

Built by cross-compiling on Linux with MinGW-w64.

Glissando is LGPL 2.1 (the app, from freedv-gui) and MIT (the modem).
Source: https://github.com/baitisj/glissando
EOF
cp "$APP/COPYING" "$OUT/COPYING.txt"

rm -f "$NAME.zip"
(cd "$STAGE" && zip -qr9 "$OLDPWD/$NAME.zip" "$NAME")
ls "$OUT"
echo "Done: app/windows/$NAME.zip"
