/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_wordcards_ui.c -- 英语单词卡片 subpage, see muse_wordcards_ui.h.
 *
 * Layout (mockup v4, user decision 2026-11-10): the set title in the
 * shared title slot (y=38), a 2x2 grid of 116x116 cards at (60,60)
 * with an 8 px gap, and nothing under the cards (the old HUD line is
 * gone with the clock, user 2026-11-11).
 * The category page (mockup v2, user decision 2026-11-10) is the
 * level above: title "单词练习", rows at the minigame list's
 * geometry (330 wide, top 84, 58 high, 10 gap, radius 18, CARD
 * fill) with the set icon (36 px) + 12 px gap + set title (Noto 16)
 * centred as one group, no count text, and the hint "限时免费" as the
 * last item of the scrolling column (user 2026-11-11: it used to sit
 * at y=232, which the third row took over once a fourth set shipped,
 * so it scrolls in below the list instead of being pinned).
 * A card's front is only its picture
 * (the A8 game-icons master, recoloured per word at runtime,
 * scaled to 72 px from the 128 px master). Its back is three centred
 * lines: the English word in gold Montserrat 20, the Chinese in
 * Noto 16, and the speaker logo (LV_SYMBOL_VOLUME_MAX, the face
 * speaker button's glyph) in white -- the v3 ~31x18 logo, not the
 * v2 48x30 one.
 *
 * The flip is a fake: 120 ms of horizontal squash (scale_x 256 -> 0
 * in 60 ms), swap the front/back at the midpoint, expand back
 * (0 -> 256 in 60 ms). The pivot is the card's own centre, so it
 * squeezes in place instead of sliding. While a card is flipping it
 * ignores taps, so a double tap cannot queue two swaps.
 *
 * There is no clock (user decision 2026-11-11, was a 2-minute round):
 * entering a set starts the round and the back arrow is the only way
 * out, so the page shows no countdown, no "已翻 n 张" line under the
 * cards and no "时间到 / 本次学了 X 张" summary -- leaving is the only
 * end. The 50 ms timer still runs, but only to drain the audio mark
 * and to keep the screen awake while a round is open.
 *
 * Pronunciation: the UI never fetches on a tap. The prefetch (started
 * on entering the page) fills the PSRAM cache in the background; the
 * 50 ms tick drains the logic's pending_audio mark -- when the word
 * is READY it plays at once, when it is still FETCHING the mark waits
 * for the next tick, when it FAILED the mark is dropped silently and
 * the card simply stays silent. Flipping a card back or leaving the
 * page drops a pending mark, so audio is never played for a card the
 * player is no longer looking at.
 */
#include "muse_wordcards_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"

#include "muse_audio.h" /* game sound effects */
#include "muse_font.h"
#include "muse_state.h" /* poke: no auto-sleep while a round runs */
#include "muse_theme.h"
#include "muse_widgets.h"
#include "muse_wordcards.h"
#include "muse_wordcards_art.h"
#include "muse_wordcards_audio.h"

/* A press that drifts further than this is a swipe, not a tap (user
 * 2026-11-10: no pause any more, so this only guards the flip). */
#define MUSE_WORDCARDS_HOLD_MOVE_PX 12
/* Timer period: the clock and the audio drain ride on it. */
#define MUSE_WORDCARDS_TICK_MS 50u
/* Flip: 60 ms squash + 60 ms expand (mockup: 120 ms total). */
#define MUSE_WORDCARDS_FLIP_HALF_MS 60u
#define MUSE_WORDCARDS_FLIP_SCALE 256
/* Card geometry (mockup v4): 116x116 cards, 8 px gap, origin (60,60). */
#define MUSE_WORDCARDS_CARD 116
#define MUSE_WORDCARDS_GAP 8
#define MUSE_WORDCARDS_ORIGIN_X 60
#define MUSE_WORDCARDS_ORIGIN_Y 60
/* Paging (user 2026-11-11, the same horizontal strip idiom as the feed
 * page): one 2x2 field is one page; an 8-word set has two pages, swiped
 * left/right with a hard stop at both ends and a snap per page. */
#define MUSE_WORDCARDS_PAGE_W 240
#define MUSE_WORDCARDS_PAGE_H 240
/* Front picture: the 128 px A8 master drawn at 72 px. */
#define MUSE_WORDCARDS_PIC_PX 72
#define MUSE_WORDCARDS_PIC_SCALE \
    (256 * MUSE_WORDCARDS_PIC_PX / MUSE_WORDCARDS_ART_PX)
/* Back lines, centred, as offsets from the card's top edge (mockup v4):
 * English (Montserrat 20) top at 22, Chinese (Noto 16) top at 52,
 * speaker glyph (~18 px tall) centred at 89, so its label top is 80.
 * A flipped card also takes the mockup's pressed fill (CARD_PRESSED)
 * and a 2 px GOLD border, so the flipped state is visible at a glance. */
#define MUSE_WORDCARDS_BACK_EN_Y 22
#define MUSE_WORDCARDS_BACK_ZH_Y 52
#define MUSE_WORDCARDS_BACK_SP_Y 80
#define MUSE_WORDCARDS_BACK_SP_CY 89
/* The speaker logo's tap target: a 48x30 box around its centre, so a
 * finger lands on it even though the glyph itself is only ~31x18. */
#define MUSE_WORDCARDS_SP_HALF_W 24
#define MUSE_WORDCARDS_SP_HALF_H 15

static muse_wordcards_t s_game;
/* Press tracking for the tap-to-flip and the hold-to-pause. */
static bool s_press_active = false;
static bool s_press_moved = false;
static lv_coord_t s_press_x = 0;
static lv_coord_t s_press_y = 0;
static int s_press_card = -1;
/* One card's flip animation in flight: taps on that card wait. */
static bool s_flip_anim[MUSE_WORDCARDS_WORDS];
static bool s_flip_target[MUSE_WORDCARDS_WORDS];
/* The side each card currently shows (-1 = unknown, 0 = front, 1 = back).
 * refresh_display() writes styles only when this changes: a style write
 * invalidates the object, and a transform_scale write pushes the card
 * through the layer path, 20 times a second for four cards. */
static int8_t s_shown[MUSE_WORDCARDS_WORDS];
/* 50 ms ticks of the current round, for the temporary liveness probe. */
static uint32_t s_probe_ticks;

static const char *TAG = "muse_wordcards";

static lv_obj_t *s_card[MUSE_WORDCARDS_WORDS];
static lv_obj_t *s_front_img[MUSE_WORDCARDS_WORDS];
static lv_obj_t *s_back[MUSE_WORDCARDS_WORDS];
/* Back-face lines, kept so open_set can rebuild them for a new set. */
static lv_obj_t *s_back_en[MUSE_WORDCARDS_WORDS];
static lv_obj_t *s_back_zh[MUSE_WORDCARDS_WORDS];
/* The paging strip, its page containers, and one tap layer per page (the
 * layer has to be a child of the page so a drag still scrolls the strip). */
static lv_obj_t *s_strip;
static lv_obj_t *s_pg[MUSE_WORDCARDS_PAGES];
static lv_obj_t *s_touch[MUSE_WORDCARDS_PAGES];
static lv_obj_t *s_back_btn;
static lv_obj_t *s_name;
static lv_obj_t *s_page;
static lv_timer_t *s_timer;
/* Category page (built once by muse_wordcards_ui_build_sets): the
 * owner callback a set row calls. */
static lv_obj_t *s_sets_page;
static muse_wordcards_open_set_cb_t s_open_set_cb;

/* Category-page geometry: the minigame list's own numbers
 * (build_minigame in muse_interact_ui.c) -- 330 wide, top 84, rows
 * 58 high with a 10 px gap, radius 18, CARD fill (muse_card). The
 * rows live in a scrolling column, like the minigame list: with four
 * sets the fixed positions put the last row under the round screen's
 * bottom curve, so the list scrolls (110 px bottom pad, scrollbar
 * off) instead. The hint "限时免费" (user 2026-11-11, replaced the old
 * "免费 · 每类限时 2 分钟") is the column's last item: with four rows
 * its old pinned slot at y=232 is under the third row, so it scrolls
 * into view below the list instead of being fixed. */
#define MUSE_WORDCARDS_SETS_W 330
#define MUSE_WORDCARDS_SETS_TOP 84
#define MUSE_WORDCARDS_SETS_ROW_H 58
#define MUSE_WORDCARDS_SETS_GAP 10
#define MUSE_WORDCARDS_SETS_RADIUS 18
#define MUSE_WORDCARDS_SETS_PAD_BOTTOM 110
/* Set icon on a category row: 36 px, 12 px before the title. */
#define MUSE_WORDCARDS_SETS_ICON_PX 36
#define MUSE_WORDCARDS_SETS_ICON_SCALE \
    (256 * MUSE_WORDCARDS_SETS_ICON_PX / MUSE_WORDCARDS_ART_PX)

/* --- small helpers, same shapes as the OX棋 page ------------------ */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font,
                       uint32_t color)
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

/* --- card geometry ------------------------------------------------ */

/* Where card `k` (0..3) sits inside its page: the mockup's 2x2 field. */
static void card_local(int k, int *x, int *y)
{
    *x = (k % 2) * (MUSE_WORDCARDS_CARD + MUSE_WORDCARDS_GAP);
    *y = (k / 2) * (MUSE_WORDCARDS_CARD + MUSE_WORDCARDS_GAP);
}

/* The card of page `pg` under a point given in that page's own
 * coordinates, or -1. `i` is the global card index (page * 4 + k). */
static int card_on_page(int pg, lv_coord_t x, lv_coord_t y)
{
    if (pg < 0 || pg >= MUSE_WORDCARDS_PAGES) {
        return -1;
    }
    for (int k = 0; k < MUSE_WORDCARDS_PER_PAGE; k++) {
        int cx, cy;
        card_local(k, &cx, &cy);
        if (x >= cx && x < cx + MUSE_WORDCARDS_CARD && y >= cy &&
            y < cy + MUSE_WORDCARDS_CARD) {
            return pg * MUSE_WORDCARDS_PER_PAGE + k;
        }
    }
    return -1;
}

/* True when the point (page-local) lands on card `k`'s speaker logo. */
static bool point_on_speaker(int k, lv_coord_t x, lv_coord_t y)
{
    int cx, cy;
    card_local(k, &cx, &cy);
    int scx = cx + MUSE_WORDCARDS_CARD / 2;
    int scy = cy + MUSE_WORDCARDS_BACK_SP_CY;
    return x >= scx - MUSE_WORDCARDS_SP_HALF_W &&
           x < scx + MUSE_WORDCARDS_SP_HALF_W &&
           y >= scy - MUSE_WORDCARDS_SP_HALF_H &&
           y < scy + MUSE_WORDCARDS_SP_HALF_H;
}

/* --- flip animation ----------------------------------------------- */

static void flip_scale_cb(void *var, int32_t v)
{
    lv_obj_set_style_transform_scale_x((lv_obj_t *)var, (int16_t)v,
                                       LV_PART_MAIN);
}

static void flip_start(int i, int32_t from, int32_t to,
                       lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_card[i]);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, MUSE_WORDCARDS_FLIP_HALF_MS);
    lv_anim_set_exec_cb(&a, flip_scale_cb);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void flip_done_cb(lv_anim_t *a)
{
    lv_obj_t *card = (lv_obj_t *)a->var;
    int i = (int)(intptr_t)lv_obj_get_user_data(card);
    s_flip_anim[i] = false;
    /* Undo the squash once, here, instead of on every 50 ms tick: a
     * transform_scale write forces the card through LVGL's layer path. */
    lv_obj_set_style_transform_scale_x(s_card[i], MUSE_WORDCARDS_FLIP_SCALE,
                                      LV_PART_MAIN);
    s_shown[i] = s_flip_target[i] ? 1 : 0;
}

static void flip_mid_cb(lv_anim_t *a)
{
    lv_obj_t *card = (lv_obj_t *)a->var;
    int i = (int)(intptr_t)lv_obj_get_user_data(card);
    /* Midpoint: the card is a vertical line, so the swap is invisible.
     * Show the side the flip is going to. */
    if (s_flip_target[i]) {
        lv_obj_add_flag(s_front_img[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_back[i], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_front_img[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_back[i], LV_OBJ_FLAG_HIDDEN);
    }
    flip_start(i, 0, MUSE_WORDCARDS_FLIP_SCALE, flip_done_cb);
}

static void start_flip(int i, bool to_back)
{
    if (s_flip_anim[i]) {
        return;
    }
    s_flip_anim[i] = true;
    s_flip_target[i] = to_back;
    flip_start(i, MUSE_WORDCARDS_FLIP_SCALE, 0, flip_mid_cb);
}

/* --- display sync ------------------------------------------------- */

static void refresh_display(void)
{
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        /* The flip animation owns the swap while it runs; otherwise
         * the shown side follows the logic state. A flipped card also
         * takes the mockup's pressed fill and 2 px GOLD border, so
         * the flipped state is visible at a glance. Nothing is written
         * when the side did not change: every style write invalidates
         * the object, and this runs every 50 ms. */
        if (!s_flip_anim[i]) {
            int8_t want = s_game.flipped[i] ? 1 : 0;
            if (s_shown[i] == want) {
                continue;
            }
            s_shown[i] = want;
            if (want) {
                lv_obj_add_flag(s_front_img[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_back[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(s_card[i],
                                          lv_color_hex(MUSE_THEME_CARD_PRESSED),
                                          LV_PART_MAIN);
                lv_obj_set_style_border_color(s_card[i],
                                              lv_color_hex(MUSE_THEME_GOLD),
                                              LV_PART_MAIN);
                lv_obj_set_style_border_width(s_card[i], 2, LV_PART_MAIN);
            } else {
                lv_obj_clear_flag(s_front_img[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_back[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(s_card[i],
                                          lv_color_hex(MUSE_THEME_CARD),
                                          LV_PART_MAIN);
                lv_obj_set_style_border_color(s_card[i],
                                              lv_color_hex(MUSE_THEME_RULE),
                                              LV_PART_MAIN);
                lv_obj_set_style_border_width(s_card[i], 1, LV_PART_MAIN);
            }
        }
    }
    /* Title: the set name, always (no 时间到 / summary any more: the
     * round has no end but the back arrow, user 2026-11-11). */
    set_label(s_name, MUSE_WORDCARDS_SETS_TABLE[s_game.set].title);
    /* Touch is live only while PLAYING; the back arrow stays visible in
     * every state (user 2026-11-10: no start page and no pause any more,
     * so leaving mid-round is the back arrow's job). */
    for (int p = 0; p < MUSE_WORDCARDS_PAGES; p++) {
        if (!s_touch[p]) {
            continue;
        }
        if (s_game.phase == MUSE_WORDCARDS_PLAYING) {
            lv_obj_clear_flag(s_touch[p], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_touch[p], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* --- game actions ------------------------------------------------- */

static void begin_game(void)
{
    /* Free game: no charge, no fee sound -- just the start chirp
     * (the shared game-start sound). The round has no end of its own,
     * so this is the only way in and the back arrow the only way out. */
    muse_audio_sfx(MUSE_SFX_START);
    s_probe_ticks = 0;
    muse_wordcards_start(&s_game);
    muse_wordcards_audio_prefetch(&s_game);
    lv_timer_resume(s_timer);
    refresh_display();
}

/* The 50 ms tick: audio drain plus the idle poke (there is no clock to
 * advance any more, user 2026-11-11). Pending audio is drained here
 * and nowhere else, so a word plays at
 * most once per flip/replay: when READY it plays and the mark is
 * cleared; when still FETCHING the mark waits for the next tick;
 * when FAILED it is dropped silently (the card stays silent). A play
 * that could not start (no task) is dropped the same way -- the user
 * can tap the speaker again. */
static void drain_pending_audio(void)
{
    int p = muse_wordcards_pending_audio(&s_game);
    if (p < 0) {
        return;
    }
    if (muse_wordcards_cache_is_ready(&s_game, p)) {
        /* Play, then drop the mark either way: a play that could not
         * start (no task) is not retried every tick -- the user can
         * tap the speaker again. */
        muse_wordcards_audio_play(&s_game, p);
        muse_wordcards_clear_pending_audio(&s_game);
    } else if (s_game.cache[p] == MUSE_WORDCARDS_CACHE_FAILED) {
        muse_wordcards_clear_pending_audio(&s_game);
    }
}

static void on_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_game.phase == MUSE_WORDCARDS_PLAYING) {
        /* A running round is activity: playing by taps alone still
         * touches the screen, but a round with no tap for a while
         * must not let the idle timer sleep the screen mid-round
         * (same idiom as video playback and a voice turn). */
        muse_state_poke();
        drain_pending_audio();
        /* Liveness probe, 5 s: if the UI ever freezes again, the serial
         * log shows whether this timer kept running (LVGL thread alive,
         * something else stuck) or stopped (thread blocked). Remove it
         * once the freeze is pinned down. */
        if ((++s_probe_ticks % (5000u / MUSE_WORDCARDS_TICK_MS)) == 0) {
            ESP_LOGI(TAG, "probe: round alive t=%us",
                     (unsigned)(s_probe_ticks / (1000u / MUSE_WORDCARDS_TICK_MS)));
        }
    }
    refresh_display();
}

/* --- touch (tap to flip, swipe to page) --------------------------- */

/* Page-local press position: the layer sits at the page's own origin, and
 * the strip's scroll offset is already accounted for by the layer's
 * coordinates, so a paged strip needs no arithmetic here. */
static void touch_local(lv_event_t *e, int *pg, lv_coord_t *lx, lv_coord_t *ly)
{
    lv_obj_t *layer = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(layer, &a);
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    *pg = (int)(intptr_t)lv_obj_get_user_data(layer);
    *lx = point.x - a.x1;
    *ly = point.y - a.y1;
}

static void on_touch_pressed(lv_event_t *e)
{
    int pg;
    lv_coord_t lx, ly;
    touch_local(e, &pg, &lx, &ly);
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    s_press_active = true;
    s_press_moved = false;
    s_press_x = point.x;
    s_press_y = point.y;
    s_press_card = card_on_page(pg, lx, ly);
    /* Nothing flips here: the flip fires on release, so a drag across the
     * grid (or a swipe to the other page) never flips a card. */
}

static void on_touch_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_press_active || s_game.phase != MUSE_WORDCARDS_PLAYING) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    int dx = point.x - s_press_x;
    int dy = point.y - s_press_y;
    if (dx * dx + dy * dy >
        MUSE_WORDCARDS_HOLD_MOVE_PX * MUSE_WORDCARDS_HOLD_MOVE_PX) {
        s_press_moved = true;
    }
}

static void on_touch_released(lv_event_t *e)
{
    bool was_active = s_press_active;
    int press_card = s_press_card;
    s_press_active = false;
    if (!was_active || s_game.phase != MUSE_WORDCARDS_PLAYING) {
        return;
    }
    /* A press that drifted is a swipe, not a tap: it flips nothing. */
    if (s_press_moved) {
        return;
    }
    int pg;
    lv_coord_t lx, ly;
    touch_local(e, &pg, &lx, &ly);
    int card = card_on_page(pg, lx, ly);
    /* A tap flips only when press and release agree on the card: a
     * swipe across the grid never flips the wrong card. A tap between
     * cards (card < 0) flips nothing. */
    if (card < 0 || card != press_card || s_flip_anim[card]) {
        return;
    }
    if (!s_game.flipped[card]) {
        if (muse_wordcards_flip_to_back(&s_game, card)) {
            start_flip(card, true);
            refresh_display();
        }
    } else if (point_on_speaker(card % MUSE_WORDCARDS_PER_PAGE, lx, ly)) {
        /* The speaker logo: replay the pronunciation. The tick
         * drains the mark, so the replay waits for the cache the
         * same way a first flip does. */
        if (muse_wordcards_replay(&s_game, card)) {
            refresh_display();
        }
    } else {
        /* Anywhere else on a flipped card: flip it back. A pending
         * pronunciation for this card is dropped -- the player is no
         * longer looking at it. */
        if (muse_wordcards_pending_audio(&s_game) == card) {
            muse_wordcards_clear_pending_audio(&s_game);
        }
        if (muse_wordcards_flip_to_front(&s_game, card)) {
            start_flip(card, false);
            refresh_display();
        }
    }
}

/* --- build -------------------------------------------------------- */

/* --- set switching + category page ------------------------------ */

/* Rebuild the cards' pictures and back texts from the current set's
 * table (the card objects themselves are built once). A slot the set does
 * not use is hidden, and the second page is shown only for a set that
 * actually has a fifth word. */
static void refresh_card_content(void)
{
    const int words = muse_wordcards_set_words(s_game.set);
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        const muse_wordcards_word_t *w =
            &MUSE_WORDCARDS_SETS_TABLE[s_game.set].words[i];
        if (!w->en) {
            lv_obj_add_flag(s_card[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_card[i], LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_front_img[i], MUSE_WORDCARDS_ART[w->art]);
        lv_obj_set_style_image_recolor(s_front_img[i],
                                       lv_color_hex(w->color),
                                       LV_PART_MAIN);
        lv_label_set_text(s_back_en[i], w->en);
        lv_label_set_text(s_back_zh[i], w->zh);
    }
    if (s_pg[1]) {
        if (words <= MUSE_WORDCARDS_PER_PAGE) {
            lv_obj_add_flag(s_pg[1], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_pg[1], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_strip) {
        lv_obj_scroll_to_x(s_strip, 0, LV_ANIM_OFF);
    }
}

void muse_wordcards_ui_open_set(int set)
{
    if (!muse_wordcards_select_set(&s_game, set)) {
        return; /* no art for that set: stay on the current one */
    }
    /* A set change ends anything the old set was doing: stop the
     * audio, abort its prefetch, drop a flip animation in flight
     * (its midpoint swap would otherwise rewrite a card that now
     * belongs to the new set). The caller starts the round and
     * prefetches the new set through ui_show(). */
    muse_wordcards_audio_stop();
    muse_wordcards_audio_prefetch_abort();
    muse_wordcards_clear_pending_audio(&s_game);
    s_press_active = false;
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        lv_anim_delete(s_card[i], NULL);
        s_flip_anim[i] = false;
        s_shown[i] = -1;   /* new words: force the next refresh to draw */
        lv_obj_set_style_transform_scale_x(s_card[i],
                                           MUSE_WORDCARDS_FLIP_SCALE,
                                           LV_PART_MAIN);
    }
    lv_timer_pause(s_timer);
    refresh_card_content();
    refresh_display();
}

static void on_set_row(lv_event_t *e)
{
    int set = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_open_set_cb) {
        s_open_set_cb(set);
    }
}

void muse_wordcards_ui_build_sets(lv_obj_t *page, lv_event_cb_t back_cb,
                                  muse_wordcards_open_set_cb_t open_cb)
{
    s_sets_page = page;
    s_open_set_cb = open_cb;
    muse_back_button(page, back_cb);
    muse_page_title(page, "单词练习", 44);
    /* Scrolling column, the minigame list's pattern (build_minigame
     * in muse_interact_ui.c): flex column, vertical scroll only,
     * scrollbar off, 10 px gaps, 110 px bottom pad so the last row
     * can be scrolled clear of the bottom curve. One row per offered
     * set, in table order: muse_card (the minigame list's row) with
     * a flex row inside -- icon + title centred as one group
     * (mockup v2: icon 36 px, 12 px gap, Noto 16 title, no count
     * text). */
    lv_obj_t *list = lv_obj_create(page);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, MUSE_WORDCARDS_SETS_W,
                    360 - MUSE_WORDCARDS_SETS_TOP);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, MUSE_WORDCARDS_SETS_TOP);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, MUSE_WORDCARDS_SETS_GAP, 0);
    lv_obj_set_style_pad_bottom(list, MUSE_WORDCARDS_SETS_PAD_BOTTOM, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    for (int s = 0; s < MUSE_WORDCARDS_SETS; s++) {
        if (!muse_wordcards_set_has_art(s)) {
            continue;
        }
        const muse_wordcards_set_t *set = &MUSE_WORDCARDS_SETS_TABLE[s];
        lv_obj_t *card = muse_card(list, lv_pct(100),
                                   MUSE_WORDCARDS_SETS_ROW_H,
                                   MUSE_WORDCARDS_SETS_RADIUS);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(card, 12, LV_PART_MAIN);
        /* First word's picture, recoloured with its own colour. */
        lv_obj_t *img = lv_image_create(card);
        lv_image_set_src(img,
                         MUSE_WORDCARDS_ART[set->words[0].art]);
        lv_image_set_scale(img, MUSE_WORDCARDS_SETS_ICON_SCALE);
        lv_obj_set_size(img, MUSE_WORDCARDS_SETS_ICON_PX,
                        MUSE_WORDCARDS_SETS_ICON_PX);
        lv_obj_set_style_image_recolor(img,
                                       lv_color_hex(set->words[0].color),
                                       LV_PART_MAIN);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER,
                                           LV_PART_MAIN);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *t = label(card, muse_font_btn(), MUSE_THEME_TEXT);
        lv_label_set_text(t, set->title);
        lv_obj_add_event_cb(card, on_set_row, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s);
    }
    /* The price line, as the column's last item (user 2026-11-11):
     * "限时免费" only, because there is no time limit any more, and it
     * cannot be pinned (a fixed y=232 slot is under the third row).
     * Scrolling the list to the bottom brings it into view. */
    lv_obj_t *hint = label(list, muse_font_cjk(), MUSE_THEME_DIM);
    lv_label_set_text(hint, "限时免费");
}

void muse_wordcards_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    muse_wordcards_init(&s_game);
    s_page = page;

    /* Paging strip: one 240x240 page per 4 cards, the feed page's idiom
     * (flex row, snap per page, scrollbar off, hard stop at both ends).
     * The 60 px side pads put the single-page field exactly where the
     * mockup had it, so a 4-word set looks unchanged. */
    s_strip = lv_obj_create(page);
    lv_obj_remove_style_all(s_strip);
    lv_obj_set_size(s_strip, 360, MUSE_WORDCARDS_PAGE_H);
    lv_obj_align(s_strip, LV_ALIGN_TOP_MID, 0, MUSE_WORDCARDS_ORIGIN_Y);
    lv_obj_set_flex_flow(s_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_left(s_strip, MUSE_WORDCARDS_ORIGIN_X, 0);
    lv_obj_set_style_pad_right(s_strip, MUSE_WORDCARDS_ORIGIN_X, 0);
    lv_obj_set_scroll_dir(s_strip, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_strip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(s_strip, LV_SCROLL_SNAP_START);
    lv_obj_remove_flag(s_strip, LV_OBJ_FLAG_SCROLL_ELASTIC);

    /* Cards: a 116x116 CARD rounded rect with a RULE outline (mockup
     * v4), four per page. The front picture is the A8 master, recoloured
     * per word at runtime and drawn at 72 px; the back is three centred
     * lines, built once per card and hidden until that card is flipped. */
    for (int p = 0; p < MUSE_WORDCARDS_PAGES; p++) {
        lv_obj_t *pg = lv_obj_create(s_strip);
        lv_obj_remove_style_all(pg);
        lv_obj_set_size(pg, MUSE_WORDCARDS_PAGE_W, MUSE_WORDCARDS_PAGE_H);
        lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
        s_pg[p] = pg;
        for (int k = 0; k < MUSE_WORDCARDS_PER_PAGE; k++) {
            int i = p * MUSE_WORDCARDS_PER_PAGE + k;
            int cx, cy;
            card_local(k, &cx, &cy);
            const muse_wordcards_word_t *w =
                &MUSE_WORDCARDS_SETS_TABLE[s_game.set].words[i];
            lv_obj_t *card = lv_obj_create(pg);
            lv_obj_set_size(card, MUSE_WORDCARDS_CARD, MUSE_WORDCARDS_CARD);
            lv_obj_set_pos(card, cx, cy);
            lv_obj_set_style_bg_color(card, lv_color_hex(MUSE_THEME_CARD),
                                      LV_PART_MAIN);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(card, lv_color_hex(MUSE_THEME_RULE),
                                          LV_PART_MAIN);
            lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
            lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
            lv_obj_set_style_transform_pivot_x(card, LV_PCT(50), LV_PART_MAIN);
            lv_obj_set_style_transform_pivot_y(card, LV_PCT(50), LV_PART_MAIN);
            lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(card, (void *)(intptr_t)i);
            s_card[i] = card;
            s_shown[i] = -1;
            /* Front: the A8 master, recoloured per word (the RPS hands'
             * image_recolor idiom) and scaled to 72 px. The 128 px image
             * object is centred on the card, so its centre is the card's
             * centre. */
            lv_obj_t *img = lv_image_create(card);
            if (w->en) {
                lv_image_set_src(img, MUSE_WORDCARDS_ART[w->art]);
            }
            lv_image_set_scale(img, MUSE_WORDCARDS_PIC_SCALE);
            lv_obj_set_style_image_recolor(img, lv_color_hex(w->color),
                                           LV_PART_MAIN);
            lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER,
                                               LV_PART_MAIN);
            lv_obj_set_pos(img,
                           (MUSE_WORDCARDS_CARD - MUSE_WORDCARDS_ART_PX) / 2,
                           (MUSE_WORDCARDS_CARD - MUSE_WORDCARDS_ART_PX) / 2);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
            s_front_img[i] = img;
            /* Back: three centred lines (mockup v4). The container is a
             * transparent box over the whole card; the lines are aligned
             * TOP_MID at their fixed offsets. */
            lv_obj_t *back = lv_obj_create(card);
            lv_obj_set_size(back, MUSE_WORDCARDS_CARD, MUSE_WORDCARDS_CARD);
            lv_obj_set_pos(back, 0, 0);
            lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(back, 0, LV_PART_MAIN);
            lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(back, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(back, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *en = label(back, &lv_font_montserrat_20,
                                 MUSE_THEME_GOLD);
            lv_label_set_text(en, w->en ? w->en : "");
            lv_obj_align(en, LV_ALIGN_TOP_MID, 0, MUSE_WORDCARDS_BACK_EN_Y);
            lv_obj_t *zh = label(back, muse_font_cjk(), MUSE_THEME_TEXT);
            lv_label_set_text(zh, w->zh ? w->zh : "");
            lv_obj_align(zh, LV_ALIGN_TOP_MID, 0, MUSE_WORDCARDS_BACK_ZH_Y);
            /* Kept so open_set can rebuild the back for a new set. */
            s_back_en[i] = en;
            s_back_zh[i] = zh;
            lv_obj_t *sp = label(back, &lv_font_montserrat_20,
                                 MUSE_THEME_TEXT);
            lv_label_set_text(sp, LV_SYMBOL_VOLUME_MAX);
            lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, MUSE_WORDCARDS_BACK_SP_Y);
            s_back[i] = back;
        }

        /* Tap layer for this page: a child of the page, so a drag still
         * scrolls the strip (the feed strip's cards behave the same way).
         * fill() strips CLICKABLE, so it is added back: without it the
         * layer would never receive PRESSED events. */
        lv_obj_t *t = fill(pg, MUSE_THEME_BG);
        lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_size(t, MUSE_WORDCARDS_PAGE_W, MUSE_WORDCARDS_PAGE_H);
        lv_obj_set_pos(t, 0, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(t, (void *)(intptr_t)p);
        lv_obj_add_event_cb(t, on_touch_pressed, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(t, on_touch_pressing, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(t, on_touch_released, LV_EVENT_RELEASED, NULL);
        s_touch[p] = t;
    }
    /* A set with four words has one page: hide the second so there is
     * nothing to swipe to (refresh_card_content keeps this in step when
     * the player opens the 8-word animal set). */
    if (muse_wordcards_set_words(s_game.set) <= MUSE_WORDCARDS_PER_PAGE) {
        lv_obj_add_flag(s_pg[1], LV_OBJ_FLAG_HIDDEN);
    }

    /* No START overlay, no PAUSED overlay and no OVER overlay (user
     * 2026-11-10 dropped the first two, 2026-11-11 dropped the third
     * with the clock): entering a set shows the 2x2 cards at once and
     * the back arrow is the only way out. */

    /* Back arrow: the shared transparent one (no filled background, user
     * request 2026-11-01), same geometry and symbol as snake / goldrain.
     * Always visible, top-left of the card page (user 2026-11-10: it is
     * the only way out now). */
    s_back_btn = muse_back_button(page, back_cb);

    /* Title (top crescent, y=38: snake's name slot, shared subpage title
     * font/colour). Nothing under the cards: the HUD went with the
     * clock (user 2026-11-11). */
    s_name = muse_page_title(page,
                             MUSE_WORDCARDS_SETS_TABLE[s_game.set].title,
                             38);

    s_timer = lv_timer_create(on_tick, MUSE_WORDCARDS_TICK_MS, NULL);
    lv_timer_pause(s_timer);

    refresh_display();
}

void muse_wordcards_ui_show(void)
{
    /* Entering the set: the 2x2 cards are the page (user 2026-11-10, no
     * start button), so a fresh round starts here and its 4 MP3s are
     * prefetched in the background. A return visit carries on with the
     * round that was running (there is no clock to run out). */
    if (s_game.phase != MUSE_WORDCARDS_PLAYING) {
        begin_game();
    } else {
        muse_wordcards_audio_prefetch(&s_game);
        lv_timer_resume(s_timer);
    }
    refresh_display();
}

void muse_wordcards_ui_hide(void)
{
    /* Leaving stops the timer and the audio; the phase stays PLAYING,
     * so coming back carries on with the same cards. */
    lv_timer_pause(s_timer);
    muse_wordcards_audio_stop();
    muse_wordcards_audio_prefetch_abort();
    muse_wordcards_clear_pending_audio(&s_game);
    s_press_active = false;
    refresh_display();
}
