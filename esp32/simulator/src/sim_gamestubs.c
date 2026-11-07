/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sim_gamestubs.c -- no-op stand-ins for the five interaction mini-game
 * pages (石头剪刀布 uses muse_interact's own page; sudoku / snake /
 * whack-a-mole / goldrain coins / doodle live in their own modules).
 *
 * The simulator compiles muse_interact_ui.c, which drives these pages' APIs,
 * but the games are render-heavy boards outside this simulator's purpose
 * (face / captions / fonts / layout). Each stub keeps the link clean:
 * build() creates nothing, show()/hide() are no-ops, fee_refused() drops the
 * refusal (the worker path it reports is stubbed too). Real games still need
 * a firmware build or a future simulator pass, see notes/20261107-滑动根因归档.md
 * for the current simulator debt list.
 */

#include "../../components/muse/muse_snake_ui.h"
#include "../../components/muse/muse_whackamole_ui.h"
#include "../../components/muse/muse_goldrain_ui.h"
#include "../../components/muse/muse_doodle_ui.h"
#include "../../components/muse/muse_sudoku_ui.h"

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

void muse_sudoku_ui_build(lv_obj_t *page, lv_event_cb_t back_cb)
{
    (void)page;
    (void)back_cb;
}
void muse_sudoku_ui_show(void) {}
STUB0(muse_sudoku_ui_hide)
