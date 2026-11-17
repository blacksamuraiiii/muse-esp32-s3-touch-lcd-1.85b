/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_jiaobei_ui.c -- 掷圣杯 page (mockup v21).
 *
 * Layout (360x360, all y from the mockup):
 *   title "掷圣杯" y=44 (shared muse_page_title, ACCENT) + "<" back
 *   two cups 100x100, centres (130,160) / (230,160), black background
 *   阴/阳 face labels y=216 (DIM, result state only)
 *   message row y=282: ready "掷筊，每次 -2 金币" (DIM, built from
 *     MUSE_PET_JIAOBEI_COST); holy
 *     "连续圣杯 ×N，神明应允" (GOLD); laughing "笑杯，神明笑了" (GOLD);
 *     yin "阴杯，时机未到" (GOLD)
 *   coin row y=302 (TEXT, from the snapshot, like every other page)
 *   limit prompt (mockup v21): a full-page dim + a centred 280x84
 *     rounded box (CARD bg, RULE border) with "今日掷筊次数已用完".
 *
 * A tap anywhere on the page (except "<") tosses, and so does a
 * shake (user decision 2026-11-14): the accelerometer is polled once
 * per 50 ms tick through the same muse_imu_read_accel() the gold-rain
 * tilt uses, only while the page is open (wake on show, sleep on
 * hide) -- never the tap / WoM interrupt engine. Both triggers go
 * through start_toss(): the UI gates on the snapshot first (coins
 * < MUSE_PET_JIAOBEI_COST (2): a note in the message row, no sound --
 * 6d39a8e changed the old silent refusal into a note; 9 tosses used up:
 * the limit prompt), then sends PET_JIAOBEI_TOSS carrying the
 * already-decided result and starts the MUSE_JIAOBEI_FLIP_MS flip
 * (1000 ms, 1 turn) at once. The
 * worker is the authority: a PET_POOR / PET_CAP that arrives after
 * the flip started cancels it back to the pre-toss board
 * (fee_refused / limit_reached, called from the feedback drain), and
 * only a settled toss records its result into the day's streak, so
 * re-entering the page shows today's last result and streak.
 */
#include "muse_jiaobei_ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "muse_audio.h"
#include "muse_font.h"
#include "muse_imu.h"       /* shake-to-toss: same polling as gold-rain tilt */
#include "muse_jiaobei.h"
#include "muse_pet.h"
#include "muse_state.h"     /* poke: shaking never touches the screen */
#include "muse_theme.h"
#include "muse_widgets.h"

static const char *TAG = "muse_jiaobei_ui";

/* Cup tile geometry (mockup v18, S=1): 100x100 tile, outer R50, inner
 * r44 offset 24, 3 px outline ring. Left cup "(" has its outer centre
 * at (64,50) and inner at (88,50); right cup ")" mirrors it. */
#define MUSE_JIAOBEI_CUP_PX 100
#define MUSE_JIAOBEI_CUP_X0 80
#define MUSE_JIAOBEI_CUP_Y 110
#define MUSE_JIAOBEI_LIMIT_MS 2000u /* prompt auto-dismiss */
/* Shake polling runs on the LVGL thread, and a wedged I2C bus blocks a
 * read for up to the driver's 1 s timeout. Three bad reads in a row, or
 * a single read slower than MUSE_JIAOBEI_SHAKE_SLOW_MS, and the page
 * stops polling until it is entered again, rather than freezing every
 * 50 ms tick (2026-11-16 audit; the slow-read rule tightened 2026-11-17,
 * review fix F2). */
#define MUSE_JIAOBEI_SHAKE_FAIL_MAX 3
#define MUSE_JIAOBEI_SHAKE_SLOW_MS 100u

/* Limit prompt box (mockup v21, measured from the PNG): 280x84 centred
 * on the screen (x 40..319, y 138..221, centre (180,180)). The dim is
 * ~35% black (the title / coin rows behind it read at ~0.65 of their
 * colour, measured from the PNG). */
#define MUSE_JIAOBEI_PROMPT_W 280
#define MUSE_JIAOBEI_PROMPT_H 84
#define MUSE_JIAOBEI_PROMPT_X 40
#define MUSE_JIAOBEI_PROMPT_Y 138
#define MUSE_JIAOBEI_PROMPT_DIM_OPA 90

/*
 * Cup tiles are painted once per (cup, face, mirror) and cached: the flip
 * is a turn about the VERTICAL centre line (user 2026-11-16: "不是上下翻转，
 * 是左右翻转"), so the far half of every turn shows the cup's mirror image
 * -- the crescent opens the other way then, which a plain scale_x cannot
 * fake. Painting
 * is 2x2-supersampled float work over 100x100 px (~160 k samples), so
 * doing it inside a flip stalls the LVGL thread -- all eight tiles are
 * painted once at build and only their descriptors are swapped
 * afterwards; refresh_display() keeps a lazy paint as a fallback for a
 * tile whose allocation failed at build (2026-11-17, review fix F4).
 * Eight tiles at 20 KB each live in PSRAM.
 */
static muse_jiaobei_t s_game;
static muse_jiaobei_shake_t s_shake;
static lv_obj_t *s_cup_img[2];
static lv_image_dsc_t s_cup_dsc[2][2][2];
static uint16_t *s_cup_buf[2][2][2];
static bool s_cup_painted[2][2][2];
static uint8_t s_shown_src[2];     /* face * 2 + mirror currently shown */
static uint32_t s_shown_sx[2];     /* last scale_x written, 1/256 */
static uint32_t s_shake_fail;   /* consecutive bad/absent IMU reads */
static lv_obj_t *s_face_label[2];
static lv_obj_t *s_msg;
static lv_obj_t *s_coins;
static lv_obj_t *s_limit_overlay;
static lv_timer_t *s_timer;
static uint32_t s_last_tick;
static uint32_t s_limit_since; /* lv_tick_get() when the prompt showed, 0 = hidden */
static char s_note[64];        /* one-shot note in the message row, "" = none */
/* The ready row's copy, built once from MUSE_PET_JIAOBEI_COST so a price
 * change cannot leave the board advertising the old one. */
static char s_ready_msg[32];
static uint32_t s_msg_color;   /* colour s_msg currently has */
static bool s_seeded;

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_note(const char *text)
{
    snprintf(s_note, sizeof(s_note), "%s", text);
}

static void seed_once(void)
{
    if (!s_seeded) {
        srand(lv_tick_get());
        s_seeded = true;
    }
}

/* The mockup geometry (jiaobei-mockup-v18.py cup_tile) drawn at S=1
 * filled the tile edge to edge: the outer circle's centre sat at y=50 with
 * r=50, so its top and bottom touched y=0 and y=100 and the 3 px outline
 * ring there was a wide, flat band -- on the board the moons looked cut
 * off square at both ends (user 2026-11-16, twice: "上下的半圆显示不完整",
 * then after the transform change "还是被切平的边缘"). Drawing at 0.9
 * about the tile centre leaves ~2.5 px of black above and below the ring,
 * so the tips stay round. Every radius and offset below is the mockup
 * number times MUSE_JIAOBEI_CUP_S. */
#define MUSE_JIAOBEI_CUP_S 0.9f
#define CUP_SQ(v) ((v) * (v))

/* One cup tile, 100x100 RGB565. 2x2 supersampling for smooth edges.
 * Outside the crescent is black (0x0000), which reads as transparent on
 * the black page. The outline ring is solid and unshaded; the gradient,
 * the round-face band and the flat-face line apply to the base (fill)
 * region only, in mockup order. */
static void paint_cup(uint16_t *buf, int side, muse_jiaobei_face_t face,
                      bool mirror)
{
    if (buf == NULL) {
        return;
    }
    /* side 0 = left "(", side 1 = right ")". Centres scale about the tile
     * centre (50, 50); dy stays measured from that centre. */
    const float ox = 50.0f + (side == 0 ? 14.0f : -14.0f) * MUSE_JIAOBEI_CUP_S;
    const float bx = 50.0f + (side == 0 ? 38.0f : -38.0f) * MUSE_JIAOBEI_CUP_S;
    const float out2 = CUP_SQ(50.0f * MUSE_JIAOBEI_CUP_S);      /* fill */
    const float in2 = CUP_SQ(44.0f * MUSE_JIAOBEI_CUP_S);
    const float out_ring2 = CUP_SQ(53.0f * MUSE_JIAOBEI_CUP_S); /* + 3 px */
    const float in_ring2 = CUP_SQ(47.0f * MUSE_JIAOBEI_CUP_S);
    const float band2 = CUP_SQ(38.0f * MUSE_JIAOBEI_CUP_S);     /* round band */
    const float line_off = 17.0f * MUSE_JIAOBEI_CUP_S;          /* flat line */
    int fr, fg, fb, or_, og, ob;
    if (face == MUSE_JIAOBEI_ROUND) {
        fr = 214; fg = 74; fb = 54;   /* 朱红 */
        or_ = 122; og = 44; ob = 34;  /* 朱红描边 */
    } else {
        fr = 255; fg = 242; fb = 206; /* 浅木 */
        or_ = 203; og = 147; ob = 79; /* 浅木描边 */
    }
    for (int y = 0; y < MUSE_JIAOBEI_CUP_PX; y++) {
        for (int x = 0; x < MUSE_JIAOBEI_CUP_PX; x++) {
            int acc_r = 0, acc_g = 0, acc_b = 0;
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    float px = (float)x + (sx == 0 ? 0.25f : 0.75f);
                    /* A flip about the vertical axis shows the cup's mirror
                     * image for the far half of every turn: sample it
                     * mirrored instead of painting a second geometry. */
                    if (mirror) {
                        px = (float)MUSE_JIAOBEI_CUP_PX - px;
                    }
                    float py = (float)y + (sy == 0 ? 0.25f : 0.75f);
                    float dxo = px - ox;
                    float dxi = px - bx;
                    float dy = py - 50.0f;
                    float d_out2 = dxo * dxo + dy * dy;
                    float d_in2 = dxi * dxi + dy * dy;
                    bool inside = d_out2 <= out2 && d_in2 > in2;
                    bool grown = d_out2 <= out_ring2 && d_in2 > in_ring2;
                    int r = 0, g = 0, b = 0;
                    if (grown && !inside) {
                        /* The 3 px outline ring: solid, unshaded. */
                        r = or_; g = og; b = ob;
                    } else if (inside) {
                        r = fr; g = fg; b = fb;
                        /* Top white gradient (base only): 85 -> 0 by
                         * mid-height. */
                        if (py < 50.0f) {
                            int a = (int)(85.0f * (1.0f - py / 50.0f));
                            r += ((255 - r) * a) / 255;
                            g += ((255 - g) * a) / 255;
                            b += ((255 - b) * a) / 255;
                        }
                        /* Bottom black gradient (base only): 0 -> 30
                         * below y=64. */
                        if (py > 64.0f) {
                            int a = (int)(30.0f * (py - 64.0f) / 36.0f);
                            r -= (r * a) / 255;
                            g -= (g * a) / 255;
                            b -= (b * a) / 255;
                        }
                        if (face == MUSE_JIAOBEI_ROUND) {
                            /* Band along the outer arc (outside R=38). */
                            if (d_out2 > band2) {
                                int a = 100;
                                r += ((255 - r) * a) / 255;
                                g += ((150 - g) * a) / 255;
                                b += ((120 - b) * a) / 255;
                            }
                        } else {
                            /* Thin dark line along the concave edge:
                             * inside the base but outside the d=17
                             * crescent (inner circle at ox +/- 17). */
                            float dx17 = px - (side == 0 ? ox + line_off
                                                         : ox - line_off);
                            float d17 = dx17 * dx17 + dy * dy;
                            if (d17 <= in2) {
                                int a = 110;
                                r += ((int)or_ - r) * a / 255;
                                g += ((int)og - g) * a / 255;
                                b += ((int)ob - b) * a / 255;
                            }
                        }
                    }
                    acc_r += r; acc_g += g; acc_b += b;
                }
            }
            int r = acc_r / 4, g = acc_g / 4, b = acc_b / 4;
            buf[y * MUSE_JIAOBEI_CUP_PX + x] =
                (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

static lv_obj_t *mk_label(lv_obj_t *parent, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, muse_font_cjk(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void set_msg(const char *text, uint32_t color)
{
    set_text(s_msg, text);
    if (s_msg_color != color) {
        lv_obj_set_style_text_color(s_msg, lv_color_hex(color), 0);
        s_msg_color = color;
    }
}

static void show_limit(void)
{
    lv_obj_clear_flag(s_limit_overlay, LV_OBJ_FLAG_HIDDEN);
    s_limit_since = lv_tick_get();
}

static void hide_limit(void)
{
    lv_obj_add_flag(s_limit_overlay, LV_OBJ_FLAG_HIDDEN);
    s_limit_since = 0;
}

static void refresh_display(void)
{
    /* Cups: all eight tiles are pre-painted at build, so a flip only
     * swaps the descriptor -- the 2x2-supersampled paint (~160 k float
     * samples per tile) never runs inside a flip on the LVGL thread.
     * The paint below is the fallback for a tile whose allocation
     * failed at build. The spin
     * angle is written only when it actually changes: rewriting a
     * transform every tick forces LVGL to redraw the whole tile, which
     * on a 50 ms timer is the stutter the word-card page was fixed for
     * in 1.5.2d. The flip is a turn about the VERTICAL centre line, so the
     * model's angle becomes scale_x = |cos| (full size at a whole turn, a
     * line at every quarter turn) and a negative cos means we are looking
     * at the cup's back: the mirrored tile carries that, because the
     * crescent opens the other way and a scale alone cannot show it. */
    uint32_t sx = 256;
    int mirror = 0;
    if (s_game.phase == MUSE_JIAOBEI_FLIPPING) {
        float rad = (float)muse_jiaobei_flip_angle(&s_game) * 3.14159265f / 180.0f;
        float c = cosf(rad);
        uint32_t v = (uint32_t)(fabsf(c) * 256.0f);
        sx = v < 1 ? 1 : v;   /* 0-wide is degenerate in LVGL */
        mirror = c < 0.0f ? 1 : 0;
    }
    for (int i = 0; i < 2; i++) {
        muse_jiaobei_face_t want = i == 0 ? s_game.left : s_game.right;
        int f = (int)want;
        uint8_t src = (uint8_t)(f * 2 + mirror);
        if (s_cup_buf[i][f][mirror] == NULL) {
            continue; /* allocation failed at build: message rows only */
        }
        if (!s_cup_painted[i][f][mirror]) {
            paint_cup(s_cup_buf[i][f][mirror], i, want, mirror != 0);
            s_cup_painted[i][f][mirror] = true;
        }
        if (src != s_shown_src[i]) {
            lv_image_set_src(s_cup_img[i], &s_cup_dsc[i][f][mirror]);
            s_shown_src[i] = src;
        }
        if (sx != s_shown_sx[i]) {
            lv_image_set_scale_x(s_cup_img[i], sx);
            s_shown_sx[i] = sx;
        }
    }
    /* Face labels: result state only. */
    bool show_faces = s_game.phase == MUSE_JIAOBEI_RESULT;
    for (int i = 0; i < 2; i++) {
        if (show_faces) {
            lv_obj_clear_flag(s_face_label[i], LV_OBJ_FLAG_HIDDEN);
            set_text(s_face_label[i],
                     (i == 0 ? s_game.left : s_game.right) == MUSE_JIAOBEI_ROUND
                         ? "阴"
                         : "阳");
        } else {
            lv_obj_add_flag(s_face_label[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Message row: a one-shot note wins; the flip keeps whatever was
     * there; READY and RESULT set their own text. */
    if (s_note[0] != '\0') {
        set_msg(s_note, MUSE_THEME_DIM);
    } else if (s_game.phase == MUSE_JIAOBEI_RESULT) {
        switch (s_game.result) {
        case MUSE_JIAOBEI_SHENG: {
            char buf[48];
            snprintf(buf, sizeof(buf), "连续圣杯 ×%u，神明应允",
                     (unsigned)s_game.streak);
            set_msg(buf, MUSE_THEME_GOLD);
            break;
        }
        case MUSE_JIAOBEI_XIAO:
            set_msg("笑杯，神明笑了", MUSE_THEME_GOLD);
            break;
        case MUSE_JIAOBEI_YIN:
            set_msg("阴杯，时机未到", MUSE_THEME_GOLD);
            break;
        default:
            break;
        }
    } else if (s_game.phase == MUSE_JIAOBEI_READY) {
        set_msg(s_ready_msg, MUSE_THEME_DIM);
    }
    /* Coin row, from the snapshot (like every other page). */
    muse_pet_model_t snap;
    if (muse_pet_snapshot(&snap)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "金币 %u/%u", (unsigned)snap.coins,
                 (unsigned)MUSE_PET_COINS_MAX);
        set_text(s_coins, buf);
    } else {
        set_text(s_coins, "金币 --/100");
    }
}

/* Defined below on_timer; the tick calls it, the definition cannot sit
 * above without the whole toss path. */
static void update_shake(uint32_t elapsed_ms);

static void on_timer(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - s_last_tick;
    s_last_tick = now;
    if (elapsed > 200) {
        elapsed = 200; /* a stalled tick must not jump the flip */
    }
    muse_jiaobei_phase_t before = s_game.phase;
    muse_jiaobei_tick(&s_game, elapsed);
    if (before == MUSE_JIAOBEI_FLIPPING &&
        s_game.phase == MUSE_JIAOBEI_RESULT) {
        /* The result landed: play its sound once (holy reuses WIN,
         * user decision 2026-11-14). */
        switch (s_game.result) {
        case MUSE_JIAOBEI_SHENG:
            muse_audio_sfx(MUSE_SFX_WIN);
            break;
        case MUSE_JIAOBEI_XIAO:
            muse_audio_sfx(MUSE_SFX_LAUGH);
            break;
        case MUSE_JIAOBEI_YIN:
            muse_audio_sfx(MUSE_SFX_BAD);
            break;
        default:
            break;
        }
    }
    /* Auto-dismiss the limit prompt after ~2 s. */
    if (s_limit_since != 0 && now - s_limit_since >= MUSE_JIAOBEI_LIMIT_MS) {
        hide_limit();
    }
    /* Shake-to-toss, same tick as the flip (only while the page is
     * open: the timer is paused on hide, so polling stops with it). */
    update_shake(elapsed);
    refresh_display();
}

/* One toss, however it was triggered (tap or shake): the snapshot
 * gate, the request carrying the already-decided result, the local
 * flip, the TOSS sound. The worker is the authority -- a refusal
 * cancels the flip (fee_refused / limit_reached from the drain), and
 * only a settled toss records its result into the day's streak. */
static void start_toss(void)
{
    if (s_game.phase == MUSE_JIAOBEI_FLIPPING) {
        return; /* mid-flip triggers are ignored, tap or shake */
    }
    /* The snapshot gate: the worker is the authority, but the UI must
     * know before it starts the flip -- a flip the worker then refuses
     * would flash and cancel. */
    muse_pet_model_t snap;
    if (muse_pet_snapshot(&snap)) {
        uint32_t remaining = 0;
        muse_pet_jiaobei_today(&snap, time(NULL), &remaining);
        if (remaining == 0) {
            /* Today's 9 tosses are used up: the limit prompt, not a
             * toss (mockup v21 -- not on the 9th toss itself). */
            muse_jiaobei_ui_limit_reached();
            return;
        }
        if (snap.coins < MUSE_PET_JIAOBEI_COST) {
            /* Not silent any more (2026-11-16, user: "目前也没法再次掷圣杯"
             * -- a tap that does nothing at all reads as a broken page).
             * The note is the same one the worker's refusal writes, so the
             * two paths say the same thing. */
            char note[32];
            snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
                     (unsigned)MUSE_PET_JIAOBEI_COST);
            set_note(note);
            refresh_display();
            return;
        }
    }
    seed_once();
    uint32_t random = (uint32_t)rand();
    muse_jiaobei_result_t result = muse_jiaobei_result_from_random(random);
    if (!muse_pet_request_jiaobei_toss((int32_t)result)) {
        set_note("互动未就绪");
        refresh_display();
        return;
    }
    s_note[0] = '\0';
    muse_jiaobei_toss(&s_game, random);
    muse_audio_sfx(MUSE_SFX_TOSS);
    refresh_display();
}

static void on_tap(lv_event_t *e)
{
    (void)e;
    start_toss();
}

/* Shake-to-toss: one accel read per 50 ms tick, the same polling the
 * gold-rain tilt uses (muse_imu_read_accel, no interrupts). A
 * sustained shake starts one toss through the same path as a tap.
 * Skipped while the limit prompt is up (the prompt is modal: a shake
 * must not re-show it) and when the chip is absent (tap still works).
 * A failed read costs one tick; MUSE_JIAOBEI_SHAKE_FAIL_MAX bad reads
 * in a row, or one read slower than MUSE_JIAOBEI_SHAKE_SLOW_MS, turn
 * polling off for the visit (the read runs on the LVGL thread and a
 * wedged I2C bus would otherwise block it every tick). */
static void update_shake(uint32_t elapsed_ms)
{
    if (s_limit_since != 0 || s_shake_fail >= MUSE_JIAOBEI_SHAKE_FAIL_MAX ||
        !muse_imu_available()) {
        return;
    }
    float ax, ay, az;
    uint32_t t0 = lv_tick_get();
    esp_err_t err = muse_imu_read_accel(&ax, &ay, &az);
    uint32_t took = lv_tick_get() - t0;
    if (err != ESP_OK) {
        /* A failed read is cheap (no I2C wait), so three in a row are
         * tolerated before shake-to-toss is off for this visit;
         * tapping still works. */
        if (++s_shake_fail == MUSE_JIAOBEI_SHAKE_FAIL_MAX) {
            ESP_LOGW(TAG, "shake off: IMU read failed");
        }
        return;
    }
    if (took > MUSE_JIAOBEI_SHAKE_SLOW_MS) {
        /* A single slow read already blocked the LVGL thread for up to
         * the driver's 1 s timeout, so one is enough: shake-to-toss is
         * off for this visit at once, or a wedged bus would freeze the
         * page once per 50 ms tick; tapping still works. */
        s_shake_fail = MUSE_JIAOBEI_SHAKE_FAIL_MAX;
        ESP_LOGW(TAG, "shake off: IMU read took %u ms", (unsigned)took);
        return;
    }
    s_shake_fail = 0;
    if (muse_jiaobei_shake_feed(&s_shake, ax, ay, az, elapsed_ms)) {
        /* Shaking never touches the screen: poke the idle timer, or
         * the screen would sleep on a shake-only player (same idiom
         * as gold-rain tilt and video playback). */
        muse_state_poke();
        start_toss();
    }
}

static void on_limit_tap(lv_event_t *e)
{
    (void)e;
    /* Any tap on the prompt (box or dim) dismisses it; the tap never
     * reaches the board underneath, so it cannot toss. */
    hide_limit();
}

void muse_jiaobei_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    /* This page is built once at boot: a second call returns here, before
     * any allocation or LVGL object creation, so it neither rebuilds the
     * objects nor re-allocates the 160 KB of cup tiles (the old buffers
     * would leak and the descriptors would dangle). */
    if (s_cup_buf[0][0][0] != NULL) {
        return;
    }
    /* Touch layer first: everything else sits on top of it, so a tap
     * anywhere on the page (except "<") tosses. */
    lv_obj_t *touch = lv_obj_create(page);
    lv_obj_set_size(touch, 360, 360);
    lv_obj_set_pos(touch, 0, 0);
    lv_obj_set_style_bg_opa(touch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(touch, 0, 0);
    lv_obj_set_style_pad_all(touch, 0, 0);
    lv_obj_remove_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, on_tap, LV_EVENT_CLICKED, NULL);

    muse_back_button(page, back_cb);
    /* The page is called 掷筊, the same name the 今日运势 card uses (user
     * 2026-11-16: the card said 掷筊 and the board said 掷圣杯, so the two
     * names read as two features). */
    muse_page_title(page, "掷筊", 44);

    /* The two cups: one PSRAM tile per face (flat / round), 20 KB each,
     * allocated up front and painted on first use (see the tile note at
     * the top). A failed allocation hides that cup; the page still
     * works (message + coin rows), it just shows no cups. */
    for (int i = 0; i < 2; i++) {
        bool any = false;
        for (int f = 0; f < 2; f++) {
            for (int m = 0; m < 2; m++) {
            s_cup_buf[i][f][m] = (uint16_t *)heap_caps_malloc(
                (size_t)MUSE_JIAOBEI_CUP_PX * MUSE_JIAOBEI_CUP_PX * sizeof(uint16_t),
                MALLOC_CAP_SPIRAM);
            if (s_cup_buf[i][f][m] == NULL) {
                ESP_LOGE(TAG, "cup %d face %d mirror %d PSRAM alloc failed", i, f, m);
            } else {
                any = true;
            }
            s_cup_painted[i][f][m] = false;
            lv_image_dsc_t *d = &s_cup_dsc[i][f][m];
            d->header.magic = LV_IMAGE_HEADER_MAGIC;
            d->header.cf = LV_COLOR_FORMAT_RGB565;
            d->header.flags = 0;
            d->header.w = MUSE_JIAOBEI_CUP_PX;
            d->header.h = MUSE_JIAOBEI_CUP_PX;
            d->header.stride = MUSE_JIAOBEI_CUP_PX * 2;
            d->header.reserved_2 = 0;
            d->data_size =
                (size_t)MUSE_JIAOBEI_CUP_PX * MUSE_JIAOBEI_CUP_PX * sizeof(uint16_t);
            d->data = (const uint8_t *)s_cup_buf[i][f][m];
            d->reserved = NULL;
            }
        }
        s_cup_img[i] = lv_image_create(page);
        lv_obj_set_pos(s_cup_img[i], MUSE_JIAOBEI_CUP_X0 + 100 * i,
                       MUSE_JIAOBEI_CUP_Y);
        lv_obj_set_size(s_cup_img[i], MUSE_JIAOBEI_CUP_PX, MUSE_JIAOBEI_CUP_PX);
        /* The flip narrows the cup with lv_image_set_scale_x() about its
         * centre -- the image's own transform, not the style transform.
         * (The flat-cut tips the user reported twice were the paint
         * geometry, not the transform: see paint_cup's note.) */
        lv_image_set_pivot(s_cup_img[i], MUSE_JIAOBEI_CUP_PX / 2,
                           MUSE_JIAOBEI_CUP_PX / 2);
        lv_obj_remove_flag(s_cup_img[i], LV_OBJ_FLAG_CLICKABLE);
        if (!any) {
            lv_obj_add_flag(s_cup_img[i], LV_OBJ_FLAG_HIDDEN);
        }
        /* Force the first paint-and-src on the first refresh. */
        s_shown_src[i] = 0xFF;
        s_shown_sx[i] = 0;
    }

    /* Pre-paint all eight tiles now (2 cups x 2 faces x 2 mirrors): the
     * mirrored tiles are first needed mid-flip (~100 ms in), and painting
     * one then would stall the LVGL thread for ~160 k float samples.
     * Build runs once at boot, so the ~1.3 M samples land at startup,
     * not in an interaction; refresh_display()'s lazy paint stays as a
     * fallback for a tile whose allocation failed here. */
    for (int i = 0; i < 2; i++) {
        for (int f = 0; f < 2; f++) {
            for (int m = 0; m < 2; m++) {
                if (s_cup_buf[i][f][m] != NULL) {
                    paint_cup(s_cup_buf[i][f][m], i, (muse_jiaobei_face_t)f,
                              m != 0);
                    s_cup_painted[i][f][m] = true;
                }
            }
        }
    }

    /* 阴/阳 face labels under each cup (result state only): the flat
     * side is 阳 and the round side is 阴 (user 2026-11-16 -- 平/凸 was the
     * shape, 阴/阳 is what the toss means). */
    for (int i = 0; i < 2; i++) {
        s_face_label[i] = mk_label(page, MUSE_THEME_DIM, "阳");
        lv_obj_align(s_face_label[i], LV_ALIGN_TOP_MID, i == 0 ? -50 : 50, 216);
        lv_obj_add_flag(s_face_label[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Message row y=282, coin row y=302 (the shared bottom rows). */
    snprintf(s_ready_msg, sizeof(s_ready_msg), "掷筊，每次 -%u 金币",
             (unsigned)MUSE_PET_JIAOBEI_COST);
    s_msg = mk_label(page, MUSE_THEME_DIM, s_ready_msg);
    lv_obj_set_width(s_msg, 320);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_DOT);
    lv_obj_align(s_msg, LV_ALIGN_TOP_MID, 0, 282);
    s_msg_color = MUSE_THEME_DIM;
    s_coins = mk_label(page, MUSE_THEME_TEXT, "金币 --/100");
    lv_obj_align(s_coins, LV_ALIGN_TOP_MID, 0, 302);

    /* Limit prompt (mockup v21): full-page dim + centred 280x84 box.
     * Created last so it sits on top of everything, including "<"
     * (a modal: while it is up, any tap dismisses it, and it goes away
     * on its own after ~2 s). Hidden until the 9 tosses are used up. */
    s_limit_overlay = lv_obj_create(page);
    lv_obj_set_size(s_limit_overlay, 360, 360);
    lv_obj_set_pos(s_limit_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_limit_overlay, MUSE_JIAOBEI_PROMPT_DIM_OPA, 0);
    lv_obj_set_style_bg_color(s_limit_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_limit_overlay, 0, 0);
    lv_obj_set_style_radius(s_limit_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_limit_overlay, 0, 0);
    lv_obj_remove_flag(s_limit_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_limit_overlay, on_limit_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_limit_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *box = lv_obj_create(s_limit_overlay);
    lv_obj_set_size(box, MUSE_JIAOBEI_PROMPT_W, MUSE_JIAOBEI_PROMPT_H);
    lv_obj_set_pos(box, MUSE_JIAOBEI_PROMPT_X, MUSE_JIAOBEI_PROMPT_Y);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(MUSE_THEME_CARD), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(MUSE_THEME_RULE), 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *prompt = mk_label(box, MUSE_THEME_TEXT, MUSE_JIAOBEI_LIMIT_TEXT);
    lv_obj_align(prompt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(prompt, LV_OBJ_FLAG_CLICKABLE);

    muse_jiaobei_init(&s_game);
    s_last_tick = lv_tick_get();
    s_timer = lv_timer_create(on_timer, 50, NULL);
    lv_timer_pause(s_timer);
    refresh_display();
}

void muse_jiaobei_ui_show(void)
{
    /* Re-entry restores today's settled board (streak + last result
     * from the snapshot); a fresh day, or nothing tossed yet, shows
     * the ready board. The streak is no longer cleared on entry (user
     * decision 2026-11-14). A snapshot that is not ready yet (worker
     * still loading) shows the ready board; the next refresh picks up
     * the settled state. */
    muse_pet_model_t snap;
    if (muse_pet_snapshot(&snap)) {
        uint32_t streak = 0, last = 0;
        muse_pet_jiaobei_result_today(&snap, time(NULL), &streak, &last);
        if (last > 3) {
            last = 0; /* defensive: the model only ever stores 0..3 */
        }
        muse_jiaobei_restore(&s_game, (muse_jiaobei_result_t)last, streak);
    } else {
        muse_jiaobei_reset(&s_game);
    }
    s_note[0] = '\0';
    hide_limit();
    muse_jiaobei_shake_init(&s_shake);
    s_shake_fail = 0; /* a new visit tries the IMU again */
    muse_imu_wake();
    s_last_tick = lv_tick_get();
    refresh_display();
    if (s_timer != NULL) {
        lv_timer_resume(s_timer);
    }
}

void muse_jiaobei_ui_hide(void)
{
    if (s_timer != NULL) {
        lv_timer_pause(s_timer);
    }
    hide_limit();
    muse_imu_sleep();
}

void muse_jiaobei_ui_fee_refused(void)
{
    if (s_game.phase == MUSE_JIAOBEI_FLIPPING) {
        muse_jiaobei_cancel(&s_game);
    }
    char note[32];
    snprintf(note, sizeof(note), "金币不够（需 %u 枚）",
             (unsigned)MUSE_PET_JIAOBEI_COST);
    set_note(note);
    refresh_display();
}

void muse_jiaobei_ui_limit_reached(void)
{
    if (s_game.phase == MUSE_JIAOBEI_FLIPPING) {
        muse_jiaobei_cancel(&s_game);
    }
    show_limit();
    refresh_display();
}
