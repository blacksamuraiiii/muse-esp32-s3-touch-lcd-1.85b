# Fonts

Since 1.5b there is ONE face for captions, replies and the interaction
pages, built in only with `CONFIG_MUSE_CJK_FONT`. The shared chain lives
in `muse_font.c` (`muse_font_cjk()` / `muse_font_caption()` /
`muse_font_reply()` / `muse_font_btn()`):

- `muse_font_cjk_noto_16.c` — the one face since 2026-11-03, unified to
  Noto 16 for all text on 2026-11-06: Noto Sans SC Medium at 16 px, 4bpp
  antialiased, taken from upstream's `muse_font_cjk_noto_16.c`
  (upstream's `gen_cjk_noto_font.sh`, GB2312 6763 hanzi + CJK
  punctuation and fullwidth forms, plus ASCII 0x20-0x7E and Latin-1
  0xA0-0xFF since the unification, 7242 glyphs, 721,254 bytes of
  (RLE-compressed) bitmap data, 793,430 bytes of flash all in, measured
  by compiling the file on the host -- the same measure as the old
  7431 glyphs / 875,719 bitmap / 970,327 flash. Size-trim 2026-11-20:
  compression + kana removed (0x3040-0x30FF, 189 glyphs; CJK
  punctuation 0x3000-0x303F kept) + kerning removed (--no-kerning),
  saving 176,897 bytes of flash: bitmap -154,465, kerning tables
  -20,798, glyph descriptors -1,512, kana cmaps -122.)
  It replaces Source Han Sans SC Regular level-1 (3755 hanzi) + the
  Unifont fallback below. The trade is coverage, not looks: a hanzi
  outside GB2312 (a GBK name character like 燊堃喆, which Unifont used
  to draw rough-but-present) now shows a tofu box, and a level-2 hanzi
  (晟淼鑫) renders smooth instead of as a 1bpp bitmap. Korean was tofu
  before and stays tofu: neither the old Unifont subset nor Noto covers
  Hangul. The unification adds 28,087 bytes to the face (bitmap +9,847,
  glyph descriptors +1,528, Latin kerning pairs +16,705, the rest tables):
  most of the cost is not the 191 Latin glyphs but their kerning, and
  the face is still the only text face, so Montserrat 14/16/20/28 stay in
  the build for the clock, keypad, pairing code and icons (below) and no
  Montserrat size is removed.
  Noto's own line metrics are 31/9; the file pins `.line_height` to 20
  and `.base_line` to 5, the line box the screen layout was tuned to (the
  same trick `tools/muse/gen_ui_font.sh` used for Source Han, and the
  same reason 1.5c's 18 px face was abandoned: its 34 px line height made
  the caption band 72 px and pushed Muse up into the READY label).
  The pin is applied by `tools/muse/gen_cjk_noto_font.sh` itself on every
  run, so re-running the script keeps it.
- `muse_font_sym_16.c` — the symbol tail of the chain, cut from DejaVu
  Sans 16 px 4bpp with lv_font_conv@1.5.3 (30 glyphs, ~1 KB of bitmap
  data; the generation command is in the file's header comment). Noto now
  covers CJK + fullwidth + ASCII + Latin-1, so the tail only carries what
  Noto lacks: the Latin-1 symbols, degree-C/F, quotes, arrows or geometric
  shapes the UI prints (weather 25℃, hints, reply text). Those live here,
  as the last link of the chain. The face is a fallback only, so its own
  21/5 line metrics never govern a layout.
  Spelled out glyph by glyph, not as the 0x2000-0x205F block: that block
  brought in the invisible format characters (U+200B-U+200F, U+202A-U+202E,
  U+2028/9), and since lv_font_conv writes each code point into a comment,
  GCC 15's `-Werror=bidi-chars` refused the file outright. None of them can
  be drawn; leaving them out is the fix.
- `muse_font_clock_48.c` — the ring page's big time face, cut from
  Montserrat Medium 48 px 4bpp with lv_font_conv@1.5.3 (0x30-0x3a:
  digits + colon, 11 glyphs, ~4.8 KB of bitmap data; the generation
  command and the source/license note are in the file's header comment,
  regenerate with `tools/muse/gen_clock_font.sh`). Montserrat is licensed
  under the SIL Open Font License 1.1. It is not part of the CJK chain:
  the ring page's `s_ring_time` is its only user, and it only ever draws
  `"%02d:%02d"`. The full 48 px Montserrat face is ~128 KB (unverified)
  and does not fit the 4 MB app slot.
- Montserrat 16/14/20 and unscii-16/8 come from LVGL itself. Montserrat's
  private-use range is what the LV_SYMBOL_* icons draw with, which is why
  icon labels can keep bare Montserrat while text labels cannot. Since the
  2026-11-06 unification no text label uses Montserrat: the top-bar clock
  and battery, the keypad, the pairing code and the icons still take bare
  Montserrat directly, and those four sizes (14/16/20/28, ~89 KB) stay
  compiled in.

Deleted from the tree in 1.5.1d (regeneratable: `muse_font_ui_16.c` (Source
Han level-1) by `tools/muse/gen_ui_font.sh`, `muse_font_cjk_16.c` (GNU Unifont
16.0.04 fallback) by `tools/muse/gen_cjk_font.sh`).
Removed in 1.5b: `muse_font_cjk_20.c` (the old 20 px 1bpp reply face,
two lines of 12 columns) and its generator `tools/muse/gen_cjk_font20.sh`.
Replies now share the 16 px caption chain and page two lines of 16 columns.

Noto Sans CJK is by Adobe and Google, licensed under the SIL Open Font
License, version 1.1 (https://openfontlicense.org). DejaVu Sans is
distributed under its permissive Bitstream Vera-based license. GNU
Unifont is by Roman Czyborra, Paul Hardy and contributors
(https://unifoundry.com/unifont/). Montserrat is by Julieta Ulanovsky
and contributors, licensed under the SIL Open Font License, version 1.1
(https://openfontlicense.org). These files are conversions of
unaltered subsets of the fonts.
