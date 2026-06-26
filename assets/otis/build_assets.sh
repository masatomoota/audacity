#!/usr/bin/env bash
# Rasterise the Otis SVG sources into the web icon set (PNG + ICO).
# Requires: rsvg-convert, magick (ImageMagick).  Run from assets/otis/.
set -euo pipefail
cd "$(dirname "$0")"

python3 generate_assets.py

# Raster icons are cut from the opaque dark tile so they stay visible on any
# browser-tab / home-screen background.
TILE=otis-icon-tile.svg

for s in 16 32 48 64; do
  rsvg-convert -w $s -h $s "$TILE" -o "favicon-${s}.png"
done
rsvg-convert -w 180 -h 180 "$TILE" -o apple-touch-icon.png
rsvg-convert -w 192 -h 192 "$TILE" -o icon-192.png
rsvg-convert -w 512 -h 512 "$TILE" -o icon-512.png
rsvg-convert -w 1024 -h 1024 otis-icon-rounded.svg -o icon-1024.png

# Multi-resolution .ico fallback for legacy browsers.
magick favicon-16.png favicon-32.png favicon-48.png favicon-64.png favicon.ico

echo "---- built ----"
ls -1
