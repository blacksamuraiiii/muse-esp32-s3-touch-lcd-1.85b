/* 描红写字 logic harness: checks muse_tracing.c without LVGL or
 * ESP-IDF. Run by tests/test_tracing_logic.py. Prints "ok" on
 * success; any failed check aborts via assert.
 *
 * The rules under test are the ones the user settled on 2026-11-20:
 *
 *   - one character at a time (30: three 期 of ten -- 天 地 人 日 月 山
 *     川 木 水 火 / 一 二 三 上 口 耳 目 手 田 禾 / 六 七 八 十 九 王
 *     午 下 个 去), graded against that character's reference strokes
 *     -- the skeleton centrelines of its 170 px Noto glyph, measured
 *     from the rendered pixels (muse_tracing.c's table);
 *   - grading is the overall coverage (covered samples / all
 *     samples): >= 50% passes, >= 90% earns the praise line and
 *     the 2-coin reward, 50..89 is "继续加油". No stroke-order
 *     check; per-stroke coverage is only a diagnostic, and the
 *     harness treats a stroke as covered at 60% (its own literal,
 *     HARNESS_STROKE_COVERED_PCT -- production defines no such
 *     threshold any more, M-8);
 *   - the pass/praise tiers straddle (M-10): the first prefix of a
 *     full trace of 天 reaching 50% is DONE without praise, the
 *     one point shorter is still TRACING; same at 90% for praise.
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

/* "Covered" for one stroke, harness-only: production grades on the
 * overall coverage (PASS_PCT 50 / PRAISE_PCT 90) and defines no
 * per-stroke threshold any more (MUSE_TRACING_COVER_FRAC was deleted
 * with the 1.5.4e review, M-8). The harness still needs a per-stroke
 * notion to assert "this stroke is done, that one is not", so it
 * keeps its own 60%. */
#define HARNESS_STROKE_COVERED_PCT 60u

static const uint32_t WANT_CODEPOINTS[MUSE_TRACING_CHARS] = {
    0x5929, 0x5730, 0x4EBA, 0x65E5, 0x6708,
    0x5C71, 0x5DDD, 0x6728, 0x6C34, 0x706B,
    0x4E00, 0x4E8C, 0x4E09, 0x4E0A, 0x53E3,
    0x8033, 0x76EE, 0x624B, 0x7530, 0x79BE,
    0x516D, 0x4E03, 0x516B, 0x5341, 0x4E5D,
    0x738B, 0x5348, 0x4E0B, 0x4E2A, 0x53BB,
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

/* Record (don't feed) the points trace_char() would feed for one
 * character, in the same order: 2 px steps along each reference
 * stroke's polyline. The buffer holds 4096; 天 feeds a few hundred. */
static int collect_char_points(int idx, muse_tracing_point_t *buf, int cap)
{
    int count = 0;
    const muse_tracing_stroke_t *strokes = muse_tracing_strokes(idx, &count);
    assert(strokes != NULL && count > 0);
    int n = 0;
    for (int i = 0; i < count; i++) {
        const muse_tracing_stroke_t *s = &strokes[i];
        for (int j = 0; j + 1 < s->n; j++) {
            int x0 = s->pts[j].x, y0 = s->pts[j].y;
            int x1 = s->pts[j + 1].x, y1 = s->pts[j + 1].y;
            int dx = x1 - x0, dy = y1 - y0;
            int steps = dx * dx + dy * dy;
            int m = 1;
            while (m * m * 4 < steps) {
                m++;
            }
            for (int k = 0; k <= m; k++) {
                assert(n < cap);
                buf[n].x = (int16_t)(x0 + dx * k / m);
                buf[n].y = (int16_t)(y0 + dy * k / m);
                n++;
            }
        }
    }
    return n;
}

/* Grade one prefix of a full trace: fresh attempt, first `n` fed
 * points, then grade(). Returns the score; *phase is the verdict. */
static uint8_t grade_prefix(muse_tracing_t *t,
                            const muse_tracing_point_t *pts, int n,
                            muse_tracing_phase_t *phase)
{
    assert(muse_tracing_set_char(t, 0));
    for (int i = 0; i < n; i++) {
        muse_tracing_add_point(t, pts[i].x, pts[i].y);
    }
    *phase = muse_tracing_grade(t);
    return t->score_pct;
}

int main(void)
{
    muse_tracing_t t;
    muse_tracing_init(&t);
    assert(t.char_idx == -1);
    assert(t.phase == MUSE_TRACING_IDLE);

    /* The character table: thirty entries, codepoints in list order. */
    assert(MUSE_TRACING_CHARS == 30);
    for (int i = 0; i < (int)MUSE_TRACING_CHARS; i++) {
        assert(MUSE_TRACING_TABLE[i].codepoint == WANT_CODEPOINTS[i]);
        assert(MUSE_TRACING_TABLE[i].utf8 != NULL &&
               strlen(MUSE_TRACING_TABLE[i].utf8) == 3);
        assert(MUSE_TRACING_TABLE[i].pinyin != NULL &&
               strlen(MUSE_TRACING_TABLE[i].pinyin) > 0);
    }

    /* Out-of-range characters are refused, state untouched. */
    assert(!muse_tracing_set_char(&t, -1));
    assert(!muse_tracing_set_char(&t, 30));
    assert(t.char_idx == -1);
    assert(muse_tracing_strokes(-1, NULL) == NULL);
    assert(muse_tracing_strokes(30, NULL) == NULL);
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
                   HARNESS_STROKE_COVERED_PCT);
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
           HARNESS_STROKE_COVERED_PCT);
    /* Stroke 1 shares the top horizontal's junction with stroke 0,
     * so its first sample is covered by stroke 0's end -- but the
     * stroke as a whole is far from covered. */
    assert(muse_tracing_stroke_coverage(&t, 1) <
           HARNESS_STROKE_COVERED_PCT);
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

    /* Pass/praise boundary straddle (1.5.4e review M-10): the two
     * tiers are MUSE_TRACING_PASS_PCT 50 and MUSE_TRACING_PRAISE_PCT
     * 90. Exact 49/50/89/90 scores are not directly constructible --
     * one stored point covers ~9 samples, so the score jumps in
     * steps of a few percent -- so the test straddles each tier
     * instead: replay prefixes of a full trace of 天, find the first
     * prefix that reaches the tier, and assert the decision flips
     * between that prefix and the one point shorter. Praise is
     * score_pct >= PRAISE_PCT (the UI reads score_pct to tell a
     * plain pass from a praise pass). */
    assert(MUSE_TRACING_PASS_PCT == 50u);
    assert(MUSE_TRACING_PRAISE_PCT == 90u);
    {
        static muse_tracing_point_t fed[4096];
        int total = collect_char_points(0, fed, 4096);
        assert(total > 0);
        /* A full trace ends at 100% (every sample covered). */
        muse_tracing_phase_t phase = MUSE_TRACING_IDLE;
        assert(grade_prefix(&t, fed, total, &phase) == 100);
        assert(phase == MUSE_TRACING_DONE);
        /* Find the first prefix reaching each tier. */
        int first_pass = -1, first_praise = -1;
        for (int n = 0; n <= total; n++) {
            uint8_t score = grade_prefix(&t, fed, n, &phase);
            if (first_pass < 0 && score >= MUSE_TRACING_PASS_PCT) {
                first_pass = n;
            }
            if (first_praise < 0 && score >= MUSE_TRACING_PRAISE_PCT) {
                first_praise = n;
            }
            if (first_pass >= 0 && first_praise >= 0) {
                break;
            }
        }
        assert(first_pass > 0 && first_praise > first_pass);
        /* Below 50: still tracing, no pass, no praise. */
        uint8_t below = grade_prefix(&t, fed, first_pass - 1, &phase);
        assert(below < MUSE_TRACING_PASS_PCT);
        assert(phase == MUSE_TRACING_TRACING);
        /* At 50: DONE, but below 90, so a plain pass, no praise. */
        uint8_t at = grade_prefix(&t, fed, first_pass, &phase);
        assert(at >= MUSE_TRACING_PASS_PCT);
        assert(at < MUSE_TRACING_PRAISE_PCT);
        assert(phase == MUSE_TRACING_DONE);
        /* Below 90: DONE, still no praise. */
        below = grade_prefix(&t, fed, first_praise - 1, &phase);
        assert(below >= MUSE_TRACING_PASS_PCT);
        assert(below < MUSE_TRACING_PRAISE_PCT);
        assert(phase == MUSE_TRACING_DONE);
        /* At 90: DONE with praise. */
        at = grade_prefix(&t, fed, first_praise, &phase);
        assert(at >= MUSE_TRACING_PRAISE_PCT);
        assert(phase == MUSE_TRACING_DONE);
    }

    printf("ok\n");
    return 0;
}
