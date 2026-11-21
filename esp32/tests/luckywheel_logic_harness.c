/* Host harness for the lucky wheel logic: muse_luckywheel.c is pure logic
 * (no LVGL, no ESP-IDF), so it builds with plain cc on the host (same
 * pattern as test_jiaobei.py). Run by tests/test_luckywheel_logic.py.
 * Prints "ok" on success; any failed check aborts via assert.
 *
 * What it pins down (user decision 2026-11-20, mockup v7):
 *   - The 12-segment table, clockwise from the top: +10, thanks, +2,
 *     thanks, +5, thanks, again, thanks, +2, thanks, again, thanks.
 *     Prizes sum to 19 over 12 segments with 2 free re-spins, so the
 *     expected value is 19 / (12 - 2) = 1.9 coins per paid spin --
 *     under the 2-coin cost, so the wheel cannot be farmed.
 *   - The target angle: rotating the disc by it puts segment k centred
 *     under the top pointer (segment 0 is baked centred at the top at
 *     angle 0). Checked as (k * 300 + target) % 3600 == 0.
 *   - The total spin angle: 3 full turns plus the forward (clockwise,
 *     never backwards) distance to the target, from any current angle
 *     -- including landing exactly on the target (0 forward) and
 *     wrapping past 3600.
 *   - The cubic ease-out: 0 -> 0, 1000 -> 1000, monotone, and halfway
 *     through the spin 87.5% of the angle is done (1000 - 500^3/1e6).
 *   - angle_at(): 0 at t=0, total at t>=2800 ms, monotone, and the
 *     halfway value matches the ease-out.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "muse_luckywheel.h"

int main(void)
{
    /* --- constants ------------------------------------------------------- */
    assert(MUSE_LUCKYWHEEL_SEGMENTS == 12);
    assert(MUSE_LUCKYWHEEL_TURNS_DEG10 == 10800);
    assert(MUSE_LUCKYWHEEL_SPIN_MS == 2800);
    assert(MUSE_LUCKYWHEEL_TICK_MS == 50);

    /* --- the segment table ---------------------------------------------- */
    assert(muse_luckywheel_result_for_segment(0) == MUSE_LUCKYWHEEL_COINS_10);
    assert(muse_luckywheel_result_for_segment(1) == MUSE_LUCKYWHEEL_THANKS);
    assert(muse_luckywheel_result_for_segment(2) == MUSE_LUCKYWHEEL_COINS_2);
    assert(muse_luckywheel_result_for_segment(3) == MUSE_LUCKYWHEEL_THANKS);
    assert(muse_luckywheel_result_for_segment(4) == MUSE_LUCKYWHEEL_COINS_5);
    assert(muse_luckywheel_result_for_segment(5) == MUSE_LUCKYWHEEL_THANKS);
    assert(muse_luckywheel_result_for_segment(6) == MUSE_LUCKYWHEEL_AGAIN);
    assert(muse_luckywheel_result_for_segment(7) == MUSE_LUCKYWHEEL_THANKS);
    assert(muse_luckywheel_result_for_segment(8) == MUSE_LUCKYWHEEL_COINS_2);
    assert(muse_luckywheel_result_for_segment(9) == MUSE_LUCKYWHEEL_THANKS);
    assert(muse_luckywheel_result_for_segment(10) == MUSE_LUCKYWHEEL_AGAIN);
    assert(muse_luckywheel_result_for_segment(11) == MUSE_LUCKYWHEEL_THANKS);
    assert(muse_luckywheel_result_for_segment(12) == MUSE_LUCKYWHEEL_NONE);
    assert(muse_luckywheel_result_for_segment(100) == MUSE_LUCKYWHEEL_NONE);

    /* --- the prizes ------------------------------------------------------ */
    assert(muse_luckywheel_coins_for_result(MUSE_LUCKYWHEEL_COINS_2) == 2);
    assert(muse_luckywheel_coins_for_result(MUSE_LUCKYWHEEL_COINS_5) == 5);
    assert(muse_luckywheel_coins_for_result(MUSE_LUCKYWHEEL_COINS_10) == 10);
    assert(muse_luckywheel_coins_for_result(MUSE_LUCKYWHEEL_THANKS) == 0);
    assert(muse_luckywheel_coins_for_result(MUSE_LUCKYWHEEL_AGAIN) == 0);
    assert(muse_luckywheel_coins_for_result(MUSE_LUCKYWHEEL_NONE) == 0);

    /* Expected value: prizes sum to 19, free re-spins are 2 of 12, so
     * E = 19 / (12 - 2) = 1.9 < 2 (the spin's cost). */
    uint32_t prize_sum = 0, again = 0;
    for (uint32_t k = 0; k < MUSE_LUCKYWHEEL_SEGMENTS; k++) {
        muse_luckywheel_result_t r = muse_luckywheel_result_for_segment(k);
        prize_sum += muse_luckywheel_coins_for_result(r);
        if (r == MUSE_LUCKYWHEEL_AGAIN) {
            again++;
        }
    }
    assert(prize_sum == 19);
    assert(again == 2);
    /* 19 / 10 = 1.9 in tenths, under the 2-coin cost (20 tenths). */
    assert(prize_sum * 10 / (MUSE_LUCKYWHEEL_SEGMENTS - again) == 19);
    assert(prize_sum * 10 / (MUSE_LUCKYWHEEL_SEGMENTS - again) < 20);

    /* --- the target angle ------------------------------------------------- */
    assert(muse_luckywheel_target_angle(0) == 0);
    assert(muse_luckywheel_target_angle(1) == 3300);
    assert(muse_luckywheel_target_angle(2) == 3000);
    assert(muse_luckywheel_target_angle(6) == 1800);
    assert(muse_luckywheel_target_angle(11) == 300);
    assert(muse_luckywheel_target_angle(12) == 0);
    /* Rotating the disc by the target puts segment k centred at the top:
     * segment k sits k * 30 degrees clockwise from the top. */
    for (uint32_t k = 0; k < MUSE_LUCKYWHEEL_SEGMENTS; k++) {
        assert((k * 300 + muse_luckywheel_target_angle(k)) % 3600 == 0);
    }

    /* --- the total spin angle --------------------------------------------- */
    /* From 0 to segment 0: exactly 3 turns, no forward distance. */
    assert(muse_luckywheel_total_angle(0, 0) == 10800);
    /* From 0 to segment 1 (target 3300): 3 turns + 3300. */
    assert(muse_luckywheel_total_angle(0, 1) == 10800 + 3300);
    /* From 3300 to segment 1: already on target, exactly 3 turns. */
    assert(muse_luckywheel_total_angle(3300, 1) == 10800);
    /* From 3400 to segment 1 (target 3300): forward is 3500, not -100. */
    assert(muse_luckywheel_total_angle(3400, 1) == 10800 + 3500);
    /* From 3599 to segment 0 (target 0): forward is 1. */
    assert(muse_luckywheel_total_angle(3599, 0) == 10800 + 1);
    /* Current angles wrap: 3600 is the same as 0. */
    assert(muse_luckywheel_total_angle(3600, 0) == 10800);
    assert(muse_luckywheel_total_angle(7200 + 1500, 6) == 10800 + 300);
    /* Every total is 3 turns plus a forward distance in 0..3599. */
    for (uint32_t cur = 0; cur < 3600; cur += 137) {
        for (uint32_t k = 0; k < MUSE_LUCKYWHEEL_SEGMENTS; k++) {
            uint32_t total = muse_luckywheel_total_angle(cur, k);
            assert(total >= 10800 && total < 10800 + 3600);
            /* Landing on the target: start + total lands the segment
             * at the top. */
            assert((cur + total + k * 300) % 3600 == 0);
        }
    }

    /* --- the cubic ease-out ----------------------------------------------- */
    assert(muse_luckywheel_ease_out(0) == 0);
    assert(muse_luckywheel_ease_out(1000) == 1000);
    assert(muse_luckywheel_ease_out(1001) == 1000);
    /* Halfway through the spin, 87.5% of the angle is done. */
    assert(muse_luckywheel_ease_out(500) == 875);
    /* Monotone, and never above the linear line after the start. */
    uint32_t prev = 0;
    for (uint32_t t = 0; t <= 1000; t += 10) {
        uint32_t e = muse_luckywheel_ease_out(t);
        assert(e >= prev);
        assert(e >= t || t == 0);
        prev = e;
    }

    /* --- angle_at ---------------------------------------------------------- */
    uint32_t total = 14100;
    assert(muse_luckywheel_angle_at(0, total) == 0);
    assert(muse_luckywheel_angle_at(2800, total) == total);
    assert(muse_luckywheel_angle_at(99999, total) == total);
    /* Halfway: total * 875 / 1000. */
    assert(muse_luckywheel_angle_at(1400, total) == total * 875 / 1000);
    /* Monotone over the 50 ms ticks, and the last tick lands exactly. */
    prev = 0;
    for (uint32_t ms = 0; ms <= 2800; ms += 50) {
        uint32_t a = muse_luckywheel_angle_at(ms, total);
        assert(a >= prev && a <= total);
        prev = a;
    }
    assert(muse_luckywheel_angle_at(2800, total) == total);

    printf("ok\n");
    return 0;
}
