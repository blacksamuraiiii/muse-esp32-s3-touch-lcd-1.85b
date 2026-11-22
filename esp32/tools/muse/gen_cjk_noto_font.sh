#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Regenerates components/muse/fonts/muse_font_cjk_noto_16.c: Noto Sans SC
# Medium at 16 px, 4 bpp, for CJK punctuation, the 6763 hanzi of GB2312
# and the fullwidth forms, plus ASCII and Latin-1 (the 2026-11-06
# unification: every label, button and reply renders in this one face at
# 16 px; Montserrat stays only for the clock, keypad, pairing code and
# LVGL icons). Needs curl, python3 and npx.
#
# Size-trim 2026-11-20: the font is generated WITHOUT kana
# (0x3040-0x30FF; the UI prints no Japanese, and the 0x3000-0x303F CJK
# punctuation stays) and WITHOUT kerning (--no-kerning: Latin kerning
# pairs cost ~20 KB and the 16 px UI never relied on them). Re-running
# this script reproduces the shipped file byte-for-byte.
#
# COMPRESSSION STAYS OFF (2026-11-21, 1.5.4e): the compressed variant
# saved ~131 KB of app slot and its bitmaps decode CORRECTLY on a
# single draw unit (same-scenario double build vs the 1.5.4d face:
# 1,761 differing bytes, all accountable to --no-kerning and the GB2312
# table). But LVGL's RLE decoder keeps its position state in a GLOBAL
# singleton (lv_font_fmt_txt.c: LV_GLOBAL_DEFAULT()->font_fmt_rle, fed
# by rle_init/rle_next), so two SW draw units decompressing glyphs
# concurrently step on each other -- on the real board (draw units = 2)
# that showed up as glyphs rendering smudged one moment and right the
# next. LV_USE_FONT_COMPRESSED stays unset in devices/sdkconfig.muse
# and simulator/lv_conf.h. Revisit only with an upstream fix (a
# per-call decoder state) verified by a bit-exact same-scenario double
# build AND a concurrent-decompress test.
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_cjk_noto_16.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -fsSL -o "$TMP/noto.otf" \
    "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/NotoSansSC-Medium.otf"
HANZI=$(python3 -c '
cps = set()
for hi in range(0xB0, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            cps.add(ord(bytes([hi, lo]).decode("gb2312")))
        except UnicodeDecodeError:
            pass
print(",".join(hex(c) for c in sorted(cps)))')
# Rare UI characters the copy needs that GB2312 does not carry. Add here,
# never by hand-editing the generated .c: the file is rewritten wholesale
# on every run. 筊 (0x7B4A) is the 掷筊 board's name -- 筊 is GBK-only, so
# the GB2312 sweep above misses it and the page rendered a blank
# (2026-11-16).
EXTRA="0x7B4A"
npx -y lv_font_conv@1.5.3 --font "$TMP/noto.otf" --size 16 --bpp 4 \
    --format lvgl --lv-font-name muse_font_cjk_noto_16 --no-kerning \
    --no-compress \
    -r 0x20-0x7E -r 0xA0-0xFF -r 0x3000-0x303F -r 0xFF00-0xFFEF -r "$HANZI" -r "$EXTRA" -o "$TMP/font.c"
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/font.c" > "$OUT"
# Pin the line box to 20/5 (see fonts/README.md): Noto's own metrics are
# 31/9, but every layout number on this screen was tuned to the 20/5 line
# box. The generator does not pin, so do it here, every run.
python3 - "$OUT" <<'EOF'
import re, sys
p = sys.argv[1]
t = open(p).read()
old_re = r"    \.line_height = 31,.*\n    \.base_line = 9,.*\n"
new = ("    /* Fork pin (2026-11-03, same trick as tools/muse/gen_ui_font.sh for\n"
       "     * Source Han): Noto's own metrics are 31/9, but every layout number\n"
       "     * on this screen was tuned to the 20/5 line box. Pinned here by the\n"
       "     * generator script, so re-running it keeps the caption band and list\n"
       "     * rows where they are. */\n"
       "    .line_height = 20,\n"
       "    .base_line = 5,\n")
t2, n = re.subn(old_re, new, t, count=1)
assert n == 1, "line metrics lines not found exactly once"
open(p, "w").write(t2)
EOF
echo "wrote $OUT"
