#!/bin/sh
# Regenerates components/muse/fonts/muse_font_pinyin_16.c: the Latin
# Extended-A cut the pinyin lines need (tiān, shuǐ, huǒ ...), 16 px 4 bpp.
#
# Why a separate face: the one CJK chain (Noto Sans SC Medium, see
# gen_cjk_noto_font.sh) carries ASCII + Latin-1 only. The tone-marked
# vowels ā ǎ ǐ ǒ ǔ live in Latin Extended-A (U+0100..U+017F), so the
# 语文练习 pinyin lines rendered "tian"/"shui"/"huo" with the vowel
# missing. The 6.4 MB CJK face is not regenerated for this: a 15 KB tail
# link is enough, and it keeps the big file byte-identical.
#
# Source: SourceHanSansSC (the same design as Noto Sans SC), shipped in
# the LVGL component, so no download is needed. Only the code points the
# source actually has in U+0100..U+017F are requested (lv_font_conv stops
# a contiguous range at the first missing glyph, hence the explicit list).
# Reproduce: sh tools/muse/gen_pinyin_font.sh
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$HERE/managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf"
OUT="$HERE/components/muse/fonts/muse_font_pinyin_16.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

[ -f "$SRC" ] || { echo "missing $SRC" >&2; exit 1; }

# fontTools is needed; PYTHON=... overrides (this box: ~/.venv/bin/python3).
PY=${PYTHON:-python3}
RANGES=$("$PY" - "$SRC" <<'EOF'
import sys
from fontTools.ttLib import TTFont
f = TTFont(sys.argv[1])
cps = set()
for t in f['cmap'].tables:
    cps |= set(t.cmap.keys())
# The tone-marked vowels the pinyin table prints: a e i o u u-umlaut with
# macron / acute / caron / grave, plus the syllabic n. Listed one by one,
# not as 0x100-0x17F: lv_font_conv cuts a contiguous range at its first
# missing glyph and the face has gaps in that block.
want = [0x00E0, 0x00E1, 0x00E8, 0x00E9, 0x00EC, 0x00ED,
        0x00F2, 0x00F3, 0x00F9, 0x00FA, 0x00FC,
        0x0101, 0x0113, 0x011B, 0x012B, 0x0144, 0x0148, 0x014D,
        0x016B, 0x01CE, 0x01D0, 0x01D2, 0x01D4, 0x01D6, 0x01D8,
        0x01DA, 0x01DC, 0x01F9]
missing = [hex(c) for c in want if c not in cps]
if missing:
    sys.stderr.write("missing glyphs: %s\n" % " ".join(missing))
    sys.exit(1)
print(" ".join("-r 0x%X" % c for c in want))
EOF
)

# shellcheck disable=SC2086
npx -y lv_font_conv@1.5.3 --font "$SRC" --size 16 --bpp 4 \
    --format lvgl --lv-font-name muse_font_pinyin_16 --no-kerning \
    $RANGES -o "$TMP/font.c"
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' "$TMP/font.c" > "$OUT"
echo "wrote $OUT"
