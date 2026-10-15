#!/bin/sh
# Regenerates the default SwineWM wallpaper (needs ImageMagick and the
# DejaVu fonts). Flat colours keep the 4K PNG small. Released with the rest
# of SwineWM under the MIT licence.
set -e
cd "$(dirname "$0")"
BG='#20242c'
LINE='#e08aa0'
FACE='#2b303b'
TEXT='#8b95a5'
convert -size 3840x2160 "xc:$BG" \
	-stroke "$LINE" -strokewidth 22 -fill "$FACE" \
	-draw "polygon 1530,640 1600,330 1790,560" \
	-draw "polygon 2310,640 2240,330 2050,560" \
	-draw "circle 1920,980 1920,500" \
	-fill "$FACE" -draw "ellipse 1920,1120 230,150 0,360" \
	-stroke none -fill "$LINE" \
	-draw "ellipse 1845,1120 38,62 0,360" \
	-draw "ellipse 1995,1120 38,62 0,360" \
	-draw "circle 1745,880 1745,920" \
	-draw "circle 2095,880 2095,920" \
	-font DejaVu-Sans-Bold -pointsize 150 -kerning 18 -fill "$TEXT" \
	-gravity north -annotate +0+1700 'SwineWM' \
	-strip -dither None -colors 16 PNG8:swinewm-default.png
