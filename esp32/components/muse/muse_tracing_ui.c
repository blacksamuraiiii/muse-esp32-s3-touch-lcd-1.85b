/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_tracing_ui.c -- 描红写字 (语文练习) subpages (see
 * muse_tracing_ui.h).
 *
 * Layout is the mockup v1 geometry, measured from the same numbers
 * the mockup script used (~/workspace/tracing-mockup-v1.py,
 * ~/workspace/learn-layout-v3.py): 360x360 page, title y=44
 * (muse_page_title), hint y=84 (DIM), list cards 104x104 in a
 * 2x2 grid over 3 swiped pages (4/4/2), dots y=340; trace grid
 * 228x228 at (66,66), hint y=302 (DIM), progress y=322.
 *
 * The list page is the word-cards category strip idiom (flex row,
 * snap per page, scrollbar off, no elastic, scroll-end clamp):
 * pages are 216 px wide (2x104 + 8 gap) with 72 px end pads, so
 * the scroll stops are 0 / 224 / 448 and the dots read
 * (scroll_x + 112) / 224.
 *
 * Ink is one lv_line per pen-down (at most MUSE_TRACING_UI_MAX_LINES
 * per attempt; a child tracing stroke by stroke lifts at most 9
 * times for 天, so 16 is ~2x headroom -- a 17th pen-down is dropped
 * visually and from grading, and the attempt can still finish on
 * the ink already down). All lines share one static point pool
 * (MUSE_TRACING_UI_POOL_POINTS points, 4 KB): each line's points
 * are a contiguous slice, so lv_line_set_points() takes a pointer
 * into the pool (LVGL 9 does not copy the array; the points are
 * lv_point_precise_t, the snake / tictactoe / sudoku line idiom).
 * The logic layer
 * (muse_tracing.c) keeps its own sampled points; the two samplings
 * are independent (the UI samples every MUSE_TRACING_UI_MIN_MOVE_PX
 * for a smooth line, the logic every MUSE_TRACING_SAMPLE_PX for
 * scoring).
 *
 * The ghost is a label in the 48 px tracing face, scaled to 170 px
 * (256 * 170 / 48 = 907) around its centre, faint fill
 * (MUSE_TRACING_UI_GHOST_OPA). LVGL 9 has no text-outline style, so
 * the ghost is fill-only -- the mockup's outline is a rendering
 * artefact, not a firmware feature; the faint fill is the real
 * ghost. The label is centred on the grid by its em box, not its
 * ink box: a few px off the mockup's ink-box centring, inside the
 * 16 px cover tolerance, and the same centre the reference strokes
 * were measured against (the logic's field coordinates are the
 * grid's own coordinates, so the strokes sit on the ghost
 * regardless of the label's box).
 */
#include "muse_tracing_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "muse_font.h"
#include "muse_pet.h"
#include "muse_pet_model.h"
#include "muse_theme.h"
#include "muse_tracing.h"
#include "muse_widgets.h"

#if CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_tracing_48_v1);
#define TRACING_FONT (&muse_font_tracing_48_v1)
/* 170 px ghost from the 48 px face. */
#define TRACING_FONT_SCALE 907
#else
/* No CJK face on this board: the ghost falls back to the 16 px
 * chain (same scale idea, 170/16). Chinese renders as tofu there,
 * as everywhere else on the board; the grid and the ink still work,
 * and the feature is tuned for the 1.85B, which has CJK. */
#define TRACING_FONT (muse_font_cjk())
#define TRACING_FONT_SCALE 2720
#endif

/* List page geometry (user decision 2026-11-21, second pass: the card
 * holds the character only and the pinyin sits UNDER it, the 英语单词
 * sets page's idiom, with the coin line at y = 302 below). One 1x10 row:
 * ten 34 px cards with a 2 px gutter fill the width (10 x 34 + 9 x 2 =
 * 358, x0 = 1), the row centred vertically at y = 126. The 48 px glyph
 * face is drawn scaled to 28 px inside the card; the 16 px pinyin line
 * is drawn scaled to 12 px under it, aligned out of the card's bottom
 * edge so it is never clipped by the card. */
#define TRACING_LIST_CARD_W 34
#define TRACING_LIST_CARD_H 76
#define TRACING_LIST_GAP 2
#define TRACING_LIST_X0 1
#define TRACING_LIST_Y0 126
#define TRACING_LIST_COINS_Y 302
#define TRACING_LIST_GLYPH_SCALE 149  /* 48 px face -> 28 px */
#define TRACING_LIST_PINYIN_SCALE 192 /* 16 px face -> 12 px */

/* Trace page geometry (mockup v1). */
#define TRACING_GRID_X 66
#define TRACING_GRID_Y 66
#define TRACING_GRID_BORDER 3
#define TRACING_GRID_DASH 12
#define TRACING_GRID_GAP 8
#define TRACING_GRID_LINE_W 2
#define TRACING_HINT_Y 302
#define TRACING_PROGRESS_Y 322
#define TRACING_NEXT_X 322
#define TRACING_NEXT_Y 180
/* Ink: white ~10 px, round ends (mockup v1). */
#define TRACING_INK_W 10
/* Faint ghost fill, out of 255 (mockup v1's pale grey). */
#define TRACING_GHOST_OPA 96
/* One lv_line per pen-down; see the file header for the 16. */
#define TRACING_UI_MAX_LINES 16
/* Shared point pool (4 KB static: 512 lv_point_precise_t): a full
 * trace is ~600 px of arc, so 512 points at 4 px sampling is ~3x
 * headroom. */
#define TRACING_UI_POOL_POINTS 512
/* Minimum travel between ink points (UI sampling, smooth line). */
#define TRACING_UI_MIN_MOVE_PX 4
/* Touch points this far outside the grid are dropped (the logic
 * clamps its own copy; the ink must not paint the black surround). */
#define TRACING_UI_EDGE_MARGIN 8
/* Wrong-flash timer period (the logic ages the flash in the same
 * steps; muse_tracing_tick() is called with this period). */
#define TRACING_UI_TICK_MS 50

static lv_obj_t *s_list_page;
static lv_obj_t *s_list_glyphs[MUSE_TRACING_CHARS];
static lv_obj_t *s_list_coins;
static muse_tracing_open_char_cb_t s_open_cb;

static lv_obj_t *s_trace_page;
static lv_obj_t *s_ghost;
static lv_obj_t *s_hint;
static lv_obj_t *s_progress;
static lv_obj_t *s_stars;
static lv_obj_t *s_next;
static lv_timer_t *s_timer;

static muse_tracing_t s_trace;
static int s_char_idx = 0;
static bool s_reward_sent;

/* Ink: one line object per pen-down, sharing s_pool. A line's
 * points are s_pool[line_off .. line_off + line_n - 1]; the pool
 * only ever grows within an attempt (reset() zeroes it). */
static lv_obj_t *s_lines[TRACING_UI_MAX_LINES];
static lv_point_precise_t s_pool[TRACING_UI_POOL_POINTS];
static uint16_t s_pool_used;
static uint8_t s_line_count;
static int s_line_off[TRACING_UI_MAX_LINES];
static int s_line_n[TRACING_UI_MAX_LINES];
static bool s_pen_down;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font,
                       uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* --- ink ---------------------------------------------------------- */

static void ink_clear(void)
{
    for (int i = 0; i < TRACING_UI_MAX_LINES; i++) {
        if (s_lines[i]) {
            lv_obj_add_flag(s_lines[i], LV_OBJ_FLAG_HIDDEN);
        }
        s_line_off[i] = 0;
        s_line_n[i] = 0;
    }
    s_pool_used = 0;
    s_line_count = 0;
    s_pen_down = false;
}

static void ink_set_color(uint32_t color)
{
    for (int i = 0; i < (int)s_line_count; i++) {
        if (s_lines[i]) {
            lv_obj_set_style_line_color(s_lines[i], lv_color_hex(color), 0);
        }
    }
}

/* Start a new pen stroke at page coords (x, y). Returns false when
 * the line pool is full (the stroke is dropped, visually and from
 * grading -- the attempt can still finish on the ink already down). */
static bool ink_begin(int x, int y)
{
    if (s_line_count >= TRACING_UI_MAX_LINES ||
        s_pool_used >= TRACING_UI_POOL_POINTS) {
        return false;
    }
    lv_obj_t *line = s_lines[s_line_count];
    if (line == NULL) {
        return false;
    }
    int off = s_pool_used;
    s_pool[off].x = (int16_t)x;
    s_pool[off].y = (int16_t)y;
    s_pool_used++;
    s_line_off[s_line_count] = off;
    s_line_n[s_line_count] = 1;
    lv_line_set_points(line, &s_pool[off], 1);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_HIDDEN);
    s_line_count++;
    s_pen_down = true;
    return true;
}

static void ink_add(int x, int y)
{
    if (!s_pen_down || s_line_count == 0 ||
        s_pool_used >= TRACING_UI_POOL_POINTS) {
        return;
    }
    int li = (int)s_line_count - 1;
    int off = s_line_off[li];
    int n = s_line_n[li];
    lv_point_precise_t last = s_pool[off + n - 1];
    int dx = x - last.x, dy = y - last.y;
    if (dx * dx + dy * dy <
        TRACING_UI_MIN_MOVE_PX * TRACING_UI_MIN_MOVE_PX) {
        return;
    }
    s_pool[s_pool_used].x = (int16_t)x;
    s_pool[s_pool_used].y = (int16_t)y;
    s_pool_used++;
    s_line_n[li] = n + 1;
    lv_line_set_points(s_lines[li], &s_pool[off], (uint32_t)(n + 1));
}

/* --- trace page state --------------------------------------------- */

static void show_praise(void)
{
    lv_label_set_text(s_hint, "写得真棒！");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(MUSE_THEME_GOLD), 0);
    lv_obj_clear_flag(s_stars, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_progress, LV_OBJ_FLAG_HIDDEN);
}

static void show_hint(void)
{
    lv_label_set_text(s_hint, "跟着虚影描一描");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(MUSE_THEME_DIM), 0);
    lv_obj_add_flag(s_stars, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_progress, LV_OBJ_FLAG_HIDDEN);
}

static void show_wrong(void)
{
    lv_label_set_text(s_hint, "描错了，再试一次");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(MUSE_THEME_DANGER), 0);
    ink_set_color(MUSE_THEME_DANGER);
    lv_obj_add_flag(s_stars, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_next, LV_OBJ_FLAG_HIDDEN);
}

static void on_done(void)
{
    if (!s_reward_sent) {
        s_reward_sent = true;
        /* Once per finished trace; the worker pays 2 for the day's
         * first correct trace of this character and PET_CAP after
         * that. The praise shows either way -- the page never prints
         * "+2 金币", so a repeat trace cannot look like a payout. */
        (void)muse_pet_request_tracing_solve(s_char_idx);
    }
    show_praise();
}

/* The timer only ages the wrong flash (50 ms steps) and clears the
 * ink when it expires. Grading itself happens on every touch point,
 * not here. */
static void on_timer(lv_timer_t *timer)
{
    (void)timer;
    if (s_trace.phase != MUSE_TRACING_WRONG) {
        return;
    }
    muse_tracing_tick(&s_trace, TRACING_UI_TICK_MS);
    if (s_trace.phase == MUSE_TRACING_TRACING) {
        /* Flash over: fresh attempt, same character. */
        muse_tracing_reset(&s_trace);
        ink_clear();
        show_hint();
    }
}

static void on_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (s_trace.phase == MUSE_TRACING_DONE ||
        s_trace.phase == MUSE_TRACING_WRONG) {
        /* Finished or flashing: the ink is frozen until the page is
         * left or the flash clears. */
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    /* Field coords: the grid's page offset, with an 8 px margin so a
     * fingertip on the grid's edge still counts. Points outside are
     * dropped, not clamped: clamping would pile ink on the border. */
    int fx = pt.x - TRACING_GRID_X;
    int fy = pt.y - TRACING_GRID_Y;
    if (fx < -TRACING_UI_EDGE_MARGIN ||
        fx > MUSE_TRACING_FIELD + TRACING_UI_EDGE_MARGIN ||
        fy < -TRACING_UI_EDGE_MARGIN ||
        fy > MUSE_TRACING_FIELD + TRACING_UI_EDGE_MARGIN) {
        if (code == LV_EVENT_RELEASED) {
            s_pen_down = false;
        }
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        if (!ink_begin(pt.x, pt.y)) {
            return;
        }
        muse_tracing_add_point(&s_trace, fx, fy);
    } else if (code == LV_EVENT_PRESSING) {
        ink_add(pt.x, pt.y);
        muse_tracing_add_point(&s_trace, fx, fy);
    } else if (code == LV_EVENT_RELEASED) {
        s_pen_down = false;
        return;
    }
    if (s_trace.phase == MUSE_TRACING_DONE) {
        on_done();
    } else if (s_trace.phase == MUSE_TRACING_WRONG) {
        show_wrong();
    }
}

static void on_next(lv_event_t *e)
{
    (void)e;
    int next = (s_char_idx + 1) % (int)MUSE_TRACING_CHARS;
    if (s_open_cb) {
        /* The ">" is the trace page's own next-character shortcut:
         * it goes through the owner's open path, so the page it
         * opens is the same one a list tap opens. */
        s_open_cb(next);
    }
}

/* --- list page ----------------------------------------------------- */

static void on_card(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_open_cb) {
        s_open_cb(idx);
    }
}

void muse_tracing_ui_build_list(lv_obj_t *page, lv_event_cb_t back_cb,
                                muse_tracing_open_char_cb_t open_cb)
{
    s_list_page = page;
    s_open_cb = open_cb;
    muse_back_button(page, back_cb);
    muse_page_title(page, "语文练习", 44);
    /* One row of ten (user decision 2026-11-21): the same single-row
     * idiom as 英语口语. Reading order is the table order,
     * 天 地 人 日 月 山 川 木 水 火. */
    for (int idx = 0; idx < (int)MUSE_TRACING_CHARS; idx++) {
        int x = TRACING_LIST_X0 + idx * (TRACING_LIST_CARD_W + TRACING_LIST_GAP);
        lv_obj_t *card = muse_card(page, TRACING_LIST_CARD_W,
                                   TRACING_LIST_CARD_H, 10);
        lv_obj_set_pos(card, x, TRACING_LIST_Y0);
        const muse_tracing_char_t *ch = &MUSE_TRACING_TABLE[idx];
        /* The card holds the character only. The 48 px face is scaled to
         * 28 px: at native size the glyph is wider than the card. Pivot
         * in percent (LV_PCT) -- a bare 50 means 50 px, which is what
         * threw the trace-page ghost out of the 田字格. */
        lv_obj_t *glyph = label(card, TRACING_FONT, MUSE_THEME_TEXT);
        lv_label_set_text(glyph, ch->utf8);
        lv_obj_set_style_transform_scale(glyph, TRACING_LIST_GLYPH_SCALE, 0);
        lv_obj_set_style_transform_pivot_x(glyph, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(glyph, LV_PCT(50), 0);
        lv_obj_center(glyph);
        lv_obj_remove_flag(glyph, LV_OBJ_FLAG_CLICKABLE);
        s_list_glyphs[idx] = glyph;
        /* Pinyin UNDER the card (the 英语单词 sets page's idiom), as a
         * sibling aligned to the card's bottom edge so the card's round
         * corners cannot clip it. Tone marks come from the Latin
         * Extended-A tail in the CJK chain (muse_font.c). */
        lv_obj_t *py = label(page, muse_font_cjk(), MUSE_THEME_DIM);
        lv_label_set_text(py, ch->pinyin);
        lv_obj_set_style_transform_scale(py, TRACING_LIST_PINYIN_SCALE, 0);
        lv_obj_set_style_transform_pivot_x(py, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(py, LV_PCT(50), 0);
        lv_obj_align_to(py, card, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
        lv_obj_remove_flag(py, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_card, LV_EVENT_CLICKED,
                            (void *)(intptr_t)idx);
    }
    /* Coin line, the 英语单词 sets page's slot (y = 302): the balance
     * lives one level up on the 学习 page too, but the list reads better
     * with it, and it costs one label. */
    s_list_coins = label(page, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(s_list_coins, "金币 --/100");
    lv_obj_align(s_list_coins, LV_ALIGN_TOP_MID, 0, TRACING_LIST_COINS_Y);
    muse_tracing_ui_refresh_list();
}

void muse_tracing_ui_refresh_list(void)
{
    if (!s_list_page) {
        return;
    }
    muse_pet_model_t snap;
    uint32_t paid = 0;
    bool have = muse_pet_snapshot(&snap);
    if (have) {
        muse_pet_tracing_today(&snap, time(NULL), &paid);
    }
    if (s_list_coins != NULL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "金币 %u/100",
                 (unsigned)(have ? snap.coins : 0));
        lv_label_set_text(s_list_coins, buf);
    }
    for (int i = 0; i < (int)MUSE_TRACING_CHARS; i++) {
        if (!s_list_glyphs[i]) {
            continue;
        }
        /* Paid today: the glyph turns gold. The 34 px card has no room
         * for the ✓ the old 104 px card carried. */
        lv_obj_set_style_text_color(
            s_list_glyphs[i],
            lv_color_hex((paid & (1u << (uint32_t)i)) != 0 ? MUSE_THEME_GOLD
                                                          : MUSE_THEME_TEXT),
            0);
    }
}

/* --- trace page ---------------------------------------------------- */

/* One dashed cross line, drawn as short segments (LVGL 9 lines are
 * solid): 12 px dash, 8 px gap, 2 px wide, DANGER. The segment
 * points live in a static array: lv_line_set_points() does NOT copy
 * its array, so stack points would dangle as soon as this returns
 * (the same reason the ink pool is static). 11 segments per line,
 * 22 in total. */
#define TRACING_GRID_DASHES 11
static lv_point_precise_t s_dash_pts[2 * 2 * TRACING_GRID_DASHES];

static void grid_dash(lv_obj_t *parent, bool vertical)
{
    int step = TRACING_GRID_DASH + TRACING_GRID_GAP;
    int base = vertical ? 0 : 2 * TRACING_GRID_DASHES;
    int n = 0;
    for (int o = 0; o + TRACING_GRID_DASH <= MUSE_TRACING_FIELD;
         o += step) {
        lv_point_precise_t *pts = &s_dash_pts[base + 2 * n];
        if (vertical) {
            pts[0].x = TRACING_GRID_X + MUSE_TRACING_FIELD / 2;
            pts[0].y = TRACING_GRID_Y + o;
            pts[1].x = pts[0].x;
            pts[1].y = TRACING_GRID_Y + o + TRACING_GRID_DASH;
        } else {
            pts[0].x = TRACING_GRID_X + o;
            pts[0].y = TRACING_GRID_Y + MUSE_TRACING_FIELD / 2;
            pts[1].x = TRACING_GRID_X + o + TRACING_GRID_DASH;
            pts[1].y = pts[0].y;
        }
        lv_obj_t *seg = lv_line_create(parent);
        lv_line_set_points(seg, pts, 2);
        lv_obj_set_style_line_width(seg, TRACING_GRID_LINE_W, 0);
        lv_obj_set_style_line_color(seg, lv_color_hex(MUSE_THEME_DANGER),
                                    0);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        n++;
    }
}

void muse_tracing_ui_build_trace(lv_obj_t *page, lv_event_cb_t back_cb)
{
    s_trace_page = page;
    muse_tracing_init(&s_trace);
    /* Full-page touch layer first (the doodle idiom): the back
     * button, title and ">" are created after it, so they stay
     * clickable above it. */
    lv_obj_t *touch = lv_obj_create(page);
    lv_obj_remove_style_all(touch);
    lv_obj_set_size(touch, 360, 360);
    lv_obj_align(touch, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(touch, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(touch, on_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(touch, on_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(touch, on_touch, LV_EVENT_RELEASED, NULL);
    muse_back_button(page, back_cb);
    muse_page_title(page, "语文练习 · 描红", 44);
    /* The 田字格: red solid border, dashed red cross. The border is
     * a plain object (no fill); the cross is dashed segments. */
    lv_obj_t *grid = lv_obj_create(page);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, MUSE_TRACING_FIELD, MUSE_TRACING_FIELD);
    lv_obj_set_pos(grid, TRACING_GRID_X, TRACING_GRID_Y);
    lv_obj_set_style_border_width(grid, TRACING_GRID_BORDER, 0);
    lv_obj_set_style_border_color(grid, lv_color_hex(MUSE_THEME_DANGER),
                                  0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    grid_dash(page, true);
    grid_dash(page, false);
    /* The ghost: the 48 px face scaled to 170 px, faint fill, centred
     * on the grid. LVGL 9 has no text outline, so the ghost is
     * fill-only (see the file header). */
    s_ghost = label(page, TRACING_FONT, MUSE_THEME_DIM);
    lv_label_set_text(s_ghost, MUSE_TRACING_TABLE[0].utf8);
    lv_obj_set_style_text_opa(s_ghost, TRACING_GHOST_OPA, 0);
    lv_obj_set_style_transform_scale(s_ghost, TRACING_FONT_SCALE, 0);
    /* Pivot in percent: 50/50 is the label's centre. The raw values are
     * pixels (or LV_PCT), NOT tenths of a percent -- the first cut passed
     * 50/50, which LVGL read as 50 px, i.e. past the 48 px box's bottom
     * right corner, and the ghost zoomed out towards the top-left and
     * left the 田字格 (user report 2026-11-21). */
    lv_obj_set_style_transform_pivot_x(s_ghost, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_ghost, LV_PCT(50), 0);
    /* The label is centred on the grid by its own box: the grid is
     * centred on the page, so LV_ALIGN_CENTER lands the label's
     * centre on the grid's centre. (The label's box is the 48 px em
     * box; the ghost's ink sits a few px off the mockup's ink-box
     * centre, inside the cover tolerance -- see the file header.) */
    lv_obj_align(s_ghost, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_ghost, LV_OBJ_FLAG_CLICKABLE);
    /* Ink lines, created once and hidden until a pen-down (see the
     * file header for the shared pool). */
    for (int i = 0; i < TRACING_UI_MAX_LINES; i++) {
        lv_obj_t *line = lv_line_create(page);
        lv_obj_set_style_line_width(line, TRACING_INK_W, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(MUSE_THEME_TEXT),
                                    0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
        s_lines[i] = line;
    }
    s_hint = label(page, muse_font_cjk(), MUSE_THEME_DIM);
    lv_label_set_text(s_hint, "跟着虚影描一描");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, TRACING_HINT_Y);
    s_progress = label(page, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(s_progress, "第 1/10 个字");
    lv_obj_align(s_progress, LV_ALIGN_TOP_MID, 0, TRACING_PROGRESS_Y);
    /* Praise + stars, hidden until DONE. "★" is in the Noto face
     * (checked against the font file: U+2605 present), so the stars
     * are a text label, not drawn polygons. */
    s_stars = label(page, muse_font_cjk(), MUSE_THEME_GOLD);
    lv_label_set_text(s_stars, "★★★");
    lv_obj_align(s_stars, LV_ALIGN_TOP_MID, 0, TRACING_PROGRESS_Y + 4);
    lv_obj_add_flag(s_stars, LV_OBJ_FLAG_HIDDEN);
    /* Next-character shortcut (mockup v1's done page): ">" in
     * Montserrat, ACCENT, at the grid's right edge. */
    s_next = label(page, &lv_font_montserrat_28, MUSE_THEME_ACCENT);
    lv_label_set_text(s_next, ">");
    lv_obj_align(s_next, LV_ALIGN_TOP_LEFT, TRACING_NEXT_X,
                 TRACING_NEXT_Y);
    lv_obj_add_flag(s_next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_next, on_next, LV_EVENT_CLICKED, NULL);
    /* Initial character, before the timer exists: open_char() only
     * resumes the timer when it is already created, so the build
     * does not start a 50 ms tick at boot -- the timer starts when
     * the page is actually opened. */
    muse_tracing_ui_open_char(0);
    s_timer = lv_timer_create(on_timer, TRACING_UI_TICK_MS, NULL);
    lv_timer_pause(s_timer);
}

void muse_tracing_ui_open_char(int idx)
{
    if (idx < 0 || idx >= (int)MUSE_TRACING_CHARS || !s_trace_page) {
        return;
    }
    s_char_idx = idx;
    s_reward_sent = false;
    muse_tracing_set_char(&s_trace, idx);
    ink_clear();
    const muse_tracing_char_t *ch = &MUSE_TRACING_TABLE[idx];
    lv_label_set_text(s_ghost, ch->utf8);
    char buf[32];
    snprintf(buf, sizeof(buf), "第 %d/10 个字", idx + 1);
    lv_label_set_text(s_progress, buf);
    show_hint();
    if (s_timer) {
        lv_timer_resume(s_timer);
    }
}

void muse_tracing_ui_hide(void)
{
    s_pen_down = false;
    if (s_timer) {
        lv_timer_pause(s_timer);
    }
}
