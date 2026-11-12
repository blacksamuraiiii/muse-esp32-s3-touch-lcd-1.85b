/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_quiz_ui.c -- 英语口语（听力练习）subpage, see muse_quiz_ui.h.
 *
 * Layout (mockup 2026-11-11 v6, 360x360 round screen, dark theme like
 * the other games): the title "英语口语" sits in the top crescent
 * (y=38, the same slot and ACCENT colour the math page uses); the
 * speaker button is a transparent 84x84 circle centred at (180,122)
 * with a white LV_SYMBOL_VOLUME_MAX in Montserrat 28 scaled ~1.5x --
 * no filled background, no border (user 2026-11-11: only the white
 * logo, same as the word-cards speaker); the three option cards are
 * 96x96 in a row at y=176 (x=28/132/236), each a CARD rounded rect
 * with a 1 px RULE border, its picture the 128 px A8 master drawn at
 * 60 px (scale 120, recoloured per word at runtime, the word-cards
 * idiom) with the Chinese word under it.
 *
 * The verdict line (y=280) and the score line (y=306, the slot the
 * snake page prints its score on) follow the math game: the verdict
 * reads "第 n/5 题" while a question is open and the green/red
 * verdict for 600 ms after a pick; the score line reads
 * "本局 n 分 · 剩余 s 秒" while playing and turns GOLD with
 * "本局 n 分，+m 金币" at OVER. Both lines clear the bezel
 * (~299 px of chord at y=280, ~253 px at the HUD's bottom).
 *
 * Overlays follow the snake page (2026-11-01, one style across the
 * mini games): the OVER card is just the 98x66 ACCENT-bordered 再来一局
 * button -- no price line under it, this game is free. There is no
 * START overlay and no PAUSED overlay: entering the page starts a
 * round at once (word-cards shape) and leaving pauses the UI timer.
 * The back arrow stays visible in every state, as on word cards.
 *
 * Payout (same shape as the math game): at OVER the HUD turns gold
 * and reads "本局 n 分，+m 金币" with m the GROSS payout the wallet
 * actually accepted (s_round_coins, from muse_pet_round_coins_cap()
 * with the shared 20-coin ceiling -- 1 point per coin; the score is
 * settled once, at OVER). The score only ever goes up (a wrong pick
 * is +0, never -1), so the HUD never shows a negative.
 *
 * Power: no sensor is used (unlike 金币雨 / 毛毛跳跃, there is no
 * tilt here), so there is nothing to wake or sleep. A running round
 * pokes the idle timer (muse_state_poke) because a fast player may
 * answer for seconds without the screen sleeping mid-round (same
 * idiom as 金币雨 and video playback).
 */
#include "muse_quiz_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_audio.h"   /* game sound effects */
#include "muse_font.h"
#include "muse_widgets.h"
#include "muse_pet.h"        /* request: the round payout */
#include "muse_pet_model.h"
#include "muse_quiz.h"
#include "muse_state.h"
#include "muse_theme.h"
#include "muse_wordcards_art.h"
#include "muse_wordcards_audio.h"

/* Speaker button: transparent 84x84, centre (180,122). */
#define MUSE_QUIZ_SPEAKER_SIZE 84
#define MUSE_QUIZ_SPEAKER_X 138
#define MUSE_QUIZ_SPEAKER_Y 80
/* Montserrat 28 speaker glyph, scaled ~1.5x (384/256). */
#define MUSE_QUIZ_SPEAKER_SCALE 384
/* Option cards: 96x96 in a row at y=176, x=28/132/236. */
#define MUSE_QUIZ_OPT_W 96
#define MUSE_QUIZ_OPT_H 96
#define MUSE_QUIZ_OPT_Y 176
#define MUSE_QUIZ_OPT_X0 28
#define MUSE_QUIZ_OPT_X1 132
#define MUSE_QUIZ_OPT_X2 236
/* The picture: the 128 px A8 master drawn at 60 px (scale 120),
 * its centre at the card's (48,36), so the drawn picture lands at
 * (18,6) -- the object is 128x128, so its top-left is (-16,-28).
 * The Chinese word sits under it at y+70. */
#define MUSE_QUIZ_PIC_PX 60
#define MUSE_QUIZ_PIC_SCALE (256 * MUSE_QUIZ_PIC_PX / MUSE_WORDCARDS_ART_PX)
#define MUSE_QUIZ_PIC_X -16
#define MUSE_QUIZ_PIC_Y -28
#define MUSE_QUIZ_OPT_ZH_Y 70

static muse_quiz_t s_game;
static bool s_seeded = false;
/* Round payout bookkeeping, same shape as the math page: converted once at
 * OVER, s_round_coins holds what the wallet actually accepted. */
static bool s_settled = false;
static uint32_t s_round_coins = 0;
/* Which question the option cards currently show (-1: none yet). The
 * cards are rebuilt when a new question lands, not every tick. */
static int s_shown_q = -1;

static lv_obj_t *s_speaker;
static lv_obj_t *s_opt[MUSE_QUIZ_OPTIONS];
static lv_obj_t *s_opt_img[MUSE_QUIZ_OPTIONS];
static lv_obj_t *s_opt_zh[MUSE_QUIZ_OPTIONS];
static lv_obj_t *s_over_ov;
static lv_obj_t *s_back;
static lv_obj_t *s_name;
static lv_obj_t *s_hud;
static lv_obj_t *s_verdict;
static lv_timer_t *s_timer;

/* Status / message slot under the cards: the verdict line at y=280
 * (the math game's slot) and the HUD at y=306 (the snake page's
 * score slot). */
#define MUSE_QUIZ_VERDICT_Y 280
#define MUSE_QUIZ_HUD_Y 306

/* --- small helpers, same shapes as the math page ------------------ */

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
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, 360, 360);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static void set_label(lv_obj_t *l, const char *text)
{
    lv_label_set_text(l, text);
}

/* The picked card's ring: pressed fill + a 3 px GOLD (right) or
 * DANGER (wrong) border. Only the picked card is ever ringed -- the
 * right answer is not shown. Off is the plain card (CARD fill, 1 px
 * RULE border). */
static void mark_card(int i, bool on, bool right)
{
    lv_obj_t *card = s_opt[i];
    lv_obj_set_style_bg_color(
        card,
        lv_color_hex(on ? MUSE_THEME_CARD_PRESSED : MUSE_THEME_CARD),
        LV_PART_MAIN);
    lv_obj_set_style_border_color(
        card,
        lv_color_hex(on ? (right ? MUSE_THEME_OK : MUSE_THEME_DANGER)
                        : MUSE_THEME_RULE),
        LV_PART_MAIN);
    lv_obj_set_style_border_width(card, on ? 3 : 1, LV_PART_MAIN);
}

/* Rebuild the three option cards from the current question: picture
 * (the A8 master, recoloured per word, drawn at 60 px -- the
 * word-cards idiom) + Chinese word. */
static void refresh_options(void)
{
    if (s_game.phase == MUSE_QUIZ_START) {
        return; /* no question dealt yet */
    }
    const muse_quiz_question_t *q = &s_game.questions[s_game.q];
    for (int i = 0; i < MUSE_QUIZ_OPTIONS; i++) {
        const muse_wordcards_word_t *w = q->options[i];
        if (w && w->art != MUSE_WORDCARDS_ART_NONE) {
            lv_image_set_src(s_opt_img[i], MUSE_WORDCARDS_ART[w->art]);
            lv_obj_set_style_image_recolor(s_opt_img[i],
                                           lv_color_hex(w->color),
                                           LV_PART_MAIN);
        }
        set_label(s_opt_zh[i], w && w->zh ? w->zh : "");
    }
    s_shown_q = s_game.q;
}

static void refresh_display(void)
{
    if (s_game.phase != MUSE_QUIZ_START && s_game.q != s_shown_q) {
        refresh_options();
    }
    /* Verdict line: only while a question is open or its verdict is
     * showing. The icon and the words share one label, so the font
     * chain keeps them on one line (Noto 16 for the hanzi, the
     * Montserrat icon tail for the LV_SYMBOL_OK / LV_SYMBOL_CLOSE
     * glyph). At OVER the line is hidden -- the payout line below
     * carries the result. */
    bool verdict = (s_game.feedback_ms != 0);
    if (s_game.phase == MUSE_QUIZ_OVER) {
        lv_obj_add_flag(s_verdict, LV_OBJ_FLAG_HIDDEN);
    } else if (verdict) {
        set_label(s_verdict, s_game.last_right
                                 ? LV_SYMBOL_OK " 答对了，+1 分"
                                 : LV_SYMBOL_CLOSE " 答错了，不得分");
        lv_obj_set_style_text_color(
            s_verdict,
            lv_color_hex(s_game.last_right ? MUSE_THEME_OK : MUSE_THEME_DANGER),
            LV_PART_MAIN);
        lv_obj_remove_flag(s_verdict, LV_OBJ_FLAG_HIDDEN);
    } else if (s_game.phase == MUSE_QUIZ_PLAYING) {
        char buf[32];
        snprintf(buf, sizeof(buf), "第 %d/5 题", s_game.q + 1);
        set_label(s_verdict, buf);
        lv_obj_set_style_text_color(s_verdict, lv_color_hex(MUSE_THEME_DIM),
                                    LV_PART_MAIN);
        lv_obj_remove_flag(s_verdict, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_verdict, LV_OBJ_FLAG_HIDDEN);
    }
    /* Ring on the picked card for the verdict window only. Cleared
     * with the verdict, when the next question lands. */
    for (int i = 0; i < MUSE_QUIZ_OPTIONS; i++) {
        mark_card(i, verdict && s_game.last_pick == i, s_game.last_right);
    }
    /* HUD: score + time while playing, gross payout at OVER. */
    char hud_buf[48];
    if (s_game.phase == MUSE_QUIZ_OVER) {
        snprintf(hud_buf, sizeof(hud_buf), "本局 %u 分，+%u 金币",
                 (unsigned)s_game.score, (unsigned)s_round_coins);
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_GOLD),
                                    LV_PART_MAIN);
    } else {
        snprintf(hud_buf, sizeof(hud_buf), "本局 %u 分 · 剩余 %u 秒",
                 (unsigned)s_game.score,
                 (unsigned)muse_quiz_remaining_s(&s_game));
        lv_obj_set_style_text_color(s_hud, lv_color_hex(MUSE_THEME_TEXT),
                                    LV_PART_MAIN);
    }
    set_label(s_hud, hud_buf);
    /* Title: the game name in every phase (no "游戏结束" swap: the
     * OX page's result-title idiom does not apply here -- the result
     * lives in the HUD line, and the OVER card carries no title). */
    set_label(s_name, "英语口语");
    /* Overlays: only the OVER card exists. */
    if (s_game.phase == MUSE_QUIZ_OVER) {
        lv_obj_remove_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_over_ov, LV_OBJ_FLAG_HIDDEN);
    }
    /* The speaker and the option cards only take input while playing. */
    bool playing = (s_game.phase == MUSE_QUIZ_PLAYING);
    lv_obj_set_style_opa(s_speaker, playing ? LV_OPA_COVER : LV_OPA_50,
                         LV_PART_MAIN);
    for (int i = 0; i < MUSE_QUIZ_OPTIONS; i++) {
        lv_obj_set_style_opa(s_opt[i], playing ? LV_OPA_COVER : LV_OPA_50,
                             LV_PART_MAIN);
    }
}

/* --- economy, same shape as the math page ------------------------- */

/* Score -> coins, once per round, at OVER. This game converts through
 * muse_pet_round_coins_cap() with the shared 20-coin ceiling -- 1
 * point per right pick, so a round tops out at 5 coins -- and the
 * entry is free, so the round's net is the payout itself. The score
 * only ever goes up, so no floor helper is needed. A refused request
 * (worker not ready / queue full) leaves s_round_coins at 0 so the
 * card never claims coins the wallet did not get. */
static void settle_round(void)
{
    if (s_settled) {
        return;
    }
    s_settled = true;
    uint32_t coins = muse_pet_round_coins_cap(s_game.score,
                                              MUSE_PET_ROUND_COIN_MAX);
    s_round_coins = 0;
    if (coins != 0 && muse_pet_request_play_reward(coins)) {
        s_round_coins = coins;
    }
    /* Settlement sound: coins in means the round was won (WIN fanfare),
     * 0 coins means 平. Keyed on what was actually credited, so the
     * sound matches the "+m 金币" the OVER card shows. */
    muse_audio_sfx(s_round_coins > 0 ? MUSE_SFX_WIN : MUSE_SFX_DRAW);
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
    /* Free game: no charge, no fee sound -- just the start chirp
     * (also on 再来一局, which comes through here with restart=true). */
    muse_audio_sfx(MUSE_SFX_START);
    seed_once();
    /* A new round: its payout has not been settled yet. */
    s_settled = false;
    s_round_coins = 0;
    s_shown_q = -1;
    if (restart) {
        muse_quiz_restart(&s_game);
    } else {
        muse_quiz_start(&s_game);
    }
    /* Prefetch the round's 5 answer words (the quiz's own slot pool):
     * a word already READY for this generation is kept, so coming
     * back to a round in progress costs no network. */
    muse_wordcards_audio_prefetch_quiz(&s_game);
    lv_timer_resume(s_timer);
    refresh_display();
}

/* The 50 ms tick: audio drain plus the round clock (the math game's
 * shape). Pending audio is drained here and nowhere else, so a word
 * plays at most once per deal/replay: when READY it plays and the
 * mark is cleared; when still FETCHING the mark waits for the next
 * tick; when FAILED it is dropped silently (the question still shows,
 * nothing blocks). A play that could not start (no task) is dropped
 * the same way -- the user can tap the speaker again. */
static void drain_pending_audio(void)
{
    if (!muse_quiz_pending_audio(&s_game)) {
        return;
    }
    if (muse_quiz_cache_is_ready(&s_game, s_game.q)) {
        /* Play, then drop the mark either way: a play that could not
         * start (no task) is not retried every tick -- the user can
         * tap the speaker again. */
        muse_wordcards_audio_play_quiz(&s_game, s_game.q);
        muse_quiz_clear_pending_audio(&s_game);
    } else if (s_game.cache[s_game.q] == MUSE_WORDCARDS_CACHE_FAILED) {
        muse_quiz_clear_pending_audio(&s_game);
    }
}

static void on_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_game.phase != MUSE_QUIZ_PLAYING) {
        return;
    }
    /* A running round is activity: a fast player may answer for
     * seconds without the screen sleeping mid-round (same idiom as
     * 金币雨 and video playback). */
    muse_state_poke();
    drain_pending_audio();
    muse_quiz_tick(&s_game, MUSE_QUIZ_TICK_MS);
    if (s_game.phase == MUSE_QUIZ_OVER) {
        settle_round();
        lv_timer_pause(s_timer);
    }
    refresh_display();
}

/* --- speaker + options -------------------------------------------- */

static void on_speaker(lv_event_t *e)
{
    (void)e;
    if (s_game.phase != MUSE_QUIZ_PLAYING) {
        return;
    }
    /* Replay queues the pronunciation; the tick drains it from the
     * cache. No-op when not PLAYING. */
    (void)muse_quiz_replay(&s_game);
}

static void on_option(lv_event_t *e)
{
    int opt = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_game.phase != MUSE_QUIZ_PLAYING || s_game.feedback_ms != 0) {
        return;
    }
    /* answer() is what grades: false here means the pick was wrong
     * (the phase and the verdict window were checked above). */
    if (!muse_quiz_answer(&s_game, opt)) {
        muse_audio_sfx(MUSE_SFX_BAD);
    } else {
        muse_audio_sfx(MUSE_SFX_GOOD);
    }
    refresh_display();
}

static void on_restart(lv_event_t *e)
{
    (void)e;
    begin_game(true);
}

/* --- page construction -------------------------------------------- */

static lv_obj_t *overlay(lv_obj_t *page)
{
    lv_obj_t *ov = fill(page, MUSE_THEME_CARD);
    lv_obj_set_size(ov, 360, 360);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(MUSE_THEME_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ov, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    return ov;
}

static lv_obj_t *centre_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    /* The shared card, a 98x66 rounded rect (user decision 2026-11-01):
     * the ACCENT ring is this page's own accent on top of the shared
     * CARD / CARD_PRESSED base. */
    lv_obj_t *btn = muse_card(parent, 98, 66, 14);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_border_color(btn, lv_color_hex(MUSE_THEME_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    return btn;
}

void muse_quiz_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    muse_quiz_init(&s_game);

    /* Speaker button: transparent, no border -- only the white logo
     * (user 2026-11-11: no filled circle, no outline, same as the
     * word-cards speaker). The glyph is Montserrat 28 scaled ~1.5x,
     * centred by lv_obj_center. */
    s_speaker = lv_btn_create(page);
    lv_obj_remove_style_all(s_speaker);
    lv_obj_set_size(s_speaker, MUSE_QUIZ_SPEAKER_SIZE, MUSE_QUIZ_SPEAKER_SIZE);
    lv_obj_set_pos(s_speaker, MUSE_QUIZ_SPEAKER_X, MUSE_QUIZ_SPEAKER_Y);
    lv_obj_set_style_bg_opa(s_speaker, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(s_speaker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sp = label(s_speaker, &lv_font_montserrat_28, MUSE_THEME_TEXT);
    lv_label_set_text(sp, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_transform_pivot_x(sp, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(sp, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_scale_x(sp, MUSE_QUIZ_SPEAKER_SCALE, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(sp, MUSE_QUIZ_SPEAKER_SCALE, LV_PART_MAIN);
    lv_obj_center(sp);
    lv_obj_add_event_cb(s_speaker, on_speaker, LV_EVENT_CLICKED, NULL);

    /* Option cards: 96x96 CARD rounded rects with a 1 px RULE border.
     * The picture is the A8 master drawn at 60 px (scale 120, the
     * word-cards idiom: the 128 px object sits at (-16,-28) so its
     * centre lands at the card's (48,36)); the Chinese word sits
     * under it at y+70. Created before the overlay so the overlay
     * dims them. */
    const lv_coord_t xs[MUSE_QUIZ_OPTIONS] = {MUSE_QUIZ_OPT_X0,
                                              MUSE_QUIZ_OPT_X1,
                                              MUSE_QUIZ_OPT_X2};
    for (int i = 0; i < MUSE_QUIZ_OPTIONS; i++) {
        lv_obj_t *card = lv_obj_create(page);
        lv_obj_set_size(card, MUSE_QUIZ_OPT_W, MUSE_QUIZ_OPT_H);
        lv_obj_set_pos(card, xs[i], MUSE_QUIZ_OPT_Y);
        lv_obj_set_style_bg_color(card, lv_color_hex(MUSE_THEME_CARD),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, lv_color_hex(MUSE_THEME_RULE),
                                      LV_PART_MAIN);
        lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_option, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_opt[i] = card;
        lv_obj_t *img = lv_image_create(card);
        lv_image_set_scale(img, MUSE_QUIZ_PIC_SCALE);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_pos(img, MUSE_QUIZ_PIC_X, MUSE_QUIZ_PIC_Y);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        s_opt_img[i] = img;
        lv_obj_t *zh = label(card, muse_font_cjk(), MUSE_THEME_TEXT);
        lv_obj_align(zh, LV_ALIGN_TOP_MID, 0, MUSE_QUIZ_OPT_ZH_Y);
        s_opt_zh[i] = zh;
    }

    /* OVER overlay: dim + 再来一局 button only (UI polish 2026-11-01).
     * No price line under it -- this game is free. The result title
     * lives in the HUD line, above the dim, so the overlay itself
     * carries no title and no payout line. */
    s_over_ov = overlay(page);
    lv_obj_t *again_btn = centre_btn(s_over_ov, 131, 147);
    lv_obj_t *again_lb = label(again_btn, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_label_set_text(again_lb, "再来一局");
    lv_obj_center(again_lb);
    lv_obj_add_event_cb(again_btn, on_restart, LV_EVENT_CLICKED, NULL);

    /* Back arrow: the shared transparent one (no filled background,
     * user request 2026-11-01), same geometry and symbol as snake /
     * math. Visible in every state -- there is no START or pause
     * page to hide it behind. Created after the overlay so it stays
     * clickable on it. */
    s_back = muse_back_button(page, back_cb);

    /* Title (top crescent, y=38: the math page's name slot, shared
     * subpage title font/colour), the verdict line and the score
     * line below the cards: page-level, above the dim overlay, so
     * they stay readable in every state. */
    s_name = muse_page_title(page, "英语口语", 38);
    s_verdict = label(page, muse_font_cjk(), MUSE_THEME_OK);
    lv_obj_align(s_verdict, LV_ALIGN_TOP_MID, 0, MUSE_QUIZ_VERDICT_Y);
    lv_obj_add_flag(s_verdict, LV_OBJ_FLAG_HIDDEN);
    s_hud = label(page, muse_font_cjk(), MUSE_THEME_TEXT);
    lv_obj_align(s_hud, LV_ALIGN_TOP_MID, 0, MUSE_QUIZ_HUD_Y);

    s_timer = lv_timer_create(on_tick, MUSE_QUIZ_TICK_MS, NULL);
    lv_timer_pause(s_timer);

    refresh_display();
}

void muse_quiz_ui_show(void)
{
    if (s_game.phase == MUSE_QUIZ_START) {
        begin_game(false);
        return;
    }
    if (s_game.phase == MUSE_QUIZ_PLAYING) {
        /* Coming back mid-round: the clock resumes and the prefetch
         * runs again -- words already READY for this generation are
         * kept, so it costs no network. */
        muse_wordcards_audio_prefetch_quiz(&s_game);
        lv_timer_resume(s_timer);
    }
    /* OVER stays: the card waits for 再来一局. */
    refresh_display();
}

void muse_quiz_ui_hide(void)
{
    /* Leaving pauses the clock (the round never ends by itself while
     * the player is away) and stops anything in flight: a playback,
     * a prefetch, a pending pronunciation. The phase is left alone,
     * so coming back resumes the round. */
    lv_timer_pause(s_timer);
    muse_wordcards_audio_stop();
    muse_wordcards_audio_prefetch_abort();
    muse_quiz_clear_pending_audio(&s_game);
    refresh_display();
}
