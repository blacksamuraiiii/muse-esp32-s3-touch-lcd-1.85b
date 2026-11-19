/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_breakout.c -- 打砖块 logic, see muse_breakout.h.
 *
 * One ball at 120 px/s at the serve, 24 bricks in 6x4, a 72x10 paddle,
 * one life (a miss ends the round). A paddle hit only bends vx (up to
 * +-90 px/s, clamped to +-220) and flips vy without rescaling it, so
 * |v| grows with repeated edge hits to about 241 px/s at vx = 220
 * (|vy| <= 98.3 px/s at the serve) -- the speed-up is by design, not
 * a difficulty ramp over time. Per 50 ms tick the ball moves v * 0.05
 * (6 px at serve speed, 11 px of vx at the 220 cap, ~12 px of |v| at
 * the ~241 px/s maximum) -- less than a brick's 16 px
 * thickness plus the ball's 10 px diameter, so a plain AABB overlap
 * check per tick cannot tunnel through a brick and no sweep test is
 * needed. At most one brick breaks per tick (first hit scanning rows
 * top-down); the bounce axis comes from the previous tick's ball
 * centre (above/below the brick -> flip vy, left/right -> flip vx,
 * corner ambiguity -> flip vy, bricks are wider than tall).
 * The paddle only catches while the ball is falling and its bottom
 * edge crosses the paddle's top line on this tick; the hit offset
 * bends vx by up to +-90 px/s, clamped to +-220.
 */
#include "muse_breakout.h"

#include <math.h>
#include <stdlib.h>

#define MUSE_BREAKOUT_PI 3.14159265358979323846f

static float clamp_paddle(float cx)
{
    if (cx < MUSE_BREAKOUT_PADDLE_MIN_CX) {
        return MUSE_BREAKOUT_PADDLE_MIN_CX;
    }
    if (cx > MUSE_BREAKOUT_PADDLE_MAX_CX) {
        return MUSE_BREAKOUT_PADDLE_MAX_CX;
    }
    return cx;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* (Re)serve: ball back to (180,260), paddle back to the centre, new
 * random serve angle 35..55 degrees upward, vx sign random. */
static void serve(muse_breakout_t *b)
{
    b->ball_x = MUSE_BREAKOUT_SERVE_X;
    b->ball_y = MUSE_BREAKOUT_SERVE_Y;
    b->paddle_cx = (float)(MUSE_BREAKOUT_FIELD_X + MUSE_BREAKOUT_FIELD_SIZE / 2);
    float angle_deg = (float)(MUSE_BREAKOUT_SERVE_ANGLE_MIN_DEG +
                              rand() % (MUSE_BREAKOUT_SERVE_ANGLE_MAX_DEG -
                                        MUSE_BREAKOUT_SERVE_ANGLE_MIN_DEG + 1));
    float angle = angle_deg * MUSE_BREAKOUT_PI / 180.0f;
    float vx = MUSE_BREAKOUT_BALL_SPEED * cosf(angle);
    if (rand() % 2 == 0) {
        vx = -vx;
    }
    b->ball_vx = vx;
    /* Upward: vy is negative. */
    b->ball_vy = -MUSE_BREAKOUT_BALL_SPEED * sinf(angle);
}

static void reset_game(muse_breakout_t *b)
{
    for (int i = 0; i < MUSE_BREAKOUT_BRICK_COUNT; i++) {
        b->bricks[i] = true;
    }
    b->score = 0;
    b->lives = MUSE_BREAKOUT_LIVES;
    b->bricks_left = MUSE_BREAKOUT_BRICK_COUNT;
    b->event = MUSE_BREAKOUT_EV_NONE;
    b->event_x = 0.0f;
    b->event_y = 0.0f;
    /* Ball parked at the serve spot with no velocity until start. */
    b->ball_x = MUSE_BREAKOUT_SERVE_X;
    b->ball_y = MUSE_BREAKOUT_SERVE_Y;
    b->ball_vx = 0.0f;
    b->ball_vy = 0.0f;
    b->paddle_cx = (float)(MUSE_BREAKOUT_FIELD_X + MUSE_BREAKOUT_FIELD_SIZE / 2);
}

void muse_breakout_init(muse_breakout_t *b)
{
    b->phase = MUSE_BREAKOUT_START;
    reset_game(b);
}

void muse_breakout_start(muse_breakout_t *b)
{
    if (b->phase != MUSE_BREAKOUT_START) {
        return;
    }
    reset_game(b);
    serve(b);
    b->phase = MUSE_BREAKOUT_PLAYING;
}

void muse_breakout_pause(muse_breakout_t *b)
{
    if (b->phase == MUSE_BREAKOUT_PLAYING) {
        b->phase = MUSE_BREAKOUT_PAUSED;
    }
}

void muse_breakout_resume(muse_breakout_t *b)
{
    if (b->phase == MUSE_BREAKOUT_PAUSED) {
        b->phase = MUSE_BREAKOUT_PLAYING;
    }
}

void muse_breakout_restart(muse_breakout_t *b)
{
    reset_game(b);
    serve(b);
    b->phase = MUSE_BREAKOUT_PLAYING;
}

void muse_breakout_set_paddle(muse_breakout_t *b, float cx)
{
    if (b->phase != MUSE_BREAKOUT_PLAYING) {
        return;
    }
    b->paddle_cx = clamp_paddle(cx);
}

void muse_breakout_brick_pos(int col, int row, float *x, float *y)
{
    *x = (float)(MUSE_BREAKOUT_BRICK_X0 + col * MUSE_BREAKOUT_BRICK_PITCH_X);
    *y = (float)(MUSE_BREAKOUT_BRICK_Y0 + row * MUSE_BREAKOUT_BRICK_PITCH_Y);
}

bool muse_breakout_brick_alive(const muse_breakout_t *b, int col, int row)
{
    if (col < 0 || col >= MUSE_BREAKOUT_BRICK_COLS || row < 0 ||
        row >= MUSE_BREAKOUT_BRICK_ROWS) {
        return false;
    }
    return b->bricks[row * MUSE_BREAKOUT_BRICK_COLS + col];
}

float muse_breakout_tilt_target(float ax_filt)
{
    return (float)(MUSE_BREAKOUT_FIELD_X + MUSE_BREAKOUT_FIELD_SIZE / 2) +
           MUSE_BREAKOUT_TILT_SIGN * MUSE_BREAKOUT_TILT_GAIN_PX_PER_MS2 *
               ax_filt;
}

bool muse_breakout_tilt_active(float ax_filt)
{
    return ax_filt < -MUSE_BREAKOUT_TILT_DEADZONE_MS2 ||
           ax_filt > MUSE_BREAKOUT_TILT_DEADZONE_MS2;
}

float muse_breakout_glide(float from, float target, uint32_t ms)
{
    float step = MUSE_BREAKOUT_PADDLE_MAX_PX_S * (float)ms / 1000.0f;
    if (target > from + step) {
        return from + step;
    }
    if (target < from - step) {
        return from - step;
    }
    return target;
}

void muse_breakout_tick(muse_breakout_t *b, uint32_t ms)
{
    if (b->phase != MUSE_BREAKOUT_PLAYING) {
        return;
    }
    b->event = MUSE_BREAKOUT_EV_NONE;
    float dt = (float)ms / 1000.0f;
    float prev_x = b->ball_x;
    float prev_y = b->ball_y;
    b->ball_x += b->ball_vx * dt;
    b->ball_y += b->ball_vy * dt;
    float r = (float)MUSE_BREAKOUT_BALL_R;

    /* Walls: left / right flip vx, top flips vy. The ball centre is
     * clamped 5 px inside the field (the ball's radius). Wall bounces
     * are silent (too frequent for a sound). */
    if (b->ball_x < (float)MUSE_BREAKOUT_FIELD_X + r) {
        b->ball_x = (float)MUSE_BREAKOUT_FIELD_X + r;
        b->ball_vx = fabsf(b->ball_vx);
    } else if (b->ball_x > (float)(MUSE_BREAKOUT_FIELD_X +
                                   MUSE_BREAKOUT_FIELD_SIZE) - r) {
        b->ball_x = (float)(MUSE_BREAKOUT_FIELD_X + MUSE_BREAKOUT_FIELD_SIZE) - r;
        b->ball_vx = -fabsf(b->ball_vx);
    }
    if (b->ball_y < (float)MUSE_BREAKOUT_FIELD_Y + r) {
        b->ball_y = (float)MUSE_BREAKOUT_FIELD_Y + r;
        b->ball_vy = fabsf(b->ball_vy);
    }

    /* Bricks: at most one per tick, rows scanned top-down. The bounce
     * axis comes from the previous tick's ball centre: above/below the
     * brick -> flip vy, left/right -> flip vx, corner ambiguity ->
     * flip vy (bricks are wider than tall, so a vertical bounce is
     * the common case). The ball is then pushed out of the overlap
     * along the bounce axis, so the next tick cannot re-hit the same
     * brick. */
    for (int row = 0; row < MUSE_BREAKOUT_BRICK_ROWS; row++) {
        bool broke = false;
        for (int col = 0; col < MUSE_BREAKOUT_BRICK_COLS; col++) {
            int idx = row * MUSE_BREAKOUT_BRICK_COLS + col;
            if (!b->bricks[idx]) {
                continue;
            }
            float bx, by;
            muse_breakout_brick_pos(col, row, &bx, &by);
            float br = bx + (float)MUSE_BREAKOUT_BRICK_W;
            float bb = by + (float)MUSE_BREAKOUT_BRICK_H;
            if (b->ball_x + r <= bx || b->ball_x - r >= br ||
                b->ball_y + r <= by || b->ball_y - r >= bb) {
                continue;
            }
            /* Overlap: break it. */
            b->bricks[idx] = false;
            b->bricks_left--;
            b->score++;
            b->event = MUSE_BREAKOUT_EV_BRICK;
            b->event_x = bx + (float)MUSE_BREAKOUT_BRICK_W / 2.0f;
            b->event_y = by + (float)MUSE_BREAKOUT_BRICK_H / 2.0f;
            bool vertical;
            if (prev_y + r <= by || prev_y - r >= bb) {
                vertical = true;
            } else if (prev_x + r <= bx || prev_x - r >= br) {
                vertical = false;
            } else {
                vertical = true; /* corner ambiguity: flip vy */
            }
            if (vertical) {
                if (prev_y < by + (float)MUSE_BREAKOUT_BRICK_H / 2.0f) {
                    /* Came from above: leave above the brick. */
                    b->ball_y = by - r - 0.5f;
                    b->ball_vy = -fabsf(b->ball_vy);
                } else {
                    b->ball_y = bb + r + 0.5f;
                    b->ball_vy = fabsf(b->ball_vy);
                }
            } else {
                if (prev_x < bx + (float)MUSE_BREAKOUT_BRICK_W / 2.0f) {
                    b->ball_x = bx - r - 0.5f;
                    b->ball_vx = -fabsf(b->ball_vx);
                } else {
                    b->ball_x = br + r + 0.5f;
                    b->ball_vx = fabsf(b->ball_vx);
                }
            }
            broke = true;
            break;
        }
        if (broke) {
            break;
        }
    }
    if (b->bricks_left == 0) {
        /* All 24 broken: OVER at once, no waiting for the ball. */
        b->phase = MUSE_BREAKOUT_OVER;
        return;
    }

    /* Paddle: only while falling (vy > 0) and only when this tick's
     * ball bottom crossed the paddle's top line, and only inside the
     * catch span (half a paddle + one ball radius = 41 px). The hit
     * offset bends vx; vy flips up; the ball is parked on top of the
     * paddle so it cannot stick. */
    float prev_bottom = prev_y + r;
    float new_bottom = b->ball_y + r;
    if (b->ball_vy > 0.0f && prev_bottom < (float)MUSE_BREAKOUT_PADDLE_Y &&
        new_bottom >= (float)MUSE_BREAKOUT_PADDLE_Y &&
        fabsf(b->ball_x - b->paddle_cx) <= 41.0f) {
        float off = (b->ball_x - b->paddle_cx) / 36.0f;
        off = clampf(off, -1.0f, 1.0f);
        b->ball_vx = clampf(b->ball_vx +
                                off * MUSE_BREAKOUT_PADDLE_KICK_PX_S,
                            -MUSE_BREAKOUT_BALL_VX_MAX,
                            MUSE_BREAKOUT_BALL_VX_MAX);
        b->ball_vy = -fabsf(b->ball_vy);
        b->ball_y = (float)MUSE_BREAKOUT_PADDLE_Y - r;
        if (b->event == MUSE_BREAKOUT_EV_NONE) {
            b->event = MUSE_BREAKOUT_EV_PADDLE;
        }
        return;
    }

    /* Fell out of the bottom: lose a life. Lives left -> ball and
     * paddle reset and a fresh serve, score kept, no re-charge. No
     * lives left -> OVER. With MUSE_BREAKOUT_LIVES 1 this re-serve
     * branch is unreachable (lives hits 0 at once); restoring the
     * constant to 3 revives it, and the re-serve path then needs a
     * host test -- the harness "lose every life" loop only runs once
     * at 1, so it never exercises a re-serve. */
    if (b->ball_y > (float)(MUSE_BREAKOUT_FIELD_Y + MUSE_BREAKOUT_FIELD_SIZE)) {
        b->event = MUSE_BREAKOUT_EV_LOST;
        if (b->lives > 0) {
            b->lives--;
        }
        if (b->lives == 0) {
            b->phase = MUSE_BREAKOUT_OVER;
            return;
        }
        serve(b);
    }
}
