#!/usr/bin/env bash
# Regenerate fonts/muse_font_ui_16.c -- the antialiased 16 px CJK face.
#
# Since 1.5b this is the ONE Chinese face for the whole UI (captions,
# replies and the interaction pages): WenQuanYi Micro Hei at 16 px, 4bpp,
# covering GB2312 level-1 hanzi (3755 characters) plus punctuation, kana
# and fullwidth forms (4363 code points in total, ~480 KB of bitmap data).
# It replaced both the old 255-character source-literal subset (which could
# never cover arbitrary replies) and the 20 px Unifont face muse_font_cjk_20.
#
# The character set is fixed, not scanned from the sources: GB2312 level-1
# is zones 16..55, i.e. high byte 0xB0..0xD7 with low byte 0xA1..0xFE,
# decoded to Unicode with Python's gb2312 codec. The full GB2312 range,
# high byte 0xB0..0xF7, would add level-2 (6763 hanzi, ~740-810 KB of
# bitmaps): change GB2312_HI_END below if that trade ever wins, and
# re-check the 4 MB app slot first. A character outside the set still
# renders -- it falls back to muse_font_cjk_16 -- so this can never produce
# a tofu box, only a slightly less smooth glyph.
#
# Needs: a WenQuanYi Micro Hei TTC, Python with fontTools, and npx for
# lv_font_conv@1.5.3. This script is NOT run by the build; run it by hand
# after changing the set, then commit the regenerated .c file.
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_ui_16.c"
TTC="${WQY_TTC:-/usr/share/fonts/truetype/wqy/wqy-microhei.ttc}"
PY="${PYTHON:-$HOME/.venv/bin/python3}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Level-1 is GB2312 qu (zone) 16..55 -> high byte 0xB0..0xD7, low 0xA1..0xFE.
# (The full GB2312 range, high byte 0xB0..0xF7, would add level-2; see the
# header comment before changing GB2312_HI_END.)
GB2312_HI_END=0xD7

"$PY" - "$TMP/chars.txt" "$TTC" "$TMP/ui.ttf" "$GB2312_HI_END" <<'PY'
import sys
chars_out, ttc, ttf_out, hi_end = sys.argv[1:5]
hi_end = int(hi_end, 16)
chars = set()
# GB2312 level-1 hanzi: high byte 0xB0..hi_end, low byte 0xA1..0xFE.
for hi in range(0xB0, hi_end + 1):
    for lo in range(0xA1, 0xFF):
        try:
            ch = bytes([hi, lo]).decode('gb2312')
        except UnicodeDecodeError:
            continue
        if len(ch) == 1:
            chars.add(ch)
# Punctuation, kana and fullwidth forms.
for a, b in [(0x3000, 0x303F), (0x3040, 0x30FF),
             (0xFF00, 0xFFEF), (0x2000, 0x206F)]:
    for cp in range(a, b + 1):
        chars.add(chr(cp))
# ASCII and the symbols a reply actually prints. Without these the digits came
# from Montserrat's fallback, which reads a size larger than the 16 px hanzi
# beside them, and "°C"/"℃" had no glyph at all (the band showed "OC").
for cp in range(0x20, 0x7F):
    chars.add(chr(cp))
for cp in (0x00A0, 0x00B0, 0x00B1, 0x00B5, 0x00D7, 0x00F7,
           0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026,
           0x2030, 0x2032, 0x2033, 0x2103, 0x2109, 0x2116,
           0x2190, 0x2191, 0x2192, 0x2193, 0x25CF, 0x25CB):
    chars.add(chr(cp))
for ws in ("\n", "\r", "\t"):
    chars.discard(ws)
# Drop format/control characters: the U+202A..U+202E and U+2066..U+2069 bidi
# overrides inside 0x2000..0x206F render as nothing in the UI, and lv_font_conv
# writes them into each glyph's comment, where GCC's -Werror=bidi-chars turns
# the build red ("unpaired UTF-8 bidirectional control character detected").
import unicodedata
chars = {c for c in chars if unicodedata.category(c)[0] != "C"}
open(chars_out, "w", encoding="utf-8").write("".join(sorted(chars)))
print("ui font level-1 set: %d code points" % len(chars))
# lv_font_conv reads single faces, not TTC collections: pull face 0 out first.
from fontTools.ttLib import TTCollection
TTCollection(ttc).fonts[0].save(ttf_out)
PY

# --symbols= with the '=' matters: the subset starts with U+2014, and a bare
# argument beginning with a non-ASCII dash is rejected by npm.
npx -y lv_font_conv@1.5.3 --font "$TMP/ui.ttf" --size 16 --bpp 4 \
    --format lvgl --lv-font-name muse_font_ui_16 --no-compress \
    --symbols="$(cat "$TMP/chars.txt")" -o "$TMP/font.c"
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" \
    "$TMP/font.c" > "$OUT"
echo "wrote $OUT"
