/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_luckywheel_ui.h -- lucky wheel page: the 240 px disc, the gold
 * pointer, the snake-style overlay layers (START undimmed, RESULT
 * 50% dim) and the 50 ms spin timer. See muse_luckywheel_ui.c.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void muse_luckywheel_ui_build(lv_obj_t *parent, lv_event_cb_t back_cb);
void muse_luckywheel_ui_show(void);
void muse_luckywheel_ui_hide(void);

/* Worker refusals, called from the interact page's feedback drain:
 * cancel an in-flight spin and show the reason on the page. */
void muse_luckywheel_ui_fee_refused(void);
void muse_luckywheel_ui_storage_error(void);

#ifdef __cplusplus
}
#endif
