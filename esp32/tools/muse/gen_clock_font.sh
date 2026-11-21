#!/usr/bin/env bash
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

# Regenerate fonts/muse_font_clock_48.c -- the ring page's big time face.
#
# Why a cut-down face instead of lv_font_montserrat_48: the full 48 px
# Montserrat is ~128 KB (unverified) of flash and the 1.85B's app slot has
# only ~50 KB free (1.5.4b estimate; see the root README) -- enabling it
# made the link fail with "all app partitions are too small". The ring
# page only draws "07:00", so digits and the colon are all it needs:
# eleven glyphs, ~4.8 KB at 4bpp.
#
# Source: the Montserrat-Medium.ttf LVGL itself uses for the built-in faces
# (lvgl/scripts/built_in_font/Montserrat-Medium.ttf). Copy that file next to
# this script (or point FONT at it) and run:
#
#   bash tools/muse/gen_clock_font.sh
#
# The result is written to components/muse/fonts/muse_font_clock_48.c and is
# referenced by components/muse/muse_alarm_ui.c as muse_font_clock_48.
# Needs npx (lv_font_conv@1.5.3, the same pin the other font scripts use)
# and python3 (to rewrite the generated header: the font path it was given
# and the source/license note).
set -euo pipefail

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_clock_48.c"
FONT="${FONT:-$HERE/tools/muse/Montserrat-Medium.ttf}"

if [ ! -d "$HERE/components/muse/fonts" ]; then
    echo "gen_clock_font.sh: run from the esp32 checkout (components/muse/fonts not found under $HERE)" >&2
    exit 1
fi
if [ ! -f "$FONT" ]; then
    echo "Montserrat-Medium.ttf not found: copy it next to this script or set FONT=/path/to/Montserrat-Medium.ttf" >&2
    exit 1
fi

npx --yes lv_font_conv@1.5.3 \
    --font "$FONT" \
    --range 0x30-0x3a \
    --size 48 \
    --bpp 4 \
    --format lvgl \
    --lv-include lvgl.h \
    --no-compress \
    --no-prefilter \
    -o "$OUT"

# lv_font_conv writes the font path it was given into the header's Opts
# line; replace it with the bare file name so the output is reproducible
# on any machine, and record the source + license next to it (Montserrat
# is SIL OFL 1.1; the committed file must carry that note, see
# components/muse/fonts/README.md).
python3 - "$OUT" "$(basename "$FONT")" <<'EOF'
import re, sys
p, base = sys.argv[1], sys.argv[2]
t = open(p).read()
t2, n = re.subn(r"--font \S+", "--font " + base, t, count=1)
assert n == 1, "Opts --font not found exactly once"
note = ("/* Source: Montserrat-Medium.ttf from LVGL's built-in faces\n"
        " * (lvgl/scripts/built_in_font/); Montserrat is licensed under the\n"
        " * SIL Open Font License 1.1. Subset: 48 px, 0x30-0x3a (digits +\n"
        " * colon, 11 glyphs), ~4.8 KB. Regenerate with\n"
        " * tools/muse/gen_clock_font.sh. */\n")
head, sep, tail = t2.partition("***/\n")
assert sep, "generated header block not found"
t2 = head + sep + note + tail
open(p, "w").write(t2)
EOF

echo "wrote $OUT"
