/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_alarm_ui.h -- the alarm v1 pages (alarm-plan §3/§11).
 *
 * Two pages, both owned here (the LVGL objects); the settings page only
 * wires the navigation:
 *
 *   - the time picker page (SETTINGS -> Alarm -> Time): two lv_roller
 *     wheels, hour 00-23 and minute 00-59, infinite scroll. Rolling only
 *     changes the display; leaving the page writes NVS once
 *     (muse_settings_set_alarm_time) and re-arms once. The page's own
 *     back button and right-swipe gesture both save first, then call the
 *     back callback the settings page gave at build time.
 *   - the ring page: a full-screen overlay on lv_layer_top(), so it
 *     covers whichever tile the alarm caught the device on without
 *     touching the 3-tile structure. It has no tile, no back button and
 *     no swipe: one Stop button (the any-key stop in muse_input.c still
 *     applies underneath, and the button itself waits out the same 6 s
 *     panel-settling grace as the keys: a press inside the grace is
 *     refused and the hint line says so). It is driven by muse_alarm_ui_tick(), called
 *     every frame from the LVGL timer in muse_ui.c: the input task runs
 *     on its own task and must never touch LVGL, so the tick polls
 *     muse_input_alarm_ringing() and shows/hides the overlay from the
 *     LVGL thread. No init call: the overlay builds lazily on the first
 *     ring.
 *
 * Call all of these from the LVGL task only.
 */
#pragma once

#include "lvgl.h"

/* Build the time picker page under `tile` (a settings sub-page, hidden
 * until shown). `back_cb` runs when the user leaves the page (back
 * button or right swipe), after the time has been saved; it only has to
 * navigate back to the Alarm page. Call once, in the LVGL task. */
lv_obj_t *muse_alarm_ui_build_time_page(lv_obj_t *tile, lv_event_cb_t back_cb);

/* Write the rollers' current values to NVS (one notification, one
 * re-arm). A no-op before the page exists. Called on leaving the page;
 * the settings page does not need to call it. */
void muse_alarm_ui_time_page_save(void);

/* Show the ring overlay while the alarm rings, hide it when the ring
 * ends. Call every frame from the LVGL timer (muse_ui.c frame_tick). */
void muse_alarm_ui_tick(void);
