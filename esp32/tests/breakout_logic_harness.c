/* 打砖块 logic harness: checks muse_breakout.c without LVGL
 * or ESP-IDF. Run by tests/test_breakout_logic.py. Prints "ok" on
 * success; any failed check aborts via assert. Deterministic: the
 * random serve picks are seeded once with srand(), and the brick /
 * paddle / life cases place the ball directly in the (public) game
 * struct instead of waiting for a lucky bounce. */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_breakout.h"
#include "muse_pet_model.h"

static void tick_n(muse_breakout_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        muse_breakout_tick(b, MUSE_BREAKOUT_TICK_MS);
    }
}

/* Place the ball at a centre with a velocity, for a deterministic
 * collision on the next tick. */
static void place_ball(muse_breakout_t *b, float x, float y,
                       float vx, float vy)
{
    b->ball_x = x;
    b->ball_y = y;
    b->ball_vx = vx;
    b->ball_vy = vy;
}

/* Break one brick by dropping the ball onto its top centre. */
static void break_brick(muse_breakout_t *b, int col, int row)
{
    float bx, by;
    muse_breakout_brick_pos(col, row, &bx, &by);
    assert(muse_breakout_brick_alive(b, col, row));
    /* Ball centre 1 px above the brick top (bottom edge 4 px clear),
     * falling at 120 px/s: one tick moves it 6 px into the brick. */
    place_ball(b, bx + (float)MUSE_BREAKOUT_BRICK_W / 2.0f,
               by - (float)MUSE_BREAKOUT_BALL_R - 1.0f, 0.0f,
               MUSE_BREAKOUT_BALL_SPEED);
    muse_breakout_tick(b, MUSE_BREAKOUT_TICK_MS);
    assert(!muse_breakout_brick_alive(b, col, row));
    assert(b->event == MUSE_BREAKOUT_EV_BRICK);
    assert(fabsf(b->event_x - (bx + (float)MUSE_BREAKOUT_BRICK_W / 2.0f)) <
           0.01f);
    assert(fabsf(b->event_y - (by + (float)MUSE_BREAKOUT_BRICK_H / 2.0f)) <
           0.01f);
}

int main(void)
{
    srand(12345);

    /* Init state. */
    muse_breakout_t b;
    muse_breakout_init(&b);
    assert(b.phase == MUSE_BREAKOUT_START);
    assert(b.score == 0);
    assert(b.lives == MUSE_BREAKOUT_LIVES);
    assert(b.bricks_left == MUSE_BREAKOUT_BRICK_COUNT);
    assert(b.event == MUSE_BREAKOUT_EV_NONE);
    assert(fabsf(b.paddle_cx - 180.0f) < 0.01f);
    assert(fabsf(b.ball_x - 180.0f) < 0.01f &&
           fabsf(b.ball_y - 260.0f) < 0.01f);
    for (int row = 0; row < MUSE_BREAKOUT_BRICK_ROWS; row++) {
        for (int col = 0; col < MUSE_BREAKOUT_BRICK_COLS; col++) {
            assert(muse_breakout_brick_alive(&b, col, row));
        }
    }
    assert(!muse_breakout_brick_alive(&b, -1, 0));
    assert(!muse_breakout_brick_alive(&b, 0, MUSE_BREAKOUT_BRICK_ROWS));

    /* Phase guards before start. */
    muse_breakout_pause(&b);
    assert(b.phase == MUSE_BREAKOUT_START);
    muse_breakout_resume(&b);
    assert(b.phase == MUSE_BREAKOUT_START);
    muse_breakout_tick(&b, 50);
    assert(b.score == 0 && b.bricks_left == MUSE_BREAKOUT_BRICK_COUNT);
    muse_breakout_set_paddle(&b, 100.0f);
    assert(fabsf(b.paddle_cx - 180.0f) < 0.01f); /* centre, untouched */

    /* Serve: speed |v| = 120, upward, 35..55 degrees. Run many serves
     * so both vx signs show up. */
    bool vx_pos = false, vx_neg = false;
    for (int i = 0; i < 40; i++) {
        muse_breakout_restart(&b);
        assert(b.phase == MUSE_BREAKOUT_PLAYING);
        assert(b.score == 0 && b.lives == MUSE_BREAKOUT_LIVES);
        assert(b.bricks_left == MUSE_BREAKOUT_BRICK_COUNT);
        float speed = sqrtf(b.ball_vx * b.ball_vx + b.ball_vy * b.ball_vy);
        assert(fabsf(speed - MUSE_BREAKOUT_BALL_SPEED) < 0.01f);
        assert(b.ball_vy < 0.0f); /* upward */
        /* sin(angle) = |vy| / 120 must sit between sin(35) and sin(55). */
        float s = -b.ball_vy / MUSE_BREAKOUT_BALL_SPEED;
        assert(s > 0.57f && s < 0.82f);
        if (b.ball_vx > 0.0f) {
            vx_pos = true;
        } else {
            vx_neg = true;
        }
        assert(fabsf(b.ball_x - MUSE_BREAKOUT_SERVE_X) < 0.01f);
        assert(fabsf(b.ball_y - MUSE_BREAKOUT_SERVE_Y) < 0.01f);
        assert(fabsf(b.paddle_cx - 180.0f) < 0.01f);
    }
    assert(vx_pos && vx_neg); /* vx sign is random */
    /* start() only works from START. */
    muse_breakout_start(&b); /* no-op from PLAYING */
    assert(b.phase == MUSE_BREAKOUT_PLAYING);

    /* Walls: left / right / top bounce, centre clamped inside. */
    place_ball(&b, 66.0f, 200.0f, -MUSE_BREAKOUT_BALL_SPEED, 0.0f);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(fabsf(b.ball_x - 65.0f) < 0.01f);
    assert(fabsf(b.ball_vx - MUSE_BREAKOUT_BALL_SPEED) < 0.01f);
    place_ball(&b, 294.0f, 200.0f, MUSE_BREAKOUT_BALL_SPEED, 0.0f);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(fabsf(b.ball_x - 295.0f) < 0.01f);
    assert(fabsf(b.ball_vx + MUSE_BREAKOUT_BALL_SPEED) < 0.01f);
    place_ball(&b, 180.0f, 66.0f, 0.0f, -MUSE_BREAKOUT_BALL_SPEED);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(fabsf(b.ball_y - 65.0f) < 0.01f);
    assert(fabsf(b.ball_vy - MUSE_BREAKOUT_BALL_SPEED) < 0.01f);

    /* Brick from above: breaks one, +1, flips vy, parks the ball
     * above the brick. */
    muse_breakout_restart(&b);
    float bx0, by0;
    muse_breakout_brick_pos(2, 0, &bx0, &by0);
    place_ball(&b, bx0 + 18.0f, by0 - 6.0f, 0.0f, MUSE_BREAKOUT_BALL_SPEED);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(b.score == 1 && b.bricks_left == MUSE_BREAKOUT_BRICK_COUNT - 1);
    assert(b.event == MUSE_BREAKOUT_EV_BRICK);
    assert(fabsf(b.ball_vy + MUSE_BREAKOUT_BALL_SPEED) < 0.01f);
    assert(fabsf(b.ball_y - (by0 - 5.5f)) < 0.01f);
    assert(!muse_breakout_brick_alive(&b, 2, 0));
    /* A second tick on the same spot does not re-break it (and the
     * ball, now moving up, hits the top wall, not a brick). */
    int32_t score_before = b.score;
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(b.score == score_before);

    /* Brick from the side: flips vx, not vy. Ball left of the row-0
     * col-0 brick, level with its centre, moving right. */
    muse_breakout_restart(&b);
    muse_breakout_brick_pos(0, 0, &bx0, &by0);
    place_ball(&b, bx0 - 6.0f, by0 + 8.0f, MUSE_BREAKOUT_BALL_SPEED, 0.0f);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(b.score == 1);
    assert(fabsf(b.ball_vx + MUSE_BREAKOUT_BALL_SPEED) < 0.01f);
    assert(fabsf(b.ball_vy) < 0.01f);
    assert(fabsf(b.ball_x - (bx0 - 5.5f)) < 0.01f);
    assert(!muse_breakout_brick_alive(&b, 0, 0));

    /* At most one brick per tick: the ball straddles the 4 px gap
     * between two bricks in the top row. */
    muse_breakout_restart(&b);
    muse_breakout_brick_pos(0, 0, &bx0, &by0);
    /* Gap between col 0 (ends 98) and col 1 (starts 102): centre the
     * 10 px ball on x = 100 so it overlaps both. */
    place_ball(&b, 100.0f, by0 - 6.0f, 0.0f, MUSE_BREAKOUT_BALL_SPEED);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(b.score == 1 && b.bricks_left == MUSE_BREAKOUT_BRICK_COUNT - 1);
    assert(muse_breakout_brick_alive(&b, 0, 0) !=
           muse_breakout_brick_alive(&b, 1, 0)); /* exactly one broke */

    /* Paddle, centre hit: vx almost unchanged, vy flips up, ball
     * parked on the paddle top. */
    muse_breakout_restart(&b);
    muse_breakout_set_paddle(&b, 180.0f);
    place_ball(&b, 180.0f, 280.0f, 0.0f, MUSE_BREAKOUT_BALL_SPEED);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(b.event == MUSE_BREAKOUT_EV_PADDLE);
    assert(fabsf(b.ball_vx) < 0.01f);
    assert(fabsf(b.ball_vy + MUSE_BREAKOUT_BALL_SPEED) < 0.01f);
    assert(fabsf(b.ball_y - 281.0f) < 0.01f);
    assert(b.score == 0);

    /* Paddle, edge hit: vx bends the same way as the offset. Ball at
     * the paddle's right edge (offset +1) with vx = 0 gains +90. */
    place_ball(&b, 216.0f, 280.0f, 0.0f, MUSE_BREAKOUT_BALL_SPEED);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(b.event == MUSE_BREAKOUT_EV_PADDLE);
    assert(fabsf(b.ball_vx - 90.0f) < 0.01f);
    assert(b.ball_vy < 0.0f);
    /* Same on the left edge: offset -1 bends vx by -90. */
    place_ball(&b, 144.0f, 280.0f, 0.0f, MUSE_BREAKOUT_BALL_SPEED);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(fabsf(b.ball_vx + 90.0f) < 0.01f);

    /* vx clamps at +-220: 200 + 90 would be 290, -200 - 90 is -290.
     * vx = 200 moves the ball 10 px in one tick, so the ball is
     * placed 10 px inside the catch edge: it crosses at x = 221
     * (|221 - 180| = 41, offset clamps to +1), and x = 139 on the
     * left. */
    place_ball(&b, 211.0f, 280.0f, 200.0f, MUSE_BREAKOUT_BALL_SPEED);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(b.event == MUSE_BREAKOUT_EV_PADDLE);
    assert(fabsf(b.ball_vx - MUSE_BREAKOUT_BALL_VX_MAX) < 0.01f);
    place_ball(&b, 149.0f, 280.0f, -200.0f, MUSE_BREAKOUT_BALL_SPEED);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(b.event == MUSE_BREAKOUT_EV_PADDLE);
    assert(fabsf(b.ball_vx + MUSE_BREAKOUT_BALL_VX_MAX) < 0.01f);

    /* A ball that misses the paddle (outside the 41 px catch span)
     * does not bounce: it keeps falling. */
    place_ball(&b, 230.0f, 280.0f, 0.0f, MUSE_BREAKOUT_BALL_SPEED);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(b.event == MUSE_BREAKOUT_EV_NONE);
    assert(b.ball_vy > 0.0f);

    /* Paddle clamp: 96..264, and set_paddle is a no-op off PLAYING. */
    muse_breakout_set_paddle(&b, 0.0f);
    assert(fabsf(b.paddle_cx - MUSE_BREAKOUT_PADDLE_MIN_CX) < 0.01f);
    muse_breakout_set_paddle(&b, 400.0f);
    assert(fabsf(b.paddle_cx - MUSE_BREAKOUT_PADDLE_MAX_CX) < 0.01f);
    muse_breakout_pause(&b);
    assert(b.phase == MUSE_BREAKOUT_PAUSED);
    muse_breakout_set_paddle(&b, 180.0f);
    assert(fabsf(b.paddle_cx - MUSE_BREAKOUT_PADDLE_MAX_CX) < 0.01f);
    muse_breakout_resume(&b);
    assert(b.phase == MUSE_BREAKOUT_PLAYING);

    /* Pause / resume: a paused tick moves nothing, and the score
     * survives the round trip. */
    muse_breakout_restart(&b);
    break_brick(&b, 0, 0);
    assert(b.score == 1);
    float px = b.ball_x, py = b.ball_y;
    muse_breakout_pause(&b);
    tick_n(&b, 10);
    assert(b.phase == MUSE_BREAKOUT_PAUSED);
    assert(fabsf(b.ball_x - px) < 0.01f && fabsf(b.ball_y - py) < 0.01f);
    assert(b.score == 1 && b.bricks_left == MUSE_BREAKOUT_BRICK_COUNT - 1);
    muse_breakout_pause(&b); /* no-op from PAUSED */
    assert(b.phase == MUSE_BREAKOUT_PAUSED);
    muse_breakout_resume(&b);
    assert(b.phase == MUSE_BREAKOUT_PLAYING);
    muse_breakout_resume(&b); /* no-op from PLAYING */
    assert(b.phase == MUSE_BREAKOUT_PLAYING);

    /* Missed ball: one life per round, so this is OVER at once (user,
     * 2026-11-18: no revive). The engine still counts lives down, so
     * raising MUSE_BREAKOUT_LIVES restores the re-serve behaviour. */
    muse_breakout_restart(&b);
    break_brick(&b, 0, 0);
    assert(b.score == 1);
    place_ball(&b, 100.0f, 295.0f, 0.0f, MUSE_BREAKOUT_BALL_SPEED);
    muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    assert(b.event == MUSE_BREAKOUT_EV_LOST);
    assert(b.lives == 0);
    assert(b.phase == MUSE_BREAKOUT_OVER);
    assert(b.score == 1); /* kept for the settlement */

    /* Losing every life -> OVER (one life: a single miss). */
    muse_breakout_restart(&b);
    for (int life = (int)MUSE_BREAKOUT_LIVES; life > 0; life--) {
        assert(b.lives == (uint32_t)life);
        place_ball(&b, 100.0f, 295.0f, 0.0f, MUSE_BREAKOUT_BALL_SPEED);
        muse_breakout_tick(&b, MUSE_BREAKOUT_TICK_MS);
    }
    assert(b.lives == 0);
    assert(b.phase == MUSE_BREAKOUT_OVER);
    assert(b.event == MUSE_BREAKOUT_EV_LOST);
    /* OVER is terminal for tick / set_paddle; restart revives it. */
    float ox = b.ball_x, oy = b.ball_y;
    tick_n(&b, 5);
    assert(fabsf(b.ball_x - ox) < 0.01f && fabsf(b.ball_y - oy) < 0.01f);
    muse_breakout_set_paddle(&b, 180.0f);
    muse_breakout_start(&b); /* no-op from OVER */
    assert(b.phase == MUSE_BREAKOUT_OVER);
    muse_breakout_restart(&b);
    assert(b.phase == MUSE_BREAKOUT_PLAYING);
    assert(b.lives == MUSE_BREAKOUT_LIVES && b.score == 0);

    /* Clear all 24 bricks -> OVER at once, without waiting for the
     * ball to fall. */
    muse_breakout_restart(&b);
    for (int row = 0; row < MUSE_BREAKOUT_BRICK_ROWS; row++) {
        for (int col = 0; col < MUSE_BREAKOUT_BRICK_COLS; col++) {
            break_brick(&b, col, row);
        }
    }
    assert(b.score == MUSE_BREAKOUT_BRICK_COUNT);
    assert(b.bricks_left == 0);
    assert(b.phase == MUSE_BREAKOUT_OVER);

    /* Settlement boundary: a full board pays 20, not 24 (the
     * MUSE_PET_ROUND_COIN_MAX ceiling), 10 pays 10, 0 pays 0. The
     * conversion lives in muse_pet_round_coins(), never here. */
    assert(muse_pet_round_coins(24) == 20);
    assert(muse_pet_round_coins(10) == 10);
    assert(muse_pet_round_coins(0) == 0);
    assert(muse_pet_round_coins(25) == 20);

    /* Tilt: dead zone, sign, gain. Inside +-0.5 m/s^2 the paddle
     * stays put; outside, the target is centre + SIGN * GAIN * ax. */
    assert(!muse_breakout_tilt_active(0.0f));
    assert(!muse_breakout_tilt_active(0.5f));
    assert(!muse_breakout_tilt_active(-0.5f));
    assert(muse_breakout_tilt_active(0.6f));
    assert(muse_breakout_tilt_active(-0.6f));
    /* ax = -5.6 (board rolled so gravity reads -5.6 on X) moves the
     * target right: 180 + (-1) * 40 * (-5.6) = 404. The sign is the
     * 金币雨 one -- flip it and the paddle runs the wrong way. */
    assert(fabsf(muse_breakout_tilt_target(-5.6f) - 404.0f) < 0.01f);
    assert(fabsf(muse_breakout_tilt_target(5.6f) - (-44.0f)) < 0.01f);
    assert(fabsf(muse_breakout_tilt_target(0.0f) - 180.0f) < 0.01f);

    /* Paddle top speed: the tilt glide moves at most 260 px/s, i.e.
     * 13 px per 50 ms tick -- no teleport. From 96 to the far end
     * 264 (168 px) takes 13 ticks (0.65 s), while a ball at the vx
     * cap of 220 px/s needs 240 / 220 = 1.09 s to cross the field,
     * so the paddle wins the corner ball. */
    assert(fabsf(muse_breakout_glide(180.0f, 400.0f, 50) - 193.0f) < 0.01f);
    assert(fabsf(muse_breakout_glide(180.0f, -40.0f, 50) - 167.0f) < 0.01f);
    /* A nearby target is reached in one tick, not overshot. */
    assert(fabsf(muse_breakout_glide(180.0f, 186.0f, 50) - 186.0f) < 0.01f);
    float cx = 96.0f;
    int ticks = 0;
    while (cx < 264.0f && ticks < 100) {
        cx = muse_breakout_glide(cx, 264.0f, MUSE_BREAKOUT_TICK_MS);
        ticks++;
    }
    assert(fabsf(cx - 264.0f) < 0.01f);
    assert(ticks == 13); /* 168 px at 13 px/tick = 12.92 ticks */
    float ball_cross_s = (float)MUSE_BREAKOUT_FIELD_SIZE /
                         MUSE_BREAKOUT_BALL_VX_MAX;
    float paddle_run_s = (264.0f - 96.0f) / MUSE_BREAKOUT_PADDLE_MAX_PX_S;
    assert(ball_cross_s > 1.0f && paddle_run_s < 0.7f &&
           paddle_run_s < ball_cross_s);

    printf("ok\n");
    return 0;
}
