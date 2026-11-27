#!/usr/bin/env bash
# tools/gen_icon.sh
# Renders the logo docs/images/logo.svg into data/icon/puppetxr_<size>.rgba: 8-bit RGBA, straight
# alpha, rows top down, no header. The build embeds them as the mirror window's icon
# (cmake/embed_icon.cmake, docs/architecture.md). Run after changing the logo and commit the result.
# Needs rsvg-convert (librsvg) and ImageMagick 7.

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
svg=$root/docs/images/logo.svg
out=$root/data/icon

rm -f "$out"/puppetxr_*.rgba
for size in 16 24 32 48 64 128 256
do
    rsvg-convert --width "$size" --height "$size" "$svg" | magick png:- -depth 8 "RGBA:$out/puppetxr_$size.rgba"
done
