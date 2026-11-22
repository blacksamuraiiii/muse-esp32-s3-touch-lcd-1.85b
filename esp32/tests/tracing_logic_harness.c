/* 描红写字 logic harness: checks muse_tracing.c without LVGL or
 * ESP-IDF. Run by tests/test_tracing_logic.py. Prints "ok" on
 * success; any failed check aborts via assert.
 *
 * The rules under test are the ones the user settled on 2026-11-20:
 *
 *   - one character at a time (10: 天 地 人 日 月 山 川 木 水 火),
 *     graded against that character's reference strokes -- the
 *     skeleton centrelines of its 170 px Noto glyph, measured from
 *     the rendered pixels (muse_tracing.c's table);
 *   - a stroke is covered when 70% of its resampled points have a
 *     traced point within 16 px; the character is done when every
 *     stroke is covered. No stroke-order check;
 *   - a traced point farther than 28 px from every sample is stray;
 *     once 40 points are down and more than 35% of them are stray,
 *     the attempt is wrong (描错标红重来);
 *   - the wrong call needs its 40-point minimum: 39 stray points
 *     are still TRACING, the 40th calls WRONG;
 *   - a wobbly start does not doom a correct trace: 5 stray points
 *     first, then a full trace, still ends DONE (the stray share
 *     stays under 35% throughout, so WRONG never fires);
 *   - the wrong flash lasts 900 ms (muse_tracing_tick), then the
 *     phase returns to TRACING; reset() starts a fresh attempt.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "muse_tracing.h"

static const uint32_t WANT_CODEPOINTS[MUSE_TRACING_CHARS] = {
    0x5929, 0x5730, 0x4EBA, 0x65E5, 0x6708,
    0x5C71, 0x5DDD, 0x6728, 0x6C34, 0x706B,
};

/* Feed points along one segment in 2 px steps (the logic samples at
 * 6 px, so every fed point is a candidate for storage). */
static void feed_segment(muse_tracing_t *t, int x0, int y0, int x1,
                         int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = dx * dx + dy * dy;
    int n = 1;
    while (n * n * 4 < steps) {
        n++;
    }
    for (int k = 0; k <= n; k++) {
        muse_tracing_add_point(t, x0 + dx * k / n, y0 + dy * k / n);
    }
}

/* Trace one reference stroke exactly (its own polyline). */
static void trace_stroke(muse_tracing_t *t,
                         const muse_tracing_stroke_t *s)
{
    for (int i = 0; i + 1 < s->n; i++) {
        feed_segment(t, s->pts[i].x, s->pts[i].y, s->pts[i + 1].x,
                     s->pts[i + 1].y);
    }
}

/* Trace a whole character along its reference strokes. */
static void trace_char(muse_tracing_t *t, int idx)
{
    int count = 0;
    const muse_tracing_stroke_t *strokes = muse_tracing_strokes(idx, &count);
    assert(strokes != NULL && count > 0);
    for (int i = 0; i < count; i++) {
        trace_stroke(t, &strokes[i]);
    }
}

/* Scribble two rows across the top of the field, clear of every
 * glyph (the highest sample of any character is y=42): every point
 * is stray. */
static void scribble_top(muse_tracing_t *t, int points)
{
    int fed = 0;
    for (int y = 8; y <= 14 && fed < points; y += 6) {
        /* x stops at 222, not 228: the field is 0..227, so a fed
         * 228 is clamped to 227 -- 5 px from the 222 point -- and
         * the 6 px sampler drops it. Feeding to 222 keeps every fed
         * point stored, so `points` is the stored count. */
        for (int x = 0; x < MUSE_TRACING_FIELD && fed < points;
             x += 6) {
            muse_tracing_add_point(t, x, y);
            fed++;
        }
    }
}

int main(void)
{
    muse_tracing_t t;
    muse_tracing_init(&t);
    assert(t.char_idx == -1);
    assert(t.phase == MUSE_TRACING_IDLE);

    /* The character table: ten entries, codepoints in list order. */
    assert(MUSE_TRACING_CHARS == 10);
    for (int i = 0; i < (int)MUSE_TRACING_CHARS; i++) {
        assert(MUSE_TRACING_TABLE[i].codepoint == WANT_CODEPOINTS[i]);
        assert(MUSE_TRACING_TABLE[i].utf8 != NULL &&
               strlen(MUSE_TRACING_TABLE[i].utf8) == 3);
        assert(MUSE_TRACING_TABLE[i].pinyin != NULL &&
               strlen(MUSE_TRACING_TABLE[i].pinyin) > 0);
    }

    /* Out-of-range characters are refused, state untouched. */
    assert(!muse_tracing_set_char(&t, -1));
    assert(!muse_tracing_set_char(&t, 10));
    assert(t.char_idx == -1);
    assert(muse_tracing_strokes(-1, NULL) == NULL);
    assert(muse_tracing_strokes(10, NULL) == NULL);
    assert(muse_tracing_stroke_coverage(&t, -1) == 0);
    assert(muse_tracing_stroke_coverage(&t, 99) == 0);

    /* Every character traces to DONE along its own reference strokes,
     * with every stroke covered. */
    for (int idx = 0; idx < (int)MUSE_TRACING_CHARS; idx++) {
        assert(muse_tracing_set_char(&t, idx));
        assert(t.phase == MUSE_TRACING_IDLE);
        assert(t.point_count == 0);
        trace_char(&t, idx);
        /* Grading is explicit now: the UI calls it once the pen has
         * rested two seconds, so a stroke in progress is never judged. */
        assert(muse_tracing_grade(&t) == MUSE_TRACING_DONE);
        assert(t.phase == MUSE_TRACING_DONE);
        int count = 0;
        (void)muse_tracing_strokes(idx, &count);
        for (int s = 0; s < count; s++) {
            assert(muse_tracing_stroke_coverage(&t, s) >=
                   MUSE_TRACING_COVER_FRAC);
        }
        /* DONE is terminal for the attempt: further points are
         * ignored, not graded. */
        muse_tracing_phase_t before = t.phase;
        muse_tracing_add_point(&t, 0, 0);
        assert(t.phase == before);
    }

    /* A half trace is still tracing: the first stroke of 天 is
     * covered, the rest are not, and the phase is neither DONE nor
     * WRONG. */
    assert(muse_tracing_set_char(&t, 0));
    int count = 0;
    const muse_tracing_stroke_t *strokes = muse_tracing_strokes(0, &count);
    assert(strokes != NULL && count == 9);
    trace_stroke(&t, &strokes[0]);
    assert(t.phase == MUSE_TRACING_TRACING);
    assert(muse_tracing_stroke_coverage(&t, 0) >=
           MUSE_TRACING_COVER_FRAC);
    /* Stroke 1 shares the top horizontal's junction with stroke 0,
     * so its first sample is covered by stroke 0's end -- but the
     * stroke as a whole is far from covered. */
    assert(muse_tracing_stroke_coverage(&t, 1) <
           MUSE_TRACING_COVER_FRAC);
    /* Incremental == full rescan (2026-11-21 review P1-2): grade()
     * rebuilds the coverage from scratch, so the state it leaves must
     * equal the incremental state the mid-trace reads showed. Any
     * drift means the O(sample_count) per-point path disagrees with
     * the judging authority. The verdict itself may legitimately be
     * DONE here: stroke 0 is the long one and its 16 px cover radius
     * reaches into the neighbours, so a fully traced first stroke can
     * already sit at MUSE_TRACING_PASS_PCT. */
    {
        uint8_t mid_cover[MUSE_TRACING_MAX_STROKES];
        memcpy(mid_cover, t.stroke_cover, sizeof(mid_cover));
        uint8_t mid_score = t.score_pct;
        uint16_t mid_covered = t.sample_covered;
        uint8_t mid_flags[sizeof(t.sample_covered_flag)];
        memcpy(mid_flags, t.sample_covered_flag, sizeof(mid_flags));
        (void)muse_tracing_grade(&t);
        assert(memcmp(mid_cover, t.stroke_cover, sizeof(mid_cover)) == 0);
        assert(mid_score == t.score_pct);
        assert(mid_covered == t.sample_covered);
        assert(memcmp(mid_flags, t.sample_covered_flag,
                      sizeof(mid_flags)) == 0);
        /* And the verdict from the full rescan agrees with the
         * incremental score the mid-trace already computed. */
        if (t.phase == MUSE_TRACING_DONE) {
            assert(mid_score >= MUSE_TRACING_PASS_PCT);
        } else {
            assert(t.phase == MUSE_TRACING_TRACING);
            assert(mid_score < MUSE_TRACING_PASS_PCT);
        }
    }

    /* A scribble far from the glyph is wrong: 40+ points, almost all
     * stray. The flash ages out with the tick and the phase returns
     * to TRACING; reset() starts a fresh attempt. */
    assert(muse_tracing_set_char(&t, 0));
    scribble_top(&t, 60);
    assert(muse_tracing_grade(&t) == MUSE_TRACING_WRONG);
    assert(t.phase == MUSE_TRACING_WRONG);
    assert(t.wrong_ms == MUSE_TRACING_WRONG_MS);
    assert(t.stray_count * 100u > t.point_count * 35u);
    muse_tracing_tick(&t, MUSE_TRACING_WRONG_MS - 1);
    assert(t.phase == MUSE_TRACING_WRONG);
    muse_tracing_tick(&t, 1);
    assert(t.phase == MUSE_TRACING_TRACING);
    assert(t.wrong_ms == 0);
    /* Points during the flash are ignored (the UI freezes the ink). */
    assert(muse_tracing_set_char(&t, 0));
    scribble_top(&t, 60);
    assert(muse_tracing_grade(&t) == MUSE_TRACING_WRONG);
    assert(t.phase == MUSE_TRACING_WRONG);
    uint32_t points_before = t.point_count;
    muse_tracing_add_point(&t, 114, 114);
    assert(t.point_count == points_before);
    muse_tracing_reset(&t);
    assert(t.phase == MUSE_TRACING_IDLE);
    assert(t.point_count == 0 && t.stray_count == 0);

    /* The wrong call needs its 40-point minimum: 39 stray points
     * are still TRACING, the 40th calls WRONG. */
    assert(muse_tracing_set_char(&t, 0));
    scribble_top(&t, 39);
    assert(muse_tracing_grade(&t) == MUSE_TRACING_TRACING);
    assert(t.phase == MUSE_TRACING_TRACING);
    scribble_top(&t, 1);
    assert(muse_tracing_grade(&t) == MUSE_TRACING_WRONG);
    assert(t.phase == MUSE_TRACING_WRONG);

    /* A wobbly start does not doom a correct trace: 5 stray points
     * first, then a full trace -- the stray share stays under 35%
     * throughout, so WRONG never fires and the attempt ends DONE. */
    assert(muse_tracing_set_char(&t, 0));
    scribble_top(&t, 5);
    assert(t.phase == MUSE_TRACING_TRACING);
    trace_char(&t, 0);
    assert(muse_tracing_grade(&t) == MUSE_TRACING_DONE);
    assert(t.phase == MUSE_TRACING_DONE);

    /* A few stray points never call an attempt wrong on their own. */
    assert(muse_tracing_set_char(&t, 0));
    scribble_top(&t, 10);
    assert(muse_tracing_grade(&t) == MUSE_TRACING_TRACING);
    assert(t.phase == MUSE_TRACING_TRACING);

    printf("ok\n");
    return 0;
}
