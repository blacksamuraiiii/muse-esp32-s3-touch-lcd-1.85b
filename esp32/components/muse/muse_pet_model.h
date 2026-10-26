/* Public-edition staging: local UI additions and/or privacy review; see source manifest. */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_pet_model.h -- 1.5a gold-only pet economy model.
 *
 * Adapted from TamaMe (asos520-rgb/tamame-esp32,
 * firmware-overlay/esp32/components/muse/muse_pet_model.h, Apache-2.0).
 * TamaMe attribution is retained above; the economy below is NOT TamaMe's:
 * hunger / mood / energy, hourly decay, 36-second quanta and the food table
 * are all deleted (design 20261025-1.5互动预览版设计方案.md §4, user decision
 * 2026-10-25: "不要饱腹、全部用金币替代").
 *
 * Differences from TamaMe, deliberate and documented in PHASE1-NOTES.md:
 *  - MUSE_PET_VERSION is 2, not 1: the struct layout is incompatible with
 *    TamaMe v1, so a TamaMe blob must fail validation (and size check) rather
 *    than be misread as a 1.5a state.
 *  - No muse_i18n.h dependency: fortune cards, grades and food names live in
 *    muse_pet_text.h/.c as static Chinese tables.
 *  - No hunger / mood / energy / sfx / last_time fields at all (design §4.1
 *    allows keeping them inert, but dropping them is stronger: they cannot
 *    silently affect coins).
 *  - Feed is three action values (rice / cake / coffee) that settle
 *    identically. The design calls feed one action; the three UI buttons need
 *    a way for phase-2 UI to know which food was fed for its subtitle
 *    ("喂了<食物名>"), and the command queue carries only an action value,
 *    so the food is encoded in the action. Reward is identical for all three.
 *  - Day numbers come from muse_pet_day(), which is a UTC day
 *    (epoch / 86400 + MUSE_PET_DAY_OFFSET_SECONDS, TZ-independent,
 *    user decision 2026-10-25). The boundary is 08:00 Beijing time
 *    with the default offset 0, not local midnight.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Struct layout version. Bump this whenever the layout below changes. */
#define MUSE_PET_VERSION 3u

/* Economy constants (design §4). All amounts are whole coins. */
#define MUSE_PET_COINS_MAX 100u        /* wallet cap: excess is discarded */
#define MUSE_PET_TOUCH_COINS 5u        /* per touch */
#define MUSE_PET_TOUCH_DAILY_MAX 50u   /* per day: the first 10 touches pay */
#define MUSE_PET_FEED_COINS 10u        /* per food, once per food per day */
#define MUSE_PET_FEED_KINDS 3u         /* rice / cake / coffee */
#define MUSE_PET_FEED_DAILY_MAX 30u    /* three foods x 10 (user decision 2026-10-25) */
#define MUSE_PET_GAME_STAKE 5u         /* win +5, lose -5, draw 0 */
#define MUSE_PET_FORTUNE_COINS 20u     /* once a day, paid with the new card */
#define MUSE_PET_DECAY_PER_DAY 20u     /* living cost, per calendar day */
#define MUSE_PET_FORTUNES 20u          /* fortune cards in muse_pet_text.c */

/*
 * "No real time yet" threshold, taken from muse_rtc.c ("Older than this means
 * 'no real time yet': 2025-01-01 00:00:00 UTC"). A clock below this is
 * treated as uncalibrated: no decay, no daily-cap reset (design §4.2).
 */
#define MUSE_PET_TIME_MIN 1735689600LL /* 2025-01-01 00:00:00 UTC */

/* State, persisted as one NVS blob (namespace "pet_pub_stats", key "state").
 * Day fields are calendar-day numbers from muse_pet_day(), or -1 when unset.
 * fortune is meaningful only when fortune_day >= 0.
 * decay_notice is the living-cost amount still to be announced once; phase-2
 * UI clears it with PET_DECAY_ACK after showing it (design §4.2). */
typedef struct {
    uint32_t version;
    uint32_t coins;              /* 0..100 */
    int32_t touch_day;           /* day of the current touch cap, -1 = none */
    uint32_t daily_touch_coins;  /* 0..50 */
    int32_t feed_day;            /* day of the current feed cap, -1 = none */
    uint32_t feed_mask;          /* bits 0..2: that food already paid today */
    int32_t fortune_day;         /* day of the current fortune, -1 = never */
    uint32_t fortune;            /* 0..19, valid only if fortune_day >= 0 */
    int32_t decay_day;           /* last day living cost was settled, -1 = none */
    uint32_t decay_notice;       /* amount to announce once, 0 = nothing pending */
} muse_pet_model_t;

/* Actions. PET_FEED_* settle identically; see the header note above.
 * PET_DECAY_ACK and PET_RESET are internal only: never sent by UI buttons.
 * PET_DECAY_ACK clears decay_notice after phase-2 UI has shown it once;
 * since 1.5b its arg is the announced decay_day, and only a notice for
 * that day is cleared (see muse_pet_decay_ack()).
 * PET_RESET rebuilds the initial state; only the Test-gated console command
 * may send it (see the muse_input.c patch note). */
typedef enum {
    PET_TOUCH = 0,
    PET_FEED_RICE,
    PET_FEED_CAKE,
    PET_FEED_COFFEE,
    PET_GAME,
    PET_FORTUNE,
    PET_DECAY_ACK,
    PET_RESET,
} muse_pet_action_t;

/* Results. UI copy must be keyed off these, never guessed (design §4.3).
 * PET_OK for feed means "settled": whether coins were actually added is read
 * from the snapshot's feed_mask, not from a separate code (design §4.3).
 * Fortune never returns a "no day" refusal any more (phase 2, user
 * decision): it always uses the current UTC day. */
typedef enum {
    PET_OK = 0,
    PET_COIN,          /* touch paid */
    PET_CAP,           /* touch daily cap reached: happy only, no coins */
    PET_POOR,          /* game: coins < 5, not settled, no round started */
    PET_FORTUNE_NEW,   /* new card drawn */
    PET_FORTUNE_SAME,  /* same-day (or prior) card shown again */
    PET_STORAGE,       /* NVS write failed: state was NOT changed */
    PET_BUSY,          /* unknown action */
    /* PET_TIME removed 2026-10-25 (user decision): fortune no longer
     * refuses on an uncalibrated clock; it always uses the current UTC
     * day (see muse_pet_day()). Kept out of the enum on purpose so no
     * caller can still branch on it. */
} muse_pet_result_t;

void muse_pet_model_init(muse_pet_model_t *s);
bool muse_pet_model_valid(const muse_pet_model_t *s);

/* Day-boundary offset, in seconds, added to the UTC epoch before dividing
 * by 86400. 0 (the default, user decision 2026-10-25) means the day rolls
 * over at 00:00 UTC, i.e. 08:00 Beijing time -- NOT at local midnight.
 * To move the boundary to local midnight later, change ONLY this constant
 * to 8*3600; no call-site logic changes. */
#define MUSE_PET_DAY_OFFSET_SECONDS 0

/*
 * UTC calendar-day number for `now` (epoch / 86400, gmtime semantics),
 * plus MUSE_PET_DAY_OFFSET_SECONDS. Deliberately TZ-independent: the board
 * clock is UTC (muse_time_start() RTC adopt + SNTP), and fortune / living
 * cost are defined on that UTC day (user decision 2026-10-25, superseding
 * the phase-1 localtime_r() + TZ design). *calibrated is set from
 * MUSE_PET_TIME_MIN only (used by the living-cost gate and the daily-cap
 * roll, NOT by fortune any more: fortune always uses the current UTC day,
 * calibrated or not, so a later SNTP jump can legitimately draw one extra
 * card -- accepted preview behaviour, not a bug).
 */
int32_t muse_pet_day(int64_t now, bool *calibrated);

/* Settle one calendar day's living cost into `s` (idempotent for a given day).
 * No-op when uncalibrated, and never moves decay_day backwards on rollback. */
void muse_pet_model_decay(muse_pet_model_t *s, int64_t now);

/* Settle one action into `s` (decay is applied first). `random` is used only
 * for PET_FORTUNE (index = random % 20). `arg` carries the outcome of a game
 * round (-1 lose / 0 draw / +1 win, decided UI-side at click time, design
 * §5.2) and is ignored by every other action: since 1.5a a round costs 5 on a
 * loss, pays 5 on a win and costs nothing on a draw. */
muse_pet_result_t muse_pet_model_apply(muse_pet_model_t *s, muse_pet_action_t action,
                                       int64_t now, uint32_t random, int32_t arg);

/* True for the three feed actions. */
bool muse_pet_action_is_feed(muse_pet_action_t action);

/* 0..2 (rice / cake / coffee) for a feed action, -1 for anything else. The
 * index is the bit in feed_mask, so each food pays once per day on its own. */
int muse_pet_feed_index(muse_pet_action_t action);
