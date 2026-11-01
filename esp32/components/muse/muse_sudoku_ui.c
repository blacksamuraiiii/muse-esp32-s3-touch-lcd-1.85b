/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_sudoku_ui.c -- 6x6 Sudoku subpage (mockups gameplay-v3/popup-v3,
 * user decision 2026-10-31). See muse_sudoku_ui.h for the ownership
 * contract and muse_sudoku.h for the board rules.
 *
 * Geometry (360x360-circle coordinates, same system as the interact
 * file; all positions are absolute inside the 360x360 page group):
 *   - Header: the shared subpage pattern (title TOP_MID y = 44, back
 *     56x48 at TOP_MID -112,28, centre x = 68). The puzzle number lives
 *     INSIDE the title since 2026-11-01 -- "数独（3/20）" -- instead of a
 *     separate right-aligned "n/20" counter at x = 288, so the header has
 *     one centred object like every other subpage (user request).
 *   - Grid: 240x240 at (60,68), cells 40px -- unchanged. It is also the
 *     largest square this circle allows: an S-px board starting at y = 68
 *     puts its bottom corners S/2 out and S-112 down, so S = 240 lands
 *     175.5 from the centre (180 available) while S = 248 already needs 179.
 *   - Side strips: 提示 and 下一题 are vertical labels beside the board, not
 *     a row under it (user decision 2026-10-31). A bottom row cannot work:
 *     the chord at y = 320 is only ~148px wide, so the old 64x24 pills sat
 *     on the round edge and were too small to hit (and 24px is below the
 *     ~40px tap target the rest of this UI uses). 34x72 strips at x = 10 /
 *     316, y = 144 keep 16px clear of the grid on both sides and put the
 *     far corner 173.8 from the centre, i.e. 6px inside the bezel.
 *   - Margin bands: a 34px-wide pill is a 4.5mm target on a 195dpi panel, so
 *     aiming at it alone still missed (second user report, 2026-10-31). The
 *     whole margin either side of the board is now a hit region: invisible
 *     56x224 bands at x = 0 and x = 304, y = 84 (the board's span minus the
 *     16px the back arrow's box needs), created before their pills so the
 *     pill stays on top and keeps its pressed look. The bands stop 4px short
 *     of the board, so no band can ever take a cell tap.
 *     The three game subpages also lock the tile swipe (muse_ui.c), so a
 *     press that drifts is no longer turned into a tile scroll.
 *   - Strip type and centring: the firmware carries exactly one CJK face
 *     (16px, see muse_font.h), so the strips scale their label instead of
 *     pulling in a second generated face: transform_scale 224 = 87.5% of
 *     16px = 14px glyphs on a 17.5px line, so 下一题 occupies 52.5px of the
 *     72px strip. Each label is centred in both axes -- lv_obj_center puts
 *     the text box on the strip's centre and the transform pivot is set to
 *     50%/50%, so scaling does not shift it, and the one-character-per-line
 *     box is the same width for every line, which keeps it centred across
 *     the strip (9px of card either side of a 16px glyph column).
 *   - Grid lines: thin lines are per-cell right/bottom borders would
 *     need per-side widths, which LVGL does not offer, so ALL lines
 *     are lv_line objects: 1px DIM for cell dividers, 3px TEXT for
 *     box dividers (after column 3, after rows 2 and 4) and the outer
 *     frame. Z-order is fills, then lines, then the transparent cell
 *     buttons with their digit labels, so a fill never covers a line.
 *   - Pad popup: dim overlay (full page, 50% black) + card 250x200 at
 *     (55,80) -- dead centre of the page -- with six 67x52 digit buttons in
 *     3x2 and a 105x40 action row at y = 140. The pad and the win card both
 *     carry an explicit pad_all = 0: the default theme applies its "card"
 *     style to every plain lv_obj (pad 16 at the 130 dpi default) and child
 *     coordinates are measured from that padded content box, which pushed
 *     the whole number pad 16px right and 16px down inside its own card
 *     ("数字盘偏向右下方", user report 2026-10-31). Zeroing it is what makes
 *     the 16/20 margins below mean what they say: digits 3 x 67 + 2 x 8 =
 *     217 wide and 2 x 52 + 8 = 112 tall, action row 2 x 105 + 7 = 217 wide
 *     and 40 tall, so x = 16 leaves 16/17 and y = 20 leaves 20/20. Tapping
 *     the overlay dismisses the popup (or the win card, when that is open);
 *     the back arrow is unreachable while a modal is open, which is the
 *     intended modal behaviour.
 *   - Win card: 260x150 at (50,105) with "完成！", the elapsed time
 *     and 下一题 / 再玩一次 (19 and 136, 105 wide: 19px clear on both
 *     sides). It shares the dim overlay with the pad.
 *   - Refusal note: y = 316, under the board in the band the old pill row
 *     used to occupy. It used to sit at y = 280, on top of the board's last
 *     row, and its old copy was ~320px wide where the circle only leaves
 *     ~299px of chord.
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
 * first empty cell. 下一题 (the right-hand side strip) cycles 1..20. The
 * entry fee is 5 coins (PET_FEE): entering the page charges it, 再玩一次
 * charges it again, and 下一题 does NOT -- one ticket covers a run of
 * puzzles (user decision 2026-11-01, superseding the per-puzzle fee of
 * 2026-10-31). Re-entering a puzzle already paid for in this run is free;
 * under 5 coins the load is refused with "金币不够…" and the current puzzle
 * stays. No 3-strike rule, no pencil marks (v1 scope, see muse_sudoku.h).
 */
#include "muse_sudoku_ui.h"

#include <stdio.h>

#include "muse_font.h"
#include "muse_pet.h"
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
/* Pad children are positioned relative to the pad card's content box, and
 * that box is the card itself only because the card sets pad_all = 0 (the
 * theme would otherwise inset it by 16px). Numbers: 217 wide means x = 16
 * centres it (16 / 17); 112 + 8 + 40 = 160 tall means y = 20 centres it
 * (20 / 20). */
#define SU_DIGIT_X0 16
#define SU_DIGIT_Y0 20
#define SU_DIGIT_W 67
#define SU_DIGIT_H 52
#define SU_DIGIT_GAP 8
#define SU_ACT_X0 16
#define SU_ACT_Y0 140 /* DIGIT_Y0 + two rows + one gap */
#define SU_ACT_H 40
#define SU_ACT_W 105
#define SU_ACT_GAP 7 /* 2 x 105 + 7 = 217, the digit block's width */
#define SU_WIN_X0 50
#define SU_WIN_Y0 105
#define SU_WIN_W 260
#define SU_WIN_H 150
/* Side strips: vertical labels that leave the board its full 240px. Both
 * are 34x72 at y = 144 (centre 180), 16px clear of the grid at x = 44 / 300,
 * and their far corners sit 173.8 from the centre -- inside the 180 bezel.
 * SU_SIDE_TEXT_SCALE is LVGL's fixed-point scale for the label (256 = 100%);
 * 224 draws the 16px CJK face at 14px. */
#define SU_SIDE_W 34
#define SU_SIDE_H 72
#define SU_SIDE_Y0 144
#define SU_SIDE_X_L 10
#define SU_SIDE_X_R 316
#define SU_SIDE_TEXT_SCALE 224
/* Margin bands: the whole strip of screen left of the board (0..56) and right
 * of it (304..360) is a hit region, so a tap does not have to land on the
 * 34px pill. 224 tall = the board's own span, minus the top 16px that the
 * back arrow's box (y 28..76) needs. */
#define SU_BAND_W 56
#define SU_BAND_Y0 84
#define SU_BAND_H 224

/* Page title carries the puzzle number since 2026-11-01: the old
 * right-aligned "n/20" counter (muse_font_btn, TOP_RIGHT -72,44) is gone
 * so the header has one centred object instead of two competing ones. */
static lv_obj_t *s_title;
static lv_obj_t *s_note;
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
/* True once the currently displayed puzzle has been paid for. Build
 * leaves it false (the boot-time load is not a charge); every user
 * load sets it through charge_for_new_puzzle, and leaving the tile
 * keeps it, so re-entering a paid puzzle never charges twice.
 * 下一题 does not charge and does not clear it, so one ticket covers a run
 * of puzzles; 再玩一次 clears it and pays again (user decision 2026-11-01). */
static bool s_current_paid;
static uint32_t s_start_ms;
/* 1 once this puzzle's clock is running. The page is built at boot, long
 * before anyone opens it, so building does NOT start the clock: the first
 * muse_sudoku_ui_show() does. Puzzles loaded from the side strip (下一题)
 * or the win card start theirs at once -- the page is on screen then. */
static bool s_timed;

static bool try_charge(void);
static void load_puzzle(int idx);

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

/* A side strip's copy: one character per line, centred in the strip in both
 * axes. Chinese stacks top to bottom, so hard line breaks give vertical type
 * without rotating the label (a rotation would re-transform the glyphs on
 * every redraw). Every line is one glyph wide, so the label box is a narrow
 * column and lv_obj_center centres it horizontally as well as vertically;
 * the transform pivot is the box centre so the shrink to 14px does not move
 * it off that centre. */
static void side_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = label(parent, muse_font_cjk(), MUSE_THEME_DIM, text);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(l, 0, 0);
    lv_obj_set_style_transform_pivot_x(l, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(l, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale_x(l, SU_SIDE_TEXT_SCALE, 0);
    lv_obj_set_style_transform_scale_y(l, SU_SIDE_TEXT_SCALE, 0);
    lv_obj_center(l);
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
    char buf[32];
    snprintf(buf, sizeof(buf), "数独（%d/20）", s_state.idx + 1);
    lv_label_set_text(s_title, buf);
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
    if (!try_charge()) {
        /* Unpaid puzzle (first entry poor, or worker not ready):
         * retrying the charge on the next tap is the only way in. */
        return;
    }
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
    /* The hint writes into the board, so it pays the same entry fee as a
     * cell tap: a puzzle refused on entry must not become solvable one
     * hint at a time. */
    if (!try_charge()) {
        return;
    }
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

static void show_note(const char *text)
{
    lv_label_set_text(s_note, text);
    lv_obj_remove_flag(s_note, LV_OBJ_FLAG_HIDDEN);
}

/* The entry fee (user decision 2026-10-31): 5 coins per puzzle, gated
 * on the pet snapshot and settled by the worker as PET_FEE. True when
 * the displayed puzzle is already paid for, or when this call just
 * paid for it; false shows the refusal note and starts nothing. */
static bool try_charge(void)
{
    if (s_current_paid) {
        return true;
    }
    muse_pet_model_t snap;
    if (!muse_pet_snapshot(&snap)) {
        show_note("互动未就绪");
        return false;
    }
    if (snap.coins < MUSE_PET_GAME_STAKE) {
        /* Short on purpose: the note lives under the board (y = 316), where
         * the circle leaves ~236px of chord; the old "金币不够，先摸摸 /
         * 喂饭 / 抽运势" was ~320px and lost both ends. */
        show_note("金币不够（需 5 枚）");
        return false;
    }
    if (!muse_pet_request(PET_FEE)) {
        show_note("互动未就绪");
        return false;
    }
    s_current_paid = true;
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    return true;
}

/* Load a puzzle and pay for it: 再玩一次 goes through here, so reopening the
 * puzzle that was just solved costs another ticket (user decision 2026-11-01).
 * 下一题 deliberately does NOT -- it keeps s_current_paid and just loads.
 * False leaves the current puzzle on screen. */
static bool load_puzzle_paid(int idx)
{
    s_current_paid = false;
    if (!try_charge()) {
        return false;
    }
    load_puzzle(idx);
    return true;
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

/* 下一题 is free: one ticket covers a whole run of puzzles (user decision
 * 2026-11-01, from a report of being charged on every flip). It still needs
 * a paid entry, otherwise a refused entry could be bypassed by flipping
 * puzzles, so an unpaid board just does nothing here -- the refusal note is
 * already up and a cell tap / 提示 retries the charge. */
static void on_next(lv_event_t *e)
{
    (void)e;
    if (!s_current_paid) {
        return;
    }
    load_puzzle(s_state.idx + 1);
}

/* The win card's 下一题 is the same "next puzzle" action and is free for the
 * same reason; 再玩一次 reopens the puzzle that was just solved and pays
 * again (user decision 2026-11-01). */
static void on_win_next(lv_event_t *e)
{
    (void)e;
    if (!s_current_paid) {
        return;
    }
    load_puzzle(s_state.idx + 1);
}

static void on_win_replay(lv_event_t *e)
{
    (void)e;
    load_puzzle_paid(s_state.idx);
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

/* A margin band beside the board: invisible, clickable, and deliberately as
 * tall as the board. The 34px pill inside it is a 4.5mm-wide target on a
 * 195dpi panel -- narrower than the ~6mm a fingertip lands on reliably -- so
 * aiming at the pill alone missed a lot (user report 2026-10-31, second
 * round: "整个六宫格左右都算命中区"). The band sits *under* the pill, so a
 * tap on the pill still hits the pill (and keeps its pressed look) while
 * everything else in the margin hits the band. It stops 4px short of the
 * board so it can never steal a cell tap, and starts at y = 84, below the
 * back arrow's 28..76 box. Bands are created before their pill. */
static lv_obj_t *side_band(lv_obj_t *parent, int x, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, SU_BAND_W, SU_BAND_H);
    lv_obj_set_pos(b, x, SU_BAND_Y0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
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
    /* "数独（n/20）": the number lives inside the title now. ACCENT for
     * the whole line, same font and y as every other subpage title; the
     * fullwidth parens come from the CJK face, the digits from its
     * Montserrat fallback. ~92 px wide, well clear of the back arrow
     * whose box ends at x = 96. */
    s_title = label(page, muse_font_cjk(), MUSE_THEME_ACCENT, "数独（1/20）");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 44);

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

    /* Side strips: 提示 on the left, 下一题 on the right, both vertical and
     * both beside the board so the grid keeps its full 240px. pad_all = 0
     * keeps the theme's 16/10 button pad out of the way -- a 34px-wide strip
     * would otherwise have a 2px content box -- so lv_obj_center in
     * side_label() centres on the strip itself. Each band is created first so
     * the pill sits on top of it. */
    side_band(page, 0, on_hint);
    lv_obj_t *hint = btn(page, SU_SIDE_X_L, SU_SIDE_Y0, SU_SIDE_W, SU_SIDE_H,
                         MUSE_THEME_CARD, 10, on_hint, NULL);
    lv_obj_set_style_pad_all(hint, 0, 0);
    side_label(hint, "提\n示");
    side_band(page, 360 - SU_BAND_W, on_next);
    lv_obj_t *next = btn(page, SU_SIDE_X_R, SU_SIDE_Y0, SU_SIDE_W, SU_SIDE_H,
                         MUSE_THEME_CARD, 10, on_next, NULL);
    lv_obj_set_style_pad_all(next, 0, 0);
    side_label(next, "下\n一\n题");

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
    /* The theme's card style pads every plain lv_obj by 16px and children are
     * laid out inside that padded box, so without this line the whole pad
     * sits 16px right / 16px down in its own card. */
    lv_obj_set_style_pad_all(s_pad_card, 0, 0);
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
    lv_obj_t *clear = btn(s_pad_card, SU_ACT_X0, SU_ACT_Y0, SU_ACT_W, SU_ACT_H,
                          MUSE_THEME_CARD_PRESSED, 10, on_pad_clear, NULL);
    lv_obj_set_style_bg_color(clear, lv_color_hex(MUSE_THEME_ACCENT), LV_STATE_PRESSED);
    lv_obj_t *cl = label(clear, muse_font_cjk(), MUSE_THEME_DIM, "清除");
    lv_obj_center(cl);
    lv_obj_t *cancel = btn(s_pad_card, SU_ACT_X0 + SU_ACT_W + SU_ACT_GAP, SU_ACT_Y0,
                           SU_ACT_W, SU_ACT_H,
                           MUSE_THEME_CARD_PRESSED, 10, on_pad_cancel, NULL);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(MUSE_THEME_ACCENT), LV_STATE_PRESSED);
    lv_obj_t *xl = label(cancel, muse_font_cjk(), MUSE_THEME_DIM, "取消");
    lv_obj_center(xl);

    /* Win card: shares the dim overlay with the pad. pad_all = 0 for the
     * same reason as the pad card -- its 19 / 136 button pair is measured
     * from the card edge, not from a 16px inset. */
    s_win_card = lv_obj_create(page);
    lv_obj_set_size(s_win_card, SU_WIN_W, SU_WIN_H);
    lv_obj_set_pos(s_win_card, SU_WIN_X0, SU_WIN_Y0);
    lv_obj_set_style_pad_all(s_win_card, 0, 0);
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

    /* Refusal note, last so it stays above the dim overlay: a blocked
     * charge (under 5 coins, or worker not ready) shows here until the next
     * charge succeeds or the tile is left. y = 316 is the band the old pill
     * row used to occupy, clear of the board's last row (which ends at 308)
     * and inside the circle's chord for a note this short. */
    s_note = label(page, muse_font_cjk(), MUSE_THEME_DANGER, "");
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, 316);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);

    load_puzzle(0);
    /* The page is built once, at boot. Loading a puzzle here would start its
     * clock hours before the player opens the page, so the first puzzle waits
     * for muse_sudoku_ui_show() to start it. The boot-time load is not a
     * charge either: the first entry pays for puzzle #01. */
    s_timed = false;
    s_current_paid = false;
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
    /* Entering with an unpaid puzzle pays for it here; a paid puzzle
     * re-enters free, and a refused charge leaves the puzzle on screen
     * with the note up (cell taps retry the charge). */
    try_charge();
    refresh_grid();
}

void muse_sudoku_ui_hide(void)
{
    close_pad();
    hide_win();
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
}
