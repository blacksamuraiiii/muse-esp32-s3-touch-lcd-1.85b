/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_interact_ui.c -- 1.5a interaction tile + feed / game / fortune /
 * sudoku / minigame / snake / whack-a-mole subpages. See
 * muse_interact_ui.h for the threading contract.
 *
 * Layout (design §5.2, mockups v11/v17/v18/v22; all coordinates are
 * 360x360-circle coordinates, NOT the 466x466 TamaMe coordinates):
 *   - Title "互动" and the back "<" share centre line y = 52. The back
 *     button is 56x48 with its top at y = 28, so its centre is x = 68
 *     (NOT x = 28: at y = 52 the circle chord starts near x = 54 and a
 *     button centred at 28 would be clipped by the round edge).
 *   - Main page (2x2 grid, 1.5.3a A flow, user decision 2026-11-11):
 *     four cards 140x95, radius 18, at (35,80) / (185,80) /
 *     (35,185) / (185,185): 今日运势, 喂饭, 学习, 小游戏. Each card
 *     carries a 56 px icon + title, no subline (user decision:
 *     未抽 / 免费 / 每种 +10金币 / 每种 -10金币 all gone; the fee
 *     lives on each game page). The far corners sit 176.1 px from
 *     the centre, inside the circle (mockup v3). The 学习 card opens
 *     the 学习 page, the 小游戏 card the mini-game list. Coin line
 *     at y = 302. The main page has NO back arrow (its header back
 *     callback is NULL); the face is one swipe away.
 *   - 学习 page (1.5.3a A flow): title "学习" with a back arrow,
 *     and a VERTICALLY SCROLLING column like the mini-game list's
 *     (same 58px rows, 10px gaps, 110px bottom pad): three rows,
 *     数学练习 / 英语单词 / 英语口语, all visible at once. All
 *     three are free (user decision 2026-11-12, 学习全部免费): no
 *     entry fee, no dimming. Back lands on the main page; the three
 *     games' own back arrows land here, not on the mini-game list.
 *   - Mini-game list page: title "小游戏" with a back arrow, and a
 *     VERTICALLY SCROLLING column like the settings page's list
 *     (2026-11-01, user request): six 330x58 cards in a flex column,
 *     10px gaps, with the coin line as the column's last item. The
 *     rows keep the main list's 58px height -- the earlier 48px
 *     "compressed" pitch is gone -- and the column scrolls instead,
 *     with a 110px bottom pad so the last row can clear the bottom
 *     curve of the bezel. All six games cost 10 coins per start (user
 *     decision 2026-10-31, retuned 2026-11-08); the rows only dim when
 *     poor, they are never disabled.
 *   - Subpages are groups inside the interaction tile, all with the same
 *     title/back pattern (title centred y = 52, "<" at centre x = 68).
 *     Since 2026-11-01 every subpage also ends with the same coin line
 *     at y = 302 (user request "统一页面内容"), the sole exception being
 *     石头剪刀布 below.
 *     The 喂饭 subpage keeps its rule line ("每种食物每天 +10") at the
 *     TOP slot (y = 84), above the six-food single-row sliding strip
 *     (y = 120), and its result line ("喂了…") at y = 262 -- the slot
 *     the rule line used to own before the two swapped on 2026-11-01
 *     (user request); the food names sit at y = 228 between them.
 *     The 今日运势 subpage: rule line "每天+20，同日再抽无效" in the same
 *     TOP slot as the 喂饭 one (y = 84), then a 300x160 card at y = 114
 *     with its content centred (grade 36, 28x2 gold rule 66, body 84),
 *     coin line at y = 302.
 *     The 石头剪刀布 game page is the one exception to the coin-slot
 *     rule: its coin line sits at y = 302 and the "点一张手出拳，每局
 *     -10 金币" cost hint at y = 84 (swapped, 2026-11-01, user request),
 *     because the result / streak lines already own y = 240 / 272.
 *     Tile swipe is DISABLED on every interaction subpage (feed,
 *     fortune, 学习, minigame list and the game boards): a subpage
 *     leaves only through its "<" back arrow, one level up, and the
 *     interaction main page is the only one that swipes (right to the
 *     face). The lock started with the four GAME pages on 2026-10-31
 *     (their buttons sit near the rim of the 360px circle and the
 *     default 10px scroll limit turned a slightly drifting press into
 *     a tile scroll, which swallowed the click and dropped the player
 *     out of the round) and was extended to the feed, fortune and
 *     minigame-list pages on 2026-11-04.
 * muse_interact_ui_swipe_locked() reports that state for muse_ui.c (backstop,
 * and the rule chrome already uses for settings subpages); since 1.5.1d
 * show_page() also toggles it directly, so leaving a subpage re-enables the
 * tile swipe on the spot instead of at the next 0.25 s tick.
 * muse_interact_ui_on_hidden() still resets the page when the tile is left
 * (BOOT included).
 *
 * Colours: the shared table in muse_theme.h (1.5b), values taken from 1.4d
 *   muse_settings_ui.c / muse_ui.c, NOT invented. DANGER is the in-repo
 *   value 0xff5c5c (the phase-2 brief listed 0xff6b6b) so the game
 *   "you lost" red matches the settings pages; to change it, change
 *   MUSE_THEME_DANGER in muse_theme.h, nowhere else.
 * Fonts: title/body use the shared chain in muse_font.h (1.5b):
 *   muse_font_cjk() for interface copy, muse_font_btn() for buttons.
 *   The mockups' "18号" body is a design size, not a font file in the
 *   repo: the code renders the fortune body at the CJK font with 26px
 *   line spacing and the per-card on-screen check (design §8) is the
 *   arbiter of fit. Do NOT invent an 18px font symbol to make the code
 *   look exact.
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
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "muse_interact_food_art.h"
#include "muse_interact_hand_assets.h"
#include "muse_audio.h"   /* game sound effects */
#include "muse_font.h"    /* muse_font_cjk() / muse_font_btn(), the one CJK chain (1.5b) */
#include "muse_theme.h"   /* MUSE_THEME_*, the one colour table (1.5b) */
#include "muse_pet.h"
#include "muse_pet_text.h"
#include "muse_state.h"
#include "muse_sudoku_ui.h"
#include "muse_snake_ui.h"
#include "muse_ui.h"
#include "muse_whackamole_ui.h"
#include "muse_goldrain_ui.h"
#include "muse_doodle_ui.h"
#include "muse_mathpractice_ui.h"
#include "muse_tictactoe_ui.h"
#include "muse_widgets.h" /* the shared back arrow / card / title (1.5g) */
#include "muse_wordcards_ui.h"
#include "muse_quiz_ui.h"
#include "muse_main_grid_art.h"
#include "muse_learn_art.h"     /* 学习子页 three-card icons (mockup v6) */

/* --- colours: muse_theme.h (see file header for the DANGER note) --- */

/* --- layout --- */
#define IC_LIST_W 330
#define IC_LIST_TOP 84
#define IC_ROW_H 58
/* Main-page 2x2 grid card (1.5.3a A flow): 140x95, icon 56 px at the
 * top, title under it. The far corners sit 176.1 px from the centre,
 * inside the circle (mockup v3). */
#define IC_GRID_CARD_W 140
#define IC_GRID_CARD_H 95
#define IC_GRID_ICON 56
#define IC_GRID_X0 35
#define IC_GRID_X1 185
#define IC_GRID_Y0 80
#define IC_GRID_Y1 185
/* Mini-game / 学习 scrolling columns (2026-11-01, user request): the
 * settings page's pattern instead of a compressed fixed pitch. 58px
 * rows, 10px gaps, 110px bottom pad. */
#define IC_MG_ROW_SPACE 10
#define IC_MG_LIST_PAD_BOTTOM 110
#define IC_RADIUS 18
#define IC_FEED_CARD 100
#define IC_FEED_ICON 80
#define IC_GAME_CHOICE 100
#define IC_HAND_RESULT 130
#define IC_FORTUNE_CARD_W 300
#define IC_FORTUNE_CARD_H 160
#define IC_FORTUNE_CARD_Y 114
/* The gold rule under the grade (v2, user request 2026-11-01): it is
 * the grade's own underline, exactly as wide as the grade draws (2-char
 * grade at 2x = 64 px, 1-char grade = 32 px), NOT a fixed 200 px divider.
 * Its width is set by fortune_layout() from the measured grade width, so
 * it can never drift from the text above it. It is 4 px and not 2 because
 * it scales with the grade (2x). */
#define IC_FORTUNE_RULE_H 4
/* Optical lift of the card's content group (user report 2026-11-01,
 * measured on a photo of the real page): dead centring left 38 px above
 * the grade's ink against 26 px under the body, which reads as "the text
 * sits low". The whole group moves up by this many px; 12 was the first
 * step and the user asked for 4 px more the same day, so a 2-line body
 * now ends up 10 px from the card's top edge and 42 px from its bottom. */
#define IC_FORTUNE_GROUP_BIAS 16
/* Grade size: the firmware carries exactly one CJK face (16 px, see
 * muse_font.h). 512 = 2.0x, i.e. the 大吉 line draws 32 px tall. The
 * glyphs are the 16 px face upscaled, so the edges are a touch soft --
 * the price of NOT adding a second font (user decision 2026-11-01). */
#define IC_FORTUNE_GRADE_SCALE 512

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
    IC_PAGE_SUDOKU,
    IC_PAGE_MINIGAME,
    IC_PAGE_SNAKE,
    IC_PAGE_WHACKAMOLE,
    IC_PAGE_GOLDRAIN,
    IC_PAGE_DOODLE,
    IC_PAGE_MATHPRACTICE,
    IC_PAGE_TICTACTOE,
    IC_PAGE_WORDCARDS,
    IC_PAGE_WORDCARDS_SETS,
    IC_PAGE_QUIZ,
    IC_PAGE_LEARN,
};

enum {
    IC_GAME_CHOICE_VIEW = 0, /* choice row visible, no result yet */
    IC_GAME_SHAKE,           /* 1s: chant + jitter, buttons dimmed */
    IC_GAME_REVEAL,          /* 0.8s: both hands shown, then hold */
    IC_GAME_RESULT,          /* hold: click a choice = next round */
};

static lv_obj_t *s_tile;
static lv_obj_t *s_main_page, *s_feed_page, *s_game_page, *s_fortune_page, *s_sudoku_page;
static lv_obj_t *s_minigame_page, *s_snake_page, *s_whackamole_page,
    *s_goldrain_page, *s_doodle_page, *s_mathpractice_page, *s_tictactoe_page,
    *s_wordcards_page, *s_wordcards_sets_page, *s_quiz_page, *s_learn_page;
static lv_obj_t *s_minigame_coins;
static lv_obj_t *s_mg_rps_lbl, *s_mg_snake_lbl, *s_mg_whack_lbl,
    *s_mg_goldrain_lbl, *s_mg_doodle_lbl,
    *s_mg_tictactoe_lbl;
/* s_mg_sudoku_lbl was here: removed 2026-11-08 with the archived 数独
 * row (see build_minigame). Re-add it when restoring 数独. */
static int s_last_feed_idx;
static uint32_t s_last_feed_before;
/* 1.5b: one feed at a time. A second tap while the first request is still
 * outstanding used to overwrite s_last_feed_idx / s_last_feed_before, so
 * the second feedback worded "+10" for a feed that paid nothing (the mask
 * bit was already set). While pending, taps are ignored; the 5 s timeout
 * in the tick is the backstop if a feedback is ever dropped. */
static bool s_feed_pending;
static uint32_t s_feed_pending_ms;
/* Backstop counter: a feed whose feedback never showed up (the 5 s timeout
 * above fired) leaves this many stale PET_FEED_* receipts in flight. The
 * drain swallows them so a late receipt is never worded with the next
 * feed's food name. */
static int s_feed_skip;
static lv_obj_t *s_status1;
static lv_obj_t *s_feed_note;   /* result line on the feed subpage itself */
static lv_obj_t *s_game_choices[3];
static int s_game_choice_base_x[3];
static lv_obj_t *s_game_you_img, *s_game_house_img;
static lv_obj_t *s_game_result_lbl, *s_game_streak_lbl, *s_game_chant_lbl;
static lv_obj_t *s_game_hint_lbl;
static lv_obj_t *s_game_coins_lbl;
static bool s_storage_noted, s_unready_noted;
static lv_obj_t *s_fortune_card, *s_fortune_grade, *s_fortune_rule,
    *s_fortune_body;
/* Bottom gold row (y = 302) on the feed and fortune subpages, added
 * 2026-11-01 so every interaction page ends with the same coin line.
 * Written by refresh_status() from the snapshot, same as s_status1 and
 * s_minigame_coins above. The learn page joined them in the v6 card
 * redesign (2026-11-12): its bottom row is the coin line too. */
static lv_obj_t *s_feed_coins, *s_fortune_coins, *s_learn_coins;

static const char *TAG = "muse_ic";

#if CONFIG_MUSE_BUILD_TEST
/* Set by the console task, consumed by the LVGL tick (see
 * muse_interact_ui_test_game at the bottom of this file). */
static volatile int s_test_game_pending = -1;
#endif

static int s_page = IC_PAGE_MAIN;
static int s_game_phase = IC_GAME_CHOICE_VIEW;
static uint32_t s_game_phase_ms;
static int s_game_player = -1, s_game_house = -1, s_game_result; /* -1 draw */
static int s_wins, s_losses, s_draws, s_streak; /* session only, NOT NVS */
static bool s_touch_failed;
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

/* Fonts come from the one shared chain (muse_font.h, 1.5b): interface copy
 * uses muse_font_cjk(), buttons muse_font_btn(). The old local helpers
 * (cjk_fallback_font / cjk_font / btn_font) are gone; the chain lives in
 * muse_font.c so a fix can no longer land in one file and miss the other. */

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

/* Title, and the back arrow when back_cb != NULL. Since mockup v15
 * (2026-10-31) the main interaction page carries the arrow too (back
 * to the face via on_back): the approved mockup shows it, and the
 * earlier no-arrow reasoning (a card behind the arrow read as a stray
 * box) no longer applies now that the arrow is a bare glyph, drawn
 * the settings way: no card, just the glyph over the title's left
 * shoulder. Both come from muse_widgets now -- the title takes the
 * settings page's place and look: TOP_MID at y = 44, accent colour --
 * so "互动" sits exactly where "SETTINGS" does one tile over. */
static void page_header(lv_obj_t *page, const char *title, lv_event_cb_t back_cb)
{
    if (back_cb) {
        muse_back_button(page, back_cb);
    }
    muse_page_title(page, title, 44);
}

static void show_page(int page)
{
    s_page = page;
    lv_obj_add_flag(s_main_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_feed_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_fortune_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sudoku_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_minigame_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_snake_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_whackamole_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_goldrain_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_doodle_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_mathpractice_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_tictactoe_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wordcards_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wordcards_sets_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_quiz_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_learn_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *show = s_main_page;
    if (page == IC_PAGE_FEED) show = s_feed_page;
    if (page == IC_PAGE_GAME) show = s_game_page;
    if (page == IC_PAGE_FORTUNE) show = s_fortune_page;
    if (page == IC_PAGE_SUDOKU) show = s_sudoku_page;
    if (page == IC_PAGE_MINIGAME) show = s_minigame_page;
    if (page == IC_PAGE_SNAKE) show = s_snake_page;
    if (page == IC_PAGE_WHACKAMOLE) show = s_whackamole_page;
    if (page == IC_PAGE_GOLDRAIN) show = s_goldrain_page;
    if (page == IC_PAGE_DOODLE) show = s_doodle_page;
    if (page == IC_PAGE_MATHPRACTICE) show = s_mathpractice_page;
    if (page == IC_PAGE_TICTACTOE) show = s_tictactoe_page;
    if (page == IC_PAGE_WORDCARDS) show = s_wordcards_page;
    if (page == IC_PAGE_WORDCARDS_SETS) show = s_wordcards_sets_page;
    if (page == IC_PAGE_QUIZ) show = s_quiz_page;
    if (page == IC_PAGE_LEARN) show = s_learn_page;
    lv_obj_remove_flag(show, LV_OBJ_FLAG_HIDDEN);
    /* The swipe lock takes effect here, not at the next 0.25 s chrome tick
     * (muse_ui.c): chrome polling left up to 250 ms after leaving a subpage
     * with the tileview still unscrollable, and the first swipe was
     * swallowed (1.5.1d). muse_ui_set_swipe_enabled ignores a NULL tv on
     * boards without touch, so this is safe at build time too. */
    muse_ui_set_swipe_enabled(s_page == IC_PAGE_MAIN);
}

bool muse_interact_ui_swipe_locked(void)
{
    /* Any interaction subpage: 喂食, 今日运势, 学习, 小游戏 (the minigame
     * list) and the six game boards (石头剪刀布 / 贪吃蛇 / 打地鼠 /
     * 金币雨 / 毛毛跳跃 / OX棋) plus the three learning games
     * (数学练习 / 英语单词 / 英语口语, reached through the 学习 page)
     * and the word-cards category page. Only the
     * main page swipes, right to the face. A subpage leaves through its
     * "<" back arrow, one level up; BOOT is the exception, and it does not
     * go through here (the voice task switches to the face and starts a
     * turn). The game boards were the original lock (2026-10-31, rim
     * buttons); the feed, fortune and minigame-list pages joined it on
     * 2026-11-04. */
    return s_page != IC_PAGE_MAIN;
}

static void on_back(lv_event_t *e)
{
    (void)e;
    /* Back goes one level up: from a game (reached through the minigame
     * menu) to the menu, from the menu and the other subpages to the
     * main page, and from the main page to the face. The word-cards
     * card page is one level deeper: it lands on the category page,
     * which lands on the 学习 page. A subpage back that skipped a level
     * would break the "left < = one level up" rule. The six menu
     * games (game, snake, whack-a-mole, goldrain, doodle, tictactoe)
     * all land on the menu; the three learning games (mathpractice,
     * wordcards, quiz) land on the 学习 page; feed and fortune are
     * main-page cards, so they still land on the main page. Leaving a
     * game page pauses a running game through its hide hook (snake
     * keeps its board, whack-a-mole, goldrain, doodle, mathpractice,
     * tictactoe, wordcards and quiz resume from their pause overlays),
     * so none plays on off-screen; the other subpages keep their own
     * return-visit rules. */
    if (s_page == IC_PAGE_SNAKE) {
        muse_snake_ui_hide();
    }
    if (s_page == IC_PAGE_WHACKAMOLE) {
        muse_whackamole_ui_hide();
    }
    if (s_page == IC_PAGE_GOLDRAIN) {
        muse_goldrain_ui_hide();
    }
    if (s_page == IC_PAGE_DOODLE) {
        muse_doodle_ui_hide();
    }
    if (s_page == IC_PAGE_MATHPRACTICE) {
        muse_mathpractice_ui_hide();
    }
    if (s_page == IC_PAGE_TICTACTOE) {
        muse_tictactoe_ui_hide();
    }
    if (s_page == IC_PAGE_WORDCARDS) {
        muse_wordcards_ui_hide();
    }
    if (s_page == IC_PAGE_QUIZ) {
        muse_quiz_ui_hide();
    }
    if (s_page == IC_PAGE_MAIN) {
        muse_ui_show_face();
    } else if (s_page == IC_PAGE_WORDCARDS) {
        /* One level deeper than the other games: back to the
         * category page, not the minigame list. */
        show_page(IC_PAGE_WORDCARDS_SETS);
    } else if (s_page == IC_PAGE_GAME || s_page == IC_PAGE_SUDOKU ||
               s_page == IC_PAGE_SNAKE || s_page == IC_PAGE_WHACKAMOLE ||
               s_page == IC_PAGE_GOLDRAIN ||
               s_page == IC_PAGE_DOODLE ||
               s_page == IC_PAGE_TICTACTOE) {
        show_page(IC_PAGE_MINIGAME);
    } else if (s_page == IC_PAGE_MATHPRACTICE ||
               s_page == IC_PAGE_QUIZ ||
               s_page == IC_PAGE_WORDCARDS_SETS) {
        /* Reached through the 学习 page, so back lands there. */
        show_page(IC_PAGE_LEARN);
    } else {
        show_page(IC_PAGE_MAIN);
    }
}

/* The game subpage writes a refusal ("金币不够…") into its own result slot:
 * the interaction main page carries the coin line only (user decision
 * 2026-10-25), so nothing prints a note below it any more. */
static void game_hint(const char *text)
{
    if (!s_game_result_lbl) {
        return;
    }
    lv_obj_set_style_text_color(s_game_result_lbl, lv_color_hex(MUSE_THEME_DIM), 0);
    lv_label_set_text(s_game_result_lbl, text);
    lv_obj_remove_flag(s_game_result_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* The feed subpage keeps its own result line: a feed no longer closes the
 * page, so the answer ("喂了米饭 +10 金币") has to appear where the click
 * happened, not only in the subtitle on the face and the main page. */
static void feed_note(const char *text)
{
    if (!s_feed_note) {
        return;
    }
    lv_label_set_text(s_feed_note, text);
}

/* lv_label_set_text has no same-text short circuit in LVGL 9.5: every call
 * frees, reallocs and re-lays-out the label. refresh_status() runs every
 * frame, so skip when nothing changed (same trick as muse_menu's set_text). */
static void set_label(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static void refresh_status(void)
{
    /* Gold rows on the main page and the mini-game menu (same text,
     * both live statuses). The main page's grid cards carry icon +
     * title only (1.5.3a A flow): there is no per-row copy any more,
     * so the feed-done / fortune-grade copy that used to live on the
     * rows is gone with them -- the feed subpage keeps its own
     * result line, the fortune page its grade. */
    if (!s_have_snap) {
        set_label(s_status1, "金币 --/100");
        set_label(s_minigame_coins, "金币 --/100");
        set_label(s_feed_coins, "金币 --/100");
        set_label(s_fortune_coins, "金币 --/100");
        set_label(s_learn_coins, "金币 --/100");
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "金币 %lu/100", (unsigned long)s_snap.coins);
    set_label(s_status1, buf);
    set_label(s_minigame_coins, buf);
    set_label(s_feed_coins, buf);
    set_label(s_fortune_coins, buf);
    set_label(s_learn_coins, buf);

    /* Mini-game rows: snake, whack-a-mole, goldrain, doodle and
     * OX棋 cost 10 coins per start, so they dim together when
     * nothing can be started. (数独 used to cost 5 once per
     * calendar day -- a day's ticket already paid meant it stayed
     * bright even at 0 coins -- but it is ARCHIVED 2026-11-08, see
     * build_minigame; its dimming line and sudoku_paid lookup below
     * are commented out with it.) Dimming is not disabling: a tap
     * still opens the game, and the gate inside refuses with
     * "金币不够…" there. */
    bool poor = s_snap.coins < MUSE_PET_GAME_STAKE;
    /* ARCHIVED 2026-11-08 with 数独 (see build_minigame). */
    /* bool sudoku_paid = false;
     * muse_pet_sudoku_today(&s_snap, time(NULL), &sudoku_paid, NULL); */
    uint32_t mg_color = poor ? MUSE_THEME_DIM : MUSE_THEME_TEXT;
    lv_obj_set_style_text_color(s_mg_rps_lbl, lv_color_hex(mg_color), 0);
    /* ARCHIVED 2026-11-08 with 数独 (see build_minigame). */
    /* lv_obj_set_style_text_color(s_mg_sudoku_lbl,
     *                           lv_color_hex(poor && !sudoku_paid ? MUSE_THEME_DIM
     *                                                            : MUSE_THEME_TEXT), 0); */
    lv_obj_set_style_text_color(s_mg_snake_lbl, lv_color_hex(mg_color), 0);
    lv_obj_set_style_text_color(s_mg_whack_lbl, lv_color_hex(mg_color), 0);
    lv_obj_set_style_text_color(s_mg_goldrain_lbl, lv_color_hex(mg_color), 0);
    lv_obj_set_style_text_color(s_mg_doodle_lbl, lv_color_hex(mg_color), 0);
    lv_obj_set_style_text_color(s_mg_tictactoe_lbl, lv_color_hex(mg_color), 0);
}

/* One card of the main page's 2x2 grid (1.5.3a A flow): 140x95 card,
 * 56 px icon at the top, title under it, no subline. The icon is an
 * A8 master recoloured white (MUSE_THEME_TEXT), the word-cards art
 * idiom. The image is not clickable: the card takes the click. */
static void grid_card(lv_obj_t *parent, int x, int y,
                      const lv_image_dsc_t *art, const char *title,
                      lv_event_cb_t cb)
{
    lv_obj_t *b = muse_card(parent, IC_GRID_CARD_W, IC_GRID_CARD_H, IC_RADIUS);
    lv_obj_set_pos(b, x, y);
    lv_obj_t *img = lv_image_create(b);
    lv_image_set_src(img, art);
    lv_obj_set_size(img, IC_GRID_ICON, IC_GRID_ICON);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_image_recolor(img, lv_color_hex(MUSE_THEME_TEXT), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = label(b, muse_font_btn(), MUSE_THEME_TEXT, title);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
}

/* One row of the mini-game menu's scrolling column: same card look and the
 * same IC_ROW_H as the main list, but sized to the column and placed by
 * flex (no set_pos), exactly like the settings page's list rows. */
static lv_obj_t *list_row(lv_obj_t *parent, const char *text,
                          lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = muse_card(parent, lv_pct(100), IC_ROW_H, IC_RADIUS);
    lv_obj_t *l = label(b, muse_font_btn(), MUSE_THEME_TEXT, text);
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
        muse_state_set_caption("%s", "");
    }
    s_pet_caption[0] = '\0';
}

/* --- feed subpage: upstream 16x16 pixel art, drawn at 5x --- */

static void draw_food_cb(lv_event_t *e)
{
    /* Draw callback: LVGL draw pass only. No allocation, no worker calls,
     * no state writes. `user_data` is the food index in
     * MUSE_INTERACT_FOODS order (0 rice .. 5 milk tea). The bound must
     * follow MUSE_PET_FEED_KINDS: it was hard-wired to the old 3 foods,
     * so burger / pizza / milk tea drew nothing and their cards stayed
     * empty (reported on the bench 2026-11-07). */
    lv_obj_t *obj = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)MUSE_PET_FEED_KINDS) {
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

/* The horizontal food strip parks where its snap point ends up, and the
 * snap pass (see lv_obj_scroll.c, find_snap_point) moves the content with
 * lv_obj_scroll_by(), which does NOT clamp to the scrollable range: a fling
 * past the last food could leave milk tea on the left edge with empty
 * space after it. Re-issue the current position through scroll_to_x(),
 * which goes through lv_obj_scroll_by_bounded() and pulls it back to the
 * last reachable snap. The follow-up pass has diff == 0 and returns. */
static void on_feed_strip_scroll_end(lv_event_t *e)
{
    lv_obj_t *strip = lv_event_get_target(e);
    lv_obj_scroll_to_x(strip, lv_obj_get_scroll_x(strip), LV_ANIM_ON);
}

static void on_feed_food(lv_event_t *e)
{
    /* Feed is the one subpage that closes on choice (design §5.2): the
     * feedback is the subtitle + the coin balance, not a feed-page state.
     * The food index is the action value: the six PET_FEED_* food
     * actions settle identically, but the subtitle needs to know WHICH
     * fed, so it is remembered here and matched against the feedback's
     * action in drain_feedback(). */
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    muse_pet_action_t act = PET_FEED_RICE;
    if (idx == 1) act = PET_FEED_CAKE;
    if (idx == 2) act = PET_FEED_COFFEE;
    if (idx == 3) act = PET_FEED_BURGER;
    if (idx == 4) act = PET_FEED_PIZZA;
    if (idx == 5) act = PET_FEED_MILKTEA;
    if (s_feed_pending) {
        return; /* 1.5b: one feed at a time -- see s_feed_pending. */
    }
    s_last_feed_idx = idx;
    /* 1.5a: each food pays on its own, so remember whether THIS food was
     * already paid (mask bit), not the old single daily counter. */
    s_last_feed_before = (s_have_snap && (s_snap.feed_mask & (1u << idx))) ? 1u : 0u;
    if (!muse_pet_request(act)) {
        write_pet_caption("互动未就绪");
        feed_note("互动未就绪");
        return;
    }
    s_feed_pending = true;
    s_feed_pending_ms = lv_tick_get();
    /* Stay on the feed page (user decision 2026-10-25): the result lands
     * in the page's own line, so a second food is one tap away instead of
     * a re-entry. The wording still comes from the worker's feedback,
     * never from the press -- "喂饭中..." only marks the wait. */
    feed_note("喂饭中...");
    (void)e;
}

/* --- game subpage --- */

static const lv_image_dsc_t *hand_img(int choice)
{
    if (choice == IC_ROCK) return &muse_interact_hand_rock;
    if (choice == IC_PAPER) return &muse_interact_hand_paper;
    return &muse_interact_hand_scissors;
}

/* 1 beats 0, 2 beats 1, 0 beats 2 (paper wraps rock, scissors cut paper,
 * rock crushes scissors). Returns +1 player wins, -1 player loses,
 * 0 draw. This is UI-local: the random result is decided at click time,
 * in this file, and passed to the model as PET_GAME's arg; the model then
 * settles it as entry fee + outcome payout (win 20 / draw 10 / loss 0),
 * and the result itself never enters NVS. */
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
                                  lv_color_hex(dim ? MUSE_THEME_CARD_PRESSED : MUSE_THEME_CARD), 0);
    }
}

static void show_game_result(void)
{
    /* Reveal view: two hands side by side, ~130px each, in place of the
     * choice row (the choice row is hidden during the 0.8s reveal).
     * Winner gets an ACCENT border; loser is recoloured to DIM (same
     * asset, the image_recolor style -- NOT a second asset, NOT an opaque
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
    lv_obj_set_style_image_recolor(s_game_you_img, lv_color_hex(MUSE_THEME_TEXT), 0);
    lv_obj_set_style_image_recolor(s_game_house_img, lv_color_hex(MUSE_THEME_TEXT), 0);
    if (s_game_result > 0) {
        lv_obj_set_style_border_width(s_game_you_img, 3, 0);
        lv_obj_set_style_border_color(s_game_you_img, lv_color_hex(MUSE_THEME_ACCENT), 0);
        lv_obj_set_style_image_recolor(s_game_house_img, lv_color_hex(MUSE_THEME_DIM), 0);
        lv_obj_set_style_text_color(s_game_result_lbl, lv_color_hex(MUSE_THEME_OK), 0);
        lv_label_set_text(s_game_result_lbl, "你赢了！");
    } else if (s_game_result < 0) {
        lv_obj_set_style_border_width(s_game_house_img, 3, 0);
        lv_obj_set_style_border_color(s_game_house_img, lv_color_hex(MUSE_THEME_ACCENT), 0);
        lv_obj_set_style_image_recolor(s_game_you_img, lv_color_hex(MUSE_THEME_DIM), 0);
        /* No error shake on losing (design §5.5): the loser dims, it
         * does not jitter. */
        lv_obj_set_style_text_color(s_game_result_lbl, lv_color_hex(MUSE_THEME_DANGER), 0);
        lv_label_set_text(s_game_result_lbl, "你输了");
    } else {
        lv_obj_set_style_text_color(s_game_result_lbl, lv_color_hex(MUSE_THEME_DIM), 0);
        lv_label_set_text(s_game_result_lbl, "平局，再来一局");
    }
    /* The reveal is the result moment: win gets the fanfare, draw the
     * neutral tone, loss the low one. */
    if (s_game_result > 0) {
        muse_audio_sfx(MUSE_SFX_WIN);
    } else if (s_game_result < 0) {
        muse_audio_sfx(MUSE_SFX_BAD);
    } else {
        muse_audio_sfx(MUSE_SFX_DRAW);
    }
    lv_obj_remove_flag(s_game_result_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_game_streak_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_game_chant_lbl, LV_OBJ_FLAG_HIDDEN);
    /* The cost hint is an instruction for a round not yet played; while
     * the hands are revealed it is noise above the result, so it hides
     * for the reveal and returns with the choice row (unified 2026-11-01). */
    lv_obj_add_flag(s_game_hint_lbl, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_remove_flag(s_game_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    set_choice_dim(false);
}

/* One round, started either by a tap or by the bench command (which is how
 * this path is verified without a finger on the panel). */
static void game_start(int choice)
{
    ESP_LOGI(TAG, "game start: choice=%d phase=%d page=%d have_snap=%d coins=%d",
             choice, s_game_phase, s_page, (int)s_have_snap,
             s_have_snap ? (int)s_snap.coins : -1);
    if (s_game_phase == IC_GAME_SHAKE) {
        return; /* shake: buttons are dimmed and not re-clickable */
    }
    if (!s_have_snap || s_snap.coins < MUSE_PET_GAME_STAKE) {
        /* Poor, checked against the last snapshot before any settlement
         * (design §5.2: coins under the entry fee never settle, never
         * start a round). */
        write_pet_caption("金币不够，先摸摸 / 喂饭 / 抽运势");
        game_hint("金币不够，先摸摸 / 喂饭 / 抽运势");
        /* Refused at the door: the low sound. */
        muse_audio_sfx(MUSE_SFX_BAD);
        return;
    }
    /* The result is decided NOW, at click time (design §5.2), and PET_GAME
     * settles it as entry fee + outcome payout (win 20 / draw 10 / loss 0),
     * so the worker needs it before it can apply anything. The animation
     * that follows only reveals this already-decided outcome. If the
     * feedback later comes back PET_POOR (snapshot was stale and the
     * balance actually was under the entry fee), the round is cancelled
     * in drain_feedback() and nothing was settled. */
    int player = choice;
    int house = (int)(esp_random() % 3);
    int outcome = game_result(player, house);
    if (!muse_pet_request_game(outcome)) {
        write_pet_caption("互动未就绪");
        return;
    }
    s_game_player = player;
    s_game_house = house;
    s_game_result = outcome;
    /* The punch landed: a short "placed" tick, not a judgement -- the
     * result sounds at the reveal, a second later. */
    muse_audio_sfx(MUSE_SFX_PLACE);
    s_game_phase = IC_GAME_SHAKE;
    s_game_phase_ms = lv_tick_get();
    ESP_LOGI(TAG, "game armed: you=%d house=%d result=%d tick=%lu", s_game_player,
             s_game_house, s_game_result, (unsigned long)s_game_phase_ms);
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

static void on_game_choice(lv_event_t *e)
{
    game_start((int)(intptr_t)lv_event_get_user_data(e));
}

static void game_tick(void)
{
    if (s_page != IC_PAGE_GAME) {
        return;
    }
    uint32_t now = lv_tick_get();
    if (s_game_phase == IC_GAME_SHAKE) {
        uint32_t elapsed = now - s_game_phase_ms;
        static bool shake_logged = false;
        if (!shake_logged) {
            shake_logged = true;
            ESP_LOGI(TAG, "game tick in SHAKE: now=%lu phase_ms=%lu elapsed=%lu",
                     (unsigned long)now, (unsigned long)s_game_phase_ms, (unsigned long)elapsed);
        }
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
                snprintf(streak, sizeof(streak), "连胜 %d ｜ %d胜 %d平 %d负",
                         s_streak, s_wins, s_draws, s_losses);
            } else {
                snprintf(streak, sizeof(streak), "%d胜 %d平 %d负",
                         s_wins, s_draws, s_losses);
            }
            lv_label_set_text(s_game_streak_lbl, streak);
            show_game_result();
            ESP_LOGI(TAG, "game -> REVEAL (elapsed=%lu)", (unsigned long)elapsed);
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
             * round (and re-checks the entry-fee balance first). */
            lv_obj_add_flag(s_game_you_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_game_house_img, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 3; i++) {
                lv_obj_remove_flag(s_game_choices[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_remove_flag(s_game_hint_lbl, LV_OBJ_FLAG_HIDDEN);
            set_choice_dim(false);
            ESP_LOGI(TAG, "game -> RESULT");
            s_game_phase = IC_GAME_RESULT;
        }
    }
}

/* --- fortune subpage --- */
/* Fortune card layout (v2, user request 2026-11-01): grade + rule +
 * body are ONE group, centred horizontally and lifted optically in the
 * 300x160 card. The group height is measured, not fixed: the grade
 * draws 32 px (16 px face at 2x), then 8 px, the 4 px rule, 12 px, then
 * the body at its real wrapped height (font line height 20 + line
 * space 6 = 26 px per line). So a 2-line body groups to 108 px; a
 * 1-line body groups to 82 px. The grade label's BOX stays 16 px tall
 * and is drawn scaled about its centre, so its box top is group_top + 8
 * for a drawn top of group_top. The rule's width is the grade's drawn
 * width: label box width x the 2x scale. Callers must set both texts
 * first; this only measures and positions.
 *
 * Optical lift (user report 2026-11-01, from a photo of the real page):
 * dead centring left 38 px of air above the grade's ink and 26 px under
 * the body, which reads as the text sitting low. The group therefore
 * sits IC_FORTUNE_GROUP_BIAS px above the geometric centre -- with the
 * bias at 16 a 2-line body groups to top 10, i.e. 10 px above and 42 px
 * below. */
static void fortune_layout(void)
{
    if (!s_fortune_card || !s_fortune_grade || !s_fortune_rule ||
        !s_fortune_body) {
        return;
    }
    lv_obj_update_layout(s_fortune_card);
    lv_coord_t body_h = lv_obj_get_height(s_fortune_body);
    if (body_h <= 0) {
        body_h = 26;
    }
    lv_coord_t grade_w = lv_obj_get_width(s_fortune_grade) *
                         (IC_FORTUNE_GRADE_SCALE / 256);
    lv_coord_t group_h = 32 + 8 + IC_FORTUNE_RULE_H + 12 + body_h;
    lv_coord_t top = (IC_FORTUNE_CARD_H - group_h) / 2 - IC_FORTUNE_GROUP_BIAS;
    if (top < 0) {
        top = 0;
    }
    lv_obj_align(s_fortune_grade, LV_ALIGN_TOP_MID, 0, top + 8);
    lv_obj_set_size(s_fortune_rule, grade_w, IC_FORTUNE_RULE_H);
    lv_obj_align(s_fortune_rule, LV_ALIGN_TOP_MID, 0, top + 40);
    lv_obj_align(s_fortune_body, LV_ALIGN_TOP_MID, 0, top + 56);
}

static void show_fortune_card(void)
{
    if (!s_have_snap || s_snap.fortune_day < 0) {
        lv_label_set_text(s_fortune_grade, "-");
        lv_label_set_text(s_fortune_body, "抽取中...");
        fortune_layout();
        return;
    }
    const char *grade = muse_pet_fortune_grade(s_snap.fortune);
    const char *text = muse_pet_fortune_text(s_snap.fortune);
    lv_label_set_text(s_fortune_grade, grade ? grade : "吉");
    lv_label_set_text(s_fortune_body, text ? text : "");
    fortune_layout();
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
        return;
    }
    show_page(IC_PAGE_FORTUNE);
    show_fortune_card(); /* "抽取中..." until the feedback lands */
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
                /* Two lines (user decision 2026-10-25): the count is in
                 * TOUCHES, not coins -- 10 a day at 5 coins each. */
                snprintf(buf, sizeof(buf), "毛毛很开心 +5 金币\n今日摸摸 %lu/%u",
                         (unsigned long)(s_snap.daily_touch_coins / MUSE_PET_TOUCH_COINS),
                         (unsigned)MUSE_PET_TOUCH_DAILY_MAX / MUSE_PET_TOUCH_COINS);
                muse_state_make_happy();
            } else if (fb.result == PET_CAP) {
                snprintf(buf, sizeof(buf), "今日摸摸奖励已满 50 金币");
                muse_state_make_happy();
            } else if (fb.result == PET_STORAGE) {
                snprintf(buf, sizeof(buf), "互动数据损坏");
            } else {
                snprintf(buf, sizeof(buf), "互动未就绪");
            }
            /* A registered tap sounds whether or not coins moved: the
             * petting is the point, not the payout (the cap-reached tap
             * still sounds). Storage / not-ready taps never registered. */
            if (fb.result == PET_COIN || fb.result == PET_CAP) {
                muse_audio_sfx(MUSE_SFX_PET);
            }
            write_pet_caption(buf);
            break;
        case PET_FEED_RICE:
        case PET_FEED_CAKE:
        case PET_FEED_COFFEE:
        case PET_FEED_BURGER:
        case PET_FEED_PIZZA:
        case PET_FEED_MILKTEA: {
            /* 1.5b: the request is answered, whatever the result, so the
             * next tap may go through. */
            s_feed_pending = false;
            if (s_feed_skip > 0) {
                /* Stale receipt: the feed it belongs to was already given up
                 * by the backstop, and s_last_feed_idx/before now describe a
                 * different (later) tap. Dropping it keeps "+10 金币" from
                 * being announced for the wrong food. */
                s_feed_skip--;
                break;
            }
            /* PET_OK means "settled"; whether coins moved is read from the
             * snapshot's feed_mask for THIS food vs the bit at click time
             * (design §4.3) -- the result code alone does not say. */
            const char *food = MUSE_INTERACT_FOODS[s_last_feed_idx].name;
            bool paid = (s_snap.feed_mask & (1u << s_last_feed_idx)) != 0;
            if (fb.result == PET_OK) {
                if (paid && !s_last_feed_before) {
                    snprintf(buf, sizeof(buf), "喂了%s +10 金币", food);
                    muse_state_make_happy();
                    feed_note(buf);
                    /* First feed of this food today: two chewing peeps.
                     * Already-fed-today stays silent. */
                    muse_audio_sfx(MUSE_SFX_EAT);
                } else {
                    snprintf(buf, sizeof(buf), "喂了%s（今天已经喂过）", food);
                    feed_note(buf);
                }
                write_pet_caption(buf);
            } else if (fb.result == PET_STORAGE) {
                write_pet_caption("互动数据损坏");
                feed_note("互动数据损坏");
            } else {
                write_pet_caption("互动未就绪");
                feed_note("互动未就绪");
            }
            break;
        }
        case PET_GAME:
            if (fb.result == PET_POOR) {
                /* Snapshot was stale: the 10 coins were never deducted,
                 * so there is nothing to refund -- cancel the animation
                 * (which was only a reveal of a never-started game). */
                reset_game_view();
                write_pet_caption("金币不够，先摸摸 / 喂饭");
                game_hint("金币不够，先摸摸 / 喂饭");
            } else if (fb.result == PET_OK) {
                /* Deduction confirmed; the outcome was already decided
                 * at click time, so nothing more to do here. If the
                 * player has already left the game page, the coin
                 * balance refresh below still lands on the tile. */
            } else if (fb.result == PET_STORAGE) {
                reset_game_view();
                write_pet_caption("互动数据损坏");
            }
            break;
        case PET_FEE:
            if (fb.result == PET_POOR) {
                /* Snapshot was stale: the 10 coins were never deducted,
                 * so there is nothing to refund -- cancel the game
                 * (which was only a start of a never-settled game).
                 * Snake, whack-a-mole, goldrain, doodle and
                 * tictactoe share PET_FEE; all five expose a cancel
                 * hook, so route by the visible page so a refusal for
                 * one game never touches another's overlay. */
                if (s_page == IC_PAGE_WHACKAMOLE) {
                    muse_whackamole_ui_fee_refused();
                } else if (s_page == IC_PAGE_DOODLE) {
                    muse_doodle_ui_fee_refused();
                } else if (s_page == IC_PAGE_SNAKE) {
                    muse_snake_ui_fee_refused();
                } else if (s_page == IC_PAGE_GOLDRAIN) {
                    muse_goldrain_ui_fee_refused();
                } else if (s_page == IC_PAGE_TICTACTOE) {
                    muse_tictactoe_ui_fee_refused();
                }
                write_pet_caption("金币不够，先摸摸 / 喂饭 / 抽运势");
            } else if (fb.result == PET_STORAGE) {
                if (s_page == IC_PAGE_WHACKAMOLE) {
                    muse_whackamole_ui_fee_refused();
                } else if (s_page == IC_PAGE_DOODLE) {
                    muse_doodle_ui_fee_refused();
                } else if (s_page == IC_PAGE_SNAKE) {
                    muse_snake_ui_fee_refused();
                } else if (s_page == IC_PAGE_GOLDRAIN) {
                    muse_goldrain_ui_fee_refused();
                } else if (s_page == IC_PAGE_TICTACTOE) {
                    muse_tictactoe_ui_fee_refused();
                }
                write_pet_caption("互动数据损坏");
            }
            /* PET_OK: deduction confirmed; the game already started, so
             * nothing more to do here. */
            break;
        case PET_SUDOKU_ENTRY:
            /* The sudoku page words its own refusals synchronously, from the
             * snapshot it just read; this branch only covers the stale-snapshot
             * case (the UI's gate passed, the worker's did not). Nothing on the
             * sudoku page listens for it, so the face caption is the only place
             * left to explain why the board did not unlock. */
            if (fb.result == PET_POOR) {
                char cap[48];
                snprintf(cap, sizeof(cap), "金币不够（数独入场 %u 枚）",
                         (unsigned)MUSE_PET_SUDOKU_ENTRY_COINS);
                write_pet_caption(cap);
            } else if (fb.result == PET_STORAGE) {
                write_pet_caption("互动数据损坏");
            }
            break;
        case PET_SUDOKU_SOLVE:
        case PET_PLAY_REWARD:
            /* Mini-game payouts. Every game page prints its own result line
             * (the sudoku win card, the snake / whack OVER cards), so the face
             * caption is deliberately left alone -- it is not on screen while a
             * game is being played anyway. */
            break;
        case PET_FORTUNE:
            show_fortune_card();
            if (fb.result == PET_FORTUNE_NEW || fb.result == PET_FORTUNE_SAME) {
                /* Interface gap (see PHASE2-NOTES.md §4): the phase-1
                 * text table has grade + body but NO card name, so the
                 * design's "今日运势：<卡名> ｜ <等级>" cannot be written
                 * verbatim. The caption uses the grade; the subpage
                 * shows grade + full body. Do NOT invent card names. */
                const char *grade = muse_pet_fortune_grade(s_snap.fortune);
                if (fb.result == PET_FORTUNE_NEW) {
                    snprintf(buf, sizeof(buf), "今日运势：%s +20 金币",
                             grade ? grade : "吉");
                    /* Fresh draw only: re-viewing today's card stays silent. */
                    muse_audio_sfx(MUSE_SFX_FORTUNE);
                } else {
                    snprintf(buf, sizeof(buf), "今日已抽过：%s",
                             grade ? grade : "吉");
                }
                write_pet_caption(buf);
            } else if (fb.result == PET_STORAGE) {
                write_pet_caption("互动数据损坏");
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
    feed_note("");
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

static void on_open_whackamole(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_WHACKAMOLE);
    muse_whackamole_ui_show();
}

static void on_open_goldrain(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_GOLDRAIN);
    muse_goldrain_ui_show();
}

static void on_open_doodle(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_DOODLE);
    muse_doodle_ui_show();
}

static void on_open_mathpractice(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_MATHPRACTICE);
    muse_mathpractice_ui_show();
}

static void on_open_tictactoe(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_TICTACTOE);
    muse_tictactoe_ui_show();
}

static void on_open_wordcards(lv_event_t *e)
{
    (void)e;
    /* The 学习 page's "英语单词" row opens the category page,
     * not a set directly (user decision 2026-11-10, three levels). */
    show_page(IC_PAGE_WORDCARDS_SETS);
}

/* A category row: switch the card page to that set (fresh round,
 * START) and show it; ui_show() then prefetches the set's 4 MP3s. */
static void on_open_wordcards_set(int set)
{
    muse_wordcards_ui_open_set(set);
    show_page(IC_PAGE_WORDCARDS);
    muse_wordcards_ui_show();
}

/* The 学习 page's "英语口语" row opens the quiz page directly
 * (moved here from the minigame list in the 1.5.3a A flow -- only
 * the entry moved, the quiz module itself did not). */
static void on_open_quiz(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_QUIZ);
    muse_quiz_ui_show();
}

/* ARCHIVED 2026-11-08 with 数独 (see build_minigame). Uncomment
 * together with the 数独 row to restore it.
static void on_open_sudoku(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_SUDOKU);
    muse_sudoku_ui_show();
}
*/

static void on_open_minigame(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_MINIGAME);
}

static void on_open_learn(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_LEARN);
}

static void on_open_snake(lv_event_t *e)
{
    (void)e;
    show_page(IC_PAGE_SNAKE);
    muse_snake_ui_show();
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
    page_header(s_main_page, "互动", NULL);
    /* 2x2 grid (1.5.3a A flow, user decision 2026-11-11): this page
     * used to be a three-row list, and before that a card grid --
     * with four entries the grid fits again, all four visible at
     * once, no scroll. Order: 今日运势, 喂饭, 学习, 小游戏. The
     * handlers are the same subpage openers the list used. Cards
     * are 140x95 at (35,80) / (185,80) / (35,185) / (185,185); the
     * far corners sit 176.1 px from the centre, inside the circle
     * (mockup v3). */
    grid_card(s_main_page, IC_GRID_X0, IC_GRID_Y0,
              MUSE_MAIN_GRID_ART[MUSE_MAIN_GRID_FORTUNE], "今日运势",
              on_fortune_open);
    grid_card(s_main_page, IC_GRID_X1, IC_GRID_Y0,
              MUSE_MAIN_GRID_ART[MUSE_MAIN_GRID_FEED], "喂饭", on_open_feed);
    grid_card(s_main_page, IC_GRID_X0, IC_GRID_Y1,
              MUSE_MAIN_GRID_ART[MUSE_MAIN_GRID_LEARN], "学习", on_open_learn);
    grid_card(s_main_page, IC_GRID_X1, IC_GRID_Y1,
              MUSE_MAIN_GRID_ART[MUSE_MAIN_GRID_GAMES], "小游戏",
              on_open_minigame);
    /* Coin line, the page's last row. It sat at y = 280 first (6 px under the
     * 今日运势 row, which ends at 274 -- too tight), then pinned to the bottom
     * (-22) which read as too low. 302 splits the gap: 28 px below the row,
     * 42 px above the bezel, and the circle's chord there is ~264 px, well over
     * the width of "金币 100/100". */
    s_status1 = label(s_main_page, muse_font_cjk(), MUSE_THEME_TEXT, "金币 --/100");
    lv_obj_align(s_status1, LV_ALIGN_TOP_MID, 0, 302);
}

/* One card of the learn page's three-card row (mockup v6, 2026-11-12):
 * the feed page's item-column pattern -- a 100x128 column holding a
 * 100x100 card (icon 80 px, centred) and the name 8 px below it, so the
 * name sits at y = 228 when the column starts at y = 120. The icon is
 * an A8 master recoloured with the card's own colour
 * (MUSE_LEARN_ART_COLOR), the word-cards idiom -- not white like the
 * main grid. The image is not clickable: the card takes the click. */
static void learn_card(lv_obj_t *parent, int x, int slot, const char *name,
                       lv_event_cb_t cb)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, IC_FEED_CARD, 128);
    lv_obj_set_pos(item, x, 120);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(item, 8, 0);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *card = muse_card(item, IC_FEED_CARD, IC_FEED_CARD, IC_RADIUS);
    lv_obj_t *img = lv_image_create(card);
    lv_image_set_src(img, MUSE_LEARN_ART[slot]);
    lv_obj_set_size(img, IC_FEED_ICON, IC_FEED_ICON);
    lv_obj_center(img);
    lv_obj_set_style_image_recolor(img,
                                   lv_color_hex(MUSE_LEARN_ART_COLOR[slot]), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    label(item, muse_font_cjk(), MUSE_THEME_DIM, name);
}

static void build_learn(void)
{
    page_header(s_learn_page, "学习", on_back);
    /* Same layout as the feed page (user decision 2026-11-12, mockup
     * v6): the motto takes the feed rule line's slot (y = 84, DIM),
     * three cards sit in a row at y = 120, and the coin line ends the
     * page at y = 302 (TEXT, live via refresh_status, same as the feed
     * and fortune pages). The three rows the A flow built (58 px rows,
     * scrolling column, 110 px bottom pad) are gone with the list: three
     * cards fit, so nothing scrolls. The names are the renamed ones
     * (user decision 2026-11-11): 数学练习 (was 加减练习), 英语单词
     * (was 单词练习), 英语口语 (the B-flow quiz, moved here from the
     * minigame list -- same page, only the entry moved). All three are
     * free (user decision 2026-11-12, 学习全部免费). */
    lv_obj_t *motto = label(s_learn_page, muse_font_cjk(), MUSE_THEME_DIM,
                            "好好学习，天天向上");
    lv_obj_align(motto, LV_ALIGN_TOP_MID, 0, 84);
    learn_card(s_learn_page, 22, MUSE_LEARN_MATH, "数学练习",
               on_open_mathpractice);
    learn_card(s_learn_page, 130, MUSE_LEARN_WORDS, "英语单词",
               on_open_wordcards);
    learn_card(s_learn_page, 238, MUSE_LEARN_QUIZ, "英语口语", on_open_quiz);
    /* Coin line: the page's last row, same slot as the other pages'
     * (y = 302), so every interaction page ends the same way
     * (2026-11-01, user request "统一页面内容"). A live status, not a
     * static label: refresh_status() rewrites it from the snapshot. */
    s_learn_coins = label(s_learn_page, muse_font_cjk(), MUSE_THEME_TEXT, "金币 --/100");
    lv_obj_align(s_learn_coins, LV_ALIGN_TOP_MID, 0, 302);
}

static void build_minigame(void)
{
    page_header(s_minigame_page, "小游戏", on_back);
    /* Scrolling column, the settings page's pattern (muse_settings_ui.c
     * page()): flex column, vertical scroll only, scrollbar off, 10px
     * gaps, 110px bottom pad so the last row can be scrolled clear of the
     * bottom curve. Full 58px rows -- no compressed pitch (2026-11-01,
     * user request). The menu itself never charges: each game page gates
     * its own fee. */
    lv_obj_t *list = lv_obj_create(s_minigame_page);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, IC_LIST_W, 360 - IC_LIST_TOP);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, IC_LIST_TOP);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, IC_MG_ROW_SPACE, 0);
    lv_obj_set_style_pad_bottom(list, IC_MG_LIST_PAD_BOTTOM, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    /* List order (user 2026-11-08): 石头剪刀布, 贪吃蛇, 打地鼠,
     * 金币雨, 毛毛跳跃, OX棋 last. The three learning games moved
     * to the 学习 page in the 1.5.3a A flow (数学练习 / 英语单词 /
     * 英语口语, renamed the same day from 加减练习 / 单词练习).
     * 凑十法练习 was fused into
     * 加减练习 the same day (one game, two kinds of question), so it is
     * gone from the list and from the tree. 数独 is ARCHIVED (same date):
     * the 360px round screen is too small to read its 6x6 grid or
     * tap its cells comfortably. The game code stays in the tree
     * (muse_sudoku_ui.c, IC_PAGE_SUDOKU, muse_sudoku_ui_build);
     * to restore it, uncomment the row below, the on_open_sudoku
     * handler, the s_mg_sudoku_lbl dimming line in refresh_status()
     * (plus its sudoku_paid lookup), and re-add *s_mg_sudoku_lbl
     * to the label declaration near the top of this file. */
    list_row(list, "石头剪刀布", on_open_game, &s_mg_rps_lbl);
    /* ARCHIVED 2026-11-08: 数独 -- screen too small, grid unreadable
     * and hard to tap. See the restore note in the comment above. */
    /* list_row(list, "数独", on_open_sudoku, &s_mg_sudoku_lbl); */
    list_row(list, "贪吃蛇", on_open_snake, &s_mg_snake_lbl);
    list_row(list, "打地鼠", on_open_whackamole, &s_mg_whack_lbl);
    list_row(list, "金币雨", on_open_goldrain, &s_mg_goldrain_lbl);
    list_row(list, "毛毛跳跃", on_open_doodle, &s_mg_doodle_lbl);
    list_row(list, "OX棋", on_open_tictactoe, &s_mg_tictactoe_lbl);

    /* Coin line: the column's last item, so it scrolls with the list
     * instead of sitting in a fixed slot the scroller would run under.
     * refresh_status() keeps rewriting it like the main page's. */
    s_minigame_coins = label(list, muse_font_cjk(), MUSE_THEME_TEXT, "金币 --/100");
}

static void build_feed(void)
{
    page_header(s_feed_page, "喂饭", on_back);
    /* Rule line: TOP slot (y = 84) since 2026-11-01 (user request). It is
     * read BEFORE a card is tapped, so it belongs above the cards, not
     * under them. Copy, font (muse_font_cjk) and DIM colour are unchanged
     * -- only the y moved. 84 clears the title (centre y = 52, ends ~62)
     * and stops ~16 px above the card row (top y = 120). */
    lv_obj_t *hint = label(s_feed_page, muse_font_cjk(), MUSE_THEME_DIM,
                           "每种食物每天 +10");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 84);
    /* Scrollable strip, 2026-11-07 (user request): ONE row with all six
     * foods, swipe left/right to choose -- NOT two pages. A single
     * horizontal scroller (flex row, snap to a food, scrollbar off)
     * holding six food columns (100px card + name below, so the name
     * scrolls with its card). 22px pads on both ends so the first three
     * sit centred like the old 3-card row, and the last three sit
     * centred when scrolled to the end. Cards are clickable containers,
     * not lv_btn: the pixel drawing is a custom draw callback on the
     * container itself, and a button's default style would fight the
     * card look. The strip is 128px tall (100 card + 8 gap + ~20 name);
     * the result line below it keeps its y = 262 slot. */
    lv_obj_t *strip = lv_obj_create(s_feed_page);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, 360, 128);
    lv_obj_align(strip, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(strip, 22, 0);
    lv_obj_set_style_pad_right(strip, 22, 0);
    lv_obj_set_style_pad_column(strip, 8, 0);
    lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(strip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(strip, LV_SCROLL_SNAP_START);
    /* Hard edge while dragging: with the default elastic flag the strip
     * can be pulled past the last food and shows empty space. */
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLL_ELASTIC);
    /* ...and the snap pass itself does not clamp (see the callback). */
    lv_obj_add_event_cb(strip, on_feed_strip_scroll_end, LV_EVENT_SCROLL_END, NULL);
    for (int i = 0; i < (int)MUSE_PET_FEED_KINDS; i++) {
        lv_obj_t *item = lv_obj_create(strip);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, IC_FEED_CARD, 128);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(item, 8, 0);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *card = lv_obj_create(item);
        lv_obj_set_size(card, IC_FEED_CARD, IC_FEED_CARD);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(MUSE_THEME_CARD), 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(MUSE_THEME_CARD_PRESSED), LV_STATE_PRESSED);
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
        label(item, muse_font_cjk(), MUSE_THEME_DIM,
              MUSE_INTERACT_FOODS[i].name);
    }
    /* Result line ("喂了米饭 +10 金币"): takes over the slot the rule line
     * used to own (y = 262), directly under the food names (y = 228, ends
     * ~244) -- the two swapped places on 2026-11-01 (user request). It
     * stays the answer to a tap on the row right above it. Same chord rule
     * as the main page's status line: the 240 px cap at y = 262 is well
     * inside the 360 circle. */
    s_feed_note = label(s_feed_page, muse_font_cjk(), MUSE_THEME_TEXT, "");
    lv_obj_align(s_feed_note, LV_ALIGN_TOP_MID, 0, 262);
    lv_label_set_long_mode(s_feed_note, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_feed_note, 240);
    lv_obj_set_style_text_align(s_feed_note, LV_TEXT_ALIGN_CENTER, 0);
    /* Coin line: the page's last row, same slot as the main page's
     * (y = 302), so every interaction page ends the same way
     * (2026-11-01, user request "统一页面内容"). The result line ends at
     * 278, leaving a 24 px gap; the chord at y = 310 is 248 px wide, well
     * over "金币 100/100". */
    s_feed_coins = label(s_feed_page, muse_font_cjk(), MUSE_THEME_TEXT, "金币 --/100");
    lv_obj_align(s_feed_coins, LV_ALIGN_TOP_MID, 0, 302);
}

static void build_game(void)
{
    /* Rock-paper-scissors page: its own title now that "小游戏" is the
     * mini-game menu (2026-10-31), so the two pages never share a name. */
    page_header(s_game_page, "石头剪刀布", on_back);
    /* Coin line sits in the BOTTOM slot (y = 302) since 2026-11-01: it
     * traded places with the "点一张手出拳" instruction line, which now
     * takes this page's top slot (y = 84). Text, colour and refresh
     * logic are unchanged -- only the y offset moved, so the coin line
     * lines up with the other pages' coin slot. */
    lv_obj_t *coins = label(s_game_page, muse_font_cjk(), MUSE_THEME_TEXT, "金币 --/100");
    lv_obj_align(coins, LV_ALIGN_TOP_MID, 0, 302);
    /* The coin line is a live status, not a static label: refresh_status
     * rewrites the main / menu coin rows, so the game page keeps its own
     * pointer and the tick rewrites it from the snapshot too. */
    s_game_coins_lbl = coins;
    /* Choice row: three 100px image buttons, 8px gaps, centred
     * (total 316px), top at y = 120. Buttons contain ONLY the hand
     * image -- no text (user decision: image click, not text button). */
    int total = 3 * IC_GAME_CHOICE + 2 * 8;
    int x0 = (360 - total) / 2;
    /* Left-to-right order 石头 / 剪刀 / 布 (user decision 2026-10-25). Slot
     * order is NOT the hand index: user_data keeps the real hand so
     * game_result() and the assets stay keyed to rock/paper/scissors. */
    static const int hand_order[3] = {0, 2, 1};
    for (int i = 0; i < 3; i++) {
        const int hand = hand_order[i];
        lv_obj_t *b = lv_btn_create(s_game_page);
        lv_obj_set_size(b, IC_GAME_CHOICE, IC_GAME_CHOICE);
        lv_obj_set_pos(b, x0 + i * (IC_GAME_CHOICE + 8), 120);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_THEME_CARD), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_THEME_CARD_PRESSED), LV_STATE_PRESSED);
        lv_obj_set_style_radius(b, IC_RADIUS, 0);
        /* Unified flat button language (2026-11-01): RULE border, no
         * shadow -- the same family as the muse_card pages, instead of
         * the raw theme button styling this page used to carry. */
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(MUSE_THEME_RULE), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_t *img = lv_image_create(b);
        lv_image_set_src(img, hand_img(hand));
        /* The master asset is 128 px; the row is 100 px. Scale to ~80 px so
         * the hands sit inside the button instead of overrunning it. */
        lv_image_set_scale(img, (uint16_t)(256 * (IC_GAME_CHOICE - 20) / 128));
        lv_obj_center(img);
        /* The image covers the whole button and LVGL makes image objects
         * clickable -- the tap landed on the image, never reached the
         * button's CLICKED handler, so the round never started. Feed cards
         * clear the same flag; this page was missing it (user report,
         * 1.5a: "点击卡片没有效果"). */
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, on_game_choice, LV_EVENT_CLICKED, (void *)(intptr_t)hand);
        s_game_choices[i] = b;
        s_game_choice_base_x[i] = x0 + i * (IC_GAME_CHOICE + 8);
    }
    /* Reveal hands: same row slot, ~130px each, left = you, right = house.
     * They are created once and hidden until a reveal. */
    s_game_you_img = lv_image_create(s_game_page);
    lv_obj_set_pos(s_game_you_img, (360 - IC_HAND_RESULT * 2 - 16) / 2, 105);
    lv_obj_set_size(s_game_you_img, IC_HAND_RESULT, IC_HAND_RESULT);
    lv_obj_add_flag(s_game_you_img, LV_OBJ_FLAG_HIDDEN);
    s_game_house_img = lv_image_create(s_game_page);
    lv_obj_set_pos(s_game_house_img, (360 - IC_HAND_RESULT * 2 - 16) / 2 + IC_HAND_RESULT + 16, 105);
    lv_obj_set_size(s_game_house_img, IC_HAND_RESULT, IC_HAND_RESULT);
    lv_obj_add_flag(s_game_house_img, LV_OBJ_FLAG_HIDDEN);
    /* 128 px master -> 130 px reveal. The recolor set in show_game_result()
     * tints white pixels, so its opacity must be cover or the loser would
     * never dim. */
    lv_image_set_scale(s_game_you_img, (uint16_t)(256 * IC_HAND_RESULT / 128));
    lv_image_set_scale(s_game_house_img, (uint16_t)(256 * IC_HAND_RESULT / 128));
    lv_obj_set_style_image_recolor_opa(s_game_you_img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor_opa(s_game_house_img, LV_OPA_COVER, 0);
    /* Result area below the row slot. */
    s_game_chant_lbl = label(s_game_page, muse_font_cjk(), MUSE_THEME_TEXT, "石头！");
    lv_obj_align(s_game_chant_lbl, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_add_flag(s_game_chant_lbl, LV_OBJ_FLAG_HIDDEN);
    s_game_result_lbl = label(s_game_page, muse_font_cjk(), MUSE_THEME_TEXT, "");
    lv_obj_align(s_game_result_lbl, LV_ALIGN_TOP_MID, 0, 240);
    /* Also carries the "金币不够" refusal now, which is a full sentence: cap
     * the width to the circle's chord and centre it instead of letting a long
     * line run off both sides. */
    lv_obj_set_width(s_game_result_lbl, 280);
    lv_label_set_long_mode(s_game_result_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_game_result_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_game_result_lbl, LV_OBJ_FLAG_HIDDEN);
    s_game_streak_lbl = label(s_game_page, muse_font_cjk(), MUSE_THEME_DIM, "");
    lv_obj_align(s_game_streak_lbl, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_add_flag(s_game_streak_lbl, LV_OBJ_FLAG_HIDDEN);
    /* Cost hint: top slot (y = 84), swapped with the coin line on
     * 2026-11-01. Same copy and DIM colour as before; 84 is the main
     * page's first-row y, so it clears the title (centre y = 52) and
     * stops ~1 px above the reveal hands (top y = 105). */
    s_game_hint_lbl = label(s_game_page, muse_font_cjk(), MUSE_THEME_DIM, "点一张手出拳，每局 -10 金币");
    lv_obj_align(s_game_hint_lbl, LV_ALIGN_TOP_MID, 0, 84);
}

static void build_fortune(void)
{
    page_header(s_fortune_page, "今日运势", on_back);
    /* Dark unified card (design v22): CARD background, no beige paper,
     * no upstream left/right split. The card is 300x160 at y = 114.
     * v2 (user request 2026-11-01, mockup fortune-mockup-v2): grade +
     * rule + body are ONE group, centred vertically and horizontally in
     * the card -- not two halves. fortune_layout() measures the body
     * height after every text change and positions the group from it,
     * so a body that wraps to a different line count stays centred
     * instead of leaving dead space at the card's bottom. The rule is
     * the grade's own underline (width = grade drawn width), not a
     * section divider.
     * The grade label's BOX stays 16 px tall (the only CJK face in the
     * firmware); transform_scale 2x around its centre makes it DRAW 32 px
     * (2026-11-01, user request "与下方文字一起居中" + "不要引入新字体，
     * 单纯放大"). The upscaled 16 px glyphs are a little soft at the
     * edges -- the accepted trade for not shipping a second font; the
     * same mechanism scales the sudoku side strips. Body in white at
     * 16 px, line spacing 6. */
    s_fortune_card = lv_obj_create(s_fortune_page);
    lv_obj_set_size(s_fortune_card, IC_FORTUNE_CARD_W, IC_FORTUNE_CARD_H);
    lv_obj_set_pos(s_fortune_card, (360 - IC_FORTUNE_CARD_W) / 2,
                   IC_FORTUNE_CARD_Y);
    lv_obj_set_style_bg_opa(s_fortune_card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_fortune_card, lv_color_hex(MUSE_THEME_CARD), 0);
    lv_obj_set_style_radius(s_fortune_card, IC_RADIUS, 0);
    lv_obj_set_style_border_width(s_fortune_card, 0, 0);
    lv_obj_remove_flag(s_fortune_card, LV_OBJ_FLAG_SCROLLABLE);
    s_fortune_grade = label(s_fortune_card, muse_font_cjk(), MUSE_THEME_GOLD, "-");
    lv_obj_align(s_fortune_grade, LV_ALIGN_TOP_MID, 0, 28);
    /* Scale about the label's own centre, so growing it does not push the
     * text off the card's centreline. */
    lv_obj_set_style_transform_pivot_x(s_fortune_grade, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_fortune_grade, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale_x(s_fortune_grade, IC_FORTUNE_GRADE_SCALE, 0);
    lv_obj_set_style_transform_scale_y(s_fortune_grade, IC_FORTUNE_GRADE_SCALE, 0);
    /* Short gold rule under the grade: purely decorative, so it is a plain
     * container strip at 47% gold (bg_opa 120), not a line object. Its
     * width is NOT fixed here: fortune_layout() sets it to the grade's
     * drawn width every time the texts change (v2 underline rule). The
     * initial aligns below are placeholders; fortune_layout() at the end
     * of this function positions grade, rule and body as one group. */
    s_fortune_rule = lv_obj_create(s_fortune_card);
    lv_obj_set_size(s_fortune_rule, 64, IC_FORTUNE_RULE_H);
    lv_obj_align(s_fortune_rule, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_bg_opa(s_fortune_rule, 120, 0);  /* 47% gold */
    lv_obj_set_style_bg_color(s_fortune_rule, lv_color_hex(MUSE_THEME_GOLD), 0);
    lv_obj_set_style_radius(s_fortune_rule, IC_FORTUNE_RULE_H / 2, 0);  /* pill ends */
    lv_obj_set_style_border_width(s_fortune_rule, 0, 0);
    lv_obj_remove_flag(s_fortune_rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_fortune_rule, LV_OBJ_FLAG_CLICKABLE);
    s_fortune_body = label(s_fortune_card, muse_font_cjk(), MUSE_THEME_TEXT, "");
    lv_label_set_long_mode(s_fortune_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_fortune_body, IC_FORTUNE_CARD_W - 32);
    lv_obj_set_style_text_line_space(s_fortune_body, 6, 0);
    lv_obj_set_style_text_align(s_fortune_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_fortune_body, LV_ALIGN_TOP_MID, 0, 97);
    /* Rule line: TOP slot (y = 84), the same slot the 喂饭 subpage's
     * rule line uses (user request 2026-11-01: "同喂饭那句的位置").
     * It is read before the card, and it keeps the bottom free
     * for the coin line alone. Card sits at 114 (300x160, ends 274) --
     * 14 px under this line and 28 px above the coin row. Copy is the
     * shortened one, DIM colour unchanged; ~148 px wide, far inside the
     * 316 px chord at y = 94. */
    lv_obj_t *rule = label(s_fortune_page, muse_font_cjk(), MUSE_THEME_DIM,
                           "每天+20，同日再抽无效");
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 84);
    /* Coin line: same slot as the main / feed / mini-game pages (y = 302),
     * so all interaction pages end the same way (2026-11-01, "统一页面
     * 内容"). Written by refresh_status() from the snapshot. */
    s_fortune_coins = label(s_fortune_page, muse_font_cjk(), MUSE_THEME_TEXT, "金币 --/100");
    lv_obj_align(s_fortune_coins, LV_ALIGN_TOP_MID, 0, 302);
    fortune_layout();
}

/* --- public API --- */

void muse_interact_ui_build(lv_obj_t *tile)
{
    s_tile = tile;
    s_main_page = page_group(tile);
    s_feed_page = page_group(tile);
    s_game_page = page_group(tile);
    s_fortune_page = page_group(tile);
    s_sudoku_page = page_group(tile);
    s_minigame_page = page_group(tile);
    s_snake_page = page_group(tile);
    s_whackamole_page = page_group(tile);
    s_goldrain_page = page_group(tile);
    s_doodle_page = page_group(tile);
    s_mathpractice_page = page_group(tile);
    s_tictactoe_page = page_group(tile);
    s_wordcards_page = page_group(tile);
    s_wordcards_sets_page = page_group(tile);
    s_quiz_page = page_group(tile);
    s_learn_page = page_group(tile);
    build_main();
    build_feed();
    build_game();
    build_fortune();
    build_minigame();
    build_learn();
    muse_sudoku_ui_build(s_sudoku_page, on_back);
    muse_snake_ui_build(s_snake_page, on_back);
    muse_whackamole_ui_build(s_whackamole_page, on_back);
    muse_goldrain_ui_build(s_goldrain_page, on_back);
    muse_doodle_ui_build(s_doodle_page, on_back);
    muse_mathpractice_ui_build(s_mathpractice_page, on_back);
    muse_tictactoe_ui_build(s_tictactoe_page, on_back);
    muse_wordcards_ui_build(s_wordcards_page, on_back);
    muse_wordcards_ui_build_sets(s_wordcards_sets_page, on_back,
                                 on_open_wordcards_set);
    muse_quiz_ui_build(s_quiz_page, on_back);
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
    /* No note line to clear any more: the coin line is a live status and the
     * subpages own their own hint slots (user decision 2026-10-25). */
    reset_game_view();
    /* Sudoku keeps its puzzle progress (static state in the sudoku UI);
     * only the pad popup and the win overlay are closed, so a return
     * visit resumes the puzzle instead of restarting it. The snake
     * page pauses the same way (a running game never plays on while
     * the tile is hidden); its board and paid state stay in RAM. */
    muse_sudoku_ui_hide();
    /* The four timed games pause the same way: their boards and paid
     * state stay in RAM, and a running game never plays on while the
     * tile is hidden. Whack-a-mole, goldrain and doodle resume from their
     * pause overlays on the next visit, snake from wherever its tick left
     * it. */
    muse_snake_ui_hide();
    muse_whackamole_ui_hide();
    muse_goldrain_ui_hide();
    muse_doodle_ui_hide();
    muse_mathpractice_ui_hide();
    muse_tictactoe_ui_hide();
    muse_wordcards_ui_hide();
    muse_quiz_ui_hide();
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
#if CONFIG_MUSE_BUILD_TEST
    /* Bench round requested from the console (see muse_interact_ui_test_game):
     * run it here, on the LVGL task, where the view may be touched. */
    if (s_test_game_pending >= 0) {
        int choice = s_test_game_pending;
        s_test_game_pending = -1;
        if (s_page != IC_PAGE_GAME) {
            show_page(IC_PAGE_GAME);
        }
        game_start(choice);
    }
#endif
    drain_feedback();

    muse_pet_model_t snap;
    if (muse_pet_snapshot(&snap)) {
        s_snap = snap;
        s_have_snap = true;
        refresh_status();
        if (s_game_coins_lbl) {
            char buf[32];
            snprintf(buf, sizeof(buf), "金币 %lu/100", (unsigned long)s_snap.coins);
            set_label(s_game_coins_lbl, buf);
        }
    } else if (muse_pet_storage_error()) {
        /* Worker stopped on a corrupt blob: show it once, and only once
         * it can actually be shown (IDLE) -- marking it "noted" while a
         * dialog runs would swallow the only announcement. */
        if (!s_storage_noted && muse_state_mode(NULL) == MUSE_MODE_IDLE) {
            s_storage_noted = true;
            write_pet_caption("互动数据损坏");
        }
    } else if (!muse_pet_ready()) {
        if (!s_unready_noted && muse_state_mode(NULL) == MUSE_MODE_IDLE) {
            s_unready_noted = true;
            write_pet_caption("互动未就绪");
        }
    }

    /* 1.5b backstop: if a feed feedback was ever dropped, the pending flag
     * must not wedge the feed buttons forever. */
    if (s_feed_pending && (int32_t)(lv_tick_get() - s_feed_pending_ms) > 5000) {
        s_feed_pending = false;
        /* The feedback for this feed can still arrive (a slow worker store or
         * a backed-up queue). s_last_feed_idx/before already describe the NEXT
         * feed, so count the stale one off instead of judging it with the
         * wrong food name. */
        s_feed_skip++;
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
        s_decay_announced_day = s_snap.decay_day;
        /* ACK after showing, not before: if the ACK raced the caption,
         * the next snapshot would have notice 0 and the amount would
         * be lost from the caption path. 1.5b: the ACK carries the day
         * it announced, and the worker only clears a notice for that
         * day -- an ACK that arrives after a midnight rollover created
         * a new notice must not silently clear the new one. */
        muse_pet_decay_ack(s_snap.decay_day);
    }

    if (s_touch_failed) {
        s_touch_failed = false;
        write_pet_caption("互动未就绪");
    }

    caption_tick();
    game_tick();
}

#if CONFIG_MUSE_BUILD_TEST
/* Bench only: run one round exactly as a tap on choice <i> would. The console
 * runs on another task, and LVGL is single-owner, so this only queues the
 * request; the LVGL tick below starts the round on the next frame. */
bool muse_interact_ui_test_game(int choice)
{
    if (choice < 0 || choice > 2) {
        return false;
    }
    s_test_game_pending = choice;
    return true;
}
#endif
