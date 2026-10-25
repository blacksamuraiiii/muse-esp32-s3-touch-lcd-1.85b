/* Public-edition staging: local UI additions and/or privacy review; see source manifest. */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_pet_model.c -- 1.5a gold-only economy.
 *
 * Adapted from TamaMe (asos520-rgb/tamame-esp32,
 * firmware-overlay/esp32/components/muse/muse_pet_model.c, Apache-2.0).
 * What was deleted versus TamaMe, and why, is in muse_pet_model.h and in
 * PHASE1-NOTES.md: hunger / mood / energy, hourly decay, 36-second quanta,
 * the food cost/fullness table, conversation rewards and SFX state are gone.
 * The only recurring charge is the daily living cost (design §4.2).
 */
#include "muse_pet_model.h"

#include <string.h>

static uint32_t capped_add(uint32_t value, uint32_t amount)
{
    uint32_t sum = value + amount;
    return sum > MUSE_PET_COINS_MAX ? MUSE_PET_COINS_MAX : sum;
}

/* UTC day, TZ-independent (user decision 2026-10-25, phase 2).
 * now is the board UTC epoch from muse_time_start(); the day number is
 * (now + MUSE_PET_DAY_OFFSET_SECONDS) / 86400, i.e. days since 1970-01-01
 * UTC. With the default offset 0 the boundary is 08:00 Beijing time.
 * *calibrated is only the MUSE_PET_TIME_MIN gate: it still guards the
 * living-cost deduction and the daily-cap roll, but fortune no longer
 * consults it (see PET_FORTUNE below). */
int32_t muse_pet_day(int64_t now, bool *calibrated)
{
    if (calibrated) {
        *calibrated = now >= MUSE_PET_TIME_MIN;
    }
    if (now < 0) {
        return -1;
    }
    return (int32_t)((now + MUSE_PET_DAY_OFFSET_SECONDS) / 86400);
}

void muse_pet_model_init(muse_pet_model_t *s)
{
    memset(s, 0, sizeof(*s));
    s->version = MUSE_PET_VERSION;
    s->touch_day = -1;
    s->feed_day = -1;
    s->fortune_day = -1;
    s->decay_day = -1;
}

bool muse_pet_model_valid(const muse_pet_model_t *s)
{
    return s->version == MUSE_PET_VERSION &&
           s->coins <= MUSE_PET_COINS_MAX &&
           s->daily_touch_coins <= MUSE_PET_TOUCH_DAILY_MAX &&
           s->daily_feed_coins <= MUSE_PET_FEED_DAILY_MAX &&
           s->fortune < MUSE_PET_FORTUNES &&
           s->touch_day >= -1 && s->feed_day >= -1 &&
           s->fortune_day >= -1 && s->decay_day >= -1 &&
           s->decay_notice <= MUSE_PET_COINS_MAX;
}

/* Start a new daily cap when the calibrated day has advanced. On rollback
 * (day <= stored day) the stored cap is kept: a clock going backwards must
 * never re-grant a day's rewards, and must never erase today's progress. */
static void roll_cap(int32_t *day_field, uint32_t *daily, int32_t day, bool calibrated)
{
    if (!calibrated) {
        return; /* design §4.2: uncalibrated clock resets nothing */
    }
    if (*day_field == -1 || day > *day_field) {
        *day_field = day;
        *daily = 0;
    }
}

void muse_pet_model_decay(muse_pet_model_t *s, int64_t now)
{
    bool calibrated = false;
    int32_t day = muse_pet_day(now, &calibrated);
    if (!calibrated) {
        return; /* design §4.2: uncalibrated clock deducts nothing */
    }
    if (s->decay_day == -1) {
        /* First calibrated run: establish the baseline, do not back-charge
         * days that predate any recorded state. */
        s->decay_day = day;
        return;
    }
    if (day <= s->decay_day) {
        return; /* same day, or clock rollback: no deduction, no refund */
    }
    uint32_t elapsed = (uint32_t)(day - s->decay_day);
    uint32_t due = elapsed * MUSE_PET_DECAY_PER_DAY;
    /* Floor at zero, never negative (design §4.2). The notice is the amount
     * actually taken, so the announced number always matches the deduction. */
    s->decay_notice = due >= s->coins ? s->coins : due;
    s->coins = due >= s->coins ? 0 : s->coins - due;
    s->decay_day = day;
}

bool muse_pet_action_is_feed(muse_pet_action_t action)
{
    return action == PET_FEED_RICE || action == PET_FEED_CAKE ||
           action == PET_FEED_COFFEE;
}

muse_pet_result_t muse_pet_model_apply(muse_pet_model_t *s, muse_pet_action_t action,
                                       int64_t now, uint32_t random)
{
    /* Internal actions bypass decay: reset means "fresh state, no
     * back-charge", and ack must not erase a notice that decay in this same
     * call would have just created (see PHASE1-NOTES.md §5). */
    if (action == PET_RESET) {
        muse_pet_model_init(s);
        return PET_OK;
    }
    if (action == PET_DECAY_ACK) {
        s->decay_notice = 0;
        return PET_OK;
    }

    muse_pet_model_decay(s, now);
    bool calibrated = false;
    int32_t day = muse_pet_day(now, &calibrated);

    if (action == PET_TOUCH) {
        roll_cap(&s->touch_day, &s->daily_touch_coins, day, calibrated);
        if (s->daily_touch_coins >= MUSE_PET_TOUCH_DAILY_MAX) {
            return PET_CAP; /* happy only, no coins (design §4.3) */
        }
        s->daily_touch_coins += MUSE_PET_TOUCH_COINS;
        s->coins = capped_add(s->coins, MUSE_PET_TOUCH_COINS);
        return PET_COIN;
    }

    if (muse_pet_action_is_feed(action)) {
        roll_cap(&s->feed_day, &s->daily_feed_coins, day, calibrated);
        if (s->daily_feed_coins >= MUSE_PET_FEED_DAILY_MAX) {
            /* Still PET_OK by design §4.3: whether coins were added is read
             * from the snapshot's daily_feed_coins, not from a failure code. */
            return PET_OK;
        }
        s->daily_feed_coins += MUSE_PET_FEED_COINS;
        s->coins = capped_add(s->coins, MUSE_PET_FEED_COINS);
        return PET_OK;
    }

    if (action == PET_GAME) {
        if (s->coins < MUSE_PET_GAME_COST) {
            return PET_POOR; /* not deducted, no game started */
        }
        s->coins -= MUSE_PET_GAME_COST;
        return PET_OK; /* win/lose/draw never refunds (design §4.3) */
    }

    if (action == PET_FORTUNE) {
        /* User decision 2026-10-25 (phase 2): fortune always uses the
         * current UTC day, calibrated or not -- no PET_TIME refusal.
         * Same UTC day shows the same card; a new UTC day draws a new one.
         * Consequence, accepted for preview: if SNTP later jumps the date,
         * the next draw is legitimately a new card for the jumped-to day. */
        (void)calibrated;
        if (s->fortune_day == day) {
            return PET_FORTUNE_SAME;
        }
        s->fortune_day = day;
        s->fortune = random % MUSE_PET_FORTUNES;
        return PET_FORTUNE_NEW;
    }

    return PET_BUSY;
}
