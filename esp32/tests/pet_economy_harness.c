/* Pet economy harness: checks the coin rules in muse_pet_model.c without
 * LVGL, ESP-IDF or NVS (the model file only includes <string.h>).
 * Run by tests/test_pet_economy.py. Prints "ok" on success; any failed
 * check aborts via assert.
 *
 * What it pins down (user decision 2026-11-01, retuned the same day after
 * 1.5f shipped the 5-coin ceiling as "v4"; entry fee and ceiling retuned
 * to 10 / 20 on 2026-11-08):
 *   - muse_pet_round_coins(): 1 point = 1 coin, 20 coins per round ceiling,
 *     and the same curve for every score-based game.
 *   - A full score round: 10-coin entry fee up front (PET_FEE) plus the
 *     payout at the end, so the net is payout minus 10 -- 20 points = +10,
 *     10 points = 0 (a draw), 0 points = -10 (a loss), 25 points = +10
 *     (capped), exactly the rock-paper-scissors outcomes.
 *   - PET_GAME (rock-paper-scissors): entry fee + outcome payout in one
 *     action -- win 20, draw 10, loss 0 -- so the net is +10 / 0 / -10.
 *   - PET_PLAY_REWARD: clamped to the 20-coin ceiling, additive, never
 *     above the wallet cap.
 *   - PET_SUDOKU_ENTRY: once per calendar day, idempotent, PET_POOR under 5.
 *     Sudoku does not follow the 2026-11-08 retune (its ticket stays 5).
 *   - PET_SUDOKU_SOLVE: 10 coins for the day's first puzzle, PET_CAP after;
 *     a ledger already holding a nonzero reward -- including a v4 blob's 5
 *     -- counts as paid and is never topped up to 10.
 *   - PET_FORTUNE (user decision 2026-11-14): 5 coins a draw, once a day,
 *     the card a pure function of the day (same card all day, same card
 *     on any board); PET_POOR under 5, PET_FORTUNE_SAME for re-views.
 *   - PET_JIAOBEI_TOSS (user decision 2026-11-14): 5 coins a toss, at
 *     most 9 a day; the 10th is PET_CAP and settles nothing. The
 *     result rides in `arg`: a settled holy toss extends the day's
 *     streak, laughing / yin clear it, and the last result is recorded
 *     (the streak survives leaving the page within the same day; the
 *     midnight day boundary clears it). A refused toss records
 *     nothing; a bare request (arg 0) charges and counts but records
 *     no result.
 *   - Day rollback (clock going backwards) never re-charges a paid day.
 *   - muse_pet_sudoku_today() answers the UI's two questions.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "muse_pet_model.h"

/* Day N as a UTC epoch: 00:00 plus a bit, so the value is unambiguously
 * inside that day. MUSE_PET_DAY_OFFSET_SECONDS is 8*3600 (local midnight),
 * so day = (now + 28800) / 86400; 3600 + 28800 = 32400 < 86400, so this
 * still lands inside day N. */
static int64_t day_now(int32_t day)
{
    return (int64_t)day * 86400 + 3600;
}

static muse_pet_result_t apply(muse_pet_model_t *m, muse_pet_action_t a,
                               int32_t day, int32_t arg)
{
    return muse_pet_model_apply(m, a, day_now(day), 0, arg);
}

int main(void)
{
    /* --- the conversion curve ------------------------------------------ */
    assert(MUSE_PET_ROUND_COIN_MAX == 20);
    assert(MUSE_PET_SUDOKU_DAILY_MAX == 10);
    assert(MUSE_PET_SUDOKU_SOLVE_COINS == 10);
    assert(MUSE_PET_SCORE_PER_COIN == 1);
    assert(muse_pet_round_coins(0) == 0);
    /* 1:1 below the ceiling -- this is the behaviour a 13-point snake round
     * was missing when the divisor was 5 (it paid 2 coins). */
    assert(muse_pet_round_coins(1) == 1);
    assert(muse_pet_round_coins(4) == 4);
    assert(muse_pet_round_coins(5) == 5);
    assert(muse_pet_round_coins(9) == 9);
    /* 20 is the ceiling: an ordinary 30 s whack-a-mole round (~28 hits)
     * and a long snake run both stop here, so the payout never exceeds
     * twice the 10-coin entry fee. */
    assert(muse_pet_round_coins(10) == 10);
    assert(muse_pet_round_coins(13) == 13);
    assert(muse_pet_round_coins(15) == 15);
    assert(muse_pet_round_coins(20) == 20);
    assert(muse_pet_round_coins(24) == 20);
    assert(muse_pet_round_coins(25) == 20);
    assert(muse_pet_round_coins(26) == 20);
    assert(muse_pet_round_coins(28) == 20);
    assert(muse_pet_round_coins(1000) == 20);
    /* 加减练习 runs 3-minute rounds since 2026-11-08, so it pays against
     * its own ceiling of 20 (user decision); the shared ceiling is now 20
     * too, so the two constants are equal -- the math game's own knob is
     * kept in case its 3-minute round ever wants a different ceiling. */
    assert(MUSE_PET_ROUND_COIN_MAX_MATHPRACTICE == 20);
    assert(MUSE_PET_ROUND_COIN_HARD_MAX == 20);
    assert(muse_pet_round_coins_cap(9, MUSE_PET_ROUND_COIN_MAX_MATHPRACTICE) == 9);
    assert(muse_pet_round_coins_cap(13, MUSE_PET_ROUND_COIN_MAX_MATHPRACTICE) == 13);
    assert(muse_pet_round_coins_cap(20, MUSE_PET_ROUND_COIN_MAX_MATHPRACTICE) == 20);
    assert(muse_pet_round_coins_cap(25, MUSE_PET_ROUND_COIN_MAX_MATHPRACTICE) == 20);
    /* the same call with the standard ceiling is the old behaviour. */
    assert(muse_pet_round_coins_cap(13, MUSE_PET_ROUND_COIN_MAX) == 13);
    assert(muse_pet_round_coins_cap(1000, 0) == 0);

    /* --- round payout -------------------------------------------------- */
    const int32_t d1 = 20400; /* 2025-11-08-ish: above MUSE_PET_TIME_MIN */
    muse_pet_model_t m;
    muse_pet_model_init(&m);
    assert(muse_pet_model_valid(&m));
    m.coins = 10;
    assert(apply(&m, PET_PLAY_REWARD, d1, 3) == PET_OK);
    assert(m.coins == 13);
    /* Clamped to the hard maximum (20, 加减练习's own ceiling), even if
     * the UI asks for more. */
    assert(apply(&m, PET_PLAY_REWARD, d1, 9) == PET_OK);
    assert(m.coins == 22);
    assert(apply(&m, PET_PLAY_REWARD, d1, 15) == PET_OK);
    assert(m.coins == 37); /* 15 asked, 15 paid: below the hard maximum */
    assert(apply(&m, PET_PLAY_REWARD, d1, 25) == PET_OK);
    assert(m.coins == 57); /* 25 asked, 20 paid */
    /* A negative arg is a zero payout, not a refund. */
    assert(apply(&m, PET_PLAY_REWARD, d1, -4) == PET_OK);
    assert(m.coins == 57);
    /* 0 is a valid settlement (a scoreless round) and changes nothing. */
    assert(apply(&m, PET_PLAY_REWARD, d1, 0) == PET_OK);
    assert(m.coins == 57);
    /* The wallet cap still applies. */
    m.coins = MUSE_PET_COINS_MAX - 2;
    assert(apply(&m, PET_PLAY_REWARD, d1, 5) == PET_OK);
    assert(m.coins == MUSE_PET_COINS_MAX);

    /* --- a full score round: fee up front, payout at the end ----------- */
    /* 20 points: fee 10, payout 20, net +10 -- the rock-paper-scissors win. */
    muse_pet_model_init(&m);
    m.coins = 20;
    assert(apply(&m, PET_FEE, d1, 0) == PET_OK);
    assert(m.coins == 10);
    assert(apply(&m, PET_PLAY_REWARD, d1,
                 (int32_t)muse_pet_round_coins(20)) == PET_OK);
    assert(m.coins == 30);
    /* 10 points: fee 10, payout 10, net 0 -- the draw. */
    muse_pet_model_init(&m);
    m.coins = 20;
    assert(apply(&m, PET_FEE, d1, 0) == PET_OK);
    assert(apply(&m, PET_PLAY_REWARD, d1,
                 (int32_t)muse_pet_round_coins(10)) == PET_OK);
    assert(m.coins == 20);
    /* 0 points: fee 10, payout 0, net -10 -- the loss. */
    muse_pet_model_init(&m);
    m.coins = 20;
    assert(apply(&m, PET_FEE, d1, 0) == PET_OK);
    assert(apply(&m, PET_PLAY_REWARD, d1,
                 (int32_t)muse_pet_round_coins(0)) == PET_OK);
    assert(m.coins == 10);
    /* 25 points: fee 10, payout capped at 20, net +10 -- the ceiling binds. */
    muse_pet_model_init(&m);
    m.coins = 20;
    assert(apply(&m, PET_FEE, d1, 0) == PET_OK);
    assert(apply(&m, PET_PLAY_REWARD, d1,
                 (int32_t)muse_pet_round_coins(25)) == PET_OK);
    assert(m.coins == 30);
    /* Under 10 coins the fee is refused and nothing is settled. 9 pins
     * the new gate: under the old 5-coin rule 9 would have started. */
    muse_pet_model_init(&m);
    m.coins = 9;
    assert(apply(&m, PET_FEE, d1, 0) == PET_POOR);
    assert(m.coins == 9);

    /* --- rock-paper-scissors: fee + outcome payout in one action ------ */
    /* Win (arg > 0): fee 10, payout 20, net +10. */
    muse_pet_model_init(&m);
    m.coins = 20;
    assert(apply(&m, PET_GAME, d1, 1) == PET_OK);
    assert(m.coins == 30);
    /* Draw (arg == 0): fee 10, payout 10, net 0. */
    muse_pet_model_init(&m);
    m.coins = 20;
    assert(apply(&m, PET_GAME, d1, 0) == PET_OK);
    assert(m.coins == 20);
    /* Loss (arg < 0): fee 10, payout 0, net -10. */
    muse_pet_model_init(&m);
    m.coins = 20;
    assert(apply(&m, PET_GAME, d1, -1) == PET_OK);
    assert(m.coins == 10);
    /* Under 10 coins the round never starts. */
    muse_pet_model_init(&m);
    m.coins = 9;
    assert(apply(&m, PET_GAME, d1, 1) == PET_POOR);
    assert(m.coins == 9);
    /* The wallet cap still applies to the win payout. */
    muse_pet_model_init(&m);
    m.coins = 95;
    assert(apply(&m, PET_GAME, d1, 1) == PET_OK);
    assert(m.coins == MUSE_PET_COINS_MAX);

    /* --- sudoku: one entry fee per day --------------------------------- */
    muse_pet_model_init(&m);
    m.coins = 40;
    assert(apply(&m, PET_SUDOKU_ENTRY, d1, 0) == PET_OK);
    assert(m.coins == 35); /* 5 charged */
    assert(m.sudoku_entry_paid == 1);
    assert(m.sudoku_day == d1);
    /* Idempotent: every later entry / 上一题 / 下一题 / 再玩一次 that day is
     * free, so the same request must not deduct again. */
    assert(apply(&m, PET_SUDOKU_ENTRY, d1, 0) == PET_OK);
    assert(m.coins == 35);
    bool paid = false;
    uint32_t got = 0;
    muse_pet_sudoku_today(&m, day_now(d1), &paid, &got);
    assert(paid);
    assert(got == 0);

    /* --- sudoku: 10 coins for the first solved puzzle, then nothing ---- */
    assert(apply(&m, PET_SUDOKU_SOLVE, d1, 0) == PET_OK);
    assert(m.coins == 45);
    assert(m.sudoku_reward_coins == 10);
    assert(apply(&m, PET_SUDOKU_SOLVE, d1, 0) == PET_CAP);
    assert(m.coins == 45); /* second and later puzzles pay nothing */
    assert(m.sudoku_reward_coins == 10);
    muse_pet_sudoku_today(&m, day_now(d1), &paid, &got);
    assert(paid);
    assert(got == 10);

    /* --- a new day rolls the sudoku ledger ----------------------------- */
    const int32_t d2 = d1 + 1;
    /* The living cost (20) is settled before the action, as for every other
     * action, so 45 - 20 - 5 = 20. */
    assert(apply(&m, PET_SUDOKU_ENTRY, d2, 0) == PET_OK);
    assert(m.coins == 20);
    assert(m.sudoku_day == d2);
    assert(m.sudoku_reward_coins == 0);
    assert(apply(&m, PET_SUDOKU_SOLVE, d2, 0) == PET_OK);
    assert(m.coins == 30);

    /* --- clock rollback never re-charges a paid day -------------------- */
    uint32_t before = m.coins;
    assert(apply(&m, PET_SUDOKU_ENTRY, d2 - 1, 0) == PET_OK);
    assert(m.coins == before);  /* no second ticket sale for the same day */
    assert(m.sudoku_day == d2); /* the stored day is kept, not moved back */

    /* --- under 5 coins the entry is refused and nothing is settled ----- */
    muse_pet_model_init(&m);
    m.coins = 4;
    assert(apply(&m, PET_SUDOKU_ENTRY, d1, 0) == PET_POOR);
    assert(m.coins == 4);
    assert(m.sudoku_entry_paid == 0);

    /* --- fortune: 5 coins a draw, once a day, same card all day --------
     * (user decision 2026-11-14: the draw used to pay 20, now it costs 5;
     * the card is a pure function of the day, not a random draw). */
    muse_pet_model_init(&m);
    m.coins = 40;
    assert(apply(&m, PET_FORTUNE, d1, 0) == PET_FORTUNE_NEW);
    assert(m.coins == 40 - MUSE_PET_FORTUNE_COST); /* charged, not paid */
    assert(m.fortune_day == d1);
    uint32_t card_d1 = m.fortune;
    assert(card_d1 < MUSE_PET_FORTUNES);
    /* Same-day repeat shows the same card and moves nothing. */
    assert(apply(&m, PET_FORTUNE, d1, 0) == PET_FORTUNE_SAME);
    assert(m.coins == 40 - MUSE_PET_FORTUNE_COST);
    assert(m.fortune == card_d1);
    /* Deterministic by date: two fresh models draw the same card on the
     * same day, whatever `random` the worker passes. */
    muse_pet_model_t m2;
    muse_pet_model_init(&m2);
    m2.coins = 40;
    assert(muse_pet_model_apply(&m2, PET_FORTUNE, day_now(d1), 12345, 0) ==
           PET_FORTUNE_NEW);
    assert(m2.fortune == card_d1);
    /* The deck spreads across days: at least 10 distinct cards over 20
     * consecutive days (the plain day % 20 cycle would show all 20 in
     * order; the hash must not collapse to a few). */
    bool seen[MUSE_PET_FORTUNES] = {false};
    uint32_t distinct = 0;
    for (int32_t d = d1; d < d1 + 20; d++) {
        muse_pet_model_t md;
        muse_pet_model_init(&md);
        md.coins = 40;
        assert(muse_pet_model_apply(&md, PET_FORTUNE, day_now(d), 0, 0) ==
               PET_FORTUNE_NEW);
        if (!seen[md.fortune]) {
            seen[md.fortune] = true;
            distinct++;
        }
    }
    assert(distinct >= 10);
    /* A new day draws a new card (the living cost is settled first). */
    assert(apply(&m, PET_FORTUNE, d2, 0) == PET_FORTUNE_NEW);
    assert(m.coins == 40 - 2 * MUSE_PET_FORTUNE_COST - MUSE_PET_DECAY_PER_DAY);
    assert(m.fortune_day == d2);
    /* Under the price the first draw of the day is refused and settles
     * nothing -- the balance never goes negative, and no card is drawn. */
    muse_pet_model_init(&m);
    m.coins = MUSE_PET_FORTUNE_COST - 1;
    assert(apply(&m, PET_FORTUNE, d1, 0) == PET_POOR);
    assert(m.coins == MUSE_PET_FORTUNE_COST - 1);
    assert(m.fortune_day == -1);
    /* Exactly the price draws fine. */
    m.coins = MUSE_PET_FORTUNE_COST;
    assert(apply(&m, PET_FORTUNE, d1, 0) == PET_FORTUNE_NEW);
    assert(m.coins == 0);
    /* Already drawn today, then spent below the price: re-viewing is still
     * free (SAME, not POOR -- the same-day check comes first). */
    assert(apply(&m, PET_FORTUNE, d1, 0) == PET_FORTUNE_SAME);
    assert(m.coins == 0);

    /* --- jiaobei: MUSE_PET_JIAOBEI_COST a toss, at most 9 a day ---------
     * (user decision 2026-11-14: pure consumption, no payout; the price
     * went 5 -> 2 on 2026-11-16, so every sum here is macro-based). */
    muse_pet_model_init(&m);
    m.coins = 100;
    uint32_t remaining = 0;
    muse_pet_jiaobei_today(&m, day_now(d1), &remaining);
    assert(remaining == 9);
    for (int i = 0; i < 9; i++) {
        assert(apply(&m, PET_JIAOBEI_TOSS, d1, 0) == PET_OK);
    }
    assert(m.coins == 100 - 9 * MUSE_PET_JIAOBEI_COST);
    assert(m.jiaobei_day == d1);
    assert(m.jiaobei_count == 9);
    /* Bare requests (arg 0) charge and count but record no result. */
    assert(m.jiaobei_streak == 0 && m.jiaobei_last == 0);
    muse_pet_jiaobei_today(&m, day_now(d1), &remaining);
    assert(remaining == 0);
    /* The 10th toss of the day: PET_CAP, and nothing changes -- no
     * charge, no count, no result (the UI shows the limit prompt). */
    assert(apply(&m, PET_JIAOBEI_TOSS, d1, 0) == PET_CAP);
    assert(m.coins == 100 - 9 * MUSE_PET_JIAOBEI_COST);
    assert(m.jiaobei_count == 9);
    /* A new day rolls the ledger (the living cost is settled first). */
    assert(apply(&m, PET_JIAOBEI_TOSS, d2, 0) == PET_OK);
    assert(m.coins == 100 - 10 * MUSE_PET_JIAOBEI_COST - MUSE_PET_DECAY_PER_DAY);
    assert(m.jiaobei_day == d2);
    assert(m.jiaobei_count == 1);
    muse_pet_jiaobei_today(&m, day_now(d2), &remaining);
    assert(remaining == 8);
    /* Clock rollback never re-grants tosses: the stored day is kept and
     * the toss settles against it (count 1 -> 2, not reset to 1). */
    assert(apply(&m, PET_JIAOBEI_TOSS, d2 - 1, 0) == PET_OK);
    assert(m.coins == 100 - 11 * MUSE_PET_JIAOBEI_COST - MUSE_PET_DECAY_PER_DAY);
    assert(m.jiaobei_day == d2);
    assert(m.jiaobei_count == 2);
    /* Under the price a toss is refused and settles nothing. */
    muse_pet_model_init(&m);
    m.coins = MUSE_PET_JIAOBEI_COST - 1;
    assert(apply(&m, PET_JIAOBEI_TOSS, d1, 0) == PET_POOR);
    assert(m.coins == MUSE_PET_JIAOBEI_COST - 1);
    assert(m.jiaobei_count == 0);
    /* Exactly the price tosses fine. */
    m.coins = MUSE_PET_JIAOBEI_COST;
    assert(apply(&m, PET_JIAOBEI_TOSS, d1, 0) == PET_OK);
    assert(m.coins == 0);
    assert(m.jiaobei_count == 1);

    /* --- jiaobei streak: holy +1, laughing / yin clear, same day ----
     * (user decision 2026-11-14: the streak survives leaving the page
     * within the same day; the result rides in the toss's `arg`). */
    muse_pet_model_init(&m);
    m.coins = 100;
    uint32_t streak = 0, last = 0;
    assert(apply(&m, PET_JIAOBEI_TOSS, d1, MUSE_PET_JIAOBEI_SHENG) == PET_OK);
    assert(m.coins == 100 - MUSE_PET_JIAOBEI_COST);
    assert(m.jiaobei_streak == 1 && m.jiaobei_last == MUSE_PET_JIAOBEI_SHENG);
    muse_pet_jiaobei_result_today(&m, day_now(d1), &streak, &last);
    assert(streak == 1 && last == MUSE_PET_JIAOBEI_SHENG);
    assert(apply(&m, PET_JIAOBEI_TOSS, d1, MUSE_PET_JIAOBEI_SHENG) == PET_OK);
    assert(m.jiaobei_streak == 2);
    assert(apply(&m, PET_JIAOBEI_TOSS, d1, MUSE_PET_JIAOBEI_XIAO) == PET_OK);
    assert(m.jiaobei_streak == 0 && m.jiaobei_last == MUSE_PET_JIAOBEI_XIAO);
    muse_pet_jiaobei_result_today(&m, day_now(d1), &streak, &last);
    assert(streak == 0 && last == MUSE_PET_JIAOBEI_XIAO);
    assert(apply(&m, PET_JIAOBEI_TOSS, d1, MUSE_PET_JIAOBEI_SHENG) == PET_OK);
    assert(m.jiaobei_streak == 1);
    assert(apply(&m, PET_JIAOBEI_TOSS, d1, MUSE_PET_JIAOBEI_YIN) == PET_OK);
    assert(m.jiaobei_streak == 0 && m.jiaobei_last == MUSE_PET_JIAOBEI_YIN);
    /* A new day clears the streak and the last result (the living cost is
     * settled first; five tosses went on d1, this one is on d2). */
    assert(apply(&m, PET_JIAOBEI_TOSS, d2, MUSE_PET_JIAOBEI_SHENG) == PET_OK);
    assert(m.coins == 100 - 6 * MUSE_PET_JIAOBEI_COST - MUSE_PET_DECAY_PER_DAY);
    assert(m.jiaobei_day == d2);
    assert(m.jiaobei_streak == 1 && m.jiaobei_last == MUSE_PET_JIAOBEI_SHENG);
    muse_pet_jiaobei_result_today(&m, day_now(d2), &streak, &last);
    assert(streak == 1 && last == MUSE_PET_JIAOBEI_SHENG);
    /* The accessor answers for the day asked about, not the stored
     * one: yesterday reads 0 / none. */
    muse_pet_jiaobei_result_today(&m, day_now(d1), &streak, &last);
    assert(streak == 0 && last == 0);
    /* A refused toss records nothing: 8 more holy (streak 9), the
     * 10th is PET_CAP and the streak + last result stand. */
    for (int i = 0; i < 8; i++) {
        assert(apply(&m, PET_JIAOBEI_TOSS, d2, MUSE_PET_JIAOBEI_SHENG) == PET_OK);
    }
    assert(m.coins == 100 - 14 * MUSE_PET_JIAOBEI_COST - MUSE_PET_DECAY_PER_DAY);
    assert(m.jiaobei_count == 9 && m.jiaobei_streak == 9);
    assert(apply(&m, PET_JIAOBEI_TOSS, d2, MUSE_PET_JIAOBEI_XIAO) == PET_CAP);
    assert(m.coins == 100 - 14 * MUSE_PET_JIAOBEI_COST - MUSE_PET_DECAY_PER_DAY);
    assert(m.jiaobei_count == 9 && m.jiaobei_streak == 9);
    assert(m.jiaobei_last == MUSE_PET_JIAOBEI_SHENG);
    /* A refused toss for lack of coins records nothing either. */
    muse_pet_model_init(&m);
    m.coins = MUSE_PET_JIAOBEI_COST - 1;
    assert(apply(&m, PET_JIAOBEI_TOSS, d1, MUSE_PET_JIAOBEI_SHENG) == PET_POOR);
    assert(m.coins == MUSE_PET_JIAOBEI_COST - 1);
    assert(m.jiaobei_count == 0 && m.jiaobei_streak == 0 && m.jiaobei_last == 0);
    /* A bare request (arg 0, e.g. from the console) charges and counts
     * the toss but records no result. */
    muse_pet_model_init(&m);
    m.coins = 100;
    for (int i = 0; i < 3; i++) {
        assert(apply(&m, PET_JIAOBEI_TOSS, d1, 0) == PET_OK);
    }
    assert(m.coins == 100 - 3 * MUSE_PET_JIAOBEI_COST);
    assert(m.jiaobei_count == 3 && m.jiaobei_streak == 0 && m.jiaobei_last == 0);
    muse_pet_jiaobei_result_today(&m, day_now(d1), &streak, &last);
    assert(streak == 0 && last == 0);

    /* --- validation catches a corrupt jiaobei ledger -------------------- */
    muse_pet_model_init(&m);
    m.jiaobei_count = MUSE_PET_JIAOBEI_DAILY_MAX + 1;
    assert(!muse_pet_model_valid(&m));
    muse_pet_model_init(&m);
    m.jiaobei_day = -2;
    assert(!muse_pet_model_valid(&m));
    muse_pet_model_init(&m);
    m.jiaobei_streak = MUSE_PET_JIAOBEI_DAILY_MAX + 1;
    assert(!muse_pet_model_valid(&m));
    muse_pet_model_init(&m);
    m.jiaobei_last = 4;
    assert(!muse_pet_model_valid(&m));

    /* --- a v4 blob's stored 5 counts as paid, never topped up --------- */
    muse_pet_model_init(&m);
    m.coins = 50;
    m.sudoku_day = d1;
    m.sudoku_entry_paid = 1;
    m.sudoku_reward_coins = 5; /* 1.5f's v4 first-solve grant */
    assert(muse_pet_model_valid(&m)); /* 5 is still within the 10 bound */
    assert(apply(&m, PET_SUDOKU_SOLVE, d1, 0) == PET_CAP);
    assert(m.coins == 50);
    assert(m.sudoku_reward_coins == 5);
    muse_pet_sudoku_today(&m, day_now(d1), &paid, &got);
    assert(got == 5); /* reads back 5; the UI treats nonzero as paid */

    /* --- validation catches a corrupt ledger --------------------------- */
    muse_pet_model_init(&m);
    assert(muse_pet_model_valid(&m));
    m.sudoku_reward_coins = MUSE_PET_SUDOKU_DAILY_MAX + 1;
    assert(!muse_pet_model_valid(&m));
    muse_pet_model_init(&m);
    m.sudoku_entry_paid = 2;
    assert(!muse_pet_model_valid(&m));

    /* --- roll_daily: the 2026-11-07 feed bug --------------------------- */
    /* Feed all six foods on d1, then simulate the worker tick on d2
     * (no feed action yet): the mask must already be clear, so the UI's
     * snapshot reads "nothing fed today" and the first rice tap of d2
     * answers "+10", not "already fed today". Before this fix the mask
     * was only rolled inside the feed action, so the first tap of a new
     * day read yesterday's mask (0b111) for s_last_feed_before. */
    muse_pet_model_init(&m);
    m.coins = 0;
    assert(apply(&m, PET_FEED_RICE, d1, 0) == PET_OK);
    assert(apply(&m, PET_FEED_CAKE, d1, 0) == PET_OK);
    assert(apply(&m, PET_FEED_COFFEE, d1, 0) == PET_OK);
    assert(apply(&m, PET_FEED_BURGER, d1, 0) == PET_OK);
    assert(apply(&m, PET_FEED_PIZZA, d1, 0) == PET_OK);
    assert(apply(&m, PET_FEED_MILKTEA, d1, 0) == PET_OK);
    assert(m.coins == 60);
    assert(m.feed_day == d1);
    assert(m.feed_mask == 0x3fu);
    /* The tick rolls the caps with no action at all. */
    muse_pet_model_roll_daily(&m, day_now(d2));
    assert(m.feed_day == d2);
    assert(m.feed_mask == 0);
    assert(m.coins == 60); /* rolling grants nothing, it only clears */
    /* Rice on the new day now pays, and the bit it sets is fresh.
     * (The 20-coin living cost is settled by the feed's decay first:
     * 60 - 20 + 10 = 50.) */
    assert(apply(&m, PET_FEED_RICE, d2, 0) == PET_OK);
    assert(m.coins == 50);
    assert(m.feed_mask == 0x1u);
    /* A second rice tap the same day pays nothing (mask bit already set). */
    assert(apply(&m, PET_FEED_RICE, d2, 0) == PET_OK);
    assert(m.coins == 50);
    assert(m.feed_mask == 0x1u);

    /* --- roll_daily also clears the touch count ------------------------ */
    muse_pet_model_init(&m);
    m.coins = 0;
    for (int i = 0; i < 10; i++) {
        assert(apply(&m, PET_TOUCH, d1, 0) == PET_COIN);
    }
    assert(m.daily_touch_coins == MUSE_PET_TOUCH_DAILY_MAX);
    assert(apply(&m, PET_TOUCH, d1, 0) == PET_CAP); /* capped for d1 */
    muse_pet_model_roll_daily(&m, day_now(d2));
    assert(m.touch_day == d2);
    assert(m.daily_touch_coins == 0);
    assert(apply(&m, PET_TOUCH, d2, 0) == PET_COIN); /* fresh day, pays */

    /* --- roll_daily is idempotent and rollback-safe -------------------- */
    /* Same day: nothing changes. */
    uint32_t mask_before = m.feed_mask;
    muse_pet_model_roll_daily(&m, day_now(d2));
    assert(m.feed_mask == mask_before);
    assert(m.feed_day == d2);
    /* Clock going backwards: the stored day and mask are kept, so a
     * rollback can never re-grant a day's rewards. */
    muse_pet_model_roll_daily(&m, day_now(d2 - 1));
    assert(m.feed_day == d2);
    assert(m.feed_mask == mask_before);

    /* --- decay_notice accumulates across un-ACKed days (audit 2026-11-08)
     * Day d2 takes 20 and the UI never ACKs it; day d3 takes another 20.
     * The notice must read 40, not 20 (the old plain assignment overwrote
     * yesterday's un-announced notice). */
    const int32_t d3 = d2 + 1;
    muse_pet_model_init(&m);
    m.coins = 100;
    assert(apply(&m, PET_TOUCH, d1, 0) == PET_COIN);
    assert(m.decay_notice == 0);
    assert(apply(&m, PET_TOUCH, d2, 0) == PET_COIN);
    assert(m.coins == 85); /* 100 - 20 decay + 5 touch */
    assert(m.decay_notice == 20);
    m.coins = 100; /* replenish before the next day, as a user would */
    assert(apply(&m, PET_TOUCH, d3, 0) == PET_COIN);
    assert(m.coins == 85); /* 100 - 20 decay + 5 touch */
    assert(m.decay_notice == 40);
    assert(apply(&m, PET_DECAY_ACK, d3, d3) == PET_OK);
    assert(m.decay_notice == 0);

    /* --- roll_daily is a no-op on an uncalibrated clock ---------------- */
    muse_pet_model_init(&m);
    m.feed_day = d1;
    m.feed_mask = 0x7u;
    muse_pet_model_roll_daily(&m, MUSE_PET_TIME_MIN - 3600);
    assert(m.feed_day == d1);
    assert(m.feed_mask == 0x7u);

    printf("ok\n");
    return 0;
}
