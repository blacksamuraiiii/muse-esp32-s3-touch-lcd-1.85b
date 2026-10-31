/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_snake_ui.c -- Snake subpage (mockup snake-mockup-v1, user
 * decision 2026-10-31). See muse_snake_ui.h for the ownership contract
 * and muse_snake.h for the game rules.
 *
 * Geometry (360x360-circle coordinates, same system as the interact
 * file; all positions are absolute inside the 360x360 page group):
 *   - Board: 240x240 at (60,60), cells 20px, 12x12. Score centred in
 *     the top crescent (TOP_MID y = 40); the START title "贪吃蛇"
 *     takes the same slot while the game has not started.
 *   - Steering: the whole circle is four 90-degree sectors (45-degree
 *     boundaries) around the centre (180,180), compared with integers
 *     only -- no atan2. The centre circle (r = 55) is the action key:
 *     START taps it to start, PLAYING taps it to pause, PAUSED taps
 *     it to resume, OVER taps it to restart.
 *   - Back: one page-level arrow, same pattern as Sudoku (56x48 at
 *     TOP_MID -112,28, centre (68,52)), visible on START / PAUSED /
 *     OVER only. PLAYING is full-screen with no chrome; the tile
 *     swipe still leaves the page.
 *   - Guides: direction arrows + sector dashes, faint DIM, on START
 *     only; PAUSED keeps the dashes (the arrows would collide with
 *     the "已暂停" title); PLAYING shows none.
 *   - Overlays: START is undimmed (title, 开始 circle, arrows,
 *     dashes, hint); PAUSED and OVER dim the board 50% black and show
 *     their title, centre circle and (PAUSED) a 重开 capsule.
 *     A blocked start shows one page-level note ("金币不够…" or
 *     "互动未就绪") at y = 280, above the dim.
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

#include "muse_font.h"
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
#define SN_RESTART_X0 138
#define SN_RESTART_Y0 314
#define SN_RESTART_W 84
#define SN_RESTART_H 32
#define SN_NOTE_Y 280
#define SN_SCORE_Y 40
#define SN_TITLE_Y 100
#define SN_HINT_Y 332

static lv_obj_t *s_page;
static lv_obj_t *s_back;
static lv_obj_t *s_score;
static lv_obj_t *s_note;
static lv_obj_t *s_touch;
static lv_obj_t *s_start_ov;
static lv_obj_t *s_pause_ov;
static lv_obj_t *s_over_ov;
static lv_obj_t *s_food;
static lv_obj_t *s_eye1;
static lv_obj_t *s_eye2;
static lv_obj_t *s_segs[MUSE_SNAKE_CELLS];
static int s_seg_count;
static lv_timer_t *s_timer;
static muse_snake_t s_game;
static bool s_seeded;

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
                     uint32_t bg, int radius, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_THEME_CARD_PRESSED),
                              LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static lv_obj_t *centre_button(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *b = btn(parent, SN_CENTRE_X0, SN_CENTRE_Y0, SN_CENTRE_SZ,
                      SN_CENTRE_SZ, MUSE_THEME_CARD, SN_CENTRE_R, cb);
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
        lv_obj_set_style_bg_color(d, lv_color_hex(0x000000), 0);
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

static void show_note(const char *text)
{
    lv_label_set_text(s_note, text);
    lv_obj_remove_flag(s_note, LV_OBJ_FLAG_HIDDEN);
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
        show_note("金币不够，先摸摸 / 喂饭 / 抽运势");
        return false;
    }
    if (!muse_pet_request(PET_FEE)) {
        show_note("互动未就绪");
        return false;
    }
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
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
        for (int i = 0; i < s_seg_count; i++) {
            lv_obj_add_flag(s_segs[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_food, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_eye1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_eye2, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    while (s_seg_count < s_game.len) {
        lv_obj_t *o = lv_obj_create(s_page);
        lv_obj_set_size(o, 18, 18);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(MUSE_THEME_OK), 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_radius(o, 5, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_segs[s_seg_count++] = o;
    }
    for (int i = 0; i < s_game.len; i++) {
        lv_obj_set_pos(s_segs[i],
                       SN_BOARD_X0 + s_game.body[i].x * SN_CELL + 1,
                       SN_BOARD_Y0 + s_game.body[i].y * SN_CELL + 1);
        lv_obj_remove_flag(s_segs[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = s_game.len; i < s_seg_count; i++) {
        lv_obj_add_flag(s_segs[i], LV_OBJ_FLAG_HIDDEN);
    }
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

static void refresh_display(void)
{
    if (s_game.state == MUSE_SNAKE_START) {
        lv_obj_add_flag(s_score, LV_OBJ_FLAG_HIDDEN);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "分数 %u", (unsigned)s_game.score);
        lv_label_set_text(s_score, buf);
        lv_obj_remove_flag(s_score, LV_OBJ_FLAG_HIDDEN);
    }
    set_hidden(s_start_ov, s_game.state != MUSE_SNAKE_START);
    set_hidden(s_pause_ov, s_game.state != MUSE_SNAKE_PAUSED);
    set_hidden(s_over_ov, s_game.state != MUSE_SNAKE_OVER);
    set_hidden(s_touch, s_game.state != MUSE_SNAKE_PLAYING);
    /* PLAYING is full-screen with no chrome, not even the back arrow. */
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
    int dx = pt.x - SN_CX;
    int dy = pt.y - SN_CY;
    if (dx * dx + dy * dy <= SN_CENTRE_R * SN_CENTRE_R) {
        pause_game();
        return;
    }
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

static void on_tick(lv_timer_t *t)
{
    (void)t;
    if (s_game.state != MUSE_SNAKE_PLAYING) {
        return;
    }
    muse_snake_tick(&s_game);
    if (s_game.state == MUSE_SNAKE_OVER) {
        lv_timer_pause(s_timer);
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
    lv_obj_add_flag(s_touch, LV_OBJ_FLAG_HIDDEN);

    /* START overlay: undimmed, title + 开始 circle + guides + hint. */
    s_start_ov = overlay(page, false);
    lv_obj_t *st = label(s_start_ov, muse_font_cjk(), MUSE_THEME_ACCENT, "贪吃蛇");
    lv_obj_align(st, LV_ALIGN_TOP_MID, 0, SN_SCORE_Y);
    lv_obj_t *start_btn = centre_button(s_start_ov, on_start);
    lv_obj_t *sl = label(start_btn, muse_font_cjk(), MUSE_THEME_TEXT, "开始");
    lv_obj_center(sl);
    arrow(s_start_ov, 180, 104, 0, -1);
    arrow(s_start_ov, 180, 256, 0, 1);
    arrow(s_start_ov, 104, 180, -1, 0);
    arrow(s_start_ov, 256, 180, 1, 0);
    sector_dashes(s_start_ov);
    lv_obj_t *hint = label(s_start_ov, muse_font_cjk(), MUSE_THEME_DIM,
                           "点四边转向 · 点中间开始");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, SN_HINT_Y);

    /* PAUSED overlay: dim + title + 继续 circle + dashes + 重开. */
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
    lv_obj_t *restart = btn(s_pause_ov, SN_RESTART_X0, SN_RESTART_Y0,
                            SN_RESTART_W, SN_RESTART_H, MUSE_THEME_CARD, 16,
                            on_restart);
    lv_obj_set_style_border_width(restart, 1, 0);
    lv_obj_set_style_border_color(restart, lv_color_hex(MUSE_THEME_RULE), 0);
    lv_obj_t *rsl = label(restart, muse_font_cjk(), MUSE_THEME_TEXT, "重开");
    lv_obj_center(rsl);

    /* OVER overlay: dim + title + 重开 circle. The final score stays
     * visible in the top crescent (the page-level score label). */
    s_over_ov = overlay(page, true);
    lv_obj_t *ot = label(s_over_ov, muse_font_cjk(), MUSE_THEME_ACCENT, "游戏结束");
    lv_obj_align(ot, LV_ALIGN_TOP_MID, 0, SN_TITLE_Y);
    lv_obj_t *over_btn = centre_button(s_over_ov, on_restart);
    lv_obj_t *ol = label(over_btn, muse_font_cjk(), MUSE_THEME_TEXT, "重开");
    lv_obj_center(ol);

    /* Back (shared, topmost): same pattern as Sudoku, hidden while
     * PLAYING. Created after the overlays so it stays clickable. */
    s_back = lv_obj_create(page);
    lv_obj_remove_style_all(s_back);
    lv_obj_set_size(s_back, 56, 48);
    lv_obj_align(s_back, LV_ALIGN_TOP_MID, -112, 28);
    lv_obj_add_event_cb(s_back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow_lbl = label(s_back, &lv_font_montserrat_20, MUSE_THEME_ACCENT,
                                LV_SYMBOL_LEFT);
    lv_obj_center(arrow_lbl);

    /* Score + note, page-level and last, so they stay above the dim. */
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
     * refusal note from the last visit is cleared. */
    refresh_display();
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
}

void muse_snake_ui_hide(void)
{
    if (s_game.state == MUSE_SNAKE_PLAYING) {
        pause_game();
    }
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
}
