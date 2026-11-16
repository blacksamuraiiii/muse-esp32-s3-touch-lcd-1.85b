/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_jiaobei_ui.h -- 掷圣杯 page (mockup v21).
 *
 * The page is built by muse_interact_ui_build() into the interaction
 * tile and shown/hidden through muse_interact_ui.c, like the other game
 * pages (OX棋 is the closest model: a self-contained lv_timer + the
 * show / hide / fee_refused hooks). The page owns NO tileview, NO dots
 * and NO caption: those stay in muse_ui.c / muse_interact_ui.c.
 *
 * The two cups are painted at runtime, per pixel, into two 100x100
 * RGB565 buffers in PSRAM (20 KB each) from the mockup geometry
 * (jiaobei-mockup-v18.py cup_tile, S=1): outer R50, inner r44 offset
 * 24, 3 px outline ring, top white / bottom black gradient, round face
 * a (255,150,120) band along the outer arc, flat face a dark line along
 * the inner edge. The buffers are NOT flash art: the cups change colour
 * with the result (left cup turns red-convex on a holy cup), so a static
 * image would need one copy per face per side. Outside the crescent is
 * black (0x0000), which reads as transparent on the black page.
 */
#pragma once

#include "lvgl.h"

/* Build the 掷圣杯 page under `page` (a 360x360 group owned by the
 * interact tile). `back_cb` is the shared "<" handler. Call once, in
 * the LVGL task. */
void muse_jiaobei_ui_build(lv_obj_t *page, lv_event_cb_t back_cb);

/* Entering the page: restore today's settled board (streak + last
 * result from the snapshot -- the streak survives leaving the page
 * within the same day, a fresh day or nothing tossed yet shows the
 * ready board), hide the limit prompt, wake the accelerometer for
 * shake-to-toss, resume the timer. */
void muse_jiaobei_ui_show(void);

/* Leaving the page (or the tile): pause the timer, hide the limit
 * prompt, sleep the accelerometer (shake polling stops with the page).
 * A flip in progress is left alone -- show() restores it from the
 * snapshot. */
void muse_jiaobei_ui_hide(void);

/* The worker refused the toss (PET_POOR after the flip already
 * started): cancel the flip back to the pre-toss board and note it in
 * the message row. */
void muse_jiaobei_ui_fee_refused(void);

/* The daily 9 tosses are used up (PET_CAP from the worker, or the
 * snapshot gate on tap): cancel any flip in progress and show the
 * limit prompt ("今日掷筊次数已用完", mockup v21). */
void muse_jiaobei_ui_limit_reached(void);
