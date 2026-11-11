#!/bin/sh
# pi/ui/fonts/gen.sh SRC_DIR - the SIDECAR theme's text faces (sidecar_theme.c): Space Grotesk Medium (SIL OFL 1.1,
# github.com/floriankarsten/space-grotesk; the Latin subset as @fontsource/space-grotesk ships it,
# space-grotesk-latin-500-normal.woff) and the eleven Font Awesome 5 symbols the pages use (LVGL's
# scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff), as LVGL tables: 4 bpp, not compressed (no decoding
# cost), ASCII 0x20-0x7E and the degree and plus-minus signs. Needs lv_font_conv 1.5.3 on the PATH
# (npm i lv_font_conv@1.5.3). SRC_DIR holds the two .woff files under the names below; the tables are written next to
# this script.
set -e
SRC=$1
OUT=$(cd "$(dirname "$0")" && pwd)
[ -f "$SRC/SpaceGrotesk-Medium.woff" ] && [ -f "$SRC/FontAwesome5.woff" ] || { echo "usage: gen.sh DIR (SpaceGrotesk-Medium.woff, FontAwesome5.woff)"; exit 1; }
# LV_SYMBOL_ CLOSE OK PLAY PAUSE STOP LEFT RIGHT WARNING UP DOWN TRASH
SYM=0xF00D,0xF00C,0xF04B,0xF04C,0xF04D,0xF053,0xF054,0xF071,0xF077,0xF078,0xF2ED
cd "$SRC"
for s in 12 14 16 18 20 22 24 28; do
	lv_font_conv --bpp 4 --size $s --no-compress --format lvgl --lv-font-name sidecar_sg$s \
		--font SpaceGrotesk-Medium.woff -r 0x20-0x7E,0xB0-0xB1 --font FontAwesome5.woff -r $SYM -o sg$s.tmp
	# LVGL's own header (lvgl.h on the include path), no build paths in the file
	sed -e 's|^#ifdef LV_LVGL_H_INCLUDE_SIMPLE$|#if 1|' -e 's|^ \* Opts: .*| * gen.sh: Space Grotesk Medium + Font Awesome 5 symbols, 4 bpp, ASCII, degree, plus-minus|' sg$s.tmp > "$OUT/sidecar_sg$s.c"
	rm sg$s.tmp
done
