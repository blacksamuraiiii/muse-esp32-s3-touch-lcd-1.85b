/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_tictactoe_ui.c -- OX棋 subpage, see muse_tictactoe_ui.h.
 *
 * Layout (mockup v4, 360x360 round screen, dark theme like the other
 * games): a 240x240 board at (60,60), 3x3 grid, cells 80 px. The top
 * crescent shows the title (y=38, the same slot and ACCENT colour the
 * snake page uses for its name): "OX棋" while START / PLAYING / PAUSED
 * ("已暂停" while paused, user 2026-11-08: the pause title moves to
 * the title slot on every game, so the pause card is just the 继续
 * button), and the result itself at OVER -- "你赢了！" gold, "平局",
 * "你输了" (mockup v4). The line under the field (y=308, the slot the
 * snake page prints its score on) never rides on the bezel: while
 * PLAYING or PAUSED it is white and reads "轮到你了", and at OVER it
 * turns GOLD and reads "+m 金币" with m the gross payout. There is no
 * pause button any more (2026-11-01, user request "参考贪吃蛇"): the
 * old 34 px one sat outside the round bezel. Pausing is a LONG HOLD
 * anywhere on the board (finger within ~12 px for 500 ms), and the
 * back arrow (which is hidden while playing, as on snake) becomes
 * reachable through it. The START overlay is NOT dimmed (the board
 * stays visible behind it); the PAUSED and OVER overlays are 50%
 * black dim.
 *
 * Overlays follow the snake page (2026-11-01, one style across the
 * mini games): the PAUSED card has a 98x66 ACCENT-bordered centre
 * button, and the OVER card has the price "- 10 金币" on its own line
 * under the replay label -- on_restart charges the fee the moment it
 * is pressed, so the price belongs on the button. The START button is
 * a single line, "开始" (the fee is never on it, exactly like the
 * other games -- the user caught the v2 mockup showing it there).
 * The START overlay carries the hint line at y=304 (DIM, same slot as
 * the snake page's hint): "点格子下棋，长按棋盘暂停".
 *
 * Payout (the RPS model, user decision 2026-11-08): at OVER the HUD
 * turns gold and reads "+m 金币" with m the GROSS payout the wallet
 * actually accepted (s_round_coins, from muse_tictactoe_payout_coins()
 * -- win 20, draw 10, loss 0, settled once). The net is deliberately
 * NOT printed: the user said kids cannot tell net apart, so the
 * result shows what was added, not what was gained after the 10-coin
 * entry fee.
 *
 * Message slot: the band under the field (y=308) hosts exactly one
 * line -- the refusal note wins it, otherwise the turn / payout line
 * does. The note shares the slot, which is why it carries the short
 * copy (the chord under the field is only ~213 px at y=325).
 *
 * Controls: tap a cell to place X (the player is X and moves first);
 * the AI (O) answers after a 500 ms beat so the player's mark is seen
 * before the answer lands. The placement fires on
 * release-after-a-short-press, never on press-down -- a hold-to-pause
 * never places a piece, which is what resolves the centre-cell
 * conflict (tapping the centre places a piece, holding it pauses;
 * the two are mutually exclusive by duration). The hold is tracked
 * by hand, not LVGL's LONG_PRESSED, for the same reason as goldrain:
 * the tap and the hold share the board, so the press must be
 * classified by duration, not by location.
 *
 * Marks are drawn, not assets: X is two lv_line strokes (the snake /
 * sudoku line idiom: LVGL keeps the point array pointer, so each
 * line has its own static storage), O is a bordered ring (a plain
 * object with an 8 px border and a transparent fill), and the win
 * line is one gold lv_line through the winning line's first and last
 * cells. X is DANGER red and O is OK green (mockup v4, user decision
 * 2026-11-08: a red/green contrast pair).
 */
#include "muse_tictactoe_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_font.h"
#include "muse_widgets.h"
#include "muse_pet.h"        /* snapshot + request: the entry fee */
#include "muse_pet_model.h"
#include "muse_theme.h"
#include "muse_tictactoe.h"
#include "muse_state.h"     /* poke: no auto-sleep while a round runs */

/* Hold to pause: the finger stays within this radius for this long. */
#define MUSE_TICTACTOE_HOLD_MS 500u
#define MUSE_TICTACTOE_HOLD_MOVE_PX 12
/* Timer period: the hold check and the AI beat both ride on it. */
#define MUSE_TICTACTOE_TICK_MS 50u
/* Beat between the player's move and the AI's answer, so the player's
 * mark is seen before the answer lands. */
#define MUSE_TICTACTOE_AI_DELAY_MS 500u
/* Mark geometry inside an 80 px cell (mockup v4): the X spans 56 px
 * on 8 px strokes, the O is a 56 px ring on an 8 px border, and the
 * win line is 6 px gold. The grid lines are 3 px DIM. */
#define MUSE_TICTACTOE_MARK_HALF 28
#define MUSE_TICTACTOE_X_WIDTH 8
#define MUSE_TICTACTOE_O_WIDTH 8
#define MUSE_TICTACTOE_WIN_WIDTH 6
#define MUSE_TICTACTOE_GRID_WIDTH 3
/* The board's fill, one step lighter than the page (mockup v4): the
 * grid needs a visible boundary on the black page. */
#define MUSE_TICTACTOE_FIELD_BG 0x0d0c14

static muse_tictactoe_t s_game;
static bool s_seeded = false;
/* Round payout bookkeeping, same shape as the goldrain page: converted
 * once at OVER, s_round_coins holds what the wallet actually accepted. */
static bool s_settled = false;
static uint32_t s_round_coins = 0;
/* The AI's answer is pending until this tick, set when the player's
 * move leaves the game playing. */
static bool s_ai_pending = false;
static uint32_t s_ai_due_ms = 0;
/* Press tracking for the tap-to-place and the hold-to-pause. */
static bool s_press_active = false;
static bool s_press_moved = false;
static uint32_t s_press_start_ms = 0;
static lv_coord_t s_press_x = 0;
static lv_coord_t s_press_y = 0;
static int s_press_cell = -1;

static lv_obj_t *s_x_line[MUSE_TICTACTOE_CELLS][2];
static lv_obj_t *s_o_ring[MUSE_TICTACTOE_CELLS];
static lv_obj_t *s_win_line;
static lv_obj_t *s_touch;
static lv_obj_t *s_start_ov;
static lv_obj_t *s_paused_ov;
static lv_obj_t *s_over_ov;
static lv_obj_t *s_back;
static lv_obj_t *s_name;
static lv_obj_t *s_hud;
static lv_obj_t *s_note;
static lv_obj_t *s_page;
static lv_timer_t *s_timer;

/* Line point storage: LVGL keeps the point array pointer (it does not
 * copy it), so each line needs its own storage, not one shared pair
 * (the snake / sudoku idiom). The geometry is fixed per cell, so the
 * arrays are filled once at build; only the win line's points move,
 * when a game ends on a line. */
static lv_point_precise_t s_grid_pts[4][2];
static lv_point_precise_t s_x_pts[MUSE_TICTACTOE_CELLS][2][2];
static lv_point_precise_t s_win_pts[2];

/* Status / message slot under the field (field ends at y=300): the HUD
 * and the refusal note share it, one at a time, so both are below the
 * play area (user request 2026-11-01). ~213 px of chord at the line's
 * bottom (325) and the longest string here ("点格子下棋，长按棋盘暂停"
 * is the START hint, not this slot; the HUD's longest is "+20 金币"). */
#define MUSE_TICTACTOE_HUD_Y 308
#define MUSE_TICTACTOE_NOTE_Y 308

/* --- small helpers, same shapes as the goldrain page ------------- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *fill(lv_obj_t *parent, uint32_t bg)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

/* One line from caller-owned point storage (see the note above). */
static lv_obj_t *line(lv_obj_t *parent, lv_point_precise_t *pts,
                      int x1, int y1, int x2, int y2, int width,
                      uint32_t color)
{
    pts[0].x = x1;
    pts[0].y = y1;
    pts[1].x = x2;
    pts[1].y = y2;
    lv_obj_t *o = lv_line_create(parent);
    lv_line_set_points(o, pts, 2);
    lv_obj_set_style_line_width(o, width, 0);
    lv_obj_set_style_line_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(o, true, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* The band under the field holds one line: the refusal note wins it,
 * the turn / payout line takes it otherwise (and stays hidden on
 * START, where there is no round to report). Without this the note
 * and the HUD printed on top of each other at y=308. */
static void sync_message_band(void)
{
    bool note_up = s_note && !lv_obj_has_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    if (s_hud) {
        if (note_up || s_game.phase == MUSE_TICTACTOE_START) {
            lv_obj_add_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* lv_label_set_text has no same-text short circuit in LVGL 9.5: every
 * call frees, reallocs and re-lays-out the label. refresh_display()
 * runs every 50 ms tick, so skip when nothing changed (same trick as
 * muse_menu's set_text). */
static void set_label(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static void show_note(const char *text)
{
    lv_label_set_text(s_note, text);
    lv_obj_clear_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    sync_message_band();
}

static void hide_note(void)
{
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    sync_message_band();
}

/* Cell (0..8, row-major) for a point on the page, or -1 when the
 * point is outside the board. */
static int cell_at(lv_coord_t x, lv_coord_t y)
{
    if (x < MUSE_TICTACTOE_FIELD_X ||
        x >= MUSE_TICTACTOE_FIELD_X + MUSE_TICTACTOE_FIELD_SIZE ||
        y < MUSE_TICTACTOE_FIELD_Y ||
        y >= MUSE_TICTACTOE_FIELD_Y + MUSE_TICTACTOE_FIELD_SIZE) {
        return -1;
    }
    int c = (x - MUSE_TICTACTOE_FIELD_X) / MUSE_TICTACTOE_CELL;
    int r = (y - MUSE_TICTACTOE_FIELD_Y) / MUSE_TICTACTOE_CELL;
    return r * 3 + c;
}

static void cell_center(int cell, int *cx, int *cy)
{
    int c = cell % 3;
    int r = cell / 3;
    *cx = MUSE_TICTACTOE_FIELD_X + c * MUSE_TICTACTOE_CELL +
          MUSE_TICTACTOE_CELL / 2;
    *cy = MUSE_TICTACTOE_FIELD_Y + r * MUSE_TICTACTOE_CELL +
          MUSE_TICTACTOE_CELL / 2;
}

/* --- display sync ------------------------------------------------- */

static void refresh_display(void)
{
    for (int i = 0; i < MUSE_TICTACTOE_CELLS; i++) {
        if (s_game.cells[i] == MUSE_TICTACTOE_X) {
            lv_obj_clear_flag(s_x_line[i][0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_x_line[i][1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_o_ring[i], LV_OBJ_FLAG_HIDDEN);
        } else if (s_game.cells[i] == MUSE_TICTACTOE_O) {
            lv_obj_add_flag(s_x_line[i][0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_x_line[i][1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_o_ring[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_x_line[i][0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_x_line[i][1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_o_ring[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    muse_tictactoe_phase_t ph = s_game.phase;
    /* Win line: gold through the winning line's first and last cells,
     * only at OVER on a win or a loss (a draw has no line). */
    if (ph == MUSE_TICTACTOE_OVER && s_game.win_line >= 0) {
        int first = -1, last = -1;
        muse_tictactoe_line_cells(s_game.win_line, &first, &last);
        if (first >= 0 && last >= 0) {
            int x0, y0, x1, y1;
            cell_center(first, &x0, &y0);
            cell_center(last, &x1, &y1);
            s_win_pts[0].x = x0;
            s_win_pts[0].y = y0;
            s_win_pts[1].x = x1;
            s_win_pts[1].y = y1;
            lv_line_set_points(s_win_line, s_win_pts, 2);
            lv_obj_clear_flag(s_win_line, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_win_line, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(s_win_line, LV_OBJ_FLAG_HIDDEN);
    }
    /* Title: "OX棋" except at OVER, where the title slot itself
     * becomes the result (mockup v4), and at PAUSED, where it becomes
     * "已暂停" (user 2026-11-08: the pause title moves to the title
     * slot on every game, so the pause card is just the 继续 button). */
    const char *title = "OX棋";
    uint32_t title_color = MUSE_THEME_ACCENT;
    if (ph == MUSE_TICTACTOE_OVER) {
        if (s_game.outcome == MUSE_TICTACTOE_WIN) {
            title = "你赢了！";
            title_color = MUSE_THEME_GOLD;
        } else if (s_game.outcome == MUSE_TICTACTOE_DRAW) {
            title = "平局";
            title_color = MUSE_THEME_TEXT;
        } else {
            title = "你输了";
            title_color = MUSE_THEME_DANGER;
        }
    } else if (ph == MUSE_TICTACTOE_PAUSED) {
        title = "已暂停";
    }
    set_label(s_name, title);
    lv_obj_set_style_text_color(s_name, lv_color_hex(title_color),
                                LV_PART_MAIN);
    /* HUD, same slot as the snake page's score line: while PLAYING or
     * PAUSED it is white and reads "轮到你了" -- no coin conversion
     * mid-round. At OVER it turns GOLD and adds the gross payout the
     * wallet actually accepted; the net is deliberately not printed
     * (user: kids cannot tell net apart). A refused settlement leaves
     * s_round_coins at 0, so the line honestly reads +0. */
    char hud[48];
    if (ph == MUSE_TICTACTOE_OVER) {
        snprintf(hud, sizeof(hud), "+%lu 金币",
                 (unsigned long)s_round_coins);
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_GOLD),
                                    LV_PART_MAIN);
    } else {
        snprintf(hud, sizeof(hud), "轮到你了");
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_TEXT),
                                    LV_PART_MAIN);
    }
    set_label(s_hud, hud);
    if (ph == MUSE_TICTACTOE_START) {
        lv_obj_clear_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_start_ov, LV_OBJ_FLAG_HIDDEN);
    }
    if (ph == MUSE_TICTACTOE_PAUSED) {
        lv_obj_clear_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_paused_ov, LV_OBJ_FLAG_HIDDEN);
    }
    if (ph == MUSE_TICTACTOE_OVER) {
        lv_obj_clear_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    }
    /* Touch only while playing. The back arrow is visible on every
     * overlay and hidden while playing (snake's rule): leaving
     * mid-game means holding the board to pause first, so a stray tap
     * on the rim can never abandon a round. */
    if (ph == MUSE_TICTACTOE_PLAYING) {
        lv_obj_clear_flag(s_touch, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_touch, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    }
    /* Which of the two lines under the field is the one on screen. */
    sync_message_band();
}

/* --- entry fee (same snapshot gate + PET_FEE pattern as snake) ---- */

static bool charge_for_game(void)
{
    muse_pet_model_t snap;
    if (!muse_pet_snapshot(&snap)) {
        show_note("互动未就绪");
        return false;
    }
    if (snap.coins < MUSE_PET_GAME_STAKE) {
        /* Short copy on purpose (see MUSE_TICTACTOE_NOTE_Y): the slot
         * under the field leaves ~213 px of chord and the long sentence
         * is ~272 px wide. Same wording as the sudoku page's note. */
        char note[32];
        snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
                 (unsigned)MUSE_PET_GAME_STAKE);
        show_note(note);
        return false;
    }
    if (!muse_pet_request(PET_FEE)) {
        show_note("互动未就绪");
        return false;
    }
    hide_note();
    return true;
}

static void seed_once(void)
{
    if (!s_seeded) {
        srand(lv_tick_get());
        s_seeded = true;
    }
}

/* --- game actions ------------------------------------------------- */

static void begin_game(bool restart)
{
    if (!charge_for_game()) {
        return;
    }
    seed_once();
    /* A new round: its payout has not been settled yet. */
    s_settled = false;
    s_round_coins = 0;
    s_ai_pending = false;
    if (restart) {
        muse_tictactoe_restart(&s_game);
    } else {
        muse_tictactoe_start(&s_game);
    }
    lv_timer_resume(s_timer);
    refresh_display();
}

static void on_start(lv_event_t *e)
{
    (void)e;
    begin_game(false);
}

static void on_restart(lv_event_t *e)
{
    (void)e;
    begin_game(true);
}

static void on_resume(lv_event_t *e)
{
    (void)e;
    muse_tictactoe_resume(&s_game);
    lv_timer_resume(s_timer);
    refresh_display();
}

static void pause_game(void)
{
    muse_tictactoe_pause(&s_game);
    lv_timer_pause(s_timer);
    refresh_display();
}

/* Payout, once per round, at OVER (user decision 2026-11-08). The
 * 10-coin entry fee was already charged at start through PET_FEE, so
 * the round's net is payout minus 10, computed nowhere and never
 * printed (kids cannot tell net apart: the card shows what was added,
 * not what was gained). A refused request (worker not ready / queue
 * full) leaves s_round_coins at 0 so the card never claims coins the
 * wallet did not get. */
static void settle_round(void)
{
    if (s_settled) {
        return;
    }
    s_settled = true;
    uint32_t coins = muse_tictactoe_payout_coins(s_game.outcome);
    s_round_coins = 0;
    if (coins != 0 && muse_pet_request_play_reward(coins)) {
        s_round_coins = coins;
    }
}

/* After a move landed (the player's, or the AI's): settle when the
 * game ended, otherwise schedule the AI's answer a beat later. */
static void after_move(void)
{
    if (s_game.phase == MUSE_TICTACTOE_OVER) {
        settle_round();
    } else if (s_game.ai_turn) {
        s_ai_pending = true;
        s_ai_due_ms = lv_tick_get() + MUSE_TICTACTOE_AI_DELAY_MS;
    }
    refresh_display();
}

static void on_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_game.phase != MUSE_TICTACTOE_PLAYING) {
        return;
    }
    /* A running round is activity: playing by taps alone still
     * touches the screen, but the AI beat is 500 ms of no touch, so
     * without this the idle timer could sleep the screen mid-round
     * (same idiom as video playback and a voice turn). */
    muse_state_poke();
    /* Hold to pause: the finger stayed within ~12 px for 500 ms. A
     * press that moved never reaches this branch (s_press_moved), and
     * the release after a hold places nothing (see on_touch_released):
     * the hold wins over the tap by duration, which is what keeps the
     * centre cell from placing a piece when it is held to pause. */
    if (s_press_active && !s_press_moved &&
        lv_tick_get() - s_press_start_ms >= MUSE_TICTACTOE_HOLD_MS) {
        s_press_active = false;
        pause_game();
        return;
    }
    /* The AI answers the player's move after a beat. */
    if (s_ai_pending && (int32_t)(lv_tick_get() - s_ai_due_ms) >= 0) {
        s_ai_pending = false;
        muse_tictactoe_ai_move(&s_game);
        after_move();
        return;
    }
    refresh_display();
}

/* --- touch (tap to place, hold to pause) -------------------------- */

static void on_touch_pressed(lv_event_t *e)
{
    (void)e;
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    s_press_active = true;
    s_press_moved = false;
    s_press_start_ms = lv_tick_get();
    s_press_x = point.x;
    s_press_y = point.y;
    s_press_cell = cell_at(point.x, point.y);
    /* Nothing is placed here: placement fires on release-after-a-
     * short-press, never on press-down, so a hold-to-pause never
     * places a piece. */
}

static void on_touch_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_press_active || s_game.phase != MUSE_TICTACTOE_PLAYING) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    int dx = point.x - s_press_x;
    int dy = point.y - s_press_y;
    if (dx * dx + dy * dy >
        MUSE_TICTACTOE_HOLD_MOVE_PX * MUSE_TICTACTOE_HOLD_MOVE_PX) {
        s_press_moved = true;
    }
}

static void on_touch_released(lv_event_t *e)
{
    (void)e;
    bool was_active = s_press_active;
    int press_cell = s_press_cell;
    s_press_active = false;
    if (!was_active || s_game.phase != MUSE_TICTACTOE_PLAYING) {
        return;
    }
    /* A hold already paused (the tick fired pause_game, and the touch
     * layer is hidden while paused): the release places nothing. A
     * slow release past the hold time without the tick having fired
     * yet is the same -- the press was a hold, not a tap. */
    if (s_press_moved ||
        lv_tick_get() - s_press_start_ms >= MUSE_TICTACTOE_HOLD_MS) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    int cell = cell_at(point.x, point.y);
    /* A tap places only when press and release agree on the cell: a
     * swipe across the board never places in the wrong cell. An
     * occupied cell is refused by the logic and changes nothing. */
    if (cell < 0 || cell != press_cell) {
        return;
    }
    if (muse_tictactoe_play(&s_game, cell)) {
        after_move();
    }
}

/* --- build -------------------------------------------------------- */

static lv_obj_t *overlay(lv_obj_t *page, bool dim)
{
    lv_obj_t *ov = fill(page, MUSE_THEME_CARD);
    lv_obj_set_size(ov, 360, 360);
    lv_obj_set_pos(ov, 0, 0);
    if (dim) {
        lv_obj_set_style_bg_color(ov, lv_color_hex(MUSE_THEME_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ov, LV_OPA_50, LV_PART_MAIN);
    } else {
        /* Undimmed START overlay: transparent, so the board stays
         * visible behind the start button. */
        lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    return ov;
}

static lv_obj_t *centre_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    /* The shared card, a 98x66 rounded rect (user decision 2026-11-01,
     * was a 110 px circle): the ACCENT ring is this page's own accent on
     * top of the shared CARD / CARD_PRESSED base. */
    lv_obj_t *btn = muse_card(parent, 98, 66, 14);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_border_color(btn, lv_color_hex(MUSE_THEME_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    return btn;
}

/* One O ring: a 56 px circle drawn as an 8 px border on a transparent
 * box (the bomb body in goldrain is the filled-circle version of the
 * same idiom). Built once per cell and shown only when that cell
 * holds an O. */
static lv_obj_t *o_ring_obj(lv_obj_t *page, int cx, int cy)
{
    lv_obj_t *o = lv_obj_create(page);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, MUSE_TICTACTOE_MARK_HALF * 2,
                    MUSE_TICTACTOE_MARK_HALF * 2);
    lv_obj_set_pos(o, cx - MUSE_TICTACTOE_MARK_HALF,
                   cy - MUSE_TICTACTOE_MARK_HALF);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, MUSE_TICTACTOE_O_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, lv_color_hex(MUSE_THEME_OK), LV_PART_MAIN);
    lv_obj_set_style_radius(o, MUSE_TICTACTOE_MARK_HALF, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

void muse_tictactoe_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    muse_tictactoe_init(&s_game);
    s_page = page;

    /* Board: a 240x240 field at (60,60), one step lighter than the
     * page, with a RULE outline (mockup v4). The grid lines sit on
     * top of it; the marks sit on top of those. */
    lv_obj_t *field = fill(page, MUSE_TICTACTOE_FIELD_BG);
    lv_obj_set_size(field, MUSE_TICTACTOE_FIELD_SIZE,
                    MUSE_TICTACTOE_FIELD_SIZE);
    lv_obj_set_pos(field, MUSE_TICTACTOE_FIELD_X, MUSE_TICTACTOE_FIELD_Y);
    lv_obj_set_style_border_width(field, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(field, lv_color_hex(MUSE_THEME_RULE),
                                  LV_PART_MAIN);
    /* Grid: two verticals at x=140/220, two horizontals at y=140/220,
     * 3 px DIM. */
    line(page, s_grid_pts[0], 140, 60, 140, 300, MUSE_TICTACTOE_GRID_WIDTH,
         MUSE_THEME_DIM);
    line(page, s_grid_pts[1], 220, 60, 220, 300, MUSE_TICTACTOE_GRID_WIDTH,
         MUSE_THEME_DIM);
    line(page, s_grid_pts[2], 60, 140, 300, 140, MUSE_TICTACTOE_GRID_WIDTH,
         MUSE_THEME_DIM);
    line(page, s_grid_pts[3], 60, 220, 300, 220, MUSE_TICTACTOE_GRID_WIDTH,
         MUSE_THEME_DIM);
    /* Marks: two X strokes and one O ring per cell, all built once and
     * hidden until the cell holds that mark. The X is DANGER red, the
     * O is OK green (mockup v4, a red/green contrast pair). */
    for (int i = 0; i < MUSE_TICTACTOE_CELLS; i++) {
        int cx, cy;
        cell_center(i, &cx, &cy);
        int h = MUSE_TICTACTOE_MARK_HALF;
        s_x_line[i][0] = line(page, s_x_pts[i][0], cx - h, cy - h, cx + h,
                              cy + h, MUSE_TICTACTOE_X_WIDTH,
                              MUSE_THEME_DANGER);
        s_x_line[i][1] = line(page, s_x_pts[i][1], cx + h, cy - h, cx - h,
                              cy + h, MUSE_TICTACTOE_X_WIDTH,
                              MUSE_THEME_DANGER);
        lv_obj_add_flag(s_x_line[i][0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_x_line[i][1], LV_OBJ_FLAG_HIDDEN);
        s_o_ring[i] = o_ring_obj(page, cx, cy);
    }
    /* Win line: gold, 6 px, through the winning line's first and last
     * cells. Its points are set in refresh_display() when a game ends
     * on a line; hidden until then. */
    s_win_line = line(page, s_win_pts, 0, 0, 0, 0, MUSE_TICTACTOE_WIN_WIDTH,
                      MUSE_THEME_GOLD);
    lv_obj_add_flag(s_win_line, LV_OBJ_FLAG_HIDDEN);

    /* Touch layer: full page, only while playing, above the board.
     * fill() strips CLICKABLE, so it is added back: without it the
     * layer would never receive PRESSED events. */
    s_touch = fill(page, MUSE_THEME_BG);
    lv_obj_set_style_bg_opa(s_touch, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_size(s_touch, 360, 360);
    lv_obj_set_pos(s_touch, 0, 0);
    lv_obj_add_flag(s_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_touch, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_touch, on_touch_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_touch, on_touch_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_touch, on_touch_released, LV_EVENT_RELEASED, NULL);

    /* START overlay: not dimmed, the board stays visible behind it. */
    s_start_ov = overlay(page, false);
    lv_obj_t *start_btn = centre_btn(s_start_ov, 131, 147);
    lv_obj_t *start_lb = label(start_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(start_lb, "开始");
    lv_obj_center(start_lb);
    lv_obj_add_event_cb(start_btn, on_start, LV_EVENT_CLICKED, NULL);
    /* Hint line (mockup v4): same slot as the snake page's hint. The
     * pause is a hold anywhere on the board, not just the middle, so
     * the copy says 棋盘, not 中间. */
    lv_obj_t *hint_lb = label(s_start_ov, muse_font_cjk(), MUSE_THEME_DIM);
    lv_label_set_text(hint_lb, "点格子下棋，长按棋盘暂停");
    lv_obj_align(hint_lb, LV_ALIGN_TOP_MID, 0, 304);

    /* PAUSED overlay: dim, 继续 button with the PLAY symbol above
     * the label (goldrain's card). No "已暂停" label on the card any
     * more: the page title itself becomes "已暂停" while paused
     * (user 2026-11-08). No restart capsule: it charged 10 coins on
     * a single tap in the narrowest part of the bezel; the back
     * arrow leaves, and 开始 charges again on the next visit. */
    s_paused_ov = overlay(page, true);
    lv_obj_t *resume_btn = centre_btn(s_paused_ov, 131, 147);
    lv_obj_t *resume_sym = label(resume_btn, &lv_font_montserrat_20, MUSE_THEME_TEXT);
    lv_label_set_text(resume_sym, LV_SYMBOL_PLAY);
    lv_obj_align(resume_sym, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_t *resume_lb = label(resume_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(resume_lb, "继续");
    lv_obj_align(resume_lb, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_add_event_cb(resume_btn, on_resume, LV_EVENT_CLICKED, NULL);

    /* OVER overlay: dim + 再来一局 button only (UI polish 2026-11-01).
     * The result title lives in the page-title slot and the result line
     * under the field -- both page-level, above the dim -- so the overlay
     * itself carries no title and no payout line any more. */
    s_over_ov = overlay(page, true);
    lv_obj_t *again_btn = centre_btn(s_over_ov, 131, 147);
    lv_obj_t *again_lb = label(again_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(again_lb, "再来一局");
    lv_obj_align(again_lb, LV_ALIGN_TOP_MID, 0, 12);
    /* on_restart charges through charge_for_game() the moment it is
     * pressed, so the price belongs on the button (same as snake; the
     * amount comes from the macro, not a literal). */
    char price_buf[16];
    snprintf(price_buf, sizeof(price_buf), "- %u 金币",
             (unsigned)MUSE_PET_GAME_STAKE);
    lv_obj_t *again_price = label(again_btn, muse_font_cjk(), MUSE_THEME_GOLD);
    lv_label_set_text(again_price, price_buf);
    lv_obj_align(again_price, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_add_event_cb(again_btn, on_restart, LV_EVENT_CLICKED, NULL);

    /* Back arrow: the shared transparent one (no filled background, user
     * request 2026-11-01), same geometry and symbol as snake / goldrain.
     * Hidden while playing. Created after the overlays so it stays
     * clickable on them. */
    s_back = muse_back_button(page, back_cb);

    /* Title (top crescent, y=38: snake's name slot, shared subpage title
     * font/colour) and the result line right under the field
     * (MUSE_TICTACTOE_HUD_Y = 308, snake's score slot): page-level, above
     * the dim overlays, so they stay readable in every state.
     * refresh_display() swaps the title to the result at OVER. */
    s_name = muse_page_title(page, "OX棋", 38);
    s_hud = label(page, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_obj_align(s_hud, LV_ALIGN_TOP_MID, 0, MUSE_TICTACTOE_HUD_Y);

    /* Refusal note: below the field, sharing the score line's slot (user
     * request 2026-11-01). */
    s_note = label(page, muse_font_cjk(), MUSE_THEME_DANGER);
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, MUSE_TICTACTOE_NOTE_Y);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(on_tick, MUSE_TICTACTOE_TICK_MS, NULL);
    lv_timer_pause(s_timer);

    refresh_display();
}

void muse_tictactoe_ui_show(void)
{
    refresh_display();
}

void muse_tictactoe_ui_hide(void)
{
    if (s_game.phase == MUSE_TICTACTOE_PLAYING) {
        muse_tictactoe_pause(&s_game);
        lv_timer_pause(s_timer);
    }
    s_press_active = false;
    refresh_display();
}

void muse_tictactoe_ui_fee_refused(void)
{
    if (s_game.phase == MUSE_TICTACTOE_PLAYING ||
        s_game.phase == MUSE_TICTACTOE_PAUSED) {
        /* The game never settled: back to START, no score kept. */
        muse_tictactoe_init(&s_game);
        lv_timer_pause(s_timer);
    }
    char note[32];
    snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
             (unsigned)MUSE_PET_GAME_STAKE);
    show_note(note);
    refresh_display();
}
