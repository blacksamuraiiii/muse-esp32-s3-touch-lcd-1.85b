/* Host harness for the 掷圣杯 logic: muse_jiaobei.c is pure logic (no
 * LVGL, no ESP-IDF), so it builds with plain cc on the host (same
 * pattern as test_tictactoe.py). Run by tests/test_jiaobei.py. Prints
 * "ok" on success; any failed check aborts via assert.
 *
 * What it pins down (user decision 2026-11-14):
 *   - Outcome distribution on two fair cups: holy 50% (random % 4 is
 *     0 or 1), laughing 25% (2), yin 25% (3).
 *   - Faces: holy is left round + right flat, laughing both flat, yin
 *     both round (mockup v12).
 *   - The 300 ms in-place flip: faces swap at the midpoint, the scale
 *     squeezes 256 -> 0 -> 256, the result lands at the end; a tap
 *     mid-flip never starts a second toss.
 *   - The consecutive-holy streak: holy +1, laughing / yin clear it,
 *     reset() starts it at 0. Re-entering the page restores it:
 *     muse_jiaobei_restore() rebuilds the settled board (result faces
 *     + persisted streak) from the snapshot, or the ready board when
 *     nothing was tossed today -- the streak survives leaving the page
 *     within the same day (user decision 2026-11-14).
 *   - Shake-to-toss (user decision 2026-11-14): the acceleration
 *     magnitude's deviation from 1g must stay above threshold for
 *     250 ms (five 50 ms samples) to fire once; a single spike, a
 *     gentle sway and rest never fire; the 1 s cooldown after a
 *     trigger swallows further shaking.
 *   - A refused toss restores the pre-toss board exactly (result,
 *     faces and streak), so a stale-snapshot PET_POOR / PET_CAP never
 *     erases a streak the player already earned.
 *   - The limit prompt's copy (mockup v21): the exact string the UI
 *     shows when the 9 daily tosses are used up.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "muse_jiaobei.h"

static void tick_to(muse_jiaobei_t *g, uint32_t ms)
{
    while (ms > 0) {
        uint32_t step = ms > 50 ? 50 : ms;
        muse_jiaobei_tick(g, step);
        ms -= step;
    }
}

int main(void)
{
    /* --- the limit prompt's copy (mockup v21) -------------------------- */
    assert(strcmp(MUSE_JIAOBEI_LIMIT_TEXT, "今日掷筊次数已用完") == 0);

    /* --- initial state -------------------------------------------------- */
    muse_jiaobei_t g;
    muse_jiaobei_init(&g);
    assert(g.phase == MUSE_JIAOBEI_READY);
    assert(g.result == MUSE_JIAOBEI_NONE);
    assert(g.left == MUSE_JIAOBEI_FLAT);
    assert(g.right == MUSE_JIAOBEI_FLAT);
    assert(g.streak == 0);
    assert(muse_jiaobei_flip_scale(&g) == 256);

    /* --- outcome mapping ------------------------------------------------ */
    assert(muse_jiaobei_result_from_random(0) == MUSE_JIAOBEI_SHENG);
    assert(muse_jiaobei_result_from_random(1) == MUSE_JIAOBEI_SHENG);
    assert(muse_jiaobei_result_from_random(2) == MUSE_JIAOBEI_XIAO);
    assert(muse_jiaobei_result_from_random(3) == MUSE_JIAOBEI_YIN);
    assert(muse_jiaobei_result_from_random(4) == MUSE_JIAOBEI_SHENG);
    assert(muse_jiaobei_result_from_random(7) == MUSE_JIAOBEI_YIN);
    /* Distribution over 400 draws: 200 holy / 100 laughing / 100 yin. */
    uint32_t sheng = 0, xiao = 0, yin = 0;
    for (uint32_t r = 0; r < 400; r++) {
        switch (muse_jiaobei_result_from_random(r)) {
        case MUSE_JIAOBEI_SHENG: sheng++; break;
        case MUSE_JIAOBEI_XIAO: xiao++; break;
        case MUSE_JIAOBEI_YIN: yin++; break;
        default: assert(false); break;
        }
    }
    assert(sheng == 200 && xiao == 100 && yin == 100);

    /* --- faces ----------------------------------------------------------- */
    muse_jiaobei_face_t l, r;
    muse_jiaobei_result_faces(MUSE_JIAOBEI_SHENG, &l, &r);
    assert(l == MUSE_JIAOBEI_ROUND && r == MUSE_JIAOBEI_FLAT);
    muse_jiaobei_result_faces(MUSE_JIAOBEI_XIAO, &l, &r);
    assert(l == MUSE_JIAOBEI_FLAT && r == MUSE_JIAOBEI_FLAT);
    muse_jiaobei_result_faces(MUSE_JIAOBEI_YIN, &l, &r);
    assert(l == MUSE_JIAOBEI_ROUND && r == MUSE_JIAOBEI_ROUND);

    /* --- one holy toss: flip, midpoint swap, result --------------------- */
    muse_jiaobei_toss(&g, 0); /* holy */
    assert(g.phase == MUSE_JIAOBEI_FLIPPING);
    assert(g.result == MUSE_JIAOBEI_SHENG);
    assert(g.streak == 1); /* decided at toss time, shown at RESULT */
    assert(g.left == MUSE_JIAOBEI_FLAT && g.right == MUSE_JIAOBEI_FLAT);
    assert(muse_jiaobei_flip_scale(&g) == 256);
    muse_jiaobei_tick(&g, 75);
    assert(muse_jiaobei_flip_scale(&g) == 128);
    assert(!g.face_swapped);
    muse_jiaobei_tick(&g, 75); /* 150 ms: the midpoint */
    assert(g.face_swapped);
    assert(g.left == MUSE_JIAOBEI_ROUND && g.right == MUSE_JIAOBEI_FLAT);
    assert(muse_jiaobei_flip_scale(&g) == 0);
    /* A tap mid-flip never starts a second toss. */
    muse_jiaobei_toss(&g, 3);
    assert(g.result == MUSE_JIAOBEI_SHENG);
    assert(g.streak == 1);
    muse_jiaobei_tick(&g, 75);
    assert(muse_jiaobei_flip_scale(&g) == 128);
    muse_jiaobei_tick(&g, 75); /* 300 ms: the result lands */
    assert(g.phase == MUSE_JIAOBEI_RESULT);
    assert(muse_jiaobei_flip_scale(&g) == 256);
    assert(g.left == MUSE_JIAOBEI_ROUND && g.right == MUSE_JIAOBEI_FLAT);

    /* --- the streak: holy +1, laughing / yin clear ---------------------- */
    muse_jiaobei_toss(&g, 1); /* holy again */
    tick_to(&g, MUSE_JIAOBEI_FLIP_MS);
    assert(g.phase == MUSE_JIAOBEI_RESULT && g.streak == 2);
    muse_jiaobei_toss(&g, 2); /* laughing */
    tick_to(&g, MUSE_JIAOBEI_FLIP_MS);
    assert(g.result == MUSE_JIAOBEI_XIAO && g.streak == 0);
    assert(g.left == MUSE_JIAOBEI_FLAT && g.right == MUSE_JIAOBEI_FLAT);
    muse_jiaobei_toss(&g, 0); /* holy */
    tick_to(&g, MUSE_JIAOBEI_FLIP_MS);
    assert(g.streak == 1);
    muse_jiaobei_toss(&g, 3); /* yin */
    tick_to(&g, MUSE_JIAOBEI_FLIP_MS);
    assert(g.result == MUSE_JIAOBEI_YIN && g.streak == 0);
    assert(g.left == MUSE_JIAOBEI_ROUND && g.right == MUSE_JIAOBEI_ROUND);

    /* --- tick is a no-op outside FLIPPING ------------------------------- */
    muse_jiaobei_tick(&g, 500);
    assert(g.phase == MUSE_JIAOBEI_RESULT && g.result == MUSE_JIAOBEI_YIN);

    /* --- a refused toss restores the pre-toss board exactly ------------- */
    muse_jiaobei_reset(&g);
    muse_jiaobei_toss(&g, 0); /* holy */
    tick_to(&g, MUSE_JIAOBEI_FLIP_MS);
    assert(g.phase == MUSE_JIAOBEI_RESULT && g.streak == 1);
    muse_jiaobei_toss(&g, 2); /* laughing: streak cleared, flip starts */
    assert(g.phase == MUSE_JIAOBEI_FLIPPING && g.streak == 0);
    muse_jiaobei_cancel(&g); /* the worker refused: never happened */
    assert(g.phase == MUSE_JIAOBEI_RESULT);
    assert(g.result == MUSE_JIAOBEI_SHENG);
    assert(g.streak == 1);
    assert(g.left == MUSE_JIAOBEI_ROUND && g.right == MUSE_JIAOBEI_FLAT);
    /* Cancel is a no-op outside FLIPPING. */
    muse_jiaobei_cancel(&g);
    assert(g.phase == MUSE_JIAOBEI_RESULT && g.streak == 1);

    /* --- reset: the fresh-board primitive starts the streak at 0 ------ */
    muse_jiaobei_reset(&g);
    assert(g.phase == MUSE_JIAOBEI_READY);
    assert(g.result == MUSE_JIAOBEI_NONE);
    assert(g.streak == 0);
    assert(g.left == MUSE_JIAOBEI_FLAT && g.right == MUSE_JIAOBEI_FLAT);

    /* --- restore: re-entering the page keeps today's board ------------- */
    /* Nothing tossed today: the ready board, both cups flat. */
    muse_jiaobei_restore(&g, MUSE_JIAOBEI_NONE, 0);
    assert(g.phase == MUSE_JIAOBEI_READY);
    assert(g.result == MUSE_JIAOBEI_NONE);
    assert(g.streak == 0);
    assert(g.left == MUSE_JIAOBEI_FLAT && g.right == MUSE_JIAOBEI_FLAT);
    assert(muse_jiaobei_flip_scale(&g) == 256);
    /* Two holy in a row, left and re-entered: the result board stands
     * with the streak intact. */
    muse_jiaobei_restore(&g, MUSE_JIAOBEI_SHENG, 2);
    assert(g.phase == MUSE_JIAOBEI_RESULT);
    assert(g.result == MUSE_JIAOBEI_SHENG);
    assert(g.streak == 2);
    assert(g.left == MUSE_JIAOBEI_ROUND && g.right == MUSE_JIAOBEI_FLAT);
    assert(muse_jiaobei_flip_scale(&g) == 256);
    /* A laughing result restores both flat with the streak cleared. */
    muse_jiaobei_restore(&g, MUSE_JIAOBEI_XIAO, 0);
    assert(g.phase == MUSE_JIAOBEI_RESULT);
    assert(g.result == MUSE_JIAOBEI_XIAO);
    assert(g.streak == 0);
    assert(g.left == MUSE_JIAOBEI_FLAT && g.right == MUSE_JIAOBEI_FLAT);
    /* A yin result restores both round. */
    muse_jiaobei_restore(&g, MUSE_JIAOBEI_YIN, 0);
    assert(g.result == MUSE_JIAOBEI_YIN);
    assert(g.left == MUSE_JIAOBEI_ROUND && g.right == MUSE_JIAOBEI_ROUND);
    /* A toss from a restored board continues the streak (2 -> 3). */
    muse_jiaobei_restore(&g, MUSE_JIAOBEI_SHENG, 2);
    muse_jiaobei_toss(&g, 1); /* holy */
    tick_to(&g, MUSE_JIAOBEI_FLIP_MS);
    assert(g.phase == MUSE_JIAOBEI_RESULT && g.streak == 3);
    /* Cancel after a restore is a no-op (nothing is flipping). */
    muse_jiaobei_restore(&g, MUSE_JIAOBEI_SHENG, 2);
    muse_jiaobei_cancel(&g);
    assert(g.phase == MUSE_JIAOBEI_RESULT && g.streak == 2);

    /* --- shake detector -------------------------------------------------- */
    /* At rest (|a| = 1g) nothing triggers, however long. */
    muse_jiaobei_shake_t sh;
    muse_jiaobei_shake_init(&sh);
    for (int i = 0; i < 40; i++) {
        assert(!muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f, 50));
    }
    /* Below threshold (a gentle sway, 1.5 m/s^2 over 1g) never triggers. */
    for (int i = 0; i < 40; i++) {
        assert(!muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f + 1.5f, 50));
    }
    /* A single spike (one 50 ms sample at 6 over 1g) never triggers. */
    assert(!muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f + 6.0f, 50));
    assert(!muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f, 50));
    /* A sustained shake (5 m/s^2 over 1g) triggers on the 5th 50 ms
     * sample (250 ms window), once. */
    for (int i = 0; i < 4; i++) {
        assert(!muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f + 5.0f, 50));
    }
    assert(muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f + 5.0f, 50));
    /* Cooldown: shaking through the 1 s cooldown triggers nothing. */
    for (int i = 0; i < 20; i++) {
        assert(!muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f + 5.0f, 50));
    }
    /* After the cooldown, a fresh sustained shake triggers again. */
    for (int i = 0; i < 4; i++) {
        assert(!muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f + 5.0f, 50));
    }
    assert(muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f + 5.0f, 50));
    /* A shake that stops just short of the window (4 samples) resets
     * the window: 4 + rest + 4 does not fire, the 5th straight one does. */
    muse_jiaobei_shake_init(&sh);
    for (int i = 0; i < 4; i++) {
        assert(!muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f + 5.0f, 50));
    }
    assert(!muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f, 50));
    for (int i = 0; i < 4; i++) {
        assert(!muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f + 5.0f, 50));
    }
    assert(muse_jiaobei_shake_feed(&sh, 0.0f, 0.0f, 9.81f + 5.0f, 50));
    /* The metric is the magnitude, not one axis: with gravity on X
     * (board held sideways) rest never fires and a shake along X
     * triggers the same way. */
    muse_jiaobei_shake_init(&sh);
    for (int i = 0; i < 40; i++) {
        assert(!muse_jiaobei_shake_feed(&sh, 9.81f, 0.0f, 0.0f, 50));
    }
    for (int i = 0; i < 4; i++) {
        assert(!muse_jiaobei_shake_feed(&sh, 9.81f + 5.0f, 0.0f, 0.0f, 50));
    }
    assert(muse_jiaobei_shake_feed(&sh, 9.81f + 5.0f, 0.0f, 0.0f, 50));

    printf("ok\n");
    return 0;
}
