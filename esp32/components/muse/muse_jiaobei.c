/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_jiaobei.c -- 掷圣杯 logic (no LVGL, no ESP-IDF).
 *
 * See muse_jiaobei.h for the rules. Time only moves through
 * muse_jiaobei_tick(): the UI calls it from its 50 ms timer with the
 * real elapsed ms, so the host test drives the flip deterministically.
 */
#include "muse_jiaobei.h"

#include <math.h>

/* 1g, in m/s^2: the shake detector measures the acceleration
 * magnitude's deviation from this. */
#define MUSE_JIAOBEI_GRAVITY_MS2 9.81f

void muse_jiaobei_init(muse_jiaobei_t *g)
{
    g->phase = MUSE_JIAOBEI_READY;
    g->result = MUSE_JIAOBEI_NONE;
    g->left = MUSE_JIAOBEI_FLAT;
    g->right = MUSE_JIAOBEI_FLAT;
    g->streak = 0;
    g->flip_ms = 0;
    g->saved_phase = MUSE_JIAOBEI_READY;
    g->saved_result = MUSE_JIAOBEI_NONE;
    g->saved_left = MUSE_JIAOBEI_FLAT;
    g->saved_right = MUSE_JIAOBEI_FLAT;
    g->saved_streak = 0;
    g->face_swapped = false;
}

void muse_jiaobei_reset(muse_jiaobei_t *g)
{
    muse_jiaobei_init(g);
}

void muse_jiaobei_restore(muse_jiaobei_t *g, muse_jiaobei_result_t result,
                          uint32_t streak)
{
    muse_jiaobei_init(g);
    if (result == MUSE_JIAOBEI_NONE) {
        return; /* nothing tossed today: the ready board */
    }
    g->phase = MUSE_JIAOBEI_RESULT;
    g->result = result;
    muse_jiaobei_result_faces(result, &g->left, &g->right);
    g->streak = streak;
    g->face_swapped = true;
    /* The saved state is the restored board: a cancel after a restore
     * (a refusal for a toss that never started) changes nothing. */
    g->saved_phase = MUSE_JIAOBEI_RESULT;
    g->saved_result = result;
    g->saved_left = g->left;
    g->saved_right = g->right;
    g->saved_streak = streak;
}

muse_jiaobei_result_t muse_jiaobei_result_from_random(uint32_t random)
{
    switch (random % 4u) {
    case 0:
    case 1:
        return MUSE_JIAOBEI_SHENG;
    case 2:
        return MUSE_JIAOBEI_XIAO;
    default:
        return MUSE_JIAOBEI_YIN;
    }
}

void muse_jiaobei_result_faces(muse_jiaobei_result_t result,
                               muse_jiaobei_face_t *left,
                               muse_jiaobei_face_t *right)
{
    muse_jiaobei_face_t l = MUSE_JIAOBEI_FLAT;
    muse_jiaobei_face_t r = MUSE_JIAOBEI_FLAT;
    switch (result) {
    case MUSE_JIAOBEI_SHENG:
        l = MUSE_JIAOBEI_ROUND;
        r = MUSE_JIAOBEI_FLAT;
        break;
    case MUSE_JIAOBEI_YIN:
        l = MUSE_JIAOBEI_ROUND;
        r = MUSE_JIAOBEI_ROUND;
        break;
    case MUSE_JIAOBEI_XIAO:
    case MUSE_JIAOBEI_NONE:
    default:
        break;
    }
    if (left) {
        *left = l;
    }
    if (right) {
        *right = r;
    }
}

void muse_jiaobei_toss(muse_jiaobei_t *g, uint32_t random)
{
    if (g->phase == MUSE_JIAOBEI_FLIPPING) {
        return;
    }
    g->saved_phase = g->phase;
    g->saved_result = g->result;
    g->saved_left = g->left;
    g->saved_right = g->right;
    g->saved_streak = g->streak;
    g->result = muse_jiaobei_result_from_random(random);
    if (g->result == MUSE_JIAOBEI_SHENG) {
        g->streak++;
    } else {
        g->streak = 0;
    }
    g->flip_ms = 0;
    g->face_swapped = false;
    g->phase = MUSE_JIAOBEI_FLIPPING;
}

void muse_jiaobei_tick(muse_jiaobei_t *g, uint32_t ms)
{
    if (g->phase != MUSE_JIAOBEI_FLIPPING) {
        return;
    }
    g->flip_ms += ms;
    /* The face changes while the cup is edge-on, so the swap cannot be
     * seen: the UI draws the spin as a flip about the horizontal axis
     * (scale_y = |cos|), which is edge-on at every odd quarter turn. With
     * MUSE_JIAOBEI_FLIP_TURNS 3 the quarter turns are 90 degrees apart, and
     * 5/12 of the spin (450 degrees) is the one nearest the middle. */
    if (!g->face_swapped &&
        g->flip_ms >= MUSE_JIAOBEI_FLIP_MS * 5u / 12u) {
        muse_jiaobei_result_faces(g->result, &g->left, &g->right);
        g->face_swapped = true;
    }
    if (g->flip_ms >= MUSE_JIAOBEI_FLIP_MS) {
        g->flip_ms = 0;
        g->phase = MUSE_JIAOBEI_RESULT;
    }
}

uint32_t muse_jiaobei_flip_angle(const muse_jiaobei_t *g)
{
    if (g->phase != MUSE_JIAOBEI_FLIPPING) {
        return 0;
    }
    /* Linear through the whole spin: the cups turn at a constant rate and
     * stop on a whole number of turns, so landing is a stop, not a jump. */
    const uint32_t span = 360u * MUSE_JIAOBEI_FLIP_TURNS;
    uint32_t a = (span * g->flip_ms) / MUSE_JIAOBEI_FLIP_MS;
    return a > span ? span : a;
}

void muse_jiaobei_cancel(muse_jiaobei_t *g)
{
    if (g->phase != MUSE_JIAOBEI_FLIPPING) {
        return;
    }
    g->phase = g->saved_phase;
    g->result = g->saved_result;
    g->left = g->saved_left;
    g->right = g->saved_right;
    g->streak = g->saved_streak;
    g->flip_ms = 0;
    g->face_swapped = false;
}

void muse_jiaobei_shake_init(muse_jiaobei_shake_t *s)
{
    s->above_ms = 0;
    s->cooldown_ms = 0;
}

bool muse_jiaobei_shake_feed(muse_jiaobei_shake_t *s, float ax, float ay,
                             float az, uint32_t ms)
{
    if (s->cooldown_ms > 0) {
        /* Still cooling down from the last trigger: swallow the shake
         * and count the cooldown down, so one shake is one toss even
         * if the player keeps shaking after the cups land. */
        s->cooldown_ms = ms >= s->cooldown_ms ? 0 : s->cooldown_ms - ms;
        s->above_ms = 0;
        return false;
    }
    /* Deviation of the acceleration magnitude from 1g: at rest this
     * is the sensor noise (~0.1 m/s^2), walking is ~1-2, a deliberate
     * shake is several m/s^2 -- and the number does not depend on how
     * the board is held, since only the magnitude is used. */
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    float dev = fabsf(mag - MUSE_JIAOBEI_GRAVITY_MS2);
    if (dev > MUSE_JIAOBEI_SHAKE_THRESHOLD_MS2) {
        s->above_ms += ms;
        if (s->above_ms >= MUSE_JIAOBEI_SHAKE_WINDOW_MS) {
            s->above_ms = 0;
            s->cooldown_ms = MUSE_JIAOBEI_SHAKE_COOLDOWN_MS;
            return true;
        }
    } else {
        s->above_ms = 0;
    }
    return false;
}
