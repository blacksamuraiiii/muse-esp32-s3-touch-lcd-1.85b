/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_sudoku_ui.c -- 6x6 Sudoku subpage (mockups gameplay-v3/popup-v3,
 * user decision 2026-10-31). See muse_sudoku_ui.h for the ownership
 * contract and muse_sudoku.h for the board rules.
 *
 * Geometry (360x360-circle coordinates, same system as the interact
 * file; all positions are absolute inside the 360x360 page group):
 *   - Header: the shared subpage pattern (title TOP_MID y = 44, back
 *     56x48 at TOP_MID -112,28, centre x = 68), plus a "n/20" counter
 *     right-aligned at x = 288.
 *   - Grid: 240x240 at (60,68), cells 40px. The mockup put the grid at
 *     y = 62, but the code title sits at y = 44 (not the mockup's 42),
 *     so the grid moved 6px down to keep clearance; the bottom pills
 *     stay at the mockup's y = 320, leaving a 12px gap under the grid.
 *   - Grid lines: thin lines are per-cell right/bottom borders would
 *     need per-side widths, which LVGL does not offer, so ALL lines
 *     are lv_line objects: 1px DIM for cell dividers, 3px TEXT for
 *     box dividers (after column 3, after rows 2 and 4) and the outer
 *     frame. Z-order is fills, then lines, then the transparent cell
 *     buttons with their digit labels, so a fill never covers a line.
 *   - Pad popup: dim overlay (full page, 50% black) + card 250x200 at
 *     (55,80), six 67x52 digit buttons in 3x2, action row 105x40 at
 *     y = 216. Tapping the overlay dismisses the popup (or the win
 *     card, when that is open); the back arrow is unreachable while a
 *     modal is open, which is the intended modal behaviour.
 *   - Win card: 260x150 at (50,105) with "完成！", the elapsed time
 *     and 下一题 / 再玩一次. It shares the dim overlay with the pad.
 *
 * Colours are the shared table in muse_theme.h, NOT the mockup's
 * palette: clues TEXT (near-white), player entries ACCENT (purple),
 * conflicts DANGER, selected cell SELECTED, same-digit cells CARD.
 * The mockup's cyan player digits were illustrative; the firmware
 * theme is purple, and inventing a cyan here would break the one
 * colour table rule.
 *
 * Interaction (mockup rules): tapping a clue selects it and highlights
 * its digits, no pad; tapping a fillable cell opens the pad; tapping
 * the selected cell again closes the pad. A digit fills immediately
 * and is flagged red on conflict, never rejected. 提示 fills the
 * selected cell when it is fillable and not already right, else the
 * first empty cell. 换一题 cycles 1..20. No coin cost, no 3-strike
 * rule, no pencil marks (v1 scope, see muse_sudoku.h).
 */
#include "muse_sudoku_ui.h"

#include <stdio.h>

#include "muse_font.h"
#include "muse_sudoku.h"
#include "muse_theme.h"

/* --- geometry --- */
#define SU_GRID_X0 60
#define SU_GRID_Y0 68
#define SU_CELL 40
#define SU_PAD_X0 55
#define SU_PAD_Y0 80
#define SU_PAD_W 250
#define SU_PAD_H 200
/* Pad children are positioned relative to the pad card, not the page:
 * digit grid starts 16px inside the card, action row 136px down. */
#define SU_DIGIT_X0 16
#define SU_DIGIT_Y0 16
#define SU_DIGIT_W 67
#define SU_DIGIT_H 52
#define SU_DIGIT_GAP 8
#define SU_ACT_Y0 136
#define SU_ACT_H 40
#define SU_ACT_W 105
#define SU_WIN_X0 50
#define SU_WIN_Y0 105
#define SU_WIN_W 260
#define SU_WIN_H 150
#define SU_PILL_W 64
#define SU_PILL_H 24
#define SU_PILL_Y0 320

static lv_obj_t *s_counter;
static lv_obj_t *s_fills[MUSE_SUDOKU_CELLS];
static lv_obj_t *s_cell_lbls[MUSE_SUDOKU_CELLS];
static lv_obj_t *s_overlay;
static lv_obj_t *s_pad_card;
static lv_obj_t *s_win_card;
static lv_obj_t *s_win_time;
static muse_sudoku_state_t s_state;
static int s_selected = -1; /* 0..35, -1 = none */
static bool s_pad_open;
static bool s_win_open;
static uint32_t s_start_ms;
/* 1 once this puzzle's clock is running. The page is built at boot, long
 * before anyone opens it, so building does NOT start the clock: the first
 * muse_sudoku_ui_show() does. Puzzles loaded from the pills (换一题,
 * 下一题, 再玩一次) start theirs at once -- the page is on screen then. */
static bool s_timed;

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

static void refresh_overlay(void)
{
    if (s_pad_open || s_win_open) {
        lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void close_pad(void)
{
    s_pad_open = false;
    lv_obj_add_flag(s_pad_card, LV_OBJ_FLAG_HIDDEN);
    refresh_overlay();
}

static void open_pad(void)
{
    s_pad_open = true;
    lv_obj_remove_flag(s_pad_card, LV_OBJ_FLAG_HIDDEN);
    refresh_overlay();
}

static void hide_win(void)
{
    s_win_open = false;
    lv_obj_add_flag(s_win_card, LV_OBJ_FLAG_HIDDEN);
    refresh_overlay();
}

static void show_win(void)
{
    uint32_t secs = (lv_tick_get() - s_start_ms) / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "用时 %lu:%02lu",
             (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    lv_label_set_text(s_win_time, buf);
    s_win_open = true;
    lv_obj_remove_flag(s_win_card, LV_OBJ_FLAG_HIDDEN);
    refresh_overlay();
}

static void refresh_counter(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/20", s_state.idx + 1);
    lv_label_set_text(s_counter, buf);
}

/* Grid digits, fills and the counter, from the logic state. The
 * selected cell keeps its fill; same-digit cells get the CARD fill
 * only when the selected cell actually holds a digit (selecting an
 * empty cell highlights nothing). */
static void refresh_grid(void)
{
    int sel_val = (s_selected >= 0) ? s_state.grid[s_selected] : 0;
    for (int i = 0; i < MUSE_SUDOKU_CELLS; i++) {
        uint8_t v = s_state.grid[i];
        char buf[2];
        if (v == 0) {
            buf[0] = '\0';
        } else {
            buf[0] = (char)('0' + v);
            buf[1] = '\0';
        }
        lv_label_set_text(s_cell_lbls[i], buf);
        uint32_t color = MUSE_THEME_ACCENT;
        if (s_state.err[i]) {
            color = MUSE_THEME_DANGER;
        } else if (s_state.given[i]) {
            color = MUSE_THEME_TEXT;
        }
        lv_obj_set_style_text_color(s_cell_lbls[i], lv_color_hex(color), 0);
        uint32_t fill = 0;
        bool show = false;
        if (i == s_selected) {
            fill = MUSE_THEME_SELECTED;
            show = true;
        } else if (sel_val != 0 && v == sel_val) {
            fill = MUSE_THEME_CARD;
            show = true;
        }
        lv_obj_set_style_bg_opa(s_fills[i], show ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (show) {
            lv_obj_set_style_bg_color(s_fills[i], lv_color_hex(fill), 0);
        }
    }
    refresh_counter();
}

static void after_fill(void)
{
    refresh_grid();
    if (muse_sudoku_is_complete(&s_state)) {
        close_pad();
        show_win();
    }
}

static void on_cell(lv_event_t *e)
{
    int pos = (int)(intptr_t)lv_event_get_user_data(e);
    /* The pad overlay covers the grid while the pad is open, so a cell
     * tap always means the pad is closed; tapping outside the pad
     * dismisses it through the overlay instead. */
    close_pad();
    s_selected = pos;
    refresh_grid();
    if (!muse_sudoku_is_given(&s_state, pos)) {
        open_pad();
    }
}

static void on_pad_digit(lv_event_t *e)
{
    int val = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_selected >= 0) {
        muse_sudoku_set(&s_state, s_selected, (uint8_t)val);
    }
    close_pad();
    after_fill();
}

static void on_pad_clear(lv_event_t *e)
{
    (void)e;
    if (s_selected >= 0) {
        muse_sudoku_clear(&s_state, s_selected);
    }
    close_pad();
    refresh_grid();
}

static void on_pad_cancel(lv_event_t *e)
{
    (void)e;
    close_pad();
}

static void on_overlay(lv_event_t *e)
{
    (void)e;
    /* Tapping outside a modal dismisses it: the pad closes, and a win
     * card steps back to the finished board so the player can look. */
    if (s_win_open) {
        hide_win();
    } else {
        close_pad();
    }
}

static void on_hint(lv_event_t *e)
{
    (void)e;
    int pos = -1;
    if (s_selected >= 0 && !muse_sudoku_is_given(&s_state, s_selected) &&
        s_state.grid[s_selected] != muse_sudoku_solution(&s_state, s_selected)) {
        pos = s_selected;
    } else {
        pos = muse_sudoku_first_empty(&s_state);
    }
    if (pos >= 0) {
        muse_sudoku_hint(&s_state, pos);
        s_selected = pos;
    }
    after_fill();
}

static void load_puzzle(int idx)
{
    muse_sudoku_load(&s_state, idx);
    s_selected = -1;
    s_start_ms = lv_tick_get();
    s_timed = true;
    close_pad();
    hide_win();
    refresh_grid();
}

static void on_next(lv_event_t *e)
{
    (void)e;
    load_puzzle(s_state.idx + 1);
}

static void on_win_next(lv_event_t *e)
{
    (void)e;
    load_puzzle(s_state.idx + 1);
}

static void on_win_replay(lv_event_t *e)
{
    (void)e;
    load_puzzle(s_state.idx);
}

/* Grid lines: LVGL keeps the point array pointer (it does not copy
 * it), so each line needs its own storage, not one shared pair. */
#define SU_MAX_LINES 16
static lv_point_precise_t s_line_pts[SU_MAX_LINES][2];
static int s_line_pts_used;

static void line(lv_obj_t *parent, int x1, int y1, int x2, int y2,
                 int width, uint32_t color)
{
    if (s_line_pts_used >= SU_MAX_LINES) {
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
                     uint32_t bg, int radius, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    return b;
}

void muse_sudoku_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    /* Header: back arrow + title, the shared subpage pattern. */
    lv_obj_t *back = lv_obj_create(page);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 56, 48);
    lv_obj_align(back, LV_ALIGN_TOP_MID, -112, 28);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = label(back, &lv_font_montserrat_20, MUSE_THEME_ACCENT, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);
    lv_obj_t *t = label(page, muse_font_cjk(), MUSE_THEME_ACCENT, "数独");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 44);
    s_counter = label(page, muse_font_btn(), MUSE_THEME_DIM, "1/20");
    lv_obj_align(s_counter, LV_ALIGN_TOP_RIGHT, -72, 44);

    /* Grid fills (behind the lines), then the lines, then the cells. */
    for (int i = 0; i < MUSE_SUDOKU_CELLS; i++) {
        int r = i / 6, c = i % 6;
        lv_obj_t *f = lv_obj_create(page);
        lv_obj_set_size(f, SU_CELL, SU_CELL);
        lv_obj_set_pos(f, SU_GRID_X0 + c * SU_CELL, SU_GRID_Y0 + r * SU_CELL);
        lv_obj_set_style_bg_opa(f, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(f, 0, 0);
        lv_obj_set_style_radius(f, 0, 0);
        lv_obj_remove_flag(f, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_fills[i] = f;
    }
    int gx1 = SU_GRID_X0 + 6 * SU_CELL;
    int gy1 = SU_GRID_Y0 + 6 * SU_CELL;
    for (int k = 1; k < 6; k++) {
        int x = SU_GRID_X0 + k * SU_CELL;
        line(page, x, SU_GRID_Y0, x, gy1, k == 3 ? 3 : 1,
             k == 3 ? MUSE_THEME_TEXT : MUSE_THEME_DIM);
    }
    for (int k = 1; k < 6; k++) {
        int y = SU_GRID_Y0 + k * SU_CELL;
        line(page, SU_GRID_X0, y, gx1, y, (k == 2 || k == 4) ? 3 : 1,
             (k == 2 || k == 4) ? MUSE_THEME_TEXT : MUSE_THEME_DIM);
    }
    line(page, SU_GRID_X0, SU_GRID_Y0, gx1, SU_GRID_Y0, 3, MUSE_THEME_TEXT);
    line(page, SU_GRID_X0, gy1, gx1, gy1, 3, MUSE_THEME_TEXT);
    line(page, SU_GRID_X0, SU_GRID_Y0, SU_GRID_X0, gy1, 3, MUSE_THEME_TEXT);
    line(page, gx1, SU_GRID_Y0, gx1, gy1, 3, MUSE_THEME_TEXT);
    for (int i = 0; i < MUSE_SUDOKU_CELLS; i++) {
        int r = i / 6, c = i % 6;
        lv_obj_t *b = lv_btn_create(page);
        lv_obj_set_size(b, SU_CELL, SU_CELL);
        lv_obj_set_pos(b, SU_GRID_X0 + c * SU_CELL, SU_GRID_Y0 + r * SU_CELL);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_add_event_cb(b, on_cell, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = label(b, muse_font_btn(), MUSE_THEME_TEXT, "");
        lv_obj_center(l);
        s_cell_lbls[i] = l;
    }

    /* Bottom pills. */
    lv_obj_t *hint = btn(page, 112, SU_PILL_Y0, SU_PILL_W, SU_PILL_H,
                         MUSE_THEME_CARD, 12, on_hint, NULL);
    lv_obj_t *hl = label(hint, muse_font_cjk(), MUSE_THEME_DIM, "提示");
    lv_obj_center(hl);
    lv_obj_t *next = btn(page, 184, SU_PILL_Y0, SU_PILL_W, SU_PILL_H,
                         MUSE_THEME_CARD, 12, on_next, NULL);
    lv_obj_t *nl = label(next, muse_font_cjk(), MUSE_THEME_DIM, "换一题");
    lv_obj_center(nl);

    /* Dim overlay (below both modals, above the grid). */
    s_overlay = lv_obj_create(page);
    lv_obj_set_size(s_overlay, 360, 360);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, on_overlay, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    /* Pad popup: 3x2 digits + 清除 / 取消. */
    s_pad_card = lv_obj_create(page);
    lv_obj_set_size(s_pad_card, SU_PAD_W, SU_PAD_H);
    lv_obj_set_pos(s_pad_card, SU_PAD_X0, SU_PAD_Y0);
    lv_obj_set_style_bg_opa(s_pad_card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_pad_card, lv_color_hex(MUSE_THEME_CARD), 0);
    lv_obj_set_style_radius(s_pad_card, 16, 0);
    lv_obj_set_style_border_width(s_pad_card, 2, 0);
    lv_obj_set_style_border_color(s_pad_card, lv_color_hex(MUSE_THEME_RULE), 0);
    lv_obj_remove_flag(s_pad_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pad_card, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 6; i++) {
        int col = i % 3, row = i / 3;
        int x = SU_DIGIT_X0 + col * (SU_DIGIT_W + SU_DIGIT_GAP);
        int y = SU_DIGIT_Y0 + row * (SU_DIGIT_H + SU_DIGIT_GAP);
        lv_obj_t *b = btn(s_pad_card, x, y, SU_DIGIT_W, SU_DIGIT_H,
                          MUSE_THEME_CARD_PRESSED, 10, on_pad_digit,
                          (void *)(intptr_t)(i + 1));
        /* Pressed flash: the theme has no lighter card shade, so the
         * press tints ACCENT instead of going invisible. */
        lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_THEME_ACCENT), LV_STATE_PRESSED);
        char buf[2] = {(char)('0' + i + 1), '\0'};
        lv_obj_t *l = label(b, muse_font_btn(), MUSE_THEME_TEXT, buf);
        lv_obj_center(l);
    }
    lv_obj_t *clear = btn(s_pad_card, SU_DIGIT_X0, SU_ACT_Y0, SU_ACT_W, SU_ACT_H,
                          MUSE_THEME_CARD_PRESSED, 10, on_pad_clear, NULL);
    lv_obj_set_style_bg_color(clear, lv_color_hex(MUSE_THEME_ACCENT), LV_STATE_PRESSED);
    lv_obj_t *cl = label(clear, muse_font_cjk(), MUSE_THEME_DIM, "清除");
    lv_obj_center(cl);
    lv_obj_t *cancel = btn(s_pad_card, SU_DIGIT_X0 + SU_ACT_W + 14, SU_ACT_Y0,
                           SU_ACT_W, SU_ACT_H,
                           MUSE_THEME_CARD_PRESSED, 10, on_pad_cancel, NULL);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(MUSE_THEME_ACCENT), LV_STATE_PRESSED);
    lv_obj_t *xl = label(cancel, muse_font_cjk(), MUSE_THEME_DIM, "取消");
    lv_obj_center(xl);

    /* Win card: shares the dim overlay with the pad. */
    s_win_card = lv_obj_create(page);
    lv_obj_set_size(s_win_card, SU_WIN_W, SU_WIN_H);
    lv_obj_set_pos(s_win_card, SU_WIN_X0, SU_WIN_Y0);
    lv_obj_set_style_bg_opa(s_win_card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_win_card, lv_color_hex(MUSE_THEME_CARD), 0);
    lv_obj_set_style_radius(s_win_card, 16, 0);
    lv_obj_set_style_border_width(s_win_card, 2, 0);
    lv_obj_set_style_border_color(s_win_card, lv_color_hex(MUSE_THEME_RULE), 0);
    lv_obj_remove_flag(s_win_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_win_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *wt = label(s_win_card, muse_font_cjk(), MUSE_THEME_ACCENT, "完成！");
    lv_obj_align(wt, LV_ALIGN_TOP_MID, 0, 16);
    s_win_time = label(s_win_card, muse_font_cjk(), MUSE_THEME_TEXT, "");
    lv_obj_align(s_win_time, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_t *wn = btn(s_win_card, 19, 94, SU_ACT_W, SU_ACT_H,
                       MUSE_THEME_CARD_PRESSED, 10, on_win_next, NULL);
    lv_obj_set_style_bg_color(wn, lv_color_hex(MUSE_THEME_ACCENT), LV_STATE_PRESSED);
    lv_obj_t *wnl = label(wn, muse_font_cjk(), MUSE_THEME_TEXT, "下一题");
    lv_obj_center(wnl);
    lv_obj_t *wr = btn(s_win_card, 136, 94, SU_ACT_W, SU_ACT_H,
                       MUSE_THEME_CARD_PRESSED, 10, on_win_replay, NULL);
    lv_obj_set_style_bg_color(wr, lv_color_hex(MUSE_THEME_ACCENT), LV_STATE_PRESSED);
    lv_obj_t *wrl = label(wr, muse_font_cjk(), MUSE_THEME_TEXT, "再玩一次");
    lv_obj_center(wrl);

    load_puzzle(0);
    /* The page is built once, at boot. Loading a puzzle here would start its
     * clock hours before the player opens the page, so the first puzzle waits
     * for muse_sudoku_ui_show() to start it. */
    s_timed = false;
}

void muse_sudoku_ui_show(void)
{
    close_pad();
    hide_win();
    if (!s_timed) {
        /* First entry since the page was built: start this puzzle's clock now.
         * Later visits leave it running -- leaving the tile keeps progress, so
         * the elapsed time a player sees includes the time away. */
        s_start_ms = lv_tick_get();
        s_timed = true;
    }
    refresh_grid();
}

void muse_sudoku_ui_hide(void)
{
    close_pad();
    hide_win();
}
