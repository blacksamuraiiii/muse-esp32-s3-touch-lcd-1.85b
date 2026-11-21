/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_luckywheel.c -- lucky wheel logic, see muse_luckywheel.h.
 * Pure integer maths; no LVGL, no ESP headers, host-testable.
 */
#include "muse_luckywheel.h"

/* Segment table, clockwise from the top (mockup v7). */
static const muse_luckywheel_result_t s_segment_result[MUSE_LUCKYWHEEL_SEGMENTS] = {
    MUSE_LUCKYWHEEL_COINS_10, /* 0: +10 */
    MUSE_LUCKYWHEEL_THANKS,   /* 1: 谢谢参与 */
    MUSE_LUCKYWHEEL_COINS_2,  /* 2: +2 */
    MUSE_LUCKYWHEEL_THANKS,   /* 3: 谢谢参与 */
    MUSE_LUCKYWHEEL_COINS_5,  /* 4: +5 */
    MUSE_LUCKYWHEEL_THANKS,   /* 5: 谢谢参与 */
    MUSE_LUCKYWHEEL_AGAIN,    /* 6: 再来一次 */
    MUSE_LUCKYWHEEL_THANKS,   /* 7: 谢谢参与 */
    MUSE_LUCKYWHEEL_COINS_2,  /* 8: +2 */
    MUSE_LUCKYWHEEL_THANKS,   /* 9: 谢谢参与 */
    MUSE_LUCKYWHEEL_AGAIN,    /* 10: 再来一次 */
    MUSE_LUCKYWHEEL_THANKS,   /* 11: 谢谢参与 */
};

muse_luckywheel_result_t muse_luckywheel_result_for_segment(uint32_t segment)
{
    if (segment >= MUSE_LUCKYWHEEL_SEGMENTS) {
        return MUSE_LUCKYWHEEL_NONE;
    }
    return s_segment_result[segment];
}

uint32_t muse_luckywheel_coins_for_result(muse_luckywheel_result_t result)
{
    switch (result) {
    case MUSE_LUCKYWHEEL_COINS_2:
        return 2;
    case MUSE_LUCKYWHEEL_COINS_5:
        return 5;
    case MUSE_LUCKYWHEEL_COINS_10:
        return 10;
    case MUSE_LUCKYWHEEL_THANKS:
    case MUSE_LUCKYWHEEL_AGAIN:
    case MUSE_LUCKYWHEEL_NONE:
    default:
        return 0;
    }
}

uint32_t muse_luckywheel_target_angle(uint32_t segment)
{
    if (segment >= MUSE_LUCKYWHEEL_SEGMENTS) {
        return 0;
    }
    /* Segment k sits k * 30 degrees clockwise from the top, so turning
     * the disc (12 - k) * 30 degrees clockwise brings it under the
     * pointer. 0.1-degree units: 30 degrees = 300. */
    return ((MUSE_LUCKYWHEEL_SEGMENTS - segment) % MUSE_LUCKYWHEEL_SEGMENTS) *
           300u;
}

uint32_t muse_luckywheel_total_angle(uint32_t current_angle10,
                                     uint32_t segment)
{
    uint32_t target = muse_luckywheel_target_angle(segment);
    uint32_t current = current_angle10 % 3600u;
    uint32_t forward = (target + 3600u - current) % 3600u;
    return MUSE_LUCKYWHEEL_TURNS_DEG10 + forward;
}

uint32_t muse_luckywheel_ease_out(uint32_t t1000)
{
    if (t1000 >= 1000u) {
        return 1000u;
    }
    uint32_t remain = 1000u - t1000;
    /* (1000-t)^3 / 1,000,000, max 1,000,000,000 / 1,000,000 = 1000:
     * fits in 32 bits, no overflow. */
    return 1000u - (remain * remain * remain) / 1000000u;
}

uint32_t muse_luckywheel_angle_at(uint32_t elapsed_ms, uint32_t total_angle10)
{
    if (elapsed_ms >= MUSE_LUCKYWHEEL_SPIN_MS) {
        return total_angle10;
    }
    uint32_t t1000 = elapsed_ms * 1000u / MUSE_LUCKYWHEEL_SPIN_MS;
    return total_angle10 * muse_luckywheel_ease_out(t1000) / 1000u;
}
