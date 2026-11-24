/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_tracing.c -- 描红写字 (语文练习) logic.
 *
 * Grading against the reference strokes (below): each traced point is
 * tested against the current character's resampled strokes, a stroke
 * is covered when enough of its samples are near a traced point, and
 * the character is done when every stroke is covered. A point far
 * from every sample is stray; too many stray points call the attempt
 * wrong. No stroke-order check (user decision 2026-11-20).
 *
 * The reference strokes are the skeleton centrelines of each
 * character's 170 px Noto Sans SC Medium glyph, measured from the
 * rendered pixels (Zhang-Suen thinning, direction-continuing walks,
 * Douglas-Peucker eps 3 px, pieces < 8 px dropped, exact duplicates
 * removed -- see ~/workspace/tracing-strokes-extract.py, which is
 * the source of truth; do not hand-edit the table below). The
 * firmware ghost is the same glyph at the same size and centre, so
 * the lines sit on the ghost the child traces over. Coordinates are
 * field pixels (0..227). The second and third ten (2026-11-23,
 * 二/三期) were extracted with the same script and the same
 * settings, and checked the same way (red polylines over the grey
 * glyph, plus the host harness tracing every character to DONE).
 * On 2026-11-24 the eight numeral characters were replaced (user
 * decision: too easy, and they duplicate the 一/二/三 phase cards):
 * 二期 一 二 三 -> 云 雨 虫, 三期 六 七 八 十 九 -> 力 男 土 心 尺
 * (same book order, skipping used characters; pinyin from the 写字表
 * photo). Their strokes were extracted with
 * ~/workspace/tracing-strokes-extract-new8.py, the same method plus
 * one rule the 8-point stroke cap forces: a polyline with more than
 * MUSE_TRACING_STROKE_MAX_PTS (8) points is split into the fewest
 * pieces of <= 8, cutting at the sharpest interior turn (ties ->
 * lowest index), recursing -- 虫 needed it (13 -> 4 pieces) and 男
 * (9 -> 2 pieces); the cut points are real skeleton corners, and
 * grading is overall coverage, so a split changes nothing but the
 * per-stroke diagnostic. The 霞鹜文楷 ghost-alignment check (1.5.5a
 * method: LVGL formula, font dsc, per-point distance to the ghost
 * ink) still has to be run for these eight once the font is
 * regenerated -- see esp32/notes/20261124-语文练习数字字替换说明.md.
 *
 * On 2026-11-24 the 1.5.5a review (B-1) flagged the Noto table
 * against the 霞鹜文楷 ghost, so the twenty-two characters whose
 * glyphs exist in muse_font_tracing_kai_48_v1.c were re-extracted
 * from that font at 48 px (~/workspace/tracing-strokes-extract-
 * lxgw22.py: decode the LVGL A8 bitmaps, place each glyph by the
 * LVGL ghost geometry -- 48 px label, line height 46, transform
 * scale 907/256 about the field centre -- then the same Zhang-Suen
 * walk, DP eps 3, < 8 px drop, and the > 8-point split above).
 * The eight replacement characters keep their Noto strokes as
 * placeholders (the font has no glyphs for them yet); their LXGW
 * extraction is a separate step, see esp32/notes/20261124-1.5.5a
 * 评审修补说明.md. Table grew 163 -> 237 strokes; the longest
 * character is now 手 at 17 strokes (MUSE_TRACING_MAX_STROKES
 * raised 10 -> 20) and 地 at 213 resampled samples (limit 320
 * untouched).
 */
#include "muse_tracing.h"

#include <string.h>

const muse_tracing_char_t MUSE_TRACING_TABLE[MUSE_TRACING_CHARS] = {
    { "\xe5\xa4\xa9", "ti\xc4\x81n", 0x5929 }, /* 天 */
    { "\xe5\x9c\xb0", "d\xc3\xac", 0x5730 },   /* 地 */
    { "\xe4\xba\xba", "r\xc3\xa9n", 0x4EBA },  /* 人 */
    { "\xe6\x97\xa5", "r\xc3\xac", 0x65E5 },   /* 日 */
    { "\xe6\x9c\x88", "yu\xc3\xa8", 0x6708 },  /* 月 */
    { "\xe5\xb1\xb1", "sh\xc4\x81n", 0x5C71 }, /* 山 */
    { "\xe5\xb7\x9d", "chu\xc4\x81n", 0x5DDD },/* 川 */
    { "\xe6\x9c\xa8", "m\xc3\xb9", 0x6728 },   /* 木 */
    { "\xe6\xb0\xb4", "shu\xc7\x90", 0x6C34 }, /* 水 */
    { "\xe7\x81\xab", "hu\xc7\x92", 0x706B },  /* 火 */
    { "\xe4\xba\x91", "y\xc3\xban", 0x4E91 }, /* 云 */
    { "\xe9\x9b\xa8", "y\xc7\x94", 0x96E8 }, /* 雨 */
    { "\xe8\x99\xab", "ch\xc3\xb3ng", 0x866B }, /* 虫 */
    { "\xe4\xb8\x8a", "sh\xc3\xa0ng", 0x4E0A }, /* 上 */
    { "\xe5\x8f\xa3", "k\xc7\x92u", 0x53E3 }, /* 口 */
    { "\xe8\x80\xb3", "\xc4\x9br", 0x8033 }, /* 耳 */
    { "\xe7\x9b\xae", "m\xc3\xb9", 0x76EE }, /* 目 */
    { "\xe6\x89\x8b", "sh\xc7\x92u", 0x624B }, /* 手 */
    { "\xe7\x94\xb0", "ti\xc3\xa1n", 0x7530 }, /* 田 */
    { "\xe7\xa6\xbe", "h\xc3\xa9", 0x79BE }, /* 禾 */
    { "\xe5\x8a\x9b", "l\xc3\xac", 0x529B }, /* 力 */
    { "\xe7\x94\xb7", "n\xc3\xa1n", 0x7537 }, /* 男 */
    { "\xe5\x9c\x9f", "t\xc7\x94", 0x571F }, /* 土 */
    { "\xe5\xbf\x83", "x\xc4\xabn", 0x5FC3 }, /* 心 */
    { "\xe5\xb0\xba", "ch\xc7\x90", 0x5C3A }, /* 尺 */
    { "\xe7\x8e\x8b", "w\xc3\xa1ng", 0x738B }, /* 王 */
    { "\xe5\x8d\x88", "w\xc7\x94", 0x5348 }, /* 午 */
    { "\xe4\xb8\x8b", "xi\xc3\xa0", 0x4E0B }, /* 下 */
    { "\xe4\xb8\xaa", "g\xc3\xa8", 0x4E2A }, /* 个 */
    { "\xe5\x8e\xbb", "q\xc3\xb9", 0x53BB }, /* 去 */
};

static const muse_tracing_stroke_t muse_tracing_stroke_table[] = {
    /* 天 */
    { 2, { {164,55}, {143,55} } },
    { 2, { {60,57}, {82,59} } },
    { 3, { {170,96}, {171,103}, {181,105} } },
    { 3, { {50,103}, {54,108}, {69,107} } },
    { 5, { {184,184}, {174,172}, {155,162}, {106,106}, {71,106} } },
    { 2, { {39,185}, {49,178} } },
    { 3, { {141,55}, {111,59}, {112,111} } },
    { 2, { {92,57}, {110,58} } },
    { 2, { {117,103}, {170,103} } },
    { 4, { {110,113}, {94,143}, {72,165}, {51,178} } },
    /* 地 */
    { 3, { {135,39}, {138,87}, {146,87} } },
    { 6, { {61,50}, {68,57}, {68,94}, {82,95}, {93,109}, {86,110} } },
    { 4, { {101,69}, {109,73}, {110,98}, {122,98} } },
    { 2, { {174,74}, {173,104} } },
    { 4, { {39,90}, {68,96}, {68,141}, {76,140} } },
    { 7, { {87,91}, {110,107}, {113,172}, {124,177}, {170,177}, {171,173}, {182,172} } },
    { 2, { {95,131}, {81,139} } },
    { 2, { {190,133}, {187,164} } },
    { 5, { {163,142}, {162,138}, {138,130}, {138,96}, {130,97} } },
    { 2, { {34,151}, {49,153} } },
    { 2, { {140,151}, {138,131} } },
    { 2, { {166,82}, {148,87} } },
    { 2, { {93,108}, {104,108} } },
    { 2, { {170,110}, {169,128} } },
    { 3, { {169,130}, {168,138}, {163,138} } },
    { 3, { {68,142}, {58,152}, {51,152} } },
    /* 人 */
    { 3, { {107,43}, {113,54}, {120,54} } },
    { 5, { {183,182}, {144,147}, {119,110}, {108,104}, {110,90} } },
    { 3, { {36,185}, {76,154}, {95,125} } },
    { 2, { {112,54}, {110,88} } },
    /* 日 */
    { 5, { {64,53}, {73,62}, {75,113}, {152,110}, {151,160} } },
    { 2, { {159,170}, {151,162} } },
    { 2, { {76,186}, {74,113} } },
    { 3, { {120,56}, {152,57}, {153,101} } },
    { 2, { {75,60}, {115,60} } },
    { 2, { {141,167}, {77,172} } },
    /* 月 */
    { 2, { {82,53}, {107,55} } },
    { 4, { {122,165}, {136,178}, {150,182}, {151,188} } },
    { 4, { {158,178}, {152,177}, {152,124}, {130,125} } },
    { 3, { {43,187}, {57,176}, {78,142} } },
    { 3, { {114,53}, {152,54}, {152,123} } },
    { 3, { {85,57}, {85,91}, {100,90} } },
    { 2, { {118,88}, {151,88} } },
    { 2, { {102,90}, {115,90} } },
    { 2, { {83,98}, {82,108} } },
    { 3, { {82,110}, {83,127}, {114,126} } },
    { 2, { {116,125}, {128,125} } },
    /* 山 */
    { 6, { {110,50}, {113,165}, {150,166}, {153,162}, {165,162}, {168,182} } },
    { 3, { {161,84}, {166,91}, {166,162} } },
    { 4, { {55,94}, {60,99}, {60,168}, {66,170} } },
    { 2, { {110,167}, {68,169} } },
    /* 川 */
    { 3, { {153,39}, {160,46}, {162,185} } },
    { 2, { {68,50}, {68,105} } },
    { 3, { {107,50}, {114,56}, {116,168} } },
    { 2, { {105,158}, {113,158} } },
    { 3, { {34,186}, {51,169}, {64,142} } },
    { 2, { {68,107}, {66,128} } },
    { 2, { {65,130}, {64,140} } },
    /* 木 */
    { 7, { {108,38}, {113,43}, {113,82}, {117,86}, {113,92}, {162,140}, {186,157} } },
    { 3, { {170,75}, {170,81}, {176,82} } },
    { 3, { {53,82}, {54,87}, {62,90} } },
    { 2, { {196,154}, {186,155} } },
    { 5, { {39,166}, {58,152}, {108,98}, {113,103}, {112,178} } },
    { 2, { {115,193}, {112,180} } },
    { 2, { {125,82}, {170,82} } },
    { 3, { {71,85}, {102,85}, {112,92} } },
    /* 水 */
    { 7, { {113,43}, {117,98}, {135,101}, {140,117}, {173,150}, {186,157}, {188,165} } },
    { 2, { {167,64}, {172,72} } },
    { 2, { {46,85}, {82,87} } },
    { 3, { {101,86}, {91,90}, {84,86} } },
    { 4, { {78,163}, {102,176}, {115,175}, {117,99} } },
    { 3, { {39,166}, {73,132}, {88,100} } },
    { 3, { {169,72}, {157,87}, {136,101} } },
    /* 火 */
    { 3, { {108,41}, {113,46}, {112,103} } },
    { 3, { {160,71}, {164,78}, {169,78} } },
    { 4, { {62,72}, {62,82}, {85,112}, {91,113} } },
    { 6, { {179,185}, {155,170}, {134,149}, {122,129}, {111,125}, {111,105} } },
    { 3, { {163,78}, {158,89}, {121,127} } },
    { 3, { {107,126}, {89,158}, {44,188} } },
    /* 云 */
    { 2, { {65,48}, {163,48} } },
    { 4, { {46,96}, {95,96}, {96,105}, {89,120} } },
    { 2, { {181,96}, {98,98} } },
    { 2, { {140,126}, {174,174} } },
    { 2, { {59,168}, {129,162} } },
    { 2, { {88,122}, {70,149} } },
    { 2, { {69,151}, {65,165} } },
    { 2, { {159,160}, {131,162} } },
    /* 雨 */
    { 7, { {45,47}, {112,47}, {114,83}, {175,84}, {174,173}, {171,178}, {153,179} } },
    { 2, { {182,47}, {114,47} } },
    { 2, { {75,107}, {92,117} } },
    { 2, { {132,107}, {154,117} } },
    { 3, { {67,140}, {74,140}, {92,152} } },
    { 2, { {130,140}, {152,152} } },
    { 4, { {113,176}, {113,84}, {53,83}, {52,179} } },
    /* 虫 */
    { 8, { {112,43}, {113,67}, {166,68}, {166,121}, {113,121}, {111,67}, {58,67}, {57,120} } },
    { 3, { {57,120}, {111,121}, {113,171} } },
    { 2, { {113,171}, {165,168} } },
    { 3, { {165,168}, {163,156}, {152,145} } },
    { 2, { {48,175}, {111,171} } },
    { 2, { {179,182}, {167,167} } },
    /* 上 */
    { 3, { {106,47}, {109,101}, {165,99} } },
    { 2, { {39,170}, {78,172} } },
    { 3, { {192,171}, {110,169}, {108,101} } },
    { 2, { {96,169}, {109,169} } },
    { 2, { {94,170}, {80,171} } },
    /* 口 */
    { 4, { {62,69}, {69,76}, {72,151}, {106,151} } },
    { 3, { {161,152}, {153,146}, {155,135} } },
    { 2, { {74,162}, {72,152} } },
    { 3, { {124,70}, {155,71}, {155,133} } },
    { 2, { {122,71}, {69,75} } },
    { 2, { {152,147}, {119,150} } },
    { 2, { {108,150}, {117,150} } },
    /* 耳 */
    { 5, { {46,51}, {57,56}, {82,56}, {84,83}, {104,83} } },
    { 5, { {180,53}, {153,49}, {150,53}, {93,53}, {84,57} } },
    { 3, { {38,139}, {40,145}, {51,147} } },
    { 2, { {185,140}, {170,139} } },
    { 4, { {146,190}, {142,183}, {142,142}, {104,144} } },
    { 3, { {142,54}, {142,140}, {161,140} } },
    { 2, { {118,81}, {141,81} } },
    { 2, { {106,83}, {115,83} } },
    { 3, { {84,84}, {85,113}, {115,112} } },
    { 2, { {125,109}, {141,109} } },
    { 3, { {84,113}, {82,144}, {102,145} } },
    { 2, { {62,145}, {82,145} } },
    /* 目 */
    { 3, { {66,52}, {71,57}, {128,56} } },
    { 5, { {161,168}, {155,166}, {154,127}, {73,130}, {71,58} } },
    { 2, { {76,186}, {74,132} } },
    { 4, { {135,53}, {152,53}, {157,59}, {156,102} } },
    { 2, { {103,92}, {156,92} } },
    { 2, { {96,94}, {74,96} } },
    { 2, { {155,104}, {155,127} } },
    { 2, { {125,166}, {154,166} } },
    { 2, { {75,170}, {116,170} } },
    /* 手 */
    { 3, { {149,39}, {149,46}, {158,48} } },
    { 2, { {61,67}, {81,64} } },
    { 2, { {155,84}, {163,89} } },
    { 2, { {64,89}, {100,91} } },
    { 3, { {183,115}, {185,121}, {191,121} } },
    { 3, { {35,122}, {46,127}, {61,126} } },
    { 3, { {79,167}, {99,179}, {113,181} } },
    { 2, { {144,47}, {118,56} } },
    { 4, { {114,57}, {117,116}, {120,122}, {127,121} } },
    { 2, { {104,60}, {112,62} } },
    { 2, { {89,62}, {101,62} } },
    { 2, { {143,87}, {117,88} } },
    { 2, { {145,87}, {155,87} } },
    { 2, { {113,88}, {102,90} } },
    { 2, { {129,120}, {183,120} } },
    { 2, { {119,122}, {118,172} } },
    { 2, { {68,124}, {118,124} } },
    /* 田 */
    { 4, { {50,64}, {60,71}, {60,116}, {68,115} } },
    { 2, { {179,65}, {144,65} } },
    { 3, { {170,163}, {163,162}, {165,130} } },
    { 2, { {66,178}, {60,118} } },
    { 2, { {139,66}, {68,70} } },
    { 3, { {114,69}, {114,112}, {147,112} } },
    { 3, { {169,69}, {167,110}, {155,111} } },
    { 2, { {165,112}, {165,128} } },
    { 4, { {79,113}, {114,114}, {115,163}, {163,163} } },
    { 2, { {86,163}, {114,163} } },
    { 2, { {76,165}, {65,166} } },
    /* 禾 */
    { 3, { {149,39}, {149,47}, {154,48} } },
    { 2, { {59,69}, {71,67} } },
    { 2, { {172,90}, {179,97} } },
    { 2, { {47,96}, {59,103} } },
    { 5, { {183,163}, {117,106}, {112,97}, {110,60}, {73,67} } },
    { 5, { {40,169}, {69,147}, {106,107}, {99,99}, {59,99} } },
    { 4, { {116,192}, {112,178}, {112,107}, {107,106} } },
    { 2, { {125,55}, {113,62} } },
    { 2, { {125,95}, {172,95} } },
    /* 力 */
    { 4, { {104,43}, {105,78}, {174,79}, {171,122} } },
    { 3, { {50,78}, {102,78}, {100,109} } },
    { 2, { {40,180}, {52,180} } },
    { 3, { {133,182}, {152,182}, {164,172} } },
    { 2, { {100,111}, {94,128} } },
    { 2, { {171,124}, {167,159} } },
    { 3, { {91,135}, {77,159}, {54,179} } },
    { 2, { {167,161}, {164,170} } },
    /* 男 */
    { 4, { {48,131}, {102,132}, {93,155}, {74,169} } },
    { 2, { {39,174}, {55,176} } },
    { 3, { {125,175}, {136,180}, {158,177} } },
    { 5, { {64,45}, {111,45}, {114,73}, {164,73}, {165,101} } },
    { 5, { {165,101}, {114,102}, {113,74}, {62,73}, {61,48} } },
    { 3, { {116,45}, {163,45}, {165,73} } },
    { 6, { {61,73}, {62,102}, {108,102}, {109,131}, {169,131}, {168,149} } },
    { 2, { {168,151}, {164,167} } },
    { 2, { {72,169}, {58,176} } },
    /* 土 */
    { 3, { {113,48}, {114,99}, {171,99} } },
    { 4, { {56,99}, {113,100}, {114,180}, {182,180} } },
    { 2, { {45,180}, {113,180} } },
    /* 心 */
    { 2, { {97,48}, {131,75} } },
    { 5, { {87,90}, {88,174}, {97,180}, {141,180}, {152,170} } },
    { 3, { {57,103}, {48,148}, {42,156} } },
    { 2, { {154,153}, {152,168} } },
    { 2, { {167,97}, {183,148} } },
    /* 尺 */
    { 3, { {40,176}, {47,174}, {57,153} } },
    { 6, { {66,45}, {66,97}, {122,98}, {140,145}, {158,165}, {179,175} } },
    { 4, { {68,45}, {167,45}, {168,97}, {124,99} } },
    { 2, { {66,98}, {57,150} } },
    { 2, { {152,158}, {158,164} } },
    /* 王 */
    { 3, { {161,52}, {161,59}, {169,60} } },
    { 4, { {59,62}, {109,63}, {111,112}, {157,115} } },
    { 3, { {65,109}, {73,115}, {89,115} } },
    { 3, { {181,163}, {181,169}, {187,170} } },
    { 3, { {41,164}, {49,173}, {82,172} } },
    { 2, { {123,60}, {161,60} } },
    { 4, { {93,113}, {111,114}, {114,169}, {181,170} } },
    { 2, { {100,169}, {113,169} } },
    { 2, { {98,170}, {84,171} } },
    /* 午 */
    { 2, { {89,36}, {87,61} } },
    { 5, { {162,68}, {152,64}, {144,68}, {88,68}, {87,63} } },
    { 4, { {53,107}, {73,86}, {76,73}, {86,67} } },
    { 3, { {177,110}, {178,117}, {189,119} } },
    { 5, { {43,118}, {45,123}, {53,124}, {114,121}, {117,191} } },
    { 3, { {116,69}, {115,118}, {126,118} } },
    { 2, { {128,117}, {177,117} } },
    /* 下 */
    { 3, { {174,50}, {176,59}, {186,59} } },
    { 3, { {43,57}, {44,61}, {61,63} } },
    { 4, { {162,122}, {110,95}, {109,60}, {66,62} } },
    { 2, { {105,179}, {112,185} } },
    { 2, { {129,57}, {173,57} } },
    { 2, { {122,59}, {110,60} } },
    { 2, { {111,98}, {110,179} } },
    /* 个 */
    { 3, { {112,38}, {114,43}, {120,43} } },
    { 3, { {107,89}, {114,96}, {114,184} } },
    { 5, { {197,116}, {182,114}, {152,94}, {112,55}, {114,51} } },
    { 3, { {36,128}, {73,101}, {111,56} } },
    /* 去 */
    { 6, { {106,39}, {113,49}, {114,78}, {140,78}, {145,74}, {162,78} } },
    { 6, { {65,73}, {67,78}, {77,81}, {112,78}, {113,116}, {119,117} } },
    { 5, { {39,117}, {49,124}, {57,120}, {102,120}, {103,126} } },
    { 6, { {136,136}, {136,146}, {143,152}, {146,164}, {155,166}, {171,182} } },
    { 2, { {51,172}, {75,176} } },
    { 2, { {121,117}, {176,117} } },
    { 2, { {114,119}, {105,122} } },
    { 4, { {101,134}, {83,158}, {80,172}, {107,172} } },
    { 2, { {130,168}, {138,168} } },
    { 2, { {128,169}, {109,172} } },
};

/* First stroke index per character, in MUSE_TRACING_CHARS
 * order; the count is the next entry minus this one. */
static const uint8_t muse_tracing_stroke_first[] = {
    0, 10, 26, 30, 36, 47, 51, 58, 66, 73,
    79, 87, 94, 100, 105, 112, 124, 133, 150, 161,
    170, 178, 187, 190, 195, 200, 209, 216, 223, 227,
};
/* Total strokes: 237. */

static int dist2(int x0, int y0, int x1, int y1)
{
    int dx = x0 - x1, dy = y0 - y1;
    return dx * dx + dy * dy;
}

/* Resample one stroke into the state's sample buffer, one sample per
 * MUSE_TRACING_SAMPLE_STEP_PX of arc. A 2-point stroke is a straight
 * segment; longer polylines are walked segment by segment. The
 * buffer is sized for the longest character (see the header); if it
 * ever filled, the remaining samples are dropped rather than
 * overflowing -- the character would grade leniently, which is the
 * safe direction. */
static void resample_stroke(muse_tracing_t *t, const muse_tracing_stroke_t *s,
                            uint8_t stroke)
{
    for (int i = 0; i + 1 < s->n; i++) {
        int x0 = s->pts[i].x, y0 = s->pts[i].y;
        int x1 = s->pts[i + 1].x, y1 = s->pts[i + 1].y;
        int dx = x1 - x0, dy = y1 - y0;
        int len2 = dx * dx + dy * dy;
        int steps = 1;
        while (steps * steps * MUSE_TRACING_SAMPLE_STEP_PX *
                       MUSE_TRACING_SAMPLE_STEP_PX <
               len2) {
            steps++;
        }
        for (int k = 0; k < steps; k++) {
            if (t->sample_count >= MUSE_TRACING_MAX_SAMPLES) {
                return;
            }
            t->samples[t->sample_count].x =
                (int16_t)(x0 + dx * k / steps);
            t->samples[t->sample_count].y =
                (int16_t)(y0 + dy * k / steps);
            t->sample_stroke[t->sample_count] = stroke;
            t->sample_count++;
        }
    }
    /* The stroke's end point, so a short stroke still owns a sample. */
    if (t->sample_count < MUSE_TRACING_MAX_SAMPLES) {
        t->samples[t->sample_count] = s->pts[s->n - 1];
        t->sample_stroke[t->sample_count] = stroke;
        t->sample_count++;
    }
}

static void rebuild_samples(muse_tracing_t *t)
{
    int count = 0;
    const muse_tracing_stroke_t *strokes = muse_tracing_strokes(t->char_idx, &count);
    t->sample_count = 0;
    /* A fresh attempt starts from zero coverage: clear the whole
     * state here, set_char() and reset() both funnel through this --
     * including the no-character case (strokes == NULL, before any
     * character is picked), so a stale attempt's coverage never
     * survives into the next one (M-18). */
    t->sample_total = 0;
    t->sample_covered = 0;
    memset(t->stroke_total, 0, sizeof(t->stroke_total));
    memset(t->stroke_cover, 0, sizeof(t->stroke_cover));
    memset(t->stroke_covered_count, 0, sizeof(t->stroke_covered_count));
    memset(t->sample_covered_flag, 0, sizeof(t->sample_covered_flag));
    t->score_pct = 0;
    if (strokes == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        resample_stroke(t, &strokes[i], (uint8_t)i);
    }
    /* Per-stroke sample counts, cached for the incremental coverage
     * path (2026-11-21 review P1-2): add_point() needs the
     * denominators without rescanning. */
    for (uint16_t i = 0; i < t->sample_count; i++) {
        uint8_t s = t->sample_stroke[i];
        if (s < MUSE_TRACING_MAX_STROKES) {
            t->stroke_total[s]++;
        }
    }
    t->sample_total = t->sample_count;
}

/* Full rescan: the judging authority. Rebuilds the per-sample covered
 * flags from the stored points, then recomputes every percent from
 * them, so grade() cannot drift from the incremental path even if
 * one ever disagreed. Cost O(points x samples), called once per
 * grade (the pen has rested), never per point. */
static void refresh_coverage(muse_tracing_t *t)
{
    int count = 0;
    (void)muse_tracing_strokes(t->char_idx, &count);
    memset(t->sample_covered_flag, 0, sizeof(t->sample_covered_flag));
    memset(t->stroke_covered_count, 0, sizeof(t->stroke_covered_count));
    t->sample_covered = 0;
    uint16_t sample_total = 0;
    for (int s = 0; s < (int)MUSE_TRACING_MAX_STROKES; s++) {
        t->stroke_cover[s] = 0;
    }
    const int cover2 =
        MUSE_TRACING_COVER_DIST_PX * MUSE_TRACING_COVER_DIST_PX;
    for (int s = 0; s < count; s++) {
        uint16_t total = 0, covered = 0;
        for (uint16_t i = 0; i < t->sample_count; i++) {
            if (t->sample_stroke[i] != s) {
                continue;
            }
            total++;
            for (uint32_t p = 0; p < t->point_count; p++) {
                if (dist2(t->samples[i].x, t->samples[i].y,
                          t->points[p].x, t->points[p].y) <= cover2) {
                    t->sample_covered_flag[i >> 3] |=
                        (uint8_t)(1u << (i & 7));
                    covered++;
                    break;
                }
            }
        }
        if (total > 0) {
            t->stroke_cover[s] = (uint8_t)(covered * 100u / total);
            t->stroke_covered_count[s] = covered;
            t->sample_covered += covered;
            sample_total += total;
        }
    }
    /* Overall coverage at the last point, for grade(): the per-stroke
     * view above keeps the harness readable, this is what decides. */
    t->score_pct = sample_total > 0
                       ? (uint8_t)((uint32_t)t->sample_covered * 100u /
                                   sample_total)
                       : 0;
}

/* Incremental coverage for one stored point (2026-11-21 review P1-2):
 * coverage is monotone within an attempt (points only accumulate and
 * the sample set is fixed after set_char), so a new point only needs
 * to mark the samples it newly covers -- O(sample_count), one pass.
 * The per-stroke percents and the overall score follow from the
 * monotonically growing counters; grade() still runs the full rescan
 * (refresh_coverage) before judging, so the verdict authority is
 * unchanged. */
static void note_point_coverage(muse_tracing_t *t, int x, int y)
{
    if (t->sample_total == 0) {
        return;
    }
    const int cover2 =
        MUSE_TRACING_COVER_DIST_PX * MUSE_TRACING_COVER_DIST_PX;
    bool any = false;
    for (uint16_t i = 0; i < t->sample_count; i++) {
        if (t->sample_covered_flag[i >> 3] & (uint8_t)(1u << (i & 7))) {
            continue;   /* already covered by an earlier point */
        }
        if (dist2(t->samples[i].x, t->samples[i].y, x, y) > cover2) {
            continue;
        }
        t->sample_covered_flag[i >> 3] |= (uint8_t)(1u << (i & 7));
        uint8_t s = t->sample_stroke[i];
        if (s < MUSE_TRACING_MAX_STROKES) {
            t->stroke_covered_count[s]++;
            t->stroke_cover[s] = (uint8_t)(
                (uint32_t)t->stroke_covered_count[s] * 100u /
                t->stroke_total[s]);
        }
        t->sample_covered++;
        any = true;
    }
    if (!any) {
        return;
    }
    t->score_pct = (uint8_t)((uint32_t)t->sample_covered * 100u /
                             t->sample_total);
}

/* Grade the attempt: DONE when every stroke is covered, WRONG when the
 * stray share is over the line (and enough points are down to call it).
 * DONE wins over WRONG: a child who traced the whole glyph and then
 * scribbled has still traced it. Called from the UI after the pen has
 * rested (muse_tracing_grade), never per point -- judging mid-stroke was
 * calling 描错了 before the child finished (user report 2026-11-21). */
static void grade_inner(muse_tracing_t *t);

muse_tracing_phase_t muse_tracing_grade(muse_tracing_t *t)
{
    if (t == NULL) {
        return MUSE_TRACING_IDLE;
    }
    refresh_coverage(t);
    grade_inner(t);
    return t->phase;
}

static void grade_inner(muse_tracing_t *t)
{
    /* Overall coverage, not every stroke: a child who wrote the shape
     * with a stroke slightly off has still written it (user decision
     * 2026-11-21). >= 50 passes, >= 90 also earns the reward; the UI
     * reads score_pct to tell the two apart. */
    if (t->score_pct >= MUSE_TRACING_PASS_PCT) {
        t->phase = MUSE_TRACING_DONE;
        return;
    }
    if (t->point_count >= MUSE_TRACING_WRONG_MIN_POINTS &&
        t->stray_count * 100u >
            t->point_count * MUSE_TRACING_WRONG_STRAY_FRAC) {
        t->phase = MUSE_TRACING_WRONG;
        t->wrong_ms = MUSE_TRACING_WRONG_MS;
    }
}

muse_tracing_phase_t muse_tracing_add_point(muse_tracing_t *t, int x, int y)
{
    if (t == NULL || t->char_idx < 0 ||
        (t->phase != MUSE_TRACING_IDLE &&
         t->phase != MUSE_TRACING_TRACING)) {
        return t == NULL ? MUSE_TRACING_IDLE : t->phase;
    }
    if (x < 0) {
        x = 0;
    } else if (x >= MUSE_TRACING_FIELD) {
        x = MUSE_TRACING_FIELD - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= MUSE_TRACING_FIELD) {
        y = MUSE_TRACING_FIELD - 1;
    }
    /* Sampling: keep one point per MUSE_TRACING_SAMPLE_PX of travel.
     * The first point is always kept. */
    if (t->point_count > 0) {
        muse_tracing_point_t last = t->points[t->point_count - 1];
        if (dist2(x, y, last.x, last.y) <
            MUSE_TRACING_SAMPLE_PX * MUSE_TRACING_SAMPLE_PX) {
            return t->phase;
        }
    }
    if (t->point_count >= MUSE_TRACING_MAX_POINTS) {
        /* Buffer full: grade on what is stored and keep the phase;
         * dropping further points can only under-count coverage,
         * never over-count it. */
        return t->phase;
    }
    t->points[t->point_count].x = (int16_t)x;
    t->points[t->point_count].y = (int16_t)y;
    t->point_count++;
    if (t->phase == MUSE_TRACING_IDLE) {
        t->phase = MUSE_TRACING_TRACING;
    }
    /* Stray: farther than MUSE_TRACING_STRAY_DIST_PX from every
     * sample. A point on the glyph is never stray (the ghost's ink
     * is ~7 px half-width, the pen radius 5 px). */
    const int stray2 =
        MUSE_TRACING_STRAY_DIST_PX * MUSE_TRACING_STRAY_DIST_PX;
    bool stray = true;
    for (uint16_t i = 0; i < t->sample_count; i++) {
        if (dist2(x, y, t->samples[i].x, t->samples[i].y) <= stray2) {
            stray = false;
            break;
        }
    }
    if (stray) {
        t->stray_count++;
    }
    /* Coverage is kept current incrementally (note_point_coverage,
     * O(sample_count) per point -- 2026-11-21 review P1-2: the full
     * O(point_count x sample_count) rescan per point cost up to
     * ~160k distance checks on the late strokes, which showed up as
     * pen lag); grading waits for the UI's settle timer and runs the
     * full rescan once (see muse_tracing_grade). */
    note_point_coverage(t, x, y);
    return t->phase;
}

void muse_tracing_init(muse_tracing_t *t)
{
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));
    t->char_idx = -1;
    t->phase = MUSE_TRACING_IDLE;
}

bool muse_tracing_set_char(muse_tracing_t *t, int char_idx)
{
    if (t == NULL || char_idx < 0 ||
        char_idx >= (int)MUSE_TRACING_CHARS) {
        return false;
    }
    t->char_idx = char_idx;
    t->phase = MUSE_TRACING_IDLE;
    t->point_count = 0;
    t->stray_count = 0;
    t->sample_covered = 0;
    t->wrong_ms = 0;
    memset(t->stroke_cover, 0, sizeof(t->stroke_cover));
    rebuild_samples(t);
    return true;
}

void muse_tracing_reset(muse_tracing_t *t)
{
    if (t == NULL || t->char_idx < 0) {
        return;
    }
    t->phase = MUSE_TRACING_IDLE;
    t->point_count = 0;
    t->stray_count = 0;
    t->sample_covered = 0;
    t->wrong_ms = 0;
    memset(t->stroke_cover, 0, sizeof(t->stroke_cover));
    /* Samples are rebuilt, not kept: the covered flags live in the
     * per-sample bookkeeping below, and a fresh attempt starts from
     * zero coverage. */
    rebuild_samples(t);
}

void muse_tracing_tick(muse_tracing_t *t, uint32_t ms)
{
    if (t == NULL || t->phase != MUSE_TRACING_WRONG) {
        return;
    }
    if (ms >= t->wrong_ms) {
        /* Flash over: back to TRACING with the ink kept. The UI
         * clears the ink and calls reset() on the same frame, so
         * the kept ink is only a safety net. */
        t->wrong_ms = 0;
        t->phase = MUSE_TRACING_TRACING;
    } else {
        t->wrong_ms -= ms;
    }
}

const muse_tracing_stroke_t *muse_tracing_strokes(int char_idx, int *count)
{
    if (char_idx < 0 || char_idx >= (int)MUSE_TRACING_CHARS) {
        if (count != NULL) {
            *count = 0;
        }
        return NULL;
    }
    int first = muse_tracing_stroke_first[char_idx];
    int next = (char_idx + 1 < (int)MUSE_TRACING_CHARS)
                   ? muse_tracing_stroke_first[char_idx + 1]
                   : (int)(sizeof(muse_tracing_stroke_table) /
                           sizeof(muse_tracing_stroke_table[0]));
    if (count != NULL) {
        *count = next - first;
    }
    return &muse_tracing_stroke_table[first];
}

uint8_t muse_tracing_stroke_coverage(const muse_tracing_t *t, int stroke)
{
    if (t == NULL || stroke < 0 ||
        stroke >= (int)MUSE_TRACING_MAX_STROKES) {
        return 0;
    }
    return t->stroke_cover[stroke];
}
