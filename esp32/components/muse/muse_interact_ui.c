/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_interact_ui.c -- 1.5a interaction tile + feed / game / fortune
 * subpages. See muse_interact_ui.h for the threading contract.
 *
 * Layout (design §5.2, mockups v11/v17/v18/v22; all coordinates are
 * 360x360-circle coordinates, NOT the 466x466 TamaMe coordinates):
 *   - Title "互动" and the back "<" share centre line y = 52. The back
 *     button is 56x48 with its top at y = 28, so its centre is x = 68
 *     (NOT x = 28: at y = 52 the circle chord starts near x = 54 and a
 *     button centred at 28 would be clipped by the round edge).
 *   - Main page: three buttons, LIST_W 330, ROW_H 58, first row at
 *     LIST_TOP 84, 8px gaps (84 / 150 / 216). Status line 1 at y ~285,
 *     status line 2 at y ~308; the result note SHARES the line-2 slot
 *     while a result exists (see the note in show_note() -- the circle
 *     chord at the bottom cannot fit buttons + two status lines + a
 *     separate note line, and the note must not be a third line that
 *     gets clipped. On leaving the tile the note is cleared and line 2
 *     returns to the摸摸/喂饭 counts).
 *   - Subpages are groups inside the interaction tile, all with the same
 *     title/back pattern (title centred y = 52, "<" at centre x = 68).
 *     Tile swipe stays ENABLED inside subpages (unlike settings subpages):
 *     design §5.2 says the tile-swipe-disable logic does not apply to the
 *     interaction tile, so swiping away from a subpage is legal and
 *     muse_interact_ui_on_hidden() resets it.
 *
 * Colours: taken from 1.4d muse_settings_ui.c / muse_ui.c, NOT invented:
 *   TEXT 0xf2efff, DIM 0x8b84a8, CARD 0x1a1530, PRESSED 0x2e2552,
 *   ACCENT 0xa77dff, OK 0x6ff0bf, GOLD 0xf6cb4b (fortune grade, and the
 *   upstream HUD coin colour). DANGER: 1.4d settings uses 0xff5c5c;
 *   the phase-2 brief listed 0xff6b6b. This file uses the in-repo value
 *   (0xff5c5c) so the game "you lost" red matches the settings pages;
 *   if the brief value is preferred, change ONE #define below.
 * Fonts: title/body use the repo CJK font (muse_font_cjk_16 under
 *   CONFIG_MUSE_CJK_FONT, else montserrat_16). The mockups' "18号" body
 *   is a design size, not a font file in the repo: the code renders the
 *   fortune body at the CJK font with 26px line spacing and the
 *   per-card on-screen check (design §8) is the arbiter of fit. Do NOT
 *   invent an 18px font symbol to make the code look exact.
 * Game hand images: see muse_interact_hand_assets.h (generated assets,
 *   Game-icons, CC BY 3.0, rotated CCW 90 degrees). Feed food images:
 *   see muse_interact_food_art.h (upstream 16x16 pixel art, drawn by
 *   draw_food_cb() at 5x, nearest neighbour).
 *
 * What this file deliberately does NOT do:
 *   - No tileview, no dots, no caption label (muse_ui.c owns those).
 *   - No muse_menu.c (that is the dual-key/keyboard menu; 1.85B has
 *     aux_button = NULL and must not use it as an interaction entry).
 *   - No IMU, no touch wake, no GPIO4 wake line, no power lock, no
 *     polling faster than the LVGL frame tick.
 *   - No NVS writes, no direct state mutation, no SFX (design §5.3).
 */
#include "muse_interact_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "muse_interact_food_art.h"
#include "muse_interact_hand_assets.h"
#include "muse_pet.h"
#include "muse_pet_text.h"
#include "muse_state.h"
#include "muse_ui.h"

/* --- colours (see file header for the DANGER note) --- */
#define IC_TEXT 0xf2efff
#define IC_DIM 0x8b84a8
#define IC_CARD 0x1a1530
#define IC_PRESSED 0x2e2552
#define IC_ACCENT 0xa77dff
#define IC_OK 0x6ff0bf
#define IC_DANGER 0xff5c5c
#define IC_GOLD 0xf6cb4b

/* --- layout --- */
#define IC_TITLE_CY 52
#define IC_BACK_CX 68
#define IC_BACK_W 56
#define IC_BACK_H 48
#define IC_BACK_Y 28
#define IC_LIST_W 330
#define IC_LIST_TOP 84
#define IC_ROW_H 58
#define IC_ROW_GAP 8
#define IC_RADIUS 18
#define IC_FEED_CARD 100
#define IC_FEED_ICON 80
#define IC_GAME_CHOICE 100
#define IC_GAME_RESULT 130
#define IC_FORTUNE_CARD_W 300
#define IC_FORTUNE_CARD_H 150

/* --- timing (design §5.5: ~2s total, never over 3s) --- */
#define IC_SHAKE_MS 1000
#define IC_SHAKE_STEP_MS 166 /* ~6 jitter steps inside the shake second */
#define IC_REVEAL_MS 800
#define IC_CAPTION_MS 3000
#define IC_SHAKE_PX 4

/* --- game choices: button order is rock, paper, scissors. The chant
 * order in the shake subtitle ("石头！剪刀！布！") is deliberately
 * different -- it is the spoken chant, not the button order. --- */
enum {
    IC_ROCK = 0,
    IC_PAPER = 1,
    IC_SCISSORS = 2,
};

enum {
    IC_PAGE_MAIN = 0,
    IC_PAGE_FEED,
    IC_PAGE_GAME,
    IC_PAGE_FORTUNE,
};

enum {
    IC_GAME_CHOICE_VIEW = 0, /* choice row visible, no result yet */
    IC_GAME_SHAKE,           /* 1s: chant + jitter, buttons dimmed */
    IC_GAME_REVEAL,          /* 0.8s: both hands shown, then hold */
    IC_GAME_RESULT,          /* hold: click a choice = next round */
};

static lv_obj_t *s_tile;
static lv_obj_t *s_main_page, *s_feed_page, *s_game_page, *s_fortune_page;
static lv_obj_t *s_btn_feed, *s_btn_game, *s_btn_fortune;
static lv_obj_t *s_btn_feed_lbl, *s_btn_game_lbl, *s_btn_fortune_lbl;
static int s_last_feed_idx;
static uint32_t s_last_feed_before;
static lv_obj_t *s_status1, *s_status2, *s_note;
static lv_obj_t *s_feed_cards[3];
static lv_obj_t *s_game_choices[3];
static int s_game_choice_base_x[3];
static lv_obj_t *s_game_you_img, *s_game_house_img;
static lv_obj_t *s_game_result_lbl, *s_game_streak_lbl, *s_game_chant_lbl;
static lv_obj_t *s_game_coins_lbl;
static bool s_storage_noted, s_unready_noted;
static lv_obj_t *s_fortune_grade, *s_fortune_body;

static int s_page = IC_PAGE_MAIN;
static int s_game_phase = IC_GAME_CHOICE_VIEW;
static uint32_t s_game_phase_ms;
static int s_game_player = -1, s_game_house = -1, s_game_result; /* -1 draw */
static int s_wins, s_losses, s_draws, s_streak; /* session only, NOT NVS */
static bool s_game_pending; /* PET_GAME requested, feedback not yet seen */
static bool s_touch_failed;
static bool s_decay_pending;
static int32_t s_decay_announced_day = -2;

/* Pet caption bookkeeping: the exact text we last wrote, and when it
 * expires. Expiry only clears the caption if the caption still IS this
 * text (see caption_tick()): a dialog caption that arrived meanwhile
 * must never be cleared by a pet timer. */
static char s_pet_caption[400];
static uint32_t s_pet_caption_until_ms;
static bool s_pet_caption_active;
static uint32_t s_caption_read_version = 0xFFFFFFFFu;

static muse_pet_model_t s_snap;
static bool s_have_snap;

#if CONFIG_MUSE_CJK_FONT
LV_FONT_DECLARE(muse_font_cjk_16)
static const lv_font_t *cjk_font(void) { return &muse_font_cjk_16; }
#else
static const lv_font_t *cjk_font(void) { return &lv_font_montserrat_16; }
#endif

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

/* Title + back button shared by the main page and all three subpages.
 * The title label is centred on y = 52 by aligning its centre, not its
 * top: copying the settings title top (y = 44) would put a larger CJK
 * title off the button's centre line (design §5.2, the v7 lesson). */
static void page_header(lv_obj_t *page, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *back = lv_btn_create(page);
    lv_obj_set_size(back, IC_BACK_W, IC_BACK_H);
    lv_obj_set_pos(back, IC_BACK_CX - IC_BACK_W / 2, IC_BACK_Y);
    lv_obj_set_style_bg_color(back, lv_color_hex(IC_CARD), 0);
    lv_obj_set_style_radius(back, IC_RADIUS, 0);
    lv_obj_t *arrow = label(back, &lv_font_montserrat_20, IC_ACCENT, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = label(page, cjk_font(), IC_ACCENT, title);
    /* Centre the title on y = 52: label height is font-dependent, so
     * align by centre rather than by a copied top offset. */
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, IC_TITLE_CY - 12);
}

static void show_page(int page)
{
    s_page = page;
    lv_obj_add_flag(s_main_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_feed_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_fortune_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *show = s_main_page;
    if (page == IC_PAGE_FEED) show = s_feed_page;
    if (page == IC_PAGE_GAME) show = s_game_page;
    if (page == IC_PAGE_FORTUNE) show = s_fortune_page;
    lv_obj_remove_flag(show, LV_OBJ_FLAG_HIDDEN);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    /* Back from a subpage always lands on the interaction main page, never
     * directly on the face: the face is one swipe (or the main page's own
     * back button) away, and a subpage back that skipped a level would
     * break the "left < = one level up" rule. */
    if (s_page == IC_PAGE_MAIN) {
        muse_ui_show_face();
    } else {
        show_page(IC_PAGE_MAIN);
    }
}

static void show_note(const char *text)
{
    /* The note shares the status-line-2 slot while a result exists (see
     * the file header). Hiding line 2 is what makes room for it without
     * pushing anything off the bottom chord of the circle. */
    lv_label_set_text(s_note, text);
    lv_obj_remove_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status2, LV_OBJ_FLAG_HIDDEN);
}

static void clear_note(void)
{
    lv_label_set_text(s_note, "");
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_status2, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_status(void)
{
    if (!s_have_snap) {
        lv_label_set_text(s_status1, "金币 --/100");
        lv_label_set_text(s_status2, "摸摸 --/50 · 喂饭 --/10");
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "金币 %lu/100", (unsigned long)s_snap.coins);
    lv_label_set_text(s_status1, buf);
    snprintf(buf, sizeof(buf), "摸摸 %lu/50 · 喂饭 %lu/10",
             (unsigned long)s_snap.daily_touch_coins,
             (unsigned long)s_snap.daily_feed_coins);
    lv_label_set_text(s_status2, buf);

    /* Dynamic button copy (design §5.2). Dimming is a pre-press signal,
     * not a disable: a capped feed button still works (happy only), and
     * a poor game button still explains itself on press. */
    lv_label_set_text(s_btn_feed_lbl,
                      s_snap.daily_feed_coins >= 10 ? "喂饭 · 今日奖励已领"
                                                    : "喂饭 · +10金币");
    lv_obj_set_style_text_color(s_btn_feed_lbl,
                                lv_color_hex(s_snap.daily_feed_coins >= 10 ? IC_DIM : IC_TEXT), 0);
    lv_label_set_text(s_btn_game_lbl,
                      s_snap.coins < 5 ? "小游戏 · 金币不够" : "小游戏 · -5金币");
    lv_obj_set_style_text_color(s_btn_game_lbl,
                                lv_color_hex(s_snap.coins < 5 ? IC_DIM : IC_TEXT), 0);
    if (s_snap.fortune_day >= 0) {
        const char *grade = muse_pet_fortune_grade(s_snap.fortune);
        char fbuf[48];
        snprintf(fbuf, sizeof(fbuf), "今日运势 · %s", grade ? grade : "已抽");
        lv_label_set_text(s_btn_fortune_lbl, fbuf);
    } else {
        lv_label_set_text(s_btn_fortune_lbl, "今日运势 · 未抽");
    }
}

static lv_obj_t *main_button(lv_obj_t *parent, int y, const char *text,
                             lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, IC_LIST_W, IC_ROW_H);
    lv_obj_set_pos(b, (360 - IC_LIST_W) / 2, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(IC_CARD), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(IC_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, IC_RADIUS, 0);
    lv_obj_t *l = label(b, &lv_font_montserrat_20, IC_TEXT, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (lbl_out) {
        *lbl_out = l;
    }
    return b;
}

/* --- global pet caption (subtitle) --- */

static void write_pet_caption(const char *text)
{
    /* Design §5.3 gate: IDLE only. A feedback item that arrives mid-dialog
     * is dropped as a caption (the interaction-tile note, where applicable,
     * still updates) -- never covering a reply caption. */
    if (muse_state_mode(NULL) != MUSE_MODE_IDLE) {
        return;
    }
    muse_state_set_caption("%s", text);
    snprintf(s_pet_caption, sizeof(s_pet_caption), "%s", text);
    s_pet_caption_until_ms = lv_tick_get() + IC_CAPTION_MS;
    s_pet_caption_active = true;
}

static void caption_tick(void)
{
    if (!s_pet_caption_active) {
        return;
    }
    if ((int32_t)(lv_tick_get() - s_pet_caption_until_ms) < 0) {
        return;
    }
    s_pet_caption_active = false;
    /* Design §5.3 adds a second guard: only clear while still IDLE. If a
     * dialog has started, the caption is the dialog's now -- drop the
     * pending flag without touching it (the strcmp check below would
     * also have caught it, but the mode guard is the documented one). */
    if (muse_state_mode(NULL) != MUSE_MODE_IDLE) {
        s_pet_caption[0] = '\0';
        return;
    }
    /* Only clear if the caption still IS the pet caption. muse_state_caption
     * copies on version change; the persistent read version below makes
     * sure a dialog caption that arrived meanwhile is seen, not assumed. */
    char cur[400];
    uint32_t ver = s_caption_read_version;
    if (muse_state_caption(cur, sizeof(cur), &ver)) {
        s_caption_read_version = ver;
    } else {
        /* Unchanged since our last read: it is still whatever we last
         * saw, which -- because we only set this flag right after writing
         * the pet caption -- is the pet caption itself. */
        snprintf(cur, sizeof(cur), "%s", s_pet_caption);
    }
    if (strcmp(cur, s_pet_caption) == 0) {
        muse_state_set_caption("");
    }
    s_pet_caption[0] = '\0';
}

/* --- feed subpage: upstream 16x16 pixel art, drawn at 5x --- */

static void draw_food_cb(lv_event_t *e)
{
    /* Draw callback: LVGL draw pass only. No allocation, no worker calls,
     * no state writes. `user_data` is the food index (0 rice, 1 cake,
     * 2 coffee, in MUSE_INTERACT_FOODS order). */
    lv_obj_t *obj = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 2) {
        return;
    }
    const muse_interact_food_art_t *food = &MUSE_INTERACT_FOODS[idx];
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int w = lv_area_get_width(&area), h = lv_area_get_height(&area);
    int cell = w / 16;
    if (cell < 1) {
        cell = 1;
    }
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint32_t rgb = muse_interact_food_pixel_color(food->rows[y][x]);
            if (rgb == 0) {
                continue; /* transparent cell: leave the card showing */
            }
            dsc.bg_color = lv_color_hex(rgb);
            lv_area_t cell_area = {
                .x1 = area.x1 + x * cell,
                .y1 = area.y1 + y * cell,
                .x2 = area.x1 + (x + 1) * cell - 1,
                .y2 = area.y1 + (y + 1) * cell - 1,
            };
            lv_draw_rect(lv_event_get_layer(e), &dsc, &cell_area);
        }
    }
    (void)h;
}

static void on_feed_food(lv_event_t *e)
{
    /* Feed is the one subpage that closes on choice (design §5.2): the
     * feedback is the subtitle + the coin balance, not a feed-page state.
     * The food index is the action value: PET_FEED_RICE/CAKE/COFFEE
     * settle identically, but the subtitle needs to know WHICH food was
     * fed, so it is remembered here and matched against the feedback's
     * action in drain_feedback(). */
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    muse_pet_action_t act = PET_FEED_RICE;
    if (idx == 1) act = PET_FEED_CAKE;
    if (idx == 2) act = PET_FEED_COFFEE;
    s_last_feed_idx = idx;
    s_last_feed_before = s_have_snap ? s_snap.daily_feed_coins : 0;
    if (!muse_pet_request(act)) {
        write_pet_caption("互动未就绪");
        show_note("互动未就绪");
        return;
    }
    show_page(IC_PAGE_MAIN);
    /* Do not pre-write a success caption: the subtitle comes from the
     * worker's feedback (design §5.3), never from the button press. */
    (void)e;
}

/* --- game subpage --- */

static const lv_image_dsc_t *hand_img(int choice)
{
    if (choice == IC_ROCK) return &muse_interact_hand_rock;
    if (choice == IC_PAPER) return &muse_interact_hand_paper;
    return &muse_interact_hand_scissors;
}

static const char *hand_name(int choice)
{
    if (choice == IC_ROCK) return "石头";
    if (choice == IC_PAPER) return "布";
    return "剪刀";
}

/* 1 beats 0, 2 beats 1, 0 beats 2 (paper wraps rock, scissors cut paper,
 * rock crushes scissors). Returns +1 player wins, -1 player loses,
 * 0 draw. This is UI-local: the model never settles win/loss (design
 * §5.2 -- the model only deducts the 5 coins; the random result is
 * decided at click time, in this file, and never enters NVS). */
static int game_result(int player, int house)
{
    if (player == house) {
        return 0;
    }
    if ((player == IC_PAPER && house == IC_ROCK) ||
        (player == IC_SCISSORS && house == IC_PAPER) ||
        (player == IC_ROCK && house == IC_SCISSORS)) {
        return 1;
    }
    return -1;
}

static void set_choice_dim(bool dim)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_game_choices[i],
                                  lv_color_hex(dim ? IC_PRESSED : IC_CARD), 0);
    }
}

static void show_game_result(void)
{
    /* Reveal view: two hands side by side, ~130px each, in place of the
     * choice row (the choice row is hidden during the 0.8s reveal).
     * Winner gets an ACCENT border; loser is recoloured to DIM (same
     * asset, lv_image_set_recolor -- NOT a second asset, NOT an opaque
     * overlay); draw gets neither. When the reveal hold ends, the tick
     * hides the hands and brings the choice row back, keeping the result
     * text + streak visible: that is the "choice button stays for the
     * next round" state (design §5.2). Keeping hands AND the choice row
     * on screen at once would not fit the 360 circle, so they share
     * the row slot in time, not in space. */
    lv_image_set_src(s_game_you_img, hand_img(s_game_player));
    lv_image_set_src(s_game_house_img, hand_img(s_game_house));
    lv_obj_set_style_border_width(s_game_you_img, 0, 0);
    lv_obj_set_style_border_width(s_game_house_img, 0, 0);
    lv_image_set_recolor(s_game_you_img, lv_color_hex(IC_TEXT));
    lv_image_set_recolor(s_game_house_img, lv_color_hex(IC_TEXT));
    if (s_game_result > 0) {
        lv_obj_set_style_border_width(s_game_you_img, 3, 0);
        lv_obj_set_style_border_color(s_game_you_img, lv_color_hex(IC_ACCENT), 0);
        lv_image_set_recolor(s_game_house_img, lv_color_hex(IC_DIM));
        lv_obj_set_style_text_color(s_game_result_lbl, lv_color_hex(IC_OK), 0);
        lv_label_set_text(s_game_result_lbl, "你赢了！");
    } else if (s_game_result < 0) {
        lv_obj_set_style_border_width(s_game_house_img, 3, 0);
        lv_obj_set_style_border_color(s_game_house_img, lv_color_hex(IC_ACCENT), 0);
        lv_image_set_recolor(s_game_you_img, lv_color_hex(IC_DIM));
        /* No error shake on losing (design §5.5): the loser dims, it
         * does not jitter. */
        lv_obj_set_style_text_color(s_game_result_lbl, lv_color_hex(IC_DANGER), 0);
        lv_label_set_text(s_game_result_lbl, "你输了");
    } else {
        lv_obj_set_style_text_color(s_game_result_lbl, lv_color_hex(IC_DIM), 0);
        lv_label_set_text(s_game_result_lbl, "平局，再来一局");
    }
    lv_obj_remove_flag(s_game_result_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_game_streak_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game_chant_lbl, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) {
        lv_obj_add_flag(s_game_choices[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_game_you_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_game_house_img, LV_OBJ_FLAG_HIDDEN);
    /* Choice row stays hidden during the reveal; the tick brings it back
     * when the reveal hold ends (see game_tick, IC_GAME_REVEAL). */
}

static void reset_game_view(void)
{
    s_game_phase = IC_GAME_CHOICE_VIEW;
    s_game_pending = false;
    s_game_player = -1;
    s_game_house = -1;
    lv_obj_add_flag(s_game_you_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game_house_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game_result_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game_streak_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game_chant_lbl, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) {
        lv_obj_remove_flag(s_game_choices[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(s_game_choices[i], s_game_choice_base_x[i]);
    }
    set_choice_dim(false);
}

static void on_game_choice(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_game_phase == IC_GAME_SHAKE) {
        return; /* shake: buttons are dimmed and not re-clickable */
    }
    if (!s_have_snap || s_snap.coins < 5) {
        /* Poor, checked against the last snapshot before any deduction
         * (design §5.2: coins < 5 never deducts, never starts a game). */
        write_pet_caption("金币不够，先摸摸 / 喂饭");
        show_note("金币不够，先摸摸 / 喂饭");
        return;
    }
    if (!muse_pet_request(PET_GAME)) {
        write_pet_caption("互动未就绪");
        show_note("互动未就绪");
        return;
    }
    /* Random result is decided NOW, at click time (design §5.2). The
     * animation that follows only reveals it; the worker's PET_GAME
     * feedback confirms the 5-coin deduction, it does not decide the
     * outcome. If that feedback later comes back PET_POOR (snapshot was
     * stale and the balance actually was < 5), the game is cancelled in
     * drain_feedback() -- the deduction never happened, so no refund is
     * owed either way. */
    s_game_player = choice;
    s_game_house = (int)(esp_random() % 3);
    s_game_result = game_result(s_game_player, s_game_house);
    s_game_phase = IC_GAME_SHAKE;
    s_game_phase_ms = lv_tick_get();
    s_game_pending = true;
    set_choice_dim(true);
    lv_obj_add_flag(s_game_you_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game_house_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game_result_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game_streak_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_game_chant_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_game_chant_lbl, "石头！");
    for (int i = 0; i < 3; i++) {
        lv_obj_remove_flag(s_game_choices[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void game_tick(void)
{
    if (s_page != IC_PAGE_GAME) {
        return;
    }
    uint32_t now = lv_tick_get();
    if (s_game_phase == IC_GAME_SHAKE) {
        uint32_t elapsed = now - s_game_phase_ms;
        /* Chant: "石头！剪刀！布！" in ~300ms steps (design §5.5). The
         * chant order is the spoken order, not the button order. */
        const char *chant = "石头！";
        if (elapsed >= 600) chant = "布！";
        else if (elapsed >= 300) chant = "剪刀！";
        lv_label_set_text(s_game_chant_lbl, chant);
        /* Jitter: +/-4px, ~3 times in the shake second (6 steps of
         * ~166ms). All three choice images jitter together. */
        int step = (int)(elapsed / IC_SHAKE_STEP_MS);
        int off = (step % 2 == 0) ? IC_SHAKE_PX : -IC_SHAKE_PX;
        for (int i = 0; i < 3; i++) {
            lv_obj_set_x(s_game_choices[i], s_game_choice_base_x[i] + off);
        }
        if (elapsed >= IC_SHAKE_MS) {
            for (int i = 0; i < 3; i++) {
                lv_obj_set_x(s_game_choices[i], s_game_choice_base_x[i]);
            }
            set_choice_dim(false);
            if (s_game_result > 0) {
                s_wins++;
                s_streak++;
            } else if (s_game_result < 0) {
                s_losses++;
                s_streak = 0;
            } else {
                s_draws++;
                s_streak = 0;
            }
            char streak[64];
            if (s_streak > 0) {
                snprintf(streak, sizeof(streak), "连胜 %d · %d胜 %d平 %d负",
                         s_streak, s_wins, s_draws, s_losses);
            } else {
                snprintf(streak, sizeof(streak), "%d胜 %d平 %d负",
                         s_wins, s_draws, s_losses);
            }
            lv_label_set_text(s_game_streak_lbl, streak);
            show_game_result();
            s_game_phase = IC_GAME_REVEAL;
            s_game_phase_ms = now;
            if (s_game_result > 0) {
                /* Win: happy is a side channel only (design §5.5) -- the
                 * game page itself already says who won; do not depend
                 * on the face tile being visible. */
                muse_state_make_happy();
            }
        }
    } else if (s_game_phase == IC_GAME_REVEAL) {
        if (now - s_game_phase_ms >= IC_REVEAL_MS) {
            /* Reveal hold ends: hands leave, choice row returns, result
             * text + streak stay. No auto-leave, no power lock
             * (design §5.2). A click on a choice now starts the next
             * round (and re-checks the 5-coin balance first). */
            lv_obj_add_flag(s_game_you_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_game_house_img, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 3; i++) {
                lv_obj_remove_flag(s_game_choices[i], LV_OBJ_FLAG_HIDDEN);
            }
            set_choice_dim(false);
            s_game_phase = IC_GAME_RESULT;
        }
    }
}

/* --- fortune subpage --- */

static void show_fortune_card(void)
{
    if (!s_have_snap || s_snap.fortune_day < 0) {
        lv_label_set_text(s_fortune_grade, "—");
        lv_label_set_text(s_fortune_body, "抽取中…");
        return;
    }
    const char *grade = muse_pet_fortune_grade(s_snap.fortune);
    const char *text = muse_pet_fortune_text(s_snap.fortune);
    lv_label_set_text(s_fortune_grade, grade ? grade : "吉");
    lv_label_set_text(s_fortune_body, text ? text : "");
}

static void on_fortune_open(lv_event_t *e)
{
    (void)e;
    /* The interact fortune button IS the draw: it requests PET_FORTUNE
     * and opens the subpage (design §5.2: direct reveal, no fake shuffle,
     * no separate "draw" button on the subpage). A same-day repeat
     * returns PET_FORTUNE_SAME and shows the same card -- that IS the
     * designed behaviour, not a bug. */
    if (!muse_pet_request(PET_FORTUNE)) {
        write_pet_caption("互动未就绪");
        show_note("互动未就绪");
        return;
    }
    show_page(IC_PAGE_FORTUNE);
    show_fortune_card(); /* "抽取中…" until the feedback lands */
}

/* --- feedback drain (LVGL task only) --- */

static void drain_feedback(void)
{
    muse_pet_feedback_t fb;
    while (muse_pet_feedback(&fb)) {
        /* Refresh the snapshot first: every caption number comes from
         * it, never from a locally guessed value (design §5.3). */
        muse_pet_model_t snap;
        if (muse_pet_snapshot(&snap)) {
            s_snap = snap;
            s_have_snap = true;
        }
        char buf[400];
        switch (fb.action) {
        case PET_TOUCH:
            if (fb.result == PET_COIN) {
                snprintf(buf, sizeof(buf), "毛毛很开心 +5 金币（今日摸摸 %lu/50）",
                         (unsigned long)s_snap.daily_touch_coins);
                muse_state_make_happy();
            } else if (fb.result == PET_CAP) {
                snprintf(buf, sizeof(buf), "今日摸摸奖励已满 50 金币");
                muse_state_make_happy();
            } else if (fb.result == PET_STORAGE) {
                snprintf(buf, sizeof(buf), "互动数据损坏");
            } else {
                snprintf(buf, sizeof(buf), "互动未就绪");
            }
            write_pet_caption(buf);
            break;
        case PET_FEED_RICE:
        case PET_FEED_CAKE:
        case PET_FEED_COFFEE: {
            /* PET_OK means "settled"; whether coins moved is read from
             * the snapshot's daily_feed_coins vs the value at click time
             * (design §4.3) -- the result code alone does not say. */
            const char *food = MUSE_INTERACT_FOODS[s_last_feed_idx].name;
            if (fb.result == PET_OK) {
                if (s_snap.daily_feed_coins > s_last_feed_before) {
                    snprintf(buf, sizeof(buf), "喂了%s +10 金币", food);
                    muse_state_make_happy();
                    show_note(buf);
                } else {
                    snprintf(buf, sizeof(buf), "喂了%s（今日奖励已领）", food);
                    show_note(buf);
                }
                write_pet_caption(buf);
            } else if (fb.result == PET_STORAGE) {
                write_pet_caption("互动数据损坏");
                show_note("互动数据损坏");
            } else {
                write_pet_caption("互动未就绪");
                show_note("互动未就绪");
            }
            break;
        }
        case PET_GAME:
            if (fb.result == PET_POOR) {
                /* Snapshot was stale: the 5 coins were never deducted,
                 * so there is nothing to refund -- cancel the animation
                 * (which was only a reveal of a never-started game). */
                reset_game_view();
                write_pet_caption("金币不够，先摸摸 / 喂饭");
                show_note("金币不够，先摸摸 / 喂饭");
            } else if (fb.result == PET_OK) {
                /* Deduction confirmed; the outcome was already decided
                 * at click time, so nothing more to do here. If the
                 * player has already left the game page, the coin
                 * balance refresh below still lands on the tile. */
                s_game_pending = false;
            } else if (fb.result == PET_STORAGE) {
                reset_game_view();
                write_pet_caption("互动数据损坏");
                show_note("互动数据损坏");
            }
            break;
        case PET_FORTUNE:
            show_fortune_card();
            if (fb.result == PET_FORTUNE_NEW || fb.result == PET_FORTUNE_SAME) {
                /* Interface gap (see PHASE2-NOTES.md §4): the phase-1
                 * text table has grade + body but NO card name, so the
                 * design's "今日运势：<卡名> · <等级>" cannot be written
                 * verbatim. The caption uses the grade; the subpage
                 * shows grade + full body. Do NOT invent card names. */
                const char *grade = muse_pet_fortune_grade(s_snap.fortune);
                if (fb.result == PET_FORTUNE_NEW) {
                    snprintf(buf, sizeof(buf), "今日运势：%s",
                             grade ? grade : "吉");
                } else {
                    snprintf(buf, sizeof(buf), "今日已抽过：%s",
                             grade ? grade : "吉");
                }
                write_pet_caption(buf);
                show_note(buf);
            } else if (fb.result == PET_STORAGE) {
                write_pet_caption("互动数据损坏");
                show_note("互动数据损坏");
            }
            break;
        default:
            /* PET_DECAY_ACK produces no feedback by design (phase 1);
             * PET_RESET is console-only. Ignore anything else rather
             * than showing a wrong caption. */
            break;
        }
        refresh_status();
    }
}

/* --- build: main page + three subpages --- */

static void on_open_feed(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_FEED);
}

static void on_open_game(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_GAME);
    /* Opening the game page never starts a game: only a choice click
     * requests PET_GAME. A stale result from a previous visit is gone
     * by then (on_hidden reset it when the tile was left). */
}

static lv_obj_t *page_group(lv_obj_t *tile)
{
    lv_obj_t *g = lv_obj_create(tile);
    lv_obj_set_size(g, 360, 360);
    lv_obj_set_pos(g, 0, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
    return g;
}

static void build_main(void)
{
    page_header(s_main_page, "互动", on_back);
    s_btn_feed = main_button(s_main_page, IC_LIST_TOP, "喂饭 · +10金币",
                             on_open_feed, &s_btn_feed_lbl);
    s_btn_game = main_button(s_main_page, IC_LIST_TOP + IC_ROW_H + IC_ROW_GAP,
                             "小游戏 · -5金币", on_open_game, &s_btn_game_lbl);
    s_btn_fortune = main_button(s_main_page,
                                IC_LIST_TOP + 2 * (IC_ROW_H + IC_ROW_GAP),
                                "今日运势 · 未抽", on_fortune_open,
                                &s_btn_fortune_lbl);
    /* Status lines, centred. Line 1: coins. Line 2: touch/feed daily
     * counts -- hidden while a result note occupies its slot. */
    s_status1 = label(s_main_page, cjk_font(), IC_TEXT, "金币 --/100");
    lv_obj_align(s_status1, LV_ALIGN_TOP_MID, 0, 282);
    s_status2 = label(s_main_page, cjk_font(), IC_TEXT, "摸摸 --/50 · 喂饭 --/10");
    lv_obj_align(s_status2, LV_ALIGN_TOP_MID, 0, 306);
    s_note = label(s_main_page, cjk_font(), IC_TEXT, "");
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, 306);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_note, 300);
}

static void build_feed(void)
{
    page_header(s_feed_page, "喂饭", on_back);
    /* Three cards in one row: 100px each, 8px gaps, centred as a group
     * (total 316px). Icon 80px inside the 100px card, food name below
     * the card in DIM. Cards are clickable containers, not lv_btn: the
     * pixel drawing is a custom draw callback on the container itself,
     * and a button's default style would fight the card look. */
    int total = 3 * IC_FEED_CARD + 2 * 8;
    int x0 = (360 - total) / 2;
    int y = 120;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *card = lv_obj_create(s_feed_page);
        lv_obj_set_size(card, IC_FEED_CARD, IC_FEED_CARD);
        lv_obj_set_pos(card, x0 + i * (IC_FEED_CARD + 8), y);
        lv_obj_set_style_bg_color(card, lv_color_hex(IC_CARD), 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(IC_PRESSED), LV_STATE_PRESSED);
        lv_obj_set_style_radius(card, IC_RADIUS, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        /* The icon is a transparent child rect, 80px, centred in the
         * card; the draw callback paints the 16x16 grid into it. */
        lv_obj_t *icon = lv_obj_create(card);
        lv_obj_set_size(icon, IC_FEED_ICON, IC_FEED_ICON);
        lv_obj_center(icon);
        lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        /* DRAW_MAIN_BEGIN, not DRAW_MAIN: the icon's own background is
         * transparent, so painting before the widget's own draw pass
         * cannot be overpainted by it, and the order against the class
         * handler is not load-bearing. If the repo's LVGL version
         * names this event differently, change only this line. */
        lv_obj_add_event_cb(icon, draw_food_cb, LV_EVENT_DRAW_MAIN_BEGIN, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, on_feed_food, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_feed_cards[i] = card;
        lv_obj_t *nm = label(s_feed_page, cjk_font(), IC_DIM,
                             MUSE_INTERACT_FOODS[i].name);
        lv_obj_align(nm, LV_ALIGN_TOP_MID,
                     x0 + i * (IC_FEED_CARD + 8) + IC_FEED_CARD / 2 - 180, y + IC_FEED_CARD + 8);
    }
    lv_obj_t *hint = label(s_feed_page, cjk_font(), IC_DIM, "喂饭不花币，每天第一次 +10 金币");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 270);
}

static void build_game(void)
{
    page_header(s_game_page, "小游戏", on_back);
    lv_obj_t *coins = label(s_game_page, cjk_font(), IC_TEXT, "金币 --/100");
    lv_obj_align(coins, LV_ALIGN_TOP_MID, 0, 84);
    /* The coin line is a live status, not a static label: refresh_status
     * rewrites s_status1 only, so the game page keeps its own pointer
     * and the tick rewrites it from the snapshot too. */
    s_game_coins_lbl = coins;
    /* Choice row: three 100px image buttons, 8px gaps, centred
     * (total 316px), top at y = 120. Buttons contain ONLY the hand
     * image -- no text (user decision: image click, not text button). */
    int total = 3 * IC_GAME_CHOICE + 2 * 8;
    int x0 = (360 - total) / 2;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_btn_create(s_game_page);
        lv_obj_set_size(b, IC_GAME_CHOICE, IC_GAME_CHOICE);
        lv_obj_set_pos(b, x0 + i * (IC_GAME_CHOICE + 8), 120);
        lv_obj_set_style_bg_color(b, lv_color_hex(IC_CARD), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(IC_PRESSED), LV_STATE_PRESSED);
        lv_obj_set_style_radius(b, IC_RADIUS, 0);
        lv_obj_t *img = lv_image_create(b);
        lv_image_set_src(img, hand_img(i));
        lv_obj_center(img);
        lv_obj_add_event_cb(b, on_game_choice, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_game_choices[i] = b;
        s_game_choice_base_x[i] = x0 + i * (IC_GAME_CHOICE + 8);
    }
    /* Reveal hands: same row slot, ~130px each, left = you, right = house.
     * They are created once and hidden until a reveal. */
    s_game_you_img = lv_image_create(s_game_page);
    lv_obj_set_pos(s_game_you_img, (360 - IC_GAME_RESULT * 2 - 16) / 2, 105);
    lv_obj_set_size(s_game_you_img, IC_GAME_RESULT, IC_GAME_RESULT);
    lv_obj_add_flag(s_game_you_img, LV_OBJ_FLAG_HIDDEN);
    s_game_house_img = lv_image_create(s_game_page);
    lv_obj_set_pos(s_game_house_img, (360 - IC_GAME_RESULT * 2 - 16) / 2 + IC_GAME_RESULT + 16, 105);
    lv_obj_set_size(s_game_house_img, IC_GAME_RESULT, IC_GAME_RESULT);
    lv_obj_add_flag(s_game_house_img, LV_OBJ_FLAG_HIDDEN);
    /* Result area below the row slot. */
    s_game_chant_lbl = label(s_game_page, cjk_font(), IC_TEXT, "石头！");
    lv_obj_align(s_game_chant_lbl, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_add_flag(s_game_chant_lbl, LV_OBJ_FLAG_HIDDEN);
    s_game_result_lbl = label(s_game_page, cjk_font(), IC_TEXT, "");
    lv_obj_align(s_game_result_lbl, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_add_flag(s_game_result_lbl, LV_OBJ_FLAG_HIDDEN);
    s_game_streak_lbl = label(s_game_page, cjk_font(), IC_DIM, "");
    lv_obj_align(s_game_streak_lbl, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_add_flag(s_game_streak_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *hint = label(s_game_page, cjk_font(), IC_DIM, "点一张手出拳，每局 -5 金币");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 300);
}

static void build_fortune(void)
{
    page_header(s_fortune_page, "今日运势", on_back);
    /* Dark unified card (design v22): CARD background, no beige paper,
     * no upstream left/right split. Card 300x150, centred, top at 105.
     * Grade in GOLD, large; body in white, centred, line spacing 6
     * (the CJK font's own size, see the file header on the 18px note). */
    lv_obj_t *card = lv_obj_create(s_fortune_page);
    lv_obj_set_size(card, IC_FORTUNE_CARD_W, IC_FORTUNE_CARD_H);
    lv_obj_set_pos(card, (360 - IC_FORTUNE_CARD_W) / 2, 105);
    lv_obj_set_style_bg_color(card, lv_color_hex(IC_CARD), 0);
    lv_obj_set_style_radius(card, IC_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    s_fortune_grade = label(card, cjk_font(), IC_GOLD, "—");
    lv_obj_align(s_fortune_grade, LV_ALIGN_TOP_MID, 0, 12);
    s_fortune_body = label(card, cjk_font(), IC_TEXT, "");
    lv_label_set_long_mode(s_fortune_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_fortune_body, IC_FORTUNE_CARD_W - 32);
    lv_obj_set_style_text_line_space(s_fortune_body, 6, 0);
    lv_obj_set_style_text_align(s_fortune_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_fortune_body, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_t *n1 = label(s_fortune_page, cjk_font(), IC_DIM, "免费 · 每天一次");
    lv_obj_align(n1, LV_ALIGN_TOP_MID, 0, 270);
    lv_obj_t *n2 = label(s_fortune_page, cjk_font(), IC_DIM, "同日再抽显示同一张");
    lv_obj_align(n2, LV_ALIGN_TOP_MID, 0, 294);
}

/* --- public API --- */

void muse_interact_ui_build(lv_obj_t *tile)
{
    s_tile = tile;
    s_main_page = page_group(tile);
    s_feed_page = page_group(tile);
    s_game_page = page_group(tile);
    s_fortune_page = page_group(tile);
    build_main();
    build_feed();
    build_game();
    build_fortune();
    show_page(IC_PAGE_MAIN);
    /* Main page is the only one shown at build; muse_ui.c's tileview
     * hides the whole tile when it is not the active tile, so no extra
     * hide is needed here. */
    if (muse_pet_snapshot(&s_snap)) {
        s_have_snap = true;
        refresh_status();
    }
}

void muse_interact_ui_on_hidden(void)
{
    clear_note();
    reset_game_view();
    show_page(IC_PAGE_MAIN);
}

void muse_interact_ui_touch_failed(void)
{
    s_touch_failed = true;
}

void muse_interact_ui_tick(bool active)
{
    (void)active;
    /* Order matters: drain feedback first (it updates the snapshot),
     * then refresh from that snapshot, then caption expiry, then the
     * game animation. Draining on every frame -- even when the tile is
     * hidden -- is what keeps the worker from stalling on a full
     * feedback queue (phase 1) and what lets a decay notice announce
     * itself the next time the user is IDLE, not only while the tile
     * is open. */
    drain_feedback();

    muse_pet_model_t snap;
    if (muse_pet_snapshot(&snap)) {
        s_snap = snap;
        s_have_snap = true;
        refresh_status();
        if (s_game_coins_lbl) {
            char buf[32];
            snprintf(buf, sizeof(buf), "金币 %lu/100", (unsigned long)s_snap.coins);
            lv_label_set_text(s_game_coins_lbl, buf);
        }
    } else if (muse_pet_storage_error()) {
        /* Worker stopped on a corrupt blob: show it once, and only once
         * it can actually be shown (IDLE) -- marking it "noted" while a
         * dialog runs would swallow the only announcement. */
        if (!s_storage_noted && muse_state_mode(NULL) == MUSE_MODE_IDLE) {
            s_storage_noted = true;
            write_pet_caption("互动数据损坏");
            show_note("互动数据损坏");
        }
    } else if (!muse_pet_ready()) {
        if (!s_unready_noted && muse_state_mode(NULL) == MUSE_MODE_IDLE) {
            s_unready_noted = true;
            write_pet_caption("互动未就绪");
            show_note("互动未就绪");
        }
    }

    /* Decay notice: announce once, then ACK. Only while IDLE (same gate
     * as any pet caption) and only once per decay_day: a notice that
     * arrives while a dialog runs waits for the next IDLE frame instead
     * of being ACKed unshown. */
    if (s_have_snap && s_snap.decay_notice > 0 &&
        s_snap.decay_day != s_decay_announced_day &&
        muse_state_mode(NULL) == MUSE_MODE_IDLE) {
        char buf[64];
        snprintf(buf, sizeof(buf), "毛毛这几天花了 %lu 金币",
                 (unsigned long)s_snap.decay_notice);
        write_pet_caption(buf);
        show_note(buf);
        s_decay_announced_day = s_snap.decay_day;
        /* ACK after showing, not before: if the ACK raced the caption,
         * the next snapshot would have notice 0 and the amount would
         * be lost from the caption path. The worker clears decay_notice
         * on ACK; the next snapshot then has 0 and this block stops
         * firing (guarded also by decay_day). */
        muse_pet_request(PET_DECAY_ACK);
    }

    if (s_touch_failed) {
        s_touch_failed = false;
        write_pet_caption("互动未就绪");
    }

    caption_tick();
    game_tick();
}
