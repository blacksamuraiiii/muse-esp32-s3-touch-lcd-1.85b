#!/usr/bin/env bash
# Regenerate fonts/muse_font_ui_16.c -- the antialiased 16 px CJK face.
#
# Since 1.5c this is the ONE Chinese face for the whole UI (captions,
# replies and the interaction pages): Source Han Sans SC Regular at 16 px,
# 4bpp, covering GB2312 level-1 hanzi (3755 characters) plus punctuation,
# kana, fullwidth forms and the symbols a reply prints (~4430 code points;
# the handful with no glyph in Source Han Sans fall back to muse_font_cjk_16
# at runtime, so nothing becomes a tofu box).
#
# It replaces WenQuanYi Micro Hei 16 px as the lead face; the layout numbers
# in muse_ui.c were never retuned for it, so the two must still agree.
#
# 1.5c tried 18 px Source Han first. Source Han's own vertical metrics put
# that face's line height at 34 px, which made the caption band 72 px
# (2 x 34 + 4) and pushed Muse up 28 px into the READY label. The face is
# therefore generated at 16 px AND its .line_height/.base_line are pinned
# below to the 1.5b values (20 / 5), so every layout number in muse_ui.c
# lands exactly where it did in 1.5b: band 44 px, Muse top at y=52.
# Do not remove the pinning sed -- it is what keeps the screen on 1.5b.
#
# The character set is fixed, not scanned from the sources: GB2312 level-1
# is zones 16..55, i.e. high byte 0xB0..0xD7 with low byte 0xA1..0xFE,
# decoded to Unicode with Python's gb2312 codec. The full GB2312 range,
# high byte 0xB0..0xF7, would add level-2 (6763 hanzi, ~740 KB of bitmaps
# at 16 px by area scaling): change GB2312_HI_END below if that trade ever
# wins, and re-check the 4 MB app slot first.
#
# Do NOT subset the OTF with pyftsubset first: subsetting was measured to
# change glyph bitmaps by a row. Generate from the full OTF; nothing of the
# OTF is committed.
#
# Needs: SourceHanSansSC-Regular.otf (SIL Open Font License 1.1,
# adobe-fonts/source-han-sans release, OTF/SimplifiedChinese/) pointed to by
# $SHS_OTF, Python 3, and npx for lv_font_conv@1.5.3. This script is NOT run
# by the build; run it by hand after changing the set, then commit the
# regenerated .c file.
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_ui_16.c"
OTF="${SHS_OTF:-}"
if [ -z "$OTF" ]; then
    echo "Set SHS_OTF to SourceHanSansSC-Regular.otf (adobe-fonts/source-han-sans" >&2
    echo "release, OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf, SIL OFL 1.1)." >&2
    exit 1
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Level-1 is GB2312 qu (zone) 16..55 -> high byte 0xB0..0xD7, low 0xA1..0xFE.
# (The full GB2312 range, high byte 0xB0..0xF7, would add level-2; see the
# header comment before changing GB2312_HI_END.)
GB2312_HI_END=0xD7
# 1.5b's line box: the layout in muse_ui.c is tuned to these two numbers.
LINE_HEIGHT=20
BASE_LINE=5

python3 - "$TMP/chars.txt" "$GB2312_HI_END" <<'PY'
import sys
chars_out, hi_end = sys.argv[1:3]
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
for cp in (0x00A0, 0x00B0, 0x00B1, 0x00B5, 0x00B7, 0x00D7, 0x00F7,
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
PY

# --symbols= with the '=' matters: the subset starts with U+2014, and a bare
# argument beginning with a non-ASCII dash is rejected by npm.
npx -y lv_font_conv@1.5.3 --font "$OTF" --size 16 --bpp 4 \
    --format lvgl --lv-font-name muse_font_ui_16 --no-compress \
    --symbols="$(cat "$TMP/chars.txt")" -o "$TMP/font.c"
# Pin the line box to 1.5b's, and fix the include the simulator needs.
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' \
    -e "s|$TMP/||g" \
    -e "s|$OTF|SourceHanSansSC-Regular.otf|g" \
    -e "s|\.line_height = [0-9]*,|.line_height = $LINE_HEIGHT,|" \
    -e "s|\.base_line = [0-9]*,|.base_line = $BASE_LINE,|" \
    "$TMP/font.c" > "$OUT"
grep -q "^    \.line_height = $LINE_HEIGHT," "$OUT" || {
    echo "line_height was not pinned; check the sed against the generator output" >&2
    exit 1
}
echo "wrote $OUT"
