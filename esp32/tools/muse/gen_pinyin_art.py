#!/usr/bin/env python3
"""Pre-render the 语文练习 pinyin lines into A8 bitmaps.

Why not text: the 16 px CJK face has the tone-marked vowels as glyphs
(checked on the board: found=1, non-empty bitmaps) yet the board draws
them blank, and neither a second font face nor the fallback tail fixed
it. Rendering the pinyin strings offline with the same type design
sidesteps the whole text path: 30 images, ~12 KB of flash, recolor at
runtime like every other A8 asset in this project.

Run: ~/.venv/bin/python3 tools/muse/gen_pinyin_art.py
Writes components/muse/muse_tracing_pinyin_art.c

The font is SourceHanSansSC-Normal (the LVGL built-in-font copy under
managed_components/, present after a build). Set PINYIN_FONT to render
with another face instead. All thirty lines in the committed file are
now that face: the eight replacement lines were first rendered with
NotoSansSC-Medium as a stand-in in a checkout without
managed_components/, then refreshed from the real font on 2026-11-24 --
the twenty-two others came out byte-for-byte identical, which confirms
the font and the settings.
(History: on 2026-11-23 the twenty 二/三期 lines were the stand-in
ones; the 1.5.5a local run re-rendered all thirty with the real font.)
"""
from __future__ import annotations

import os
import pathlib

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[2]
FONT = pathlib.Path(
    os.environ.get("PINYIN_FONT", "")
    or str(ROOT / "managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf")
)
OUT = ROOT / "components/muse/muse_tracing_pinyin_art.c"

# Same order as MUSE_TRACING_TABLE in muse_tracing.c: three 期 of ten
# (2026-11-23: 10 -> 30; 2026-11-24: the eight numerals replaced --
# 二期 一 二 三 -> 云 雨 虫, 三期 六 七 八 十 九 -> 力 男 土 心 尺).
# Tone marks verified against the lesson order; the codepoints are
# MUSE_TRACING_TABLE's.
PINYIN = ["tiān", "dì", "rén", "rì", "yuè",
          "shān", "chuān", "mù", "shuǐ", "huǒ",
          "yún", "yǔ", "chóng", "shàng", "kǒu",
          "ěr", "mù", "shǒu", "tián", "hé",
          "lì", "nán", "tǔ", "xīn", "chǐ",
          "wáng", "wǔ", "xià", "gè", "qù"]

# The 期 cards show the phase NUMBER (一 / 二 / 三), not the phase's first
# character, so their pinyin cannot come from PINYIN -- the numerals left
# the character table on 2026-11-24 and entries 10/11/12 are now
# yún/yǔ/chóng (2026-11-24: the cards showed a box for the glyph and the
# wrong pinyin under it). Render these three after the thirty and expose
# them as MUSE_TRACING_PHASE_PINYIN_ART, one per phase.
PHASE_PINYIN = ["yī", "èr", "sān"]
SIZE = 17  # px: the UI's 16 px line, a touch larger so the marks stay legible
PAD = 1


def main() -> None:
    font = ImageFont.truetype(str(FONT), SIZE)
    images: list[Image.Image] = []
    for text in [*PINYIN, *PHASE_PINYIN]:
        # Render on a generous canvas, then crop to the ink.
        canvas = Image.new("L", (128, 48), 0)
        draw = ImageDraw.Draw(canvas)
        draw.text((4, 4), text, fill=255, font=font)
        bbox = canvas.getbbox()
        if bbox is None:
            raise SystemExit(f"empty render for {text!r}")
        x0, y0, x1, y1 = bbox
        crop = canvas.crop((x0, y0, x1, y1))
        padded = Image.new("L", (crop.width + 2 * PAD, crop.height + 2 * PAD), 0)
        padded.paste(crop, (PAD, PAD))
        images.append(padded)

    lines = [
        "/* SPDX-License-Identifier: Apache-2.0 */",
        "/*",
        " * muse_tracing_pinyin_art.c -- the pinyin lines of the 语文练习 pages:",
        " * thirty for the character table, plus three for the 期号 cards",
        " * (yī / èr / sān), pre-rendered A8 bitmaps (generated, do not",
        " * hand-edit).",
        " *",
        " * The tone-marked vowels exist in the 16 px CJK face as glyphs but the",
        " * board drew them blank; images sidestep the text path entirely.",
        " * Recolor at runtime (MUSE_THEME_DIM / MUSE_THEME_TEXT / GOLD), the",
        " * project's usual A8 idiom. Regenerate with",
        " * tools/muse/gen_pinyin_art.py. All thirty-three lines are rendered with",
        " * SourceHanSansSC-Normal (the LVGL built-in font); the eight",
        " * replacement lines (yún/yǔ/chóng/lì/nán/tǔ/xīn/chǐ, 2026-11-24)",
        " * were first rendered with NotoSansSC-Medium as a stand-in in a",
        " * checkout without managed_components/ and refreshed from the real",
        " * font on 2026-11-24, the twenty-two others unchanged.",
        " */",
        '#include "muse_tracing_pinyin_art.h"',
        "",
    ]
    names = []
    for i, img in enumerate(images):
        w, h = img.size
        name = f"muse_pinyin_art_{i}"
        names.append(name)
        data = img.tobytes()
        # LVGL A8 rows are padded to 4 bytes.
        stride = (w + 3) & ~3
        rows = bytearray()
        for y in range(h):
            row = data[y * w:(y + 1) * w]
            rows += row + b"\x00" * (stride - w)
        body = ",\n    ".join(
            ", ".join(f"0x{b:02x}" for b in rows[o:o + 16])
            for o in range(0, len(rows), 16)
        )
        lines += [
            f"static const uint8_t {name}_map[] = {{",
            f"    {body},",
            "};",
            "",
            f"const lv_image_dsc_t {name} = {{",
            "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
            "    .header.cf = LV_COLOR_FORMAT_A8,",
            f"    .header.w = {w},",
            f"    .header.h = {h},",
            f"    .header.stride = {stride},",
            f"    .data_size = sizeof({name}_map),",
            f"    .data = {name}_map,",
            "};",
            "",
        ]
    lines += [
        "/* One per character, MUSE_TRACING_TABLE order. */",
        "const lv_image_dsc_t *const MUSE_TRACING_PINYIN_ART[MUSE_TRACING_CHARS] = {",
    ]
    for name in names[:len(PINYIN)]:
        lines.append(f"    &{name},")
    lines += [
        "};",
        "",
        "/* The 期 cards' pinyin (yī / èr / sān -- the phase numbers, not each",
        " * phase's first character), one per phase. */",
        "const lv_image_dsc_t *const MUSE_TRACING_PHASE_PINYIN_ART[MUSE_TRACING_PHASES] = {",
    ]
    for name in names[len(PINYIN):]:
        lines.append(f"    &{name},")
    lines += ["};", ""]
    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT} ({len(images)} images, sizes: "
          f"{', '.join(f'{i.width}x{i.height}' for i in images)})")


if __name__ == "__main__":
    main()
