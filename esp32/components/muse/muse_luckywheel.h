/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_luckywheel.h -- lucky wheel logic (no LVGL, no ESP headers):
 * the 12-segment table, the draw, the spin angle maths and the cubic
 * ease-out. Time only moves through muse_luckywheel_angle_at(); the UI
 * owns the 50 ms timer. Host-testable (see tests/test_luckywheel_logic.py).
 *
 * Segments (clockwise from the top, mockup v7): +10 red, thanks, +2,
 * thanks, +5, thanks, again, thanks, +2, thanks, again, thanks.
 * Expected value 19 / (12 - 2) = 1.9 coins per paid spin, under the
 * 2-coin cost, so the wheel cannot be farmed.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_LUCKYWHEEL_SEGMENTS 12u
/* Spin: 3 full turns, cubic ease-out, ~2.8 s, 50 ms UI ticks. */
#define MUSE_LUCKYWHEEL_TURNS_DEG10 10800u
#define MUSE_LUCKYWHEEL_SPIN_MS 2800u
#define MUSE_LUCKYWHEEL_TICK_MS 50u

typedef enum {
    MUSE_LUCKYWHEEL_NONE = 0,
    MUSE_LUCKYWHEEL_THANKS = 1,  /* 谢谢参与, +0 coins */
    MUSE_LUCKYWHEEL_COINS_2 = 2, /* +2 coins */
    MUSE_LUCKYWHEEL_COINS_5 = 3, /* +5 coins */
    MUSE_LUCKYWHEEL_COINS_10 = 4, /* +10 coins */
    MUSE_LUCKYWHEEL_AGAIN = 5,   /* 再来一次: a free re-spin */
} muse_luckywheel_result_t;

/* Result of segment k (0..11), clockwise from the top. */
muse_luckywheel_result_t muse_luckywheel_result_for_segment(uint32_t segment);

/* Coins paid for a result: 2 / 5 / 10, 0 for THANKS and AGAIN. */
uint32_t muse_luckywheel_coins_for_result(muse_luckywheel_result_t result);

/* Target angle (0.1-degree units, 0..3599) that puts segment k centred
 * under the top pointer. The disc art has segment 0 baked centred at
 * the top at angle 0, so segment k needs ((12 - k) % 12) * 300. */
uint32_t muse_luckywheel_target_angle(uint32_t segment);

/* Total spin angle from the current angle: 3 full turns plus the
 * forward (clockwise, never backwards) distance to the target, 0..3599. */
uint32_t muse_luckywheel_total_angle(uint32_t current_angle10,
                                     uint32_t segment);

/* Cubic ease-out, t in 0..1000 -> 0..1000: 1000 - (1000-t)^3 / 1,000,000. */
uint32_t muse_luckywheel_ease_out(uint32_t t1000);

/* Disc angle travelled (0.1-degree units, 0..total) at elapsed_ms of
 * the spin; clamps at total once elapsed_ms >= MUSE_LUCKYWHEEL_SPIN_MS. */
uint32_t muse_luckywheel_angle_at(uint32_t elapsed_ms,
                                  uint32_t total_angle10);

#ifdef __cplusplus
}
#endif
