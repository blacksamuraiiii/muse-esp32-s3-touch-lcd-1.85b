#!/usr/bin/env bash
# Regenerate fonts/muse_font_ui_16.c -- the antialiased 16 px UI subset.
#
# Why this exists: the interface used Unifont 16x16 at 1bpp, whose strokes are
# hard one-pixel lines and look rougher than the antialiased Latin text next to
# them (user report 2026-10-25). This script renders the same 16 px cell from
# WenQuanYi Micro Hei at 4bpp and keeps only the characters the sources use
# (~250 of them, ~35 KB of bitmap data instead of ~250 KB for the whole GB2312
# range). A character outside the subset still renders -- it falls back to
# muse_font_cjk_16 -- so this can never produce a tofu box, only a slightly less
# smooth glyph.
#
# Re-run after adding copy that introduces new Hanzi.
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_ui_16.c"
TTC="${WQY_TTC:-/usr/share/fonts/truetype/wqy/wqy-microhei.ttc}"
PY="${PYTHON:-$HOME/.venv/bin/python3}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

"$PY" - "$HERE" "$TMP/chars.txt" "$TTC" "$TMP/ui.ttf" <<'PY'
import glob, os, re, sys
root, chars_out, ttc, ttf_out = sys.argv[1:5]
chars = set()
# String literals only: comments (and the generated font tables) must not
# inflate the subset.
for f in glob.glob(root + "/components/muse/*.c") + glob.glob(root + "/main/*.c"):
    if "font" in os.path.basename(f):
        continue
    txt = open(f, encoding="utf-8", errors="replace").read()
    for m in re.finditer(r'"(?:[^"\\]|\\.)*"', txt):
        for ch in m.group(0):
            cp = ord(ch)
            if (0x3000 <= cp <= 0x303F or 0x4E00 <= cp <= 0x9FFF or
                    0xFF00 <= cp <= 0xFFEF or 0x2000 <= cp <= 0x206F):
                chars.add(ch)
for ws in ("\n", "\r", "\t"):
    chars.discard(ws)
open(chars_out, "w", encoding="utf-8").write("".join(sorted(chars)))
print("ui font subset: %d characters" % len(chars))
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
