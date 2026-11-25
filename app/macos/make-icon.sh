#!/bin/sh
# Build the Finder/Dock icon from the existing 256 px application artwork.
set -eu
source_png=$1
output_icns=$2
mkdir -p "$(dirname "$output_icns")"
icon_tmp=$(mktemp -d)
trap 'rm -rf "$icon_tmp"' EXIT HUP INT TERM
iconset="$icon_tmp/glissando.iconset"
mkdir "$iconset"
for size in 16 32 128 256; do
    /usr/bin/sips -z "$size" "$size" "$source_png" --out "$iconset/icon_${size}x${size}.png" >/dev/null
done
for size in 16 32 128; do
    retina=$((size * 2))
    /usr/bin/sips -z "$retina" "$retina" "$source_png" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
/usr/bin/iconutil -c icns "$iconset" -o "$output_icns"
