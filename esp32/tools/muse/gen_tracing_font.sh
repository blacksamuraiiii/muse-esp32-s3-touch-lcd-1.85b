#!/usr/bin/env bash
# Regenerate fonts/muse_font_tracing_kai_48_v1.c -- the 语文练习 trace
 * page's big ghost character and the character list's 48 px face.
#
# Why a cut-down face instead of the full 48 px Noto: the full 48 px
# Noto Sans SC is far over 100 KB and the 1.85B's app slot has ~50 KB
# free. The trace page only draws the ten first-grade characters
# (天地人日月山川木水火), so ten glyphs at 4bpp, ~9.8 KB, is all it
# needs. A 170 px subset would be ~115.9 KB and does not fit at all,
# so the ghost is this 48 px face scaled to 170 px at runtime
# (lv_obj_set_style_transform_scale); a faint fill is all the ghost
# needs, and the softness of a 3.5x upscale is acceptable there.
#
# Source: AR PL UKai CN (文鼎楷体, Arphic Public License -- GPL-like with
# a font-embedding exception, so shipping the ten glyphs inside the
# firmware is allowed; the note in the generated file records it). The
# user asked for 楷体 on 2026-11-21: 黑体's 火 draws its dots outward, 楷体
# draws them inward, and 楷体 is the model handwriting taught in school.
# Get the face with (no root needed):
#   curl -o /tmp/ukai.deb https://mirrors.tuna.tsinghua.edu.cn/debian/pool/main/f/fonts-arphic-ukai/fonts-arphic-ukai_0.2.20080216.2-5_all.deb
#   dpkg-deb -x /tmp/ukai.deb /tmp/ukaix
#   python3 -c "from fontTools.ttLib import TTCollection; \
#     TTCollection('/tmp/ukaix/usr/share/fonts/truetype/arphic/ukai.ttc').fonts[0].save('/tmp/ukai_cn.ttf')"
# then FONT=/tmp/ukai_cn.ttf and run from esp32/:
#
#   bash tools/muse/gen_tracing_font.sh
#
# The result is written to
# components/muse/fonts/muse_font_tracing_kai_48_v1.c and is referenced by
# components/muse/muse_tracing_ui.c as muse_font_tracing_kai_48_v1.
# Needs npx (lv_font_conv@1.5.2, the pin that generated the committed
# file) and python3 (to rewrite the generated header: the font path it
# was given and the source/license note).
set -euo pipefail

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_tracing_kai_48_v1.c"
FONT="${FONT:-/tmp/ukai_cn.ttf}"

if [ ! -d "$HERE/components/muse/fonts" ]; then
    echo "gen_tracing_font.sh: run from the esp32 checkout (components/muse/fonts not found under $HERE)" >&2
    exit 1
fi
if [ ! -f "$FONT" ]; then
    echo "ukai_cn.ttf not found: see the header for how to extract it, or set FONT=/path/to/ukai_cn.ttf" >&2
    exit 1
fi

npx --yes lv_font_conv@1.5.3 \
    --font "$FONT" \
    --range 0x5929,0x5730,0x4eba,0x65e5,0x6708,0x5c71,0x5ddd,0x6728,0x6c34,0x706b \
    --size 48 \
    --bpp 4 \
    --format lvgl \
    --lv-include lvgl.h \
    --no-compress \
    --no-prefilter \
    -o "$OUT"

# lv_font_conv writes the font path it was given into the header's Opts
# line; replace it with the bare file name so the output is reproducible
# on any machine, and record the source + license next to it (Noto Sans
# CJK is SIL OFL 1.1; the committed file must carry that note, see
# components/muse/fonts/README.md).
python3 - "$OUT" "$(basename "$FONT")" <<'EOF'
import re, sys
p, base = sys.argv[1], sys.argv[2]
t = open(p).read()
t2, n = re.subn(r"--font \S+", "--font " + base, t, count=1)
assert n == 1, "Opts --font not found exactly once"
t2, n = re.subn(r"-o \S+", "-o components/muse/fonts/muse_font_tracing_kai_48_v1.c", t2, count=1)
assert n == 1, "Opts -o not found exactly once"
note = ("/* Source: AR PL UKai CN (文鼎楷体), Arphic Public License with\n"
        " * the font-embedding exception. 楷体 rather than 黑体 because the\n"
        " * trace page teaches handwriting (user decision 2026-11-21).\n"
        " * Subset: 48 px, 10 glyphs (0x5929,0x5730,0x4eba,\n"
        " * 0x65e5,0x6708,0x5c71,0x5ddd,0x6728,0x6c34,0x706b -- 天 地 人 日\n"
        " * 月 山 川 木 水 火), ~9.8 KB. Regenerate with\n"
        " * tools/muse/gen_tracing_font.sh. */\n")
head, sep, tail = t2.partition("***/\n")
assert sep, "generated header block not found"
t2 = head + sep + note + tail
open(p, "w").write(t2)
EOF

echo "wrote $OUT"
