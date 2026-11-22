/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sim_gamestubs.c -- no-op stand-ins for the five interaction mini-game
 * pages (石头剪刀布 uses muse_interact's own page; snake /
 * whack-a-mole / goldrain coins / doodle / breakout live in their own
 * modules).
 *
 * The simulator compiles muse_interact_ui.c, which drives these pages' APIs,
 * but the games are render-heavy boards outside this simulator's purpose
 * (face / captions / fonts / layout). Each stub keeps the link clean:
 * build() creates nothing, show()/hide() are no-ops, fee_refused() drops the
 * refusal (the worker path it reports is stubbed too). Real games still need
 * a firmware build or a future simulator pass, see notes/20261107-滑动根因归档.md
 * for the current simulator debt list. 数独 was deleted 2026-11-20
 * (size-trim), so its stub went with it; see
 * esp32/notes/20261120-数独删除说明.md to restore it.
 */

#include "../../components/muse/muse_snake_ui.h"
#include "../../components/muse/muse_whackamole_ui.h"
#include "../../components/muse/muse_goldrain_ui.h"
#include "../../components/muse/muse_doodle_ui.h"
#include "../../components/muse/muse_breakout_ui.h"

/* 2026-11-21: the same treatment for the pages the 1.5..1.5.4 era added
 * to muse_interact_ui.c (jiaobei / luckywheel / mathpractice / quiz /
 * tictactoe / wordcards / tracing / the alarm overlay) plus the art
 * arrays those pages reference. The simulator's golden-frame scenarios
 * never visit these tiles, so no-op stubs keep the link clean exactly
 * like the five above. */
#include "../../components/muse/muse_jiaobei_ui.h"
#include "../../components/muse/muse_luckywheel_ui.h"
#include "../../components/muse/muse_mathpractice_ui.h"
#include "../../components/muse/muse_quiz_ui.h"
#include "../../components/muse/muse_tictactoe_ui.h"
#include "../../components/muse/muse_wordcards_ui.h"
#include "../../components/muse/muse_tracing_ui.h"
#include "../../components/muse/muse_alarm_ui.h"
#include "../../components/muse/muse_audio.h"
#include "../../components/muse/muse_doodle_art.h"
#include "../../components/muse/muse_fortune_art.h"
#include "../../components/muse/muse_goldrain_art.h"
#include "../../components/muse/muse_learn_art.h"
#include "../../components/muse/muse_main_grid_art.h"
#include "../../components/muse/muse_whackamole_art.h"

#define STUB0(name) \
    void name(void) {}

void muse_snake_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_snake_ui_show(void) {}
STUB0(muse_snake_ui_hide)
STUB0(muse_snake_ui_fee_refused)

void muse_whackamole_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_whackamole_ui_show(void) {}
STUB0(muse_whackamole_ui_hide)
STUB0(muse_whackamole_ui_fee_refused)

void muse_goldrain_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_goldrain_ui_show(void) {}
STUB0(muse_goldrain_ui_hide)
STUB0(muse_goldrain_ui_fee_refused)

void muse_doodle_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_doodle_ui_show(void) {}
STUB0(muse_doodle_ui_hide)
STUB0(muse_doodle_ui_fee_refused)

void muse_breakout_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_breakout_ui_show(void) {}
STUB0(muse_breakout_ui_hide)
STUB0(muse_breakout_ui_fee_refused)

/* ---- 1.5..1.5.4 pages: same shape, driven by muse_interact_ui.c ---- */

void muse_jiaobei_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_jiaobei_ui_show(void) {}
STUB0(muse_jiaobei_ui_hide)
STUB0(muse_jiaobei_ui_fee_refused)
STUB0(muse_jiaobei_ui_limit_reached)

void muse_luckywheel_ui_build(lv_obj_t *parent, lv_event_cb_t back_cb)
{
    (void)parent;
    (void)back_cb;
}
void muse_luckywheel_ui_show(void) {}
STUB0(muse_luckywheel_ui_hide)
STUB0(muse_luckywheel_ui_fee_refused)
STUB0(muse_luckywheel_ui_storage_error)

void muse_mathpractice_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_mathpractice_ui_show(void) {}
STUB0(muse_mathpractice_ui_hide)

void muse_quiz_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_quiz_ui_show(void) {}
STUB0(muse_quiz_ui_hide)

void muse_tictactoe_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_tictactoe_ui_show(void) {}
STUB0(muse_tictactoe_ui_hide)
STUB0(muse_tictactoe_ui_fee_refused)

void muse_wordcards_ui_build_sets(lv_obj_t *page, lv_event_cb_t back_cb,
                                  muse_wordcards_open_set_cb_t open_cb)
{
    (void)page;
    (void)back_cb;
    (void)open_cb;
}
void muse_wordcards_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_wordcards_ui_show(void) {}
STUB0(muse_wordcards_ui_hide)
void muse_wordcards_ui_set_coins(const char *text) { (void)text; }
void muse_wordcards_ui_open_set(int set) { (void)set; }

void muse_tracing_ui_build_list(lv_obj_t *page, lv_event_cb_t back_cb,
                                muse_tracing_open_char_cb_t open_cb)
{
    (void)page;
    (void)back_cb;
    (void)open_cb;
}
void muse_tracing_ui_build_trace(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_tracing_ui_open_char(int idx) { (void)idx; }
void muse_tracing_ui_refresh_list(void) {}
STUB0(muse_tracing_ui_hide)

STUB0(muse_alarm_ui_tick)

/* The audio-effect entry points (the firmware's sfx worker is stubbed
 * away with the audio stack itself). */
esp_err_t muse_audio_sfx(muse_sfx_t sfx) { (void)sfx; return 0; }
esp_err_t muse_audio_sfx_sync(muse_sfx_t sfx) { (void)sfx; return 0; }

/* ---- Art data the built pages reference ----
 * A 1x1 fully transparent A8 tile stands in for every bitmap: the icon
 * objects still exist with sane geometry (that is what the simulator
 * measures), they just draw nothing. */

static const uint8_t s_stub_pixel[1] = { 0x00 };
#define STUB_DSC_NAME muse_sim_stub_dsc
static const lv_image_dsc_t STUB_DSC_NAME = {
    .header = { .w = 1, .h = 1, .cf = LV_COLOR_FORMAT_A8 },
    .data_size = sizeof(s_stub_pixel),
    .data = s_stub_pixel,
};

const lv_image_dsc_t muse_doodle_sprite = STUB_DSC_NAME;
const lv_image_dsc_t muse_goldrain_sprite = STUB_DSC_NAME;
const lv_image_dsc_t muse_whackamole_mole = STUB_DSC_NAME;

const lv_image_dsc_t *const MUSE_FORTUNE_ART[MUSE_FORTUNE_COUNT] = {
    &STUB_DSC_NAME, &STUB_DSC_NAME, &STUB_DSC_NAME,
};
const lv_image_dsc_t *const MUSE_LEARN_ART[MUSE_LEARN_COUNT] = {
    &STUB_DSC_NAME, &STUB_DSC_NAME, &STUB_DSC_NAME, &STUB_DSC_NAME,
};
const uint32_t MUSE_LEARN_ART_COLOR[MUSE_LEARN_COUNT] = {
    0xff7a59, 0xf6cb4b, 0x8ec9ff, 0xff8fb3,
};
const lv_image_dsc_t *const MUSE_MAIN_GRID_ART[MUSE_MAIN_GRID_COUNT] = {
    &STUB_DSC_NAME, &STUB_DSC_NAME, &STUB_DSC_NAME, &STUB_DSC_NAME,
};
const uint32_t MUSE_MAIN_GRID_ART_COLOR[MUSE_MAIN_GRID_COUNT] = {
    0xa77dff, 0xff8fb3, 0x8ec9ff, 0x6ff0bf,
};
