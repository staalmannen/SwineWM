#!/bin/sh
# Regenerates swinewm.ico (needs ImageMagick): the wallpaper's pig on a
# rounded dark square, at the sizes Windows uses.
set -e
cd "$(dirname "$0")"
BG='#20242c'
LINE='#e08aa0'
FACE='#2b303b'
convert -size 512x512 xc:none \
	-fill "$BG" -draw "roundrectangle 0,0 511,511 90,90" \
	-stroke "$LINE" -strokewidth 26 -fill "$FACE" \
	-draw "polygon 120,200 150,40 250,150" \
	-draw "polygon 392,200 362,40 262,150" \
	-draw "circle 256,280 256,90" \
	-draw "ellipse 256,330 105,72 0,360" \
	-stroke none -fill "$LINE" \
	-draw "ellipse 220,330 20,32 0,360" \
	-draw "ellipse 292,330 20,32 0,360" \
	-draw "circle 180,230 180,252" \
	-draw "circle 332,230 332,252" \
	-define icon:auto-resize=256,64,48,32,24,16 swinewm.ico
