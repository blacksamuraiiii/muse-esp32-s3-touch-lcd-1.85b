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

# Regenerates components/muse/fonts/muse_font_cjk_20.c, the 20 px face a Chinese
# reply is drawn in (CONFIG_MUSE_CJK_FONT): GNU Unifont's 16.0.04 outlines at
# 20 px, 1 bpp, so it keeps the same 1-bit look as unscii but is a quarter
# larger. Coverage is ASCII, CJK punctuation and kana, the fullwidth forms and
# the 6763 GB2312 hanzi - a full CJK Unified Ideographs range at 20 px would be
# 1.4 MB and overflow the 4 MB app slot, and anything outside GB2312 falls back
# to muse_font_cjk_16 anyway. Needs curl, python3 and npx (Node.js).
set -eu

VERSION=16.0.04
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_cjk_20.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

SHA256=0e3981ab552231b5a2a870f2b61741903a4bf25c23ef5aeb05fdced1b3c7af4d
curl -fsSL -o "$TMP/unifont.otf" \
    "https://unifoundry.com/pub/unifont/unifont-$VERSION/font-builds/unifont-$VERSION.otf"
echo "$SHA256  $TMP/unifont.otf" | shasum -a 256 -c - >/dev/null

# Every hanzi GB2312 encodes (levels 1 and 2), plus the symbols Chinese replies
# use that GB2312 lacks.
python3 - "$TMP/symbols.txt" <<'PY'
import sys
chars = []
for hi in range(0xB0, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            chars.append(bytes([hi, lo]).decode('gb2312'))
        except UnicodeDecodeError:
            continue
extra = '·—…‘’“”°℃±×÷≤≥≠∞①②③④⑤⑥⑦⑧⑨⑩'
with open(sys.argv[1], 'w', encoding='utf-8') as f:
    f.write(''.join(sorted(set(chars + list(extra)))))
PY

npx -y lv_font_conv@1.5.3 --font "$TMP/unifont.otf" --size 20 --bpp 1 \
    --format lvgl --lv-font-name muse_font_cjk_20 --no-compress \
    -r 0x20-0x7E -r 0x3000-0x30FF -r 0xFF00-0xFFEF \
    --symbols "$(cat "$TMP/symbols.txt")" -o "$TMP/font.c"
# The project includes LVGL as "lvgl.h"; the header names no temp paths.
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/font.c" > "$OUT"
echo "wrote $OUT"
