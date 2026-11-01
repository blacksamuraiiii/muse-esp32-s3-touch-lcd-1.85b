/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_snake_ui.c -- Snake subpage (mockup snake-mockup-v1, user
 * decision 2026-10-31). See muse_snake_ui.h for the ownership contract
 * and muse_snake.h for the game rules.
 *
 * Geometry (360x360-circle coordinates, same system as the interact
 * file; all positions are absolute inside the 360x360 page group):
 *   - Board: 240x240 at (60,60), cells 20px, 12x12. The page name
 *     "贪吃蛇" sits above the board in the ACCENT colour page_header()
 *     uses, but 6px higher than the other two mini-games' y = 44: their
 *     content starts lower, while this board's 3px frame is already at
 *     y = 60 (see SN_NAME_Y). It is page-level, so it never changes with
 *     the state. The score line "本局 n 分 · 兑 m 金币" sits under the
 *     board at y = 308 (7px of air below the 3px frame), also page-level,
 *     so it survives the dim on the PAUSED and OVER screens. It carries
 *     the conversion as well (user request 2026-11-01), so the number the
 *     player is chasing is printed under the board and not only on the
 *     OVER card.
 *   - Steering: the whole circle is four 90-degree sectors (45-degree
 *     boundaries) around the centre (180,180), compared with integers
 *     only -- no atan2. There is NO board-bounds test: a press anywhere
 *     on the 360x360 page, including the four rim areas far outside the
 *     240px board, steers, so the touch area is already the maximum the
 *     panel gives (evaluated 2026-11-01 for the user's "扩大触屏区域"
 *     request). The only inert region is the centre disc (r = 55), which
 *     exists so a thumb resting mid-screen does not steer -- shrinking
 *     it would trade steering area for the "长按中间暂停" target, which
 *     is why it stays at 55. A point inside that disc steers
 *     nowhere: a point at the exact centre has no dominant axis, so that
 *     110px disc is the action key. START taps it to start, PAUSED to
 *     resume, OVER to restart. During PLAYING it is the pause key, but
 *     as a HOLD, not a tap -- a tap there used to pause the game, and
 *     since the disc covers 16.5% of the board a thumb steering a run
 *     through the middle kept pausing by accident.
 *   - Back: one page-level arrow, same pattern as Sudoku (56x48 at
 *     TOP_MID -112,28, centre (68,52)), visible on START / PAUSED /
 *     OVER only. PLAYING has no chrome at all except the score line and
 *     the page name, both outside the play area; the tile swipe still
 *     leaves the page.
 *   - Guides: direction arrows + sector dashes, faint DIM, on START
 *     only; PAUSED keeps the dashes (the arrows would collide with
 *     the "已暂停" title); PLAYING shows none.
 *   - Overlays: START is undimmed (开始 circle, arrows, dashes, one hint
 *     line); PAUSED and OVER dim the board 50% black and show their
 *     title and centre circle only. PAUSED has NO restart button
 *     on purpose: an 84x32 one at y = 314 -- the narrowest part of the
 *     bezel -- charged 5 coins on a single tap with no confirmation.
 *     Leaving goes through the shared back arrow, which is visible
 *     whenever the game is not PLAYING. OVER's circle carries the price
 *     ("- 5 金币"), because the fee is otherwise invisible.
 *     A blocked start shows one page-level note ("金币不够（需 5 枚）" or
 *     "互动未就绪") under the board, above the dim (see the message
 *     slot below).
 *   - Payout: OVER also carries the round's NET at y = 64 (user
 *     decision 2026-11-01, retuned the same day: the score is settled
 *     once, at OVER, through PET_PLAY_REWARD). The page-level score line
 *     keeps showing score and gross payout ("本局 n 分 · 兑 m 金币",
 *     m from muse_pet_round_coins() -- 1 point per coin, 10-coin
 *     ceiling), so the OVER card must not restate the payout as if it
 *     were the gain: it shows the net, payout minus the 5-coin entry
 *     fee paid at start ("净 +5 / 0 / -5 金币"). The line sits between
 *     the page name (ends ~55) and the title (100); at y = 81
 *     the circle still leaves ~300 px of chord.
 *   - Message slot (user request 2026-11-01, "提示统一显示在游戏区域下方"):
 *     the band under the board (y = 304..325) hosts exactly ONE line at a
 *     time, in this priority: refusal note > score line > START hint. The
 *     note used to sit at y = 280, i.e. inside the board where it covered
 *     the last two rows of cells; it now shares the score line's slot
 *     (308), which is why its copy is the short one the sudoku page uses
 *     (the chord under the board is only ~213 px at y = 325).
 *
 * Colours are the shared table in muse_theme.h, NOT the mockup's
 * palette: snake OK (mint green), food DANGER, centre rings ACCENT.
 * The mockup's brighter green and the pause ▶ glyph are the only
 * deviations: the theme has one green, and the pause icon is the
 * LVGL PLAY symbol above a "继续" label (no font-dependent ▶).
 */
#include "muse_snake_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_font.h"
#include "muse_widgets.h"
#include "muse_pet.h"
#include "muse_snake.h"
#include "muse_theme.h"

/* --- geometry --- */
#define SN_BOARD_X0 60
#define SN_BOARD_Y0 60
#define SN_CELL 20
#define SN_CX 180
#define SN_CY 180
#define SN_CENTRE_R 55
#define SN_CENTRE_X0 125 /* CX - R */
#define SN_CENTRE_Y0 125
#define SN_CENTRE_SZ 110 /* 2 * R */
/* Refusal note: below the board, sharing the score line's slot (user
 * request 2026-11-01, "消息提示统一显示在游戏区域下方"). It used to sit at
 * y = 280 -- inside the board, covering the last two rows of cells, which
 * is where a snake running along the bottom edge lives. The band under the
 * board only has room for one line, so refresh_display() shows the note and
 * hides the score line and the START hint while a note is up. That also
 * caps the note's width, hence the short copy (see charge_for_game()). */
#define SN_NOTE_Y 308
/* Page name above the board. page_header() in muse_interact_ui.c puts the
 * other two games' titles at TOP_MID y = 44, and this page copied that --
 * but their content starts at y = 68 or 84, while this board's frame is
 * already at y = 60 with a 3px stroke (so 58.5 is its outer edge). A 17px
 * line at 44 has its baseline at 59, i.e. the title's ink ran into the
 * frame (user report 2026-11-01). 38 moves the baseline to 53, leaving
 * 5.5px of air, and costs nothing: 3 glyphs are 48px wide, so even at
 * y = 38 the corners are only 144px from the centre (180 available).
 *
 * The back arrow deliberately does NOT move with it: its box is
 * TOP_MID -112,28, so the glyph centre sits at (68,52) -- already 170px
 * from the centre -- and 6px higher would put its top-left ink corner at
 * ~181, outside the bezel. A 6px difference between a chevron at x = 68
 * and centred text at x = 180 is not readable as misalignment. */
#define SN_NAME_Y 38
/* "本局 n 分 · 兑 m 金币" under the board. The interaction pages put their
 * coin line at y = 302, but their content above ends at 274; this board's
 * frame ends at 300, so 308 keeps ~7px of air under the 3px frame instead
 * of gluing the text to it. The line grew from ~115px to ~152px when the
 * conversion was added (2026-11-01) and the chord at its bottom (325) is
 * ~213px, so it still clears the bezel with room to spare. */
#define SN_SCORE_Y 308
#define SN_TITLE_Y 100
/* Round payout line on the OVER card (see the file header). Between the
 * page name (SN_NAME_Y, ends ~55) and SN_TITLE_Y, and 300 px of chord at
 * its bottom edge, so a 13-glyph line never touches the bezel. */
#define SN_PAYOUT_Y 64
/* START hint, ONE line since 2026-11-01 (user request): "点四周转向，长按
 * 中间暂停" is ~208 px wide and its bottom (321) sits where the chord is
 * still ~224 px, so it clears the bezel. The old second slot (322, for
 * "游戏中长按中间暂停") is retired -- the score line and the refusal note
 * own the space under the board in every other state (see SN_NOTE_Y). */
#define SN_HINT_Y 304
/* Hold forgiveness: the finger may drift this far and still count as
 * "holding the centre", so a resting thumb mid-steer never pauses. */
#define SN_HOLD_SLOP 20

static lv_obj_t *s_page;
static lv_obj_t *s_back;
static lv_obj_t *s_score;
static lv_obj_t *s_note;
static lv_obj_t *s_touch;
static lv_obj_t *s_start_ov;
static lv_obj_t *s_pause_ov;
static lv_obj_t *s_over_ov;
static lv_obj_t *s_over_net;
static lv_obj_t *s_hint;
static lv_obj_t *s_food;
static lv_obj_t *s_eye1;
static lv_obj_t *s_eye2;
/* All MUSE_SNAKE_CELLS segments exist from build time, hidden. Creating
 * them lazily on demand (as v1 did) put them ABOVE the PAUSED/OVER
 * overlays, because a new child is appended last in z-order: a paused
 * game kept its snake at full brightness over the 50% dim and could hide
 * the "已暂停" title. It also malloc'd inside the tick callback. A fixed
 * pool fixes both, and the widgets land in PSRAM (see muse_lv_mem.c), so
 * 144 of them cost nothing that matters. */
static lv_obj_t *s_segs[MUSE_SNAKE_CELLS];
static int s_seg_shown; /* how many segments are visible right now */
static lv_point_t s_press_pt;
static bool s_press_in_centre;
static lv_timer_t *s_timer;
static muse_snake_t s_game;
static bool s_seeded;
/* Round payout bookkeeping (user decision 2026-11-01): the score is converted
 * exactly once, when the round reaches OVER, and s_round_coins remembers what
 * the wallet actually accepted so the OVER card cannot claim more. */
static bool s_settled;
static uint32_t s_round_coins;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                       const char *text)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_label_set_text(o, text);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Grid lines + arrows + dashes: LVGL keeps the point array pointer
 * (it does not copy it), so each line needs its own storage, not one
 * shared pair. Board 26 + START arrows 12 + START dashes 16 + PAUSED
 * dashes 16 = 70; 72 leaves spares. */
#define SN_MAX_LINES 72
static lv_point_precise_t s_line_pts[SN_MAX_LINES][2];
static int s_line_pts_used;

static void line(lv_obj_t *parent, int x1, int y1, int x2, int y2,
                 int width, uint32_t color)
{
    if (s_line_pts_used >= SN_MAX_LINES) {
        return;
    }
    lv_point_precise_t *pts = s_line_pts[s_line_pts_used++];
    pts[0].x = x1;
    pts[0].y = y1;
    pts[1].x = x2;
    pts[1].y = y2;
    lv_obj_t *o = lv_line_create(parent);
    lv_line_set_points(o, pts, 2);
    lv_obj_set_style_line_width(o, width, 0);
    lv_obj_set_style_line_color(o, lv_color_hex(color), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *btn(lv_obj_t *parent, int x, int y, int w, int h,
                     int radius, lv_event_cb_t cb)
{
    lv_obj_t *b = muse_card(parent, w, h, radius);
    lv_obj_set_pos(b, x, y);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static lv_obj_t *centre_button(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *b = btn(parent, SN_CENTRE_X0, SN_CENTRE_Y0, SN_CENTRE_SZ,
                      SN_CENTRE_SZ, SN_CENTRE_R, cb);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(MUSE_THEME_ACCENT), 0);
    return b;
}

static lv_obj_t *overlay(lv_obj_t *page, bool dim)
{
    lv_obj_t *o = lv_obj_create(page);
    lv_obj_set_size(o, 360, 360);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    if (dim) {
        lv_obj_t *d = lv_obj_create(o);
        lv_obj_set_size(d, 360, 360);
        lv_obj_set_pos(d, 0, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(MUSE_THEME_BG), 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_radius(d, 0, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    }
    return o;
}

/* One direction arrow: shaft + chevron head, centred at (cx, cy),
 * pointing (dx, dy). Faint DIM, START overlay only. */
static void arrow(lv_obj_t *parent, int cx, int cy, int dx, int dy)
{
    int px = -dy, py = dx;
    line(parent, cx - dx * 8, cy - dy * 8, cx + dx * 6, cy + dy * 6, 2,
         MUSE_THEME_DIM);
    line(parent, cx + dx * 18, cy + dy * 18, cx + dx * 2 + px * 11,
         cy + dy * 2 + py * 11, 2, MUSE_THEME_DIM);
    line(parent, cx + dx * 18, cy + dy * 18, cx + dx * 2 - px * 11,
         cy + dy * 2 - py * 11, 2, MUSE_THEME_DIM);
}

/* Sector dashes: 4 diagonals at 45 degrees, dashed from the centre
 * circle outwards (a = 45..117, dash 12). START and PAUSED overlays. */
static void sector_dashes(lv_obj_t *parent)
{
    const int steps[4] = {45, 65, 85, 105};
    for (int s = 0; s < 4; s++) {
        int a = steps[s];
        line(parent, SN_CX - a, SN_CY - a, SN_CX - (a + 12), SN_CY - (a + 12),
             2, MUSE_THEME_DIM);
        line(parent, SN_CX + a, SN_CY - a, SN_CX + (a + 12), SN_CY - (a + 12),
             2, MUSE_THEME_DIM);
        line(parent, SN_CX - a, SN_CY + a, SN_CX - (a + 12), SN_CY + (a + 12),
             2, MUSE_THEME_DIM);
        line(parent, SN_CX + a, SN_CY + a, SN_CX + (a + 12), SN_CY + (a + 12),
             2, MUSE_THEME_DIM);
    }
}

/* The band under the board (see SN_NOTE_Y) has room for exactly one line,
 * so the three candidates take it in this order: a refusal note first (it
 * is the answer to the tap that was just refused), then the score line,
 * then the START hint. Without this a refused start printed the note and
 * the hint on top of each other at y = 304 / 308. */
static void sync_message_band(void)
{
    bool note_up = s_note && !lv_obj_has_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    if (s_hint) {
        set_hidden(s_hint, note_up);
    }
}

/* lv_label_set_text has no same-text short circuit in LVGL 9.5; the score
 * line is rewritten every 500 ms tick, so skip when nothing changed (same
 * trick as muse_menu's set_text). */
static void set_label(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static void show_note(const char *text)
{
    lv_label_set_text(s_note, text);
    lv_obj_remove_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    sync_message_band();
}

static void hide_note(void)
{
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    sync_message_band();
}

/* The entry fee (user decision 2026-10-31): 5 coins per game, gated
 * on the pet snapshot and settled by the worker as PET_FEE. False
 * shows the refusal note and starts nothing. */
static bool charge_for_game(void)
{
    muse_pet_model_t snap;
    if (!muse_pet_snapshot(&snap)) {
        show_note("互动未就绪");
        return false;
    }
    if (snap.coins < MUSE_PET_GAME_STAKE) {
        /* Short copy on purpose (see SN_NOTE_Y): the slot under the board
         * leaves ~213px of chord, and the long sentence the other pages use
         * is ~272px wide. Same wording as the sudoku page's note. */
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
        /* First start tap: user timing makes the food sequence differ
         * per game. The build-time init food (unseeded rand) is only
         * the very first food on the very first game. */
        srand(lv_tick_get());
        s_seeded = true;
    }
}

static void sync_snake(void)
{
    if (s_game.state == MUSE_SNAKE_START) {
        /* START shows an empty grid (mockup); the snake appears once
         * the game starts. */
        for (int i = 0; i < s_seg_shown; i++) {
            lv_obj_add_flag(s_segs[i], LV_OBJ_FLAG_HIDDEN);
        }
        s_seg_shown = 0;
        lv_obj_add_flag(s_food, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_eye1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_eye2, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    for (int i = 0; i < s_game.len; i++) {
        lv_obj_set_pos(s_segs[i],
                       SN_BOARD_X0 + s_game.body[i].x * SN_CELL + 1,
                       SN_BOARD_Y0 + s_game.body[i].y * SN_CELL + 1);
        lv_obj_remove_flag(s_segs[i], LV_OBJ_FLAG_HIDDEN);
    }
    /* A restart shrinks the snake again, so hide the surplus rather than
     * touching the whole pool every tick. */
    for (int i = s_game.len; i < s_seg_shown; i++) {
        lv_obj_add_flag(s_segs[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_seg_shown = s_game.len;
    lv_obj_set_pos(s_food,
                   SN_BOARD_X0 + s_game.food.x * SN_CELL + 3,
                   SN_BOARD_Y0 + s_game.food.y * SN_CELL + 3);
    lv_obj_remove_flag(s_food, LV_OBJ_FLAG_HIDDEN);
    /* Eyes on the head, towards the direction of travel. */
    int bx = SN_BOARD_X0 + s_game.body[0].x * SN_CELL;
    int by = SN_BOARD_Y0 + s_game.body[0].y * SN_CELL;
    int e1x = bx, e1y = by, e2x = bx, e2y = by;
    switch (s_game.dir) {
    case MUSE_SNAKE_RIGHT:
        e1x = bx + 13; e1y = by + 5; e2x = bx + 13; e2y = by + 13;
        break;
    case MUSE_SNAKE_LEFT:
        e1x = bx + 1; e1y = by + 5; e2x = bx + 1; e2y = by + 13;
        break;
    case MUSE_SNAKE_UP:
        e1x = bx + 5; e1y = by + 1; e2x = bx + 13; e2y = by + 1;
        break;
    case MUSE_SNAKE_DOWN:
        e1x = bx + 5; e1y = by + 13; e2x = bx + 13; e2y = by + 13;
        break;
    }
    lv_obj_set_pos(s_eye1, e1x, e1y);
    lv_obj_set_pos(s_eye2, e2x, e2y);
    lv_obj_remove_flag(s_eye1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_eye2, LV_OBJ_FLAG_HIDDEN);
}

/* Score -> coins, once per round, at OVER (user decision 2026-11-01,
 * ceiling retuned to 10 the same day).
 * muse_pet_round_coins() is the only conversion curve (1 point per food,
 * 1 point per coin, 10 coins per round ceiling), and the 5-coin entry fee
 * was already charged at start through PET_FEE -- so the round's net is
 * payout minus 5, computed on the OVER card, never folded into the
 * conversion. A refused request (worker not ready or queue full) leaves
 * s_round_coins at 0: the card must never claim coins the wallet did not
 * get, and the net then honestly reads -5 (fee paid, payout lost). */
static void settle_round(void)
{
    if (s_settled) {
        return;
    }
    s_settled = true;
    uint32_t coins = muse_pet_round_coins(s_game.score);
    s_round_coins = 0;
    if (coins != 0 && muse_pet_request_play_reward(coins)) {
        s_round_coins = coins;
    }
}

static void refresh_display(void)
{
    /* One line under the board: the note wins the slot, the score line
     * takes it otherwise (and is hidden on START, where the hint sits there
     * instead). The conversion is printed live from the same curve the
     * settlement uses, so the number under the board and the number on the
     * OVER card can never disagree -- except at OVER, where the line shows
     * what the wallet actually accepted, not what it was asked for. */
    bool note_up = !lv_obj_has_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    if (s_game.state == MUSE_SNAKE_START || note_up) {
        lv_obj_add_flag(s_score, LV_OBJ_FLAG_HIDDEN);
    } else {
        char buf[64];
        uint32_t coins = (s_game.state == MUSE_SNAKE_OVER)
                             ? s_round_coins
                             : muse_pet_round_coins(s_game.score);
        snprintf(buf, sizeof(buf), "本局 %u 分 · 兑 %u 金币",
                 (unsigned)s_game.score, (unsigned)coins);
        set_label(s_score, buf);
        lv_obj_remove_flag(s_score, LV_OBJ_FLAG_HIDDEN);
    }
    sync_message_band();
    if (s_game.state == MUSE_SNAKE_OVER) {
        /* Settlement summary: the NET, not the payout. The page-level
         * score line already shows score and gross payout
         * ("本局 n 分 · 兑 m 金币") and stays readable through the dim,
         * so restating the payout here would read as the gain; the net
         * is payout minus the 5-coin entry fee, from what the wallet
         * actually accepted (s_round_coins, 0 when the request failed). */
        char buf[64];
        int net = (int)s_round_coins - (int)MUSE_PET_GAME_STAKE;
        if (net == 0) {
            snprintf(buf, sizeof(buf), "净 0 金币");
        } else {
            snprintf(buf, sizeof(buf), "净 %+d 金币", net);
        }
        set_label(s_over_net, buf);
    }
    set_hidden(s_start_ov, s_game.state != MUSE_SNAKE_START);
    set_hidden(s_pause_ov, s_game.state != MUSE_SNAKE_PAUSED);
    set_hidden(s_over_ov, s_game.state != MUSE_SNAKE_OVER);
    set_hidden(s_touch, s_game.state != MUSE_SNAKE_PLAYING);
    /* PLAYING is full-screen with no chrome, not even the back arrow. The
     * score line stays up: it sits below the board, outside the play area,
     * and it is the only feedback that a food was actually eaten. */
    set_hidden(s_back, s_game.state == MUSE_SNAKE_PLAYING);
    sync_snake();
}

static void pause_game(void)
{
    muse_snake_pause(&s_game);
    lv_timer_pause(s_timer);
    refresh_display();
}

static void on_start(lv_event_t *e)
{
    (void)e;
    if (!charge_for_game()) {
        return;
    }
    seed_once();
    s_settled = false;
    s_round_coins = 0;
    muse_snake_start(&s_game);
    lv_timer_resume(s_timer);
    refresh_display();
}

static void on_restart(lv_event_t *e)
{
    (void)e;
    /* A restart is a new game, so it charges again; a resume never does. */
    if (!charge_for_game()) {
        return;
    }
    seed_once();
    s_settled = false;
    s_round_coins = 0;
    muse_snake_restart(&s_game);
    lv_timer_resume(s_timer);
    refresh_display();
}

static void on_resume(lv_event_t *e)
{
    (void)e;
    muse_snake_resume(&s_game);
    lv_timer_resume(s_timer);
    refresh_display();
}

static int dist2_to_centre(const lv_point_t *p)
{
    int dx = p->x - SN_CX;
    int dy = p->y - SN_CY;
    return dx * dx + dy * dy;
}

static void on_touch(lv_event_t *e)
{
    (void)e;
    if (s_game.state != MUSE_SNAKE_PLAYING) {
        return;
    }
    /* The page group sits at the screen origin while the tile is
     * active, so the indev point is already page coordinates. */
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    s_press_pt = pt;
    s_press_in_centre = dist2_to_centre(&pt) <= SN_CENTRE_R * SN_CENTRE_R;
    if (s_press_in_centre) {
        /* Swallowed, not steered: a press this close to the centre has no
         * dominant axis. The pause comes from on_touch_hold(), so a tap
         * here does nothing at all and a run through the middle no longer
         * stops the game. */
        return;
    }
    int dx = pt.x - SN_CX;
    int dy = pt.y - SN_CY;
    /* Integer sector compare: |dx| > |dy| splits left/right, else
     * up/down. No atan2, no float. Screen y grows downwards, so a tap
     * below the centre is DOWN. */
    muse_snake_dir_t dir;
    if (abs(dx) > abs(dy)) {
        dir = (dx > 0) ? MUSE_SNAKE_RIGHT : MUSE_SNAKE_LEFT;
    } else {
        dir = (dy > 0) ? MUSE_SNAKE_DOWN : MUSE_SNAKE_UP;
    }
    muse_snake_set_dir(&s_game, dir);
}

/* Pause on a HOLD inside the centre circle, not on a tap. The disc is
 * 16.5% of the board and sits exactly where a run crosses the middle, so
 * a tap there used to end the run by accident. LVGL's long-press time
 * (LV_INDEV_DEF_LONG_PRESS_TIME, 400 ms) is global and other pages use
 * long press too, so this page does not change it.
 *
 * Two guards keep a resting thumb from counting as a hold: the press must
 * have landed inside the disc, and the finger must not have drifted more
 * than SN_HOLD_SLOP nor left the disc by the time the timer fires. */
static void on_touch_hold(lv_event_t *e)
{
    (void)e;
    if (s_game.state != MUSE_SNAKE_PLAYING || !s_press_in_centre) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    int ddx = pt.x - s_press_pt.x;
    int ddy = pt.y - s_press_pt.y;
    if (ddx * ddx + ddy * ddy > SN_HOLD_SLOP * SN_HOLD_SLOP) {
        return;
    }
    if (dist2_to_centre(&pt) > SN_CENTRE_R * SN_CENTRE_R) {
        return;
    }
    pause_game();
}

static void on_tick(lv_timer_t *t)
{
    (void)t;
    if (s_game.state != MUSE_SNAKE_PLAYING) {
        return;
    }
    muse_snake_tick(&s_game);
    if (s_game.state == MUSE_SNAKE_OVER) {
        lv_timer_pause(s_timer);
        /* The round is over: convert the score once, here and nowhere else,
         * so leaving the page (which only pauses) can never settle twice. */
        settle_round();
    }
    refresh_display();
}

void muse_snake_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    s_page = page;

    /* Board field + grid lines (fills first, then lines). */
    lv_obj_t *field = lv_obj_create(page);
    lv_obj_set_size(field, 240, 240);
    lv_obj_set_pos(field, SN_BOARD_X0, SN_BOARD_Y0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(field, lv_color_hex(MUSE_THEME_CARD), 0);
    lv_obj_set_style_border_width(field, 0, 0);
    lv_obj_set_style_radius(field, 0, 0);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    int gx1 = SN_BOARD_X0 + 12 * SN_CELL;
    int gy1 = SN_BOARD_Y0 + 12 * SN_CELL;
    for (int k = 1; k < 12; k++) {
        int x = SN_BOARD_X0 + k * SN_CELL;
        line(page, x, SN_BOARD_Y0, x, gy1, 1, MUSE_THEME_DIM);
    }
    for (int k = 1; k < 12; k++) {
        int y = SN_BOARD_Y0 + k * SN_CELL;
        line(page, SN_BOARD_X0, y, gx1, y, 1, MUSE_THEME_DIM);
    }
    line(page, SN_BOARD_X0, SN_BOARD_Y0, gx1, SN_BOARD_Y0, 3, MUSE_THEME_TEXT);
    line(page, SN_BOARD_X0, gy1, gx1, gy1, 3, MUSE_THEME_TEXT);
    line(page, SN_BOARD_X0, SN_BOARD_Y0, SN_BOARD_X0, gy1, 3, MUSE_THEME_TEXT);
    line(page, gx1, SN_BOARD_Y0, gx1, gy1, 3, MUSE_THEME_TEXT);

    /* Food + eyes, created once; segments are created on demand. */
    s_food = lv_obj_create(page);
    lv_obj_set_size(s_food, 14, 14);
    lv_obj_set_style_bg_opa(s_food, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_food, lv_color_hex(MUSE_THEME_DANGER), 0);
    lv_obj_set_style_border_width(s_food, 0, 0);
    lv_obj_set_style_radius(s_food, 7, 0);
    lv_obj_remove_flag(s_food, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_food, LV_OBJ_FLAG_HIDDEN);
    s_eye1 = lv_obj_create(page);
    s_eye2 = lv_obj_create(page);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *eye = (i == 0) ? s_eye1 : s_eye2;
        lv_obj_set_size(eye, 6, 6);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(eye, lv_color_hex(MUSE_THEME_TEXT), 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_radius(eye, 3, 0);
        lv_obj_remove_flag(eye, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(eye, LV_OBJ_FLAG_HIDDEN);
    }

    /* Segment pool: all MUSE_SNAKE_CELLS objects, created once and hidden.
     * They must exist before the overlays so the 50% dim lands on top of a
     * paused snake (see the s_segs declaration). */
    for (int i = 0; i < MUSE_SNAKE_CELLS; i++) {
        lv_obj_t *o = lv_obj_create(page);
        lv_obj_set_size(o, 18, 18);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(MUSE_THEME_OK), 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_radius(o, 5, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        s_segs[i] = o;
    }
    s_seg_shown = 0;

    /* Touch layer: full page, PLAYING only. It sits above the board
     * (which is non-clickable) and below the overlays, so overlay
     * buttons always win while an overlay is open. */
    s_touch = lv_obj_create(page);
    lv_obj_set_size(s_touch, 360, 360);
    lv_obj_set_pos(s_touch, 0, 0);
    lv_obj_set_style_bg_opa(s_touch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_touch, 0, 0);
    lv_obj_set_style_radius(s_touch, 0, 0);
    lv_obj_remove_flag(s_touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_touch, on_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_touch, on_touch_hold, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_flag(s_touch, LV_OBJ_FLAG_HIDDEN);

    /* START overlay: undimmed, 开始 circle + guides + hints. The page name
     * is NOT here: it is a page-level label (see the bottom of this
     * function) so it stays visible in every state, exactly like the
     * titles the other two mini-games carry. */
    s_start_ov = overlay(page, false);
    lv_obj_t *start_btn = centre_button(s_start_ov, on_start);
    lv_obj_t *sl = label(start_btn, muse_font_cjk(), MUSE_THEME_TEXT, "开始");
    lv_obj_center(sl);
    arrow(s_start_ov, 180, 104, 0, -1);
    arrow(s_start_ov, 180, 256, 0, 1);
    arrow(s_start_ov, 104, 180, -1, 0);
    arrow(s_start_ov, 256, 180, 1, 0);
    sector_dashes(s_start_ov);
    /* One line instead of two (2026-11-01, user request): the copy is
     * "点四周转向，长按中间暂停" -- 13 CJK glyphs, ~208 px, which still
     * fits the 244 px chord at y = 312. It sits in SN_HINT_Y (304); the
     * old second line's slot (SN_HINT2_Y) is retired, not re-used: the
     * score line owns 308 in every other state and the round edge below
     * 320 shears long text. */
    s_hint = label(s_start_ov, muse_font_cjk(), MUSE_THEME_DIM,
                   "点四周转向，长按中间暂停");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, SN_HINT_Y);

    /* PAUSED overlay: dim + title + 继续 circle + dashes. No restart:
     * see the file header. */
    s_pause_ov = overlay(page, true);
    lv_obj_t *pt = label(s_pause_ov, muse_font_cjk(), MUSE_THEME_ACCENT, "已暂停");
    lv_obj_align(pt, LV_ALIGN_TOP_MID, 0, SN_TITLE_Y);
    lv_obj_t *resume_btn = centre_button(s_pause_ov, on_resume);
    lv_obj_t *sym = label(resume_btn, &lv_font_montserrat_20, MUSE_THEME_TEXT,
                          LV_SYMBOL_PLAY);
    lv_obj_align(sym, LV_ALIGN_TOP_MID, 0, 31);
    lv_obj_t *rl = label(resume_btn, muse_font_cjk(), MUSE_THEME_TEXT, "继续");
    lv_obj_align(rl, LV_ALIGN_TOP_MID, 0, 59);
    sector_dashes(s_pause_ov);
    /* The 重开 capsule that used to sit at (138,314) is gone: it charged
     * 5 coins on one tap, with no confirmation, in the narrowest part of
     * the bezel. Leaving is the back arrow's job; restarting is offered
     * on the OVER card, where the price is printed. */

    /* OVER overlay: dim + net line + title + 再来一局 circle. The score
     * needs no copy here: the page-level "本局 n 分" line sits below the
     * board and is created after the overlays, so it stays readable through
     * the dim. s_over_net carries the round's NET (payout minus the
     * 5-coin entry fee) -- that line is written by refresh_display() only
     * while OVER is up. */
    s_over_ov = overlay(page, true);
    s_over_net = label(s_over_ov, muse_font_cjk(), MUSE_THEME_GOLD, "");
    lv_obj_align(s_over_net, LV_ALIGN_TOP_MID, 0, SN_PAYOUT_Y);
    lv_obj_t *ot = label(s_over_ov, muse_font_cjk(), MUSE_THEME_ACCENT, "游戏结束");
    lv_obj_align(ot, LV_ALIGN_TOP_MID, 0, SN_TITLE_Y);
    lv_obj_t *over_btn = centre_button(s_over_ov, on_restart);
    lv_obj_t *ol = label(over_btn, muse_font_cjk(), MUSE_THEME_TEXT, "再来一局");
    lv_obj_align(ol, LV_ALIGN_TOP_MID, 0, 32);
    /* on_restart charges through charge_for_game() the moment it is
     * pressed, so the price belongs on the button (fee from the macro,
     * not a literal, so a stake change cannot desync the copy). */
    char price_buf[16];
    snprintf(price_buf, sizeof(price_buf), "- %u 金币",
             (unsigned)MUSE_PET_GAME_STAKE);
    lv_obj_t *price = label(over_btn, muse_font_cjk(), MUSE_THEME_GOLD, price_buf);
    lv_obj_align(price, LV_ALIGN_TOP_MID, 0, 56);

    /* Back (shared, topmost): same pattern as Sudoku, hidden while
     * PLAYING. Created after the overlays so it stays clickable. */
    s_back = muse_back_button(page, back_cb);

    /* Name + score + note, page-level and last, so they stay above the
     * dim. The name is the same slot/page_header() pattern the other two
     * mini-games use ("贪吃蛇" above the play area), and the score takes
     * the coin line's slot from the interaction pages (y = 302, under the
     * board) so all four screens read the same way. */
    /* The shared subpage title (muse_font_cjk + ACCENT); y stays this
     * page's calibrated 38 -- at 44 the title's ink ran into the board
     * frame (see SN_NAME_Y above, user report 2026-11-01). */
    muse_page_title(page, "贪吃蛇", SN_NAME_Y);
    s_score = label(page, muse_font_cjk(), MUSE_THEME_TEXT, "");
    lv_obj_align(s_score, LV_ALIGN_TOP_MID, 0, SN_SCORE_Y);
    s_note = label(page, muse_font_cjk(), MUSE_THEME_DANGER, "");
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, SN_NOTE_Y);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

    muse_snake_init(&s_game);
    s_timer = lv_timer_create(on_tick, MUSE_SNAKE_TICK_MS, NULL);
    lv_timer_pause(s_timer);
    refresh_display();
}

void muse_snake_ui_show(void)
{
    /* A fresh entry never resets and never charges: START stays on its
     * title, PAUSED stays resumable, OVER stays restartable. A stale
     * refusal note from the last visit is cleared, and so is the "press
     * landed in the centre" latch, or a later hold elsewhere would be
     * judged against coordinates from the previous visit. */
    s_press_in_centre = false;
    /* Clear the stale note BEFORE the refresh, or the refresh would keep
     * the score line hidden for a note that is already gone. */
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    refresh_display();
}

void muse_snake_ui_hide(void)
{
    if (s_game.state == MUSE_SNAKE_PLAYING) {
        pause_game();
    }
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    sync_message_band();
}

void muse_snake_ui_fee_refused(void)
{
    if (s_game.state == MUSE_SNAKE_PLAYING ||
        s_game.state == MUSE_SNAKE_PAUSED) {
        /* The round never settled (the fee was refused): back to START,
         * no score kept. Same contract as muse_whackamole_ui_fee_refused. */
        muse_snake_init(&s_game);
        lv_timer_pause(s_timer);
    }
    char note[32];
    snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
             (unsigned)MUSE_PET_GAME_STAKE);
    show_note(note);
    refresh_display();
}
