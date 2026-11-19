#!/usr/bin/env bash
# Regenerate fonts/muse_font_clock_48.c -- the ring page's big time face.
#
# Why a cut-down face instead of lv_font_montserrat_48: the full 48 px
# Montserrat is ~128 KB of flash and the 1.85B's app slot has ~60 KB free
# (enabling it made the link fail with "all app partitions are too small").
# The ring page only draws "07:00", so digits and the colon are all it needs:
# ten glyphs, ~16 KB at 4bpp.
#
# Source: the Montserrat-Medium.ttf LVGL itself uses for the built-in faces
# (lvgl/scripts/built_in_font/Montserrat-Medium.ttf). Copy that file next to
# this script (or point FONT at it) and run from esp32/:
#
#   bash tools/muse/gen_clock_font.sh
#
# The result is written to components/muse/fonts/muse_font_clock_48.c and is
# referenced by components/muse/muse_alarm_ui.c as muse_font_clock_48.
set -euo pipefail

FONT="${FONT:-Montserrat-Medium.ttf}"
OUT="components/muse/fonts/muse_font_clock_48.c"

if [ ! -f "$FONT" ]; then
    echo "Montserrat-Medium.ttf not found: set FONT=/path/to/Montserrat-Medium.ttf" >&2
    exit 1
fi

npx --yes lv_font_conv@1.5.2 \
    --font "$FONT" \
    --range 0x30-0x3a \
    --size 48 \
    --bpp 4 \
    --format lvgl \
    --lv-include lvgl.h \
    --no-compress \
    --no-prefilter \
    -o "$OUT"

echo "wrote $OUT"
