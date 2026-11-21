/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_tracing.h -- 描红写字 (语文练习) logic (no LVGL, no ESP-IDF).
 *
 * One character at a time (user decision 2026-11-20): the child traces
 * the ghost of a first-grade character in a 228x228 田字格, and the
 * trace is graded against the character's reference strokes -- the
 * skeleton centrelines of its 170 px Noto Sans SC Medium glyph,
 * measured from the rendered pixels (see
 * ~/workspace/tracing-strokes-extract.py; the table lives in
 * muse_tracing.c). No stroke-order check (user decision): only the
 * shape counts.
 *
 * Grading, per attempt: the finger's points are sampled (one stored
 * point per MUSE_TRACING_SAMPLE_PX of travel) and compared with the
 * reference strokes resampled every MUSE_TRACING_SAMPLE_STEP_PX. A
 * stroke is covered when MUSE_TRACING_COVER_FRAC percent of its
 * samples have a traced point within MUSE_TRACING_COVER_DIST_PX.
 * The character is done when every stroke is covered. A traced point
 * farther than MUSE_TRACING_STRAY_DIST_PX from every sample is stray;
 * once MUSE_TRACING_WRONG_MIN_POINTS points are down and more than
 * MUSE_TRACING_WRONG_STRAY_FRAC percent of them are stray, the
 * attempt is wrong (描错标红重来, user decision 2026-11-20). The
 * wrong call only ever fires on genuinely off-glyph ink: a correct
 * trace has ~0 stray points, and a half-finished trace has too few
 * points to trip it. All distances are field pixels (the 228x228
 * grid); the thresholds are the tuning knobs for the real-board
 * pass, not constants of the scoring -- see the header note on each.
 *
 * Timing is in milliseconds and advances only through
 * muse_tracing_tick(), so the host test drives it deterministically
 * (no lv_tick_get() here). There is no clock in this game at all:
 * the tick only ages the WRONG flash (the UI clears the ink after
 * MUSE_TRACING_WRONG_MS); grading itself happens on every point.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The ten characters, in list order (user decision 2026-11-20: the
 * 人教版一年级上册 "天地人" "日月水火" "山川" three lessons' own
 * characters). The order is the list order and the "第 N/10 个字"
 * number; the reward mask (muse_pet_model.h) uses the same index. */
#define MUSE_TRACING_CHARS 10u
/* Longest reference polyline, points (measured: 月's outer box has
 * 8; the table in muse_tracing.c must stay within this). */
#define MUSE_TRACING_STROKE_MAX_PTS 8u
/* Stored finger points per attempt (sampled, see below). A full
 * trace of the longest character is ~600 px of travel, so 512
 * points at 6 px spacing is ~5x headroom; past it, points are
 * dropped (the attempt keeps grading on what it has). */
#define MUSE_TRACING_MAX_POINTS 512u
/* Field geometry: the trace grid is 228x228 (mockup v1), and the
 * reference strokes are in field coordinates (0..227). The UI adds
 * the grid's page offset (66,66) when it draws. */
#define MUSE_TRACING_FIELD 228
/* How long the wrong flash lasts before the UI clears the ink. */
#define MUSE_TRACING_WRONG_MS 900u
/* Minimum travel between stored points: the touch layer reports far
 * denser points than the scoring needs, and a stored point per pixel
 * would make the stray fraction noisy. 6 px is ~half the pen width. */
#define MUSE_TRACING_SAMPLE_PX 6
/* Reference strokes are resampled to one sample per 4 px for the
 * coverage test. */
#define MUSE_TRACING_SAMPLE_STEP_PX 4
/* A traced point within this distance of a sample covers it. 16 px
 * is the pen radius (5) plus the ghost stroke's half-width at
 * 170 px (~7) plus a child's wobble (~4). */
#define MUSE_TRACING_COVER_DIST_PX 16
/* Percent of a stroke's samples that must be covered for that
 * stroke to count (per-stroke view, kept for the harness). */
#define MUSE_TRACING_COVER_FRAC 60u
/* Overall coverage (covered samples / all samples) decides the result
 * (user decision 2026-11-21): >= PASS_PCT passes the attempt, and only
 * >= PRAISE_PCT earns the 2-coin reward and the praise line. 50..89 is
 * "继续加油" -- the character counts as written, but pays nothing. Below
 * PASS_PCT the attempt is not finished (a scribble still bounces off the
 * stray rule). */
#define MUSE_TRACING_PASS_PCT 50u
#define MUSE_TRACING_PRAISE_PCT 90u
/* A traced point farther than this from every sample is stray.
 * 28 px is well outside the ghost's ink (half-width ~7) plus pen
 * radius, so a point on the glyph never counts as stray. */
#define MUSE_TRACING_STRAY_DIST_PX 28
/* A wrong call needs this many points first: a single stray tap
 * (or the first wobbly millimetres of a real stroke) must never
 * flash red. */
#define MUSE_TRACING_WRONG_MIN_POINTS 40u
/* Stray share that calls an attempt wrong, once the minimum above
 * is met. A correct trace is ~0% stray; a scribble across the grid
 * is mostly stray. */
#define MUSE_TRACING_WRONG_STRAY_FRAC 35u

/* Most reference strokes of any one character (measured: 天 has 9). */
#define MUSE_TRACING_MAX_STROKES 9u
/* Resampled reference samples for one character: the longest
 * character's strokes total ~600 px of arc, so 320 samples at a 4 px
 * step is ~2x headroom. */
#define MUSE_TRACING_MAX_SAMPLES 320u

typedef struct {
    int16_t x, y; /* field pixels */
} muse_tracing_point_t;

typedef struct {
    uint8_t n; /* points in this stroke, 2..MUSE_TRACING_STROKE_MAX_PTS */
    muse_tracing_point_t pts[MUSE_TRACING_STROKE_MAX_PTS];
} muse_tracing_stroke_t;

/* One character: what the list page and the trace page show. `utf8`
 * is the single character (3 bytes + NUL); `pinyin` carries the tone
 * mark (Noto draws ā á ǎ à ē é ě è ō ó ǒ ò ū ú ǔ ù ǖ ǘ ǚ ǜ;
 * Montserrat has none of them, so the pinyin line must use the CJK
 * face -- see muse_tracing_ui.c). */
typedef struct {
    const char *utf8;
    const char *pinyin;
    uint32_t codepoint;
} muse_tracing_char_t;

extern const muse_tracing_char_t MUSE_TRACING_TABLE[MUSE_TRACING_CHARS];

typedef enum {
    MUSE_TRACING_IDLE = 0, /* no points yet this attempt */
    MUSE_TRACING_TRACING,  /* points down, not finished */
    MUSE_TRACING_DONE,    /* every stroke covered */
    MUSE_TRACING_WRONG,   /* stray share over the line */
} muse_tracing_phase_t;

typedef struct {
    int char_idx; /* 0..MUSE_TRACING_CHARS-1, -1 = none */
    muse_tracing_phase_t phase;
    /* Stored finger points (field coords), sampled at
     * MUSE_TRACING_SAMPLE_PX spacing. */
    muse_tracing_point_t points[MUSE_TRACING_MAX_POINTS];
    uint32_t point_count;
    uint32_t stray_count;
    /* Resampled reference samples for the current character, built
     * by set_char (at most MUSE_TRACING_MAX_SAMPLES). */
    muse_tracing_point_t samples[MUSE_TRACING_MAX_SAMPLES];
    uint8_t sample_stroke[MUSE_TRACING_MAX_SAMPLES]; /* which stroke each sample belongs to */
    uint16_t sample_count;
    uint16_t sample_covered; /* samples with a traced point near them */
    uint8_t score_pct;       /* overall coverage at the last grade, 0..100 */
    /* Per-stroke coverage, percent, for the UI and the harness. */
    uint8_t stroke_cover[MUSE_TRACING_MAX_STROKES];
    uint32_t wrong_ms; /* ms left of the wrong flash, 0 = not flashing */
} muse_tracing_t;

void muse_tracing_init(muse_tracing_t *t);

/* Pick a character and start a fresh attempt. No-op (returns false)
 * for an out-of-range index; the current attempt is left untouched. */
bool muse_tracing_set_char(muse_tracing_t *t, int char_idx);

/* Add one finger point (field coords, 0..227). Points outside the
 * field are clamped, not dropped: a fingertip on the grid's edge is
 * still a trace attempt. A point that moved less than
 * MUSE_TRACING_SAMPLE_PX from the last stored point is not stored
 * (but it still counts for nothing else). No-op unless IDLE or
 * TRACING; returns the phase after the point. */
/* Grade the attempt: DONE when overall coverage reaches
 * MUSE_TRACING_PASS_PCT (score_pct says how much: >= PRAISE_PCT earns
 * the reward, 50..89 is a pass without one), WRONG when too many points
 * are stray (and MUSE_TRACING_WRONG_MIN_POINTS are down). add_point() deliberately does
 * NOT call this any more: the UI grades once the pen has rested for two
 * seconds, so a stroke in progress is never judged (user decision
 * 2026-11-21). Returns the resulting phase. */
muse_tracing_phase_t muse_tracing_grade(muse_tracing_t *t);

muse_tracing_phase_t muse_tracing_add_point(muse_tracing_t *t, int x, int y);

/* Clear the attempt (same character, fresh ink). The phase returns
 * to IDLE; the wrong flash, if any, is cancelled. */
void muse_tracing_reset(muse_tracing_t *t);

/* Advance the wrong-flash clock by `ms`. No-op unless WRONG. When
 * the flash expires the phase returns to TRACING with the ink kept
 * (the UI clears the ink and calls reset() on the same frame). */
void muse_tracing_tick(muse_tracing_t *t, uint32_t ms);

/* Reference strokes for one character (read-only, for the harness
 * and the UI's debug path). Returns NULL for an out-of-range index. */
const muse_tracing_stroke_t *muse_tracing_strokes(int char_idx, int *count);

/* Percent of one stroke's samples covered (0..100), for the UI's
 * per-stroke debug and the harness. Out-of-range stroke: 0. */
uint8_t muse_tracing_stroke_coverage(const muse_tracing_t *t, int stroke);
