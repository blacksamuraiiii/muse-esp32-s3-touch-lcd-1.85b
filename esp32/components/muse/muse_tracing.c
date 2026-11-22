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
 * field pixels (0..227).
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
};

static const muse_tracing_stroke_t muse_tracing_stroke_table[] = {
    /* 天 */
    { 6, { {54,49}, {111,49}, {113,111}, {154,162}, {176,174}, {187,172} } },
    { 2, { {172,49}, {112,49} } },
    { 2, { {48,101}, {110,103} } },
    { 2, { {179,101}, {114,104} } },
    { 2, { {41,173}, {51,174} } },
    { 2, { {112,111}, {104,121} } },
    { 2, { {104,123}, {98,134} } },
    { 3, { {97,136}, {82,155}, {64,168} } },
    { 2, { {137,146}, {143,152} } },
    /* 地 */
    { 3, { {143,44}, {142,84}, {159,81} } },
    { 3, { {63,46}, {63,83}, {82,84} } },
    { 3, { {108,60}, {108,98}, {126,95} } },
    { 4, { {43,84}, {63,85}, {63,145}, {79,142} } },
    { 7, { {92,112}, {108,108}, {108,166}, {114,181}, {174,183}, {181,179}, {185,169} } },
    { 4, { {164,142}, {176,136}, {177,79}, {161,81} } },
    { 3, { {142,148}, {142,94}, {128,95} } },
    { 2, { {63,147}, {56,153} } },
    /* 人 */
    { 6, { {112,45}, {112,93}, {121,99}, {144,148}, {167,172}, {180,178} } },
    { 2, { {39,179}, {52,178} } },
    { 2, { {111,93}, {104,102} } },
    { 2, { {104,104}, {98,119} } },
    { 2, { {95,126}, {85,145} } },
    { 2, { {84,147}, {54,177} } },
    { 2, { {149,155}, {163,169} } },
    /* 日 */
    { 6, { {162,176}, {161,167}, {65,166}, {65,49}, {161,48}, {162,167} } },
    { 2, { {65,177}, {65,167} } },
    { 2, { {66,106}, {161,106} } },
    /* 月 */
    { 8, { {119,172}, {131,179}, {159,174}, {161,128}, {70,127}, {71,46}, {160,46}, {161,127} } },
    { 2, { {45,176}, {56,162} } },
    { 2, { {71,86}, {160,86} } },
    { 2, { {67,130}, {60,152} } },
    /* 山 */
    { 4, { {113,45}, {113,170}, {172,171}, {173,182} } },
    { 3, { {54,78}, {54,170}, {112,171} } },
    { 2, { {173,78}, {173,171} } },
    /* 川 */
    { 3, { {63,47}, {63,112}, {54,157} } },
    { 2, { {171,48}, {171,177} } },
    { 2, { {116,53}, {116,162} } },
    { 2, { {46,175}, {53,162} } },
    /* 木 */
    { 4, { {113,43}, {114,91}, {123,83}, {180,83} } },
    { 5, { {47,83}, {104,83}, {173,158}, {180,163}, {188,161} } },
    { 2, { {46,163}, {88,122} } },
    { 4, { {113,184}, {113,98}, {98,106}, {89,120} } },
    { 2, { {151,137}, {161,147} } },
    /* 水 */
    { 7, { {114,42}, {116,98}, {135,109}, {140,125}, {166,154}, {179,163}, {191,161} } },
    { 2, { {173,76}, {136,108} } },
    { 3, { {48,85}, {87,87}, {78,120} } },
    { 3, { {42,164}, {48,165}, {67,143} } },
    { 4, { {93,184}, {107,183}, {113,178}, {117,100} } },
    /* 火 */
    { 7, { {113,45}, {110,118}, {120,125}, {132,147}, {153,168}, {175,179}, {184,177} } },
    { 2, { {40,178}, {54,179} } },
    { 2, { {67,82}, {57,108} } },
    { 2, { {171,83}, {167,91} } },
    { 2, { {165,97}, {159,107} } },
    { 2, { {100,134}, {94,144} } },
    { 2, { {93,146}, {69,171} } },
};

/* First stroke index per character, in MUSE_TRACING_CHARS
 * order; the count is the next entry minus this one. */
static const uint8_t muse_tracing_stroke_first[] = {
    0, 9, 17, 24, 27, 31, 34, 38, 43, 48,
};
/* Total strokes: 55. */

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
