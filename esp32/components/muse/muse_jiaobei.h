/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_jiaobei.h -- 掷圣杯 logic (no LVGL, no ESP-IDF).
 *
 * Two crescent cups ("筊"), tossed by tapping the board: one flat side
 * (平面, 阳面) and one round side (凸面, 阴面) each. The outcome is pure
 * chance on two fair cups -- holy 50% (one flat, one round), laughing
 * 25% (both flat), yin 25% (both round) -- decided by the caller-supplied
 * random value so the host test drives the game deterministically (no
 * rand() in here; the UI seeds once from lv_tick_get, as the snake page
 * does for food placement).
 *
 * The board never leaves the page: there is no start page and no OVER
 * settlement (user decision 2026-11-14). A tap in READY or RESULT starts
 * an in-place flip (FLIPPING) that spins the cups MUSE_JIAOBEI_FLIP_TURNS
 * times (user 2026-11-16, replacing the 300 ms squeeze); the faces swap at
 * the midpoint and the result lands at the end. The UI reads the flip
 * progress through muse_jiaobei_flip_angle() and flips both cups over their
 * horizontal axis (user 2026-11-16: an in-plane spin read as a clock hand;
 * a tossed cup tips end over end) -- the cups never change position.
 *
 * The consecutive-holy streak ("连续圣杯 xN") is session state with a
 * day: it counts consecutive holy results, clears on a laughing or yin
 * result, and lives in the pet model (jiaobei_streak + jiaobei_last,
 * same day as jiaobei_count) -- leaving the page keeps it within the
 * same day, and the midnight day boundary clears it (user decision
 * 2026-11-14, superseding the earlier enter-the-page reset). The
 * model only records a result for a toss the worker settled: the UI
 * sends the already-decided result with the toss request
 * (muse_pet_request_jiaobei_toss), so a refused toss records nothing.
 * The economy lives in the pet model, not here: each toss costs
 * MUSE_PET_JIAOBEI_COST (2 since 2026-11-16, 5 before) and the day holds at
 * most 9 tosses (no cap in a Test build)
 * (MUSE_PET_JIAOBEI_DAILY_MAX), both settled by the worker through
 * PET_JIAOBEI_TOSS. This file never touches coins.
 *
 * Shake-to-toss (user decision 2026-11-14): the QMI8658 accelerometer
 * is polled, never the tap / WoM interrupt engine (unreliable on
 * battery, the 1.4 lesson) -- one read per 50 ms UI tick through the
 * same muse_imu_read_accel() the gold-rain tilt uses, only while the
 * page is open (wake on show, sleep on hide), so it costs no standby
 * power and adds no wake source. A shake is a sustained deviation of
 * the acceleration magnitude from 1g: it must stay above
 * MUSE_JIAOBEI_SHAKE_THRESHOLD_MS2 for MUSE_JIAOBEI_SHAKE_WINDOW_MS
 * (a single spike never triggers), then the 1 s cooldown swallows
 * further shaking. The thresholds below are starting values -- tune
 * them on the board (light shake / hard shake / walking / on the
 * table) before trusting them. Tapping stays as the backup trigger;
 * both go through the same toss path.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Flip duration, ms. The cups spin MUSE_JIAOBEI_FLIP_TURNS times in place
 * and land showing the result; the faces swap at the halfway point, where
 * the spin is fastest and the swap cannot be seen (user 2026-11-16: "转三圈
 * 再落", the earlier 300 ms squeeze read as a blink). */
#define MUSE_JIAOBEI_FLIP_MS 1000u
#define MUSE_JIAOBEI_FLIP_TURNS 3u

/* Shake-to-toss tuning (see the header note). Starting values only --
 * the right numbers depend on how hard the player shakes, so tune on
 * the board before shipping. */
#define MUSE_JIAOBEI_SHAKE_THRESHOLD_MS2 3.0f /* |a| - 1g above this counts */
#define MUSE_JIAOBEI_SHAKE_WINDOW_MS 250u     /* must hold this long to fire */
#define MUSE_JIAOBEI_SHAKE_COOLDOWN_MS 1000u  /* shakes ignored after a toss */

/* The limit prompt's copy (mockup v21): the UI shows this exact text,
 * and the host test pins it -- the copy is the contract, not a
 * rewording. The ready row does NOT show the remaining count (v20's
 * "今日还剩 N 次" was withdrawn 2026-11-14): the prompt is the only
 * place the cap is announced, and only when the user taps after the
 * 9 tosses are used up. */
#define MUSE_JIAOBEI_LIMIT_TEXT "今日掷筊次数已用完"

typedef enum {
    MUSE_JIAOBEI_READY = 0, /* board shown, tap to toss */
    MUSE_JIAOBEI_FLIPPING,  /* 300 ms in-place flip, then RESULT */
    MUSE_JIAOBEI_RESULT,    /* result held until the next toss */
} muse_jiaobei_phase_t;

typedef enum {
    MUSE_JIAOBEI_NONE = 0, /* no toss yet this visit */
    MUSE_JIAOBEI_SHENG,    /* one flat, one round: 神明应允 */
    MUSE_JIAOBEI_XIAO,     /* both flat: 神明笑了 */
    MUSE_JIAOBEI_YIN,      /* both round: 时机未到 */
} muse_jiaobei_result_t;

typedef enum {
    MUSE_JIAOBEI_FLAT = 0,  /* 平面朝上; the board labels it 阳 */
    MUSE_JIAOBEI_ROUND,     /* 凸面朝上; the board labels it 阴 */
} muse_jiaobei_face_t;

typedef struct {
    muse_jiaobei_phase_t phase;
    muse_jiaobei_result_t result; /* NONE until the first toss lands */
    muse_jiaobei_face_t left;     /* face shown on the left cup now */
    muse_jiaobei_face_t right;    /* face shown on the right cup now */
    uint32_t streak;              /* consecutive holy cups, this visit */
    uint32_t flip_ms;             /* ms elapsed in FLIPPING */
    /* Pre-toss snapshot, so a refused toss (the worker answered
     * PET_POOR / PET_CAP after the flip already started) restores the
     * board exactly as it was -- result, faces and streak included. */
    muse_jiaobei_phase_t saved_phase;
    muse_jiaobei_result_t saved_result;
    muse_jiaobei_face_t saved_left;
    muse_jiaobei_face_t saved_right;
    uint32_t saved_streak;
    bool face_swapped; /* flip passed its midpoint (faces exchanged) */
} muse_jiaobei_t;

/* Shake detector state (pure logic, fed by the UI's 50 ms tick). */
typedef struct {
    uint32_t above_ms;    /* ms the shake level has stayed above threshold */
    uint32_t cooldown_ms; /* ms left in the post-toss cooldown */
} muse_jiaobei_shake_t;

void muse_jiaobei_init(muse_jiaobei_t *g);

/* Any phase -> READY with both cups flat and the streak at 0. The
 * fresh-board primitive; re-entering the page uses
 * muse_jiaobei_restore() instead (the streak survives leaving the
 * page within the same day). The daily toss count is NOT here -- it
 * lives in the pet model and survives re-entry; only the streak resets. */
void muse_jiaobei_reset(muse_jiaobei_t *g);

/* Restore the board to a settled result (re-entering the page):
 * phase RESULT with the result's faces and the persisted streak, or
 * READY with both cups flat and the streak at 0 when result is NONE
 * (nothing tossed today). The saved pre-toss state is set to the
 * restored board, so a cancel after a restore is a no-op. */
void muse_jiaobei_restore(muse_jiaobei_t *g, muse_jiaobei_result_t result,
                          uint32_t streak);

/* The outcome of `random`: random % 4 -> 0,1 holy (50%), 2 laughing
 * (25%), 3 yin (25%). Pure, so the host test pins the distribution
 * without a random source. */
muse_jiaobei_result_t muse_jiaobei_result_from_random(uint32_t random);

/* The two faces a result shows: holy is left round + right flat
 * (mockup v12: the left cup turns red-convex, the right stays flat
 * wood), laughing both flat, yin both round. */
void muse_jiaobei_result_faces(muse_jiaobei_result_t result,
                               muse_jiaobei_face_t *left,
                               muse_jiaobei_face_t *right);

/* Start a toss (READY or RESULT -> FLIPPING). Decides the result from
 * `random`, updates the streak at once (holy +1, laughing/yin clear)
 * and saves the pre-toss state for muse_jiaobei_cancel(). No-op while
 * FLIPPING: a tap mid-flip never starts a second toss. */
void muse_jiaobei_toss(muse_jiaobei_t *g, uint32_t random);

/* Advance the flip by `ms` (FLIPPING only). The faces swap at the
 * midpoint; at MUSE_JIAOBEI_FLIP_MS the phase becomes RESULT. */
void muse_jiaobei_tick(muse_jiaobei_t *g, uint32_t ms);

/* Spin for the flip, in degrees: 0 at rest, 0 .. 360*MUSE_JIAOBEI_FLIP_TURNS
 * while FLIPPING, and 0 from RESULT on (a whole number of turns, so the
 * image is back where it started and nothing jumps). The UI turns this into
 * a flip about the horizontal axis -- scale_y = |cos(angle)|, the way a
 * tossed cup tips over and comes back -- not an in-plane spin. */
uint32_t muse_jiaobei_flip_angle(const muse_jiaobei_t *g);

/* FLIPPING -> the pre-toss state (muse_jiaobei_toss() saved it): the
 * toss never happened, so the previous result, faces and streak stand.
 * No-op from any other phase. Only the worker's refusal calls this. */
void muse_jiaobei_cancel(muse_jiaobei_t *g);

/* Shake detector: zero its window and cooldown (entering the page). */
void muse_jiaobei_shake_init(muse_jiaobei_shake_t *s);

/* Feed one accelerometer sample (m/s^2) with the ms since the last
 * sample (~50 ms, the UI timer tick). Returns true once when the
 * acceleration magnitude has stayed further than
 * MUSE_JIAOBEI_SHAKE_THRESHOLD_MS2 from 1g for
 * MUSE_JIAOBEI_SHAKE_WINDOW_MS -- the caller starts one toss. A
 * single spike never triggers (the window needs five consecutive
 * 50 ms samples above threshold); after a trigger the 1 s cooldown
 * swallows further shaking, so one shake is one toss. Pure logic, so
 * the host test drives it with synthetic samples. */
bool muse_jiaobei_shake_feed(muse_jiaobei_shake_t *s, float ax, float ay,
                             float az, uint32_t ms);
