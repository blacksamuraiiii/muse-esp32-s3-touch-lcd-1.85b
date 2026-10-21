/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_input.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#include "esp_sleep.h"
#endif

#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_console.h"
#include "muse_imu.h"
#include "muse_rtc.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_menu.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif

static atomic_uint s_touch_talk_events;

void muse_input_touch_talk(bool pressed)
{
    atomic_fetch_or(&s_touch_talk_events, pressed ? MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_RELEASE);
}

static const char *TAG = "muse_input";

#define POLL_MS 10
#define REST_POLL_MS 50        /* screen off and paused: still quick to wake */
#define REST_WAIT_MS 1000      /* paused, buttons that interrupt: the rest waits this long */
#define NAP_WAIT_MS 10000      /* ... and Wi-Fi napping */
#define POWER_MS 2000          /* refresh battery */
#define REST_POWER_MS 10000    /* ... while paused */
#define WIFI_NAP_MS (2 * 60 * 1000)   /* low power this long: Wi-Fi off until it ends */
#define DOUBLE_TICKS 35        /* 350 ms: a second aux press within this toggles phone setup */

#define GOODBYE_MS 1500        /* let the goodbye animation play */
#define HINT_TICKS 60          /* 0.6 s: warn that holding powers off */
#define LONG_TICKS 150         /* 1.5 s: power off */
/* Eight seconds on the button, held while parked, is the way out of a park that
 * was decided wrongly - a reference learned face down, or a part that disagrees
 * with the vector it reported at boot. Nothing else in a parked state is
 * listening and the screen is dark, so there is no hint to give: the exit is
 * silent by design. */
#define PARK_EXIT_TICKS 800    /* 8 s at POLL_MS */
#define SLEEP_CHECK_MS 100
/* Picking the gadget up wakes the screen. STATUS1 is read this often whether
 * the screen is on or off: left latched, the chip holds INT1 high, and the line
 * the next sleep arms on is already satisfied - which is how a gesture stops
 * working after a while. The INT1 line is armed with the buttons in
 * wait_buttons(), so this poll only has to cover an edge the sleep missed. */
#define IMU_POLL_MS 200
/* CTRL1/CTRL2/CTRL7/CTRL8 - the accelerometer's ODR among them - are compared
 * against what init wrote this often, and the wake-on-motion parameters
 * (CAL1_L/H) this often again. A drifting chip is re-initialised by
 * muse_imu_check(). The ODR matters most: with it wrong the engine still looks
 * configured while it gets no samples. */
#define IMU_VERIFY_MS 1000
#define IMU_FULL_VERIFY_MS (60 * 1000)
/* The engine is restarted from scratch this often while asleep. It covers the
 * one failure that leaves no readable trace: an engine that stops detecting
 * with its configuration intact. A restart resets the chip and reconfigures it,
 * so it is not free - a minute is often enough and rare enough. */
#define IMU_REARM_MS (60 * 1000)
/* Motion beyond gravity above this leaves a line in the log, next to what
 * STATUS1 said. That pair is what separates "the engine never saw the
 * movement" from "it saw it and nobody read it". */
#define IMU_BUMP_LOG_MG 150
#define IMU_BUMP_LOG_QUIET_MS 700
/* The lift check: the motion event only arms it, and the posture 400 ms later
 * decides. Long enough for the hand to stop, short enough that the screen still
 * feels like it answered. */
#define WAKE_CONFIRM_MS 400
/* Raised gate once three lifts in a row were lit for nobody: the gadget was in
 * a bag, not in a hand. */
#define WAKE_ANGLE_SUPPRESSED_DEG 55
/* A screen lit by a lift that nobody touches is a screen lit for nothing, and
 * that is the whole cost of carrying the gadget in a bag. After this long with
 * no interaction it goes back to sleep and counts as a false wake. */
#define WAKE_IDLE_RECHECK_MS 6000
#define WAKE_IDLE_RECHECK_SUPPRESSED_MS 2500
#define FALSE_WAKE_LIMIT 3
#define SERIAL_RX 1024         /* the driver drops what doesn't fit, so a console line must */
#define SERIAL_LINE 1024
#define CHAT_MAX (192 * 1024)  /* a typed message, assembled from "chat+=" lines */

static QueueHandle_t s_queue;
static bool (*s_console_hook)(const char *line);
static TaskHandle_t s_input;
static bool s_talk_down;
static bool s_cpu_low;      /* display stopped and the CPU allowed to sleep */
static volatile bool s_power_off_requested;
static bool s_cpu_pd;       /* muse_input_power_init() holds the CPU's retention memory */
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_awake;   /* with s_cpu_pd: held whenever !s_cpu_low */
static bool s_awake_held;
#endif
static volatile bool s_nap_now;   /* ">nap": asleep, as if on battery, nap without waiting WIFI_NAP_MS */
static bool s_force_low;          /* ">pmlow=1": light-sleep even with a USB host attached */
/* The gadget is lying face down. This is not "asleep" with an extra step: it is
 * a stronger state that also silences the glass. Face down on a table the panel
 * is pressed by the weight of the device, the touch controller reports it as a
 * finger, and a 500 ms press on the face tile starts a recording nobody asked
 * for - which is where the blank voice notes came from. Parking turns the
 * screen off and drops every touch event on the floor, so the only thing that
 * brings it back is turning it over. */
static bool s_parked;
/* The IMU timers below are wall-clock, not ticks. On battery the CPU
 * light-sleeps between polls, and a tick-based window can be skipped for the
 * whole sleep: the poll then runs late (or, if the tick did not move at all, is
 * skipped entirely), STATUS1 is left unread and the IMU keeps INT1 high - which
 * the next sleep already satisfies, so the gesture never wakes it again.
 * esp_timer_get_time() counts through light sleep, so the windows mean what
 * they say. */
static int64_t s_imu_checked_ms;  /* last STATUS1 poll, ms of wall clock */
static int64_t s_imu_verified_ms; /* last CTRL1/2/7/8 comparison */
static int64_t s_imu_full_ms;     /* last wake-on-motion parameter comparison */
static int64_t s_imu_rearmed_ms;  /* last engine restart, counted from the last wake */
static int64_t s_bump_logged_ms;  /* last motion that made it into the log */

/* The lift check, and what a lift that nobody answered costs. */
static bool s_wake_pending;       /* a motion event is waiting for its posture */
static int64_t s_wake_check_ms;   /* ... until this wall-clock ms */
static int64_t s_wake_hold_ms;    /* when a lift was accepted, 0 = none */
static bool s_wake_interacted;    /* a touch or a button since then */
static bool s_wake_suppressed;    /* three idle wakes in a row: the bag case */
static int s_false_wakes;

/* Wake forensics, printed by "imu": they separate "the line never rose" from
 * "the line rose and nobody read STATUS1" when a gesture fails on battery. */
static uint32_t s_stat_sleeps;      /* wait_buttons() returns */
static uint32_t s_stat_wgpio;       /* ... ended by a line (cause GPIO) */
static uint32_t s_stat_wtimer;      /* ... ended by the nap running out */
static uint32_t s_stat_wother;      /* ... ended any other way */
static uint32_t s_stat_line_high;   /* ... of which the IMU line was up */
static uint32_t s_stat_wake_btn;    /* ... of which the button's line was down */
static uint32_t s_stat_touch_wakes; /* wakes this task made from the touch line */
static uint32_t s_stat_motion;      /* motion events the poll saw while asleep */
static uint32_t s_stat_motion_awake; /* ... and while the screen was already on */
static uint32_t s_stat_ignored;     /* ... of which the posture gate dropped */
static int64_t s_stat_slept_ms = 1000;  /* before the first wait: not a rejected sleep */
static int64_t s_stat_tick_ms;      /* ... and what the FreeRTOS tick made of it */
static int s_stat_int1;             /* IMU line level when that wait returned */
static int s_stat_btn_level;        /* button level when that wait returned */
static int s_stat_touch_level;      /* touch INT level when that wait returned */
static int s_stat_touch_armed;      /* ...and when the wait armed it (-1 unknown) */

static void post(muse_ptt_t type, bool wake)
{
    muse_input_event_t ev = { .type = type, .wake = wake };
    ESP_LOGI(TAG, "PTT %s%s", type == MUSE_PTT_DOWN ? "down" : "up", wake ? " (waking)" : "");
    xQueueSend(s_queue, &ev, 0);
}

static void set_asleep(bool asleep, const char *why);

static bool update_power(void);

/* A hand on the glass pulls the touch controller's INT to the level light sleep
 * waits on, and it stays there until the controller is read - which, with the
 * screen dark on battery, nothing does: the display is paused, and with it the
 * LVGL task that runs on_cover_event(). Reading the line here is what turns a
 * touch into a wake, whether or not the line woke the chip by itself (on the
 * 1.85B it does; a board whose line is not a wake source gets the wake from the
 * 1 s / 10 s return instead, as long as the hand is still there). */
static void check_touch_line(void)
{
    if (s_parked) {
        /* Face down: the line is low because the table is leaning on the panel.
         * Waking for that is the misfire this state exists to stop. */
        return;
    }
    int i = muse_gpio_wake_line_touch_index();
    if (i < 0 || muse_gpio_wake_line_armed(i) != 0) {
        return;   /* no touch line registered, or "tpwake=0" took it out */
    }
    /* One wake per touch: the line stays low until the controller is read, and
     * the read is what the resume triggers. A line that is already low when the
     * screen goes dark therefore wakes it once, not once per poll. Clear the
     * latch whenever the line is back high - awake or not, or a touch taken
     * while the screen was on would lock every later tap out. */
    static bool latched;
    if (muse_gpio_wake_line_live(i) != 0) {
        latched = false;
        return;
    }
    if (muse_gpio_wake_line_armed_level(i) != 1) {
        /* The line was already low when the sleep was armed: the controller
         * still holds it (nobody read it), so this low is stale, not a new
         * touch. Counted at arm time as a skip, and it must not relight the
         * screen the moment it goes dark. */
        return;
    }
    if (s_stat_slept_ms < 80) {
        /* The wait never entered light sleep, so this level was satisfied as it
         * went in: the pulse the touch controller emits when the panel is
         * switched off, not a hand on the glass. */
        return;
    }
    if (latched || !muse_state_asleep()) {
        return;
    }
    latched = true;
    s_stat_touch_wakes++;
    set_asleep(false, "touch");
}

static void power_off(void)
{
    if (!muse_board->power_off) {
        return;   /* a physical switch powers it off */
    }
    ESP_LOGI(TAG, "shutting down");
    muse_state_set_asleep(false);
    update_power();
    muse_state_set_progress(0);
    muse_state_set_level(0);
    muse_state_set_mode(MUSE_MODE_OFF);
    muse_state_set_caption("GOODBYE!");
    vTaskDelay(pdMS_TO_TICKS(GOODBYE_MS));
#if CONFIG_PM_ENABLE
    /* Automatic light sleep leaves the timer armed, and a deep sleep would
     * wake on it at once (with s_cpu_pd light sleep stays configured). */
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
#endif
    esp_err_t err = muse_board->power_off();
    /* Only reached if the board couldn't power off. */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGE(TAG, "power-off failed (%s)", esp_err_to_name(err));
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("COULDN'T POWER OFF");
}

static void set_asleep(bool asleep, const char *why)
{
    if (asleep != muse_state_asleep()) {
        ESP_LOGI(TAG, "%s (%s)", asleep ? "sleeping" : "waking", why);
        muse_state_set_asleep(asleep);
        if (asleep) {
            /* The posture the user left it in is the one a lift has to change
             * away from. Taken here and not at init: the gadget is put down in
             * a different place every time. */
            muse_imu_note_sleep_posture();
            s_wake_pending = false;
            s_wake_hold_ms = 0;
        }
        if (s_input) {
            xTaskNotifyGive(s_input);   /* out of wait_buttons() */
        }
        if (!asleep && !muse_wifi_connected()) {
            muse_wifi_apply();   /* the screen says reconnecting: don't sit out the backoff */
        }
    }
}

/* Face down: screen off, touch ignored, and nothing but turning it over brings
 * it back. Kept as a state rather than an action because the panel is still
 * reporting presses while the device lies there - each of those has to be
 * dropped, not just the first one. */
static void park(bool on, const char *why)
{
    if (on == s_parked) {
        /* The state is a latch, the screen is not: an image, a pairing prompt or
         * a stray press on the cover can light it while the device still lies
         * face down. Re-asserting is what keeps "parked" meaning "dark". */
        if (on && !muse_state_asleep()) {
            set_asleep(true, "face down again");
        }
        return;
    }
    s_parked = on;
    ESP_LOGI(TAG, "%s (%s)", on ? "face down: parked" : "face up: unparked", why);
    if (on) {
        set_asleep(true, "face down");
    } else {
        set_asleep(false, "face up");
    }
}

bool muse_input_parked(void)
{
    return s_parked;
}

static void toggle_phone_setup(void)
{
    bool on = !muse_settings_ble_on();
    ESP_LOGI(TAG, "phone setup %s", on ? "on" : "off");
    muse_settings_set_ble_on(on);
    muse_ble_status_t b;
    muse_ble_status(&b);
    if (on && b.name[0]) {
        muse_state_set_caption("PHONE SETUP: %s", b.name);
    } else {
        muse_state_set_caption("PHONE SETUP %s", on ? "ON" : "OFF");
    }
}

/*
 * Aux button: short press sleeps, a 1.5 s hold powers off, any press wakes.
 * Two quick presses toggle BLE phone setup, so the sleep waits a moment to
 * see whether a second press follows.
 */
static void aux_button(bool pressed, bool edge)
{
    static int held;
    static bool swallow;
    static bool hinted;
    static int sleep_in;    /* ticks until a pending single press sleeps */
    static int park_held;
    static char saved_caption[64];

    /* Parked, the press is the table and every other gesture is dropped - but a
     * park decided wrongly has to be escapable without a serial cable, so the
     * button still counts here, and eight seconds of it clears the park and
     * throws the reference away (the reference is usually why we are here). */
    if (s_parked) {
        if (edge && pressed) {
            park_held = 0;
        }
        if (pressed) {
            if (++park_held == PARK_EXIT_TICKS) {
                park_held = 0;
                muse_imu_face_calibrate();
                park(false, "8 s hold");
            }
            return;
        }
        if (edge && !pressed) {
            park_held = 0;
        }
        return;
    }

    if (sleep_in && --sleep_in == 0) {
        set_asleep(true, muse_board->aux_button);
    }
    if (edge && pressed) {
        held = 0;
        hinted = false;
        /* Parked, the press is the table: it must not light the screen. The
         * hold counter below is left armed so power_off() stays reachable. */
        swallow = muse_state_asleep() && !s_parked;
        if (swallow) {
            set_asleep(false, muse_board->aux_button);
        } else if (sleep_in && !s_parked) {
            sleep_in = 0;
            swallow = true;
            toggle_phone_setup();
        }
        return;
    }
    if (pressed && !swallow) {
        if (!muse_board->power_off) {
            return;   /* no hold to power off: a press only sleeps */
        }
        held++;
        if (held == HINT_TICKS) {
            uint32_t v = UINT32_MAX;
            muse_state_caption(saved_caption, sizeof(saved_caption), &v);
            muse_state_set_caption("HOLD TO POWER OFF");
            hinted = true;
        } else if (held == LONG_TICKS) {
            swallow = true;
            power_off();
        }
        return;
    }
    if (edge && !pressed && !swallow) {
        if (hinted) {
            muse_state_set_caption("%s", saved_caption);   /* let go early: cancel */
        } else {
            sleep_in = DOUBLE_TICKS;
        }
    }
}

/* No touch: the aux button opens the menu and steps down it; any press wakes. */
static void menu_button(bool pressed, bool edge)
{
    if (!edge || !pressed || s_parked) {
        return;
    }
    if (muse_state_asleep()) {
        set_asleep(false, muse_board->aux_button);
    } else if (!s_talk_down) {
        muse_state_poke();
        muse_menu_key(MUSE_MENU_DOWN);
    }
}

static void aux_key(bool pressed, bool edge)
{
    if (muse_board->touch) {
        aux_button(pressed, edge);
    } else {
        menu_button(pressed, edge);
    }
}

/* Talk button: push-to-talk, or Select while the menu is open. Asleep, the
 * press wakes and is posted as a waking one: muse_voice records only if it's
 * still held once awake. */
static void talk_button(unsigned ev)
{
    /* Face down, the glass is being pressed by whatever the device is lying on.
     * Not waking for it is the point of the park; a take already running when it
     * was flipped over is closed here rather than left open. */
    if (s_parked) {
        if (s_talk_down) {
            post(MUSE_PTT_UP, false);
            s_talk_down = false;
        }
        return;
    }
    bool talk_down = s_talk_down;
    static bool swallow;
#if CONFIG_MUSE_WATCHER_CAMERA
    static TickType_t last_release;
    if ((ev & MUSE_BTN_TALK_PRESS) && !muse_state_asleep()
        && !muse_menu_is_open() && last_release
        && xTaskGetTickCount() - last_release <= pdMS_TO_TICKS(350)) {
        ESP_LOGI(TAG, "wheel double-click: camera preview/shutter");
        last_release = 0;
        watcher_camera_preview_toggle();
        swallow = true;
        return;
    }
#endif

    /* A quick tap can latch press and release in the same poll, and a release
     * can land just before the next press; keep them ordered. */
    bool released = ev & MUSE_BTN_TALK_RELEASE;
#if CONFIG_MUSE_WATCHER_CAMERA
    bool saw_release = released;
#endif
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(MUSE_PTT_UP, false);
        }
        talk_down = swallow = false;
        released = false;
    }
    if (!talk_down && !swallow && (ev & MUSE_BTN_TALK_PRESS)) {
        if (muse_link_talk_press()) {
            /* Confirmed a Muse app pairing (Link's setup button). */
            muse_state_poke();
            swallow = true;
        } else if (muse_state_asleep()) {
            /* Screen dark: the press only lights it, and the take starts on the
             * next press with the screen already up. A hand inside a bag finds
             * this panel constantly, and the first thing it used to do was open
             * a recording nobody asked for. */
            set_asleep(false, muse_board->talk_button);
            swallow = true;
        } else if (muse_menu_is_open()) {
            muse_state_poke();
            if (!muse_board->keyboard) muse_menu_key(MUSE_MENU_SELECT);
            swallow = true;
        } else {
            post(MUSE_PTT_DOWN, false);
            talk_down = true;
        }
    }
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(MUSE_PTT_UP, false);
        }
        talk_down = swallow = false;
    }
#if CONFIG_MUSE_WATCHER_CAMERA
    if (saw_release && !swallow) {
        last_release = xTaskGetTickCount();
    }
#endif
    s_talk_down = talk_down;
}

/* Keyboard menus don't repurpose Space/GO as Select. Navigation wakes the
 * screen without accidentally changing a setting; Enter can confirm pairing. */
static void keyboard_buttons(unsigned ev)
{
    const unsigned mask = MUSE_BTN_UP | MUSE_BTN_DOWN | MUSE_BTN_LEFT |
                          MUSE_BTN_RIGHT | MUSE_BTN_ENTER | MUSE_BTN_ESCAPE;
    if (!(ev & mask) || s_talk_down) return;
    if ((ev & MUSE_BTN_ENTER) && muse_link_talk_press()) {
        muse_state_poke();
        return;
    }
    if (muse_state_asleep()) {
        set_asleep(false, "keyboard");
        return;
    }
    muse_state_poke();
    if (ev & MUSE_BTN_ESCAPE) muse_menu_key(MUSE_MENU_BACK);
    else if (ev & MUSE_BTN_UP) muse_menu_key(MUSE_MENU_UP);
    else if (ev & MUSE_BTN_DOWN) muse_menu_key(MUSE_MENU_DOWN);
    else if (ev & MUSE_BTN_LEFT) muse_menu_key(MUSE_MENU_LEFT);
    else if (ev & MUSE_BTN_RIGHT) muse_menu_key(MUSE_MENU_RIGHT);
    else if (ev & MUSE_BTN_ENTER) muse_menu_key(MUSE_MENU_SELECT);
}

/* A pairing prompt wakes the screen and keeps it on; otherwise idle sleeps. */
static void check_sleep(void)
{
    /* Parked outranks every reason to wake: a pairing prompt can wait until the
     * device is turned over, and the screen coming back on face down would put
     * the touch panel under a table again. */
    if (s_parked) {
        return;
    }
    muse_ble_status_t ble;
    muse_ble_status(&ble);
    bool prompt = ble.passkey || muse_link_state() == MUSE_LINK_CONFIRM;
    if (prompt) {
        set_asleep(false, "pairing");
        return;
    }
    int after = muse_settings_sleep_s();
    float mode_t;
    if (after && !muse_state_asleep() && muse_state_mode(&mode_t) == MUSE_MODE_IDLE
        && muse_state_idle_secs() > after) {
        set_asleep(true, "auto-sleep");
    }
}

static void set_cpu_low(bool low)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = low ? CONFIG_XTAL_FREQ : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = low,
    };
    if (s_cpu_pd) {
        /* Light sleep stays configured: turning it off would free the
         * retention memory. The lock keeps the chip out of it instead, taken
         * before the full clock and let go only after the low one. */
        pm.light_sleep_enable = true;
        if (!low && !s_awake_held) {
            esp_pm_lock_acquire(s_awake);
            s_awake_held = true;
        }
    }
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management: %s", esp_err_to_name(err));
    }
    if (s_cpu_pd && low && s_awake_held && err == ESP_OK) {
        esp_pm_lock_release(s_awake);
        s_awake_held = false;
    }
#else
    (void)low;
#endif
}

void muse_input_power_init(void)
{
#if CONFIG_PM_ENABLE && CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP
    esp_err_t err = esp_sleep_cpu_retention_init();
    if (err == ESP_OK) {
        err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "muse_awake", &s_awake);
    }
    if (err == ESP_OK) {
        esp_pm_lock_acquire(s_awake);
        s_awake_held = true;
        const esp_pm_config_t pm = {
            .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .light_sleep_enable = true,
        };
        err = esp_pm_configure(&pm);
    }
    s_cpu_pd = err == ESP_OK;
    if (!s_cpu_pd) {
        if (s_awake) {
            if (s_awake_held) {
                esp_pm_lock_release(s_awake);
                s_awake_held = false;
            }
            esp_pm_lock_delete(s_awake);
            s_awake = NULL;
        }
        esp_sleep_cpu_retention_deinit();
        ESP_LOGW(TAG, "CPU stays powered in light sleep: no retention memory (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CPU powers down in light sleep: retention memory held");
    }
#endif
    muse_battery_note_cpu_pd(s_cpu_pd);
}

bool muse_input_cpu_pd(void)
{
    return s_cpu_pd;
}

/*
 * On battery with the screen dark and voice resting: stop the display and let
 * the CPU drop to the crystal clock and light-sleep between polls, or until a
 * button interrupts (wait_buttons). Returns true while the display is stopped.
 * With a USB host attached (">nap" on the bench) the CPU stays at full speed:
 * at the crystal clock a long line of serial output can stall the USB console
 * until the host reopens the port.
 */
static bool update_power(void)
{
    static bool paused;
    bool pause = muse_board->display_pause && muse_state_on_battery() && muse_state_asleep()
                 && muse_ui_dark() && muse_voice_resting();
    bool want_low = pause && (s_force_low || !muse_console_host());
    if (pause == paused && want_low == s_cpu_low) {
        return paused;
    }
    if (pause && !paused) {
        muse_board->display_pause(true);
    }
    if (want_low != s_cpu_low) {
        set_cpu_low(want_low);
    }
    if (!pause && paused) {
        muse_board->display_pause(false);
    }
    paused = pause;
    s_cpu_low = want_low;
    ESP_LOGI(TAG, "%s", s_cpu_low ? "low power: display paused"
                        : paused  ? "display paused (USB host: CPU at full speed)"
                                  : "full power");
    return paused;
}

/*
 * Low power for WIFI_NAP_MS: Wi-Fi off, since keeping it associated costs
 * more than the rest of the chip. It rejoins when the screen wakes or USB
 * power arrives; meanwhile nothing reaches the device over the network.
 * Not while a voice note recorded offline waits to go (for a while). Returns
 * true while napping.
 */
static bool update_wifi_nap(TickType_t now, bool paused)
{
    static TickType_t low_since;
    static bool napping;
    if (!s_cpu_low) {
        low_since = now;
    }
    if (!muse_state_asleep() && s_nap_now) {
        s_nap_now = false;
        muse_state_set_as_if_battery(false);
    }
    bool nap = paused && !muse_voice_notes_waiting()
               && (s_nap_now || (s_cpu_low && now - low_since >= pdMS_TO_TICKS(WIFI_NAP_MS)));
    if (nap != napping) {
        napping = nap;
        ESP_LOGI(TAG, "Wi-Fi %s", nap ? "napping" : "waking");
        muse_wifi_nap(nap);
    }
    return napping;
}

/* Independent POWER never goes through PTT, AUX's menu or its setup double
 * click. LONG is supplied by the board only after a validated hold. */
static bool s_power_down, s_power_swallow;

static void power_release(void)
{
    if (!s_power_down) return;
    s_power_down = false;
    if (!s_power_swallow) set_asleep(true, muse_board->power_button);
    s_power_swallow = false;
}

static void dedicated_buttons(unsigned ev)
{
    bool release = (ev & MUSE_BTN_POWER_RELEASE) != 0;
    /* Preserve release-before-new-press ordering for latched boards. */
    if (s_power_down && release) {
        power_release();
        release = false;
    }
    if (!s_power_down && (ev & MUSE_BTN_POWER_PRESS)) {
        s_power_down = true;
        /* Parked, a press must not light the screen - but the hold below still
         * reaches power_off(), which is the way out if the panel is ever parked
         * by mistake. */
        s_power_swallow = muse_state_asleep() && !s_parked;
        muse_state_poke();
        if (s_power_swallow) set_asleep(false, muse_board->power_button);
    }
    if (s_power_down && !s_power_swallow && (ev & MUSE_BTN_POWER_LONG)) {
        s_power_swallow = true;   /* no sleep on release after shutdown/failure */
        if (s_talk_down) talk_button(MUSE_BTN_TALK_RELEASE);
        power_off();
    }
    if (release) power_release();
    if (ev & MUSE_BTN_SPEAKER_PRESS) {
        muse_state_poke();
        if (muse_state_asleep() && !s_parked) {
            set_asleep(false, muse_board->speaker_button);   /* wake-only press */
        } else if (!muse_state_asleep() && muse_state_mode(NULL) != MUSE_MODE_OFF) {
            bool on = !muse_settings_speaker_on();
            muse_settings_set_speaker_on(on);
            if (muse_state_mode(NULL) == MUSE_MODE_IDLE) {
                muse_state_set_caption(on ? "SPEAKER ON" : "SPEAKER OFF");
            }
        }
    }
}

static void input_task(void *arg)
{
    (void)arg;
    bool aux_down = false;
    bool paused = false;
    TickType_t checked = xTaskGetTickCount() - pdMS_TO_TICKS(SLEEP_CHECK_MS);
    TickType_t powered = xTaskGetTickCount() - pdMS_TO_TICKS(POWER_MS);

    for (;;) {
        unsigned ev = muse_board->poll_buttons() | atomic_exchange(&s_touch_talk_events, 0);
        if (ev & (MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE)) {
            ESP_LOGI(TAG, "talk key:%s%s", ev & MUSE_BTN_TALK_PRESS ? " press" : "",
                     ev & MUSE_BTN_TALK_RELEASE ? " release" : "");
            talk_button(ev);
        }
        keyboard_buttons(ev);
        dedicated_buttons(ev);
        /* A latched key (the 1.75's PMU) can report press and release in the
         * same poll, and a release can land just before the next press; keep
         * them ordered, as talk_button does. */
        bool aux_press = ev & MUSE_BTN_AUX_PRESS;
        bool aux_release = ev & MUSE_BTN_AUX_RELEASE;
        bool aux_edge = false;
        if (aux_down && aux_release) {
            aux_down = false;
            aux_release = false;
            aux_edge = true;
            aux_key(false, true);
        }
        if (!aux_down && aux_press) {
            aux_down = aux_edge = true;
            aux_key(true, true);
            if (aux_release) {
                aux_down = false;
                aux_key(false, true);
            }
        }
        if (!aux_edge) {
            aux_key(aux_down, false);
        }

        if (s_power_off_requested) {
            s_power_off_requested = false;
            power_off();
        }

        TickType_t now = xTaskGetTickCount();
        int64_t now_ms = esp_timer_get_time() / 1000;   /* counts through light sleep */
        /* A hand on the glass is the other thing that wakes the screen, and the
         * only reader of that line lives here: see check_touch_line(). */
        check_touch_line();
        /* Picking the gadget up wakes the screen. STATUS1 is read in both
         * states: it latches until it is read, and a motion taken while the
         * screen was already on used to leave the bit set, after which the
         * engine stopped reporting anything. The value only acts while asleep. */
        if (now_ms - s_imu_checked_ms >= IMU_POLL_MS) {
            s_imu_checked_ms = now_ms;
            bool moved = muse_imu_moved();
            /* Which way up the panel is. Read here and not only in the voice
             * path, because this poll runs whether or not a reply is playing -
             * and it is the same 200 ms window as the motion read, so the two
             * gestures cost one transaction between them. */
            bool down = muse_imu_face_down();
            park(down, "imu");
            /* A lift wakes the screen, but not on the motion alone: the event
             * only arms a check, and WAKE_CONFIRM_MS later the posture decides.
             * A press from inside a bag and a wobble on a desk both arm it and
             * both fail it. While parked, none of it counts - waking for one
             * would put the touch panel back under the table for a moment,
             * which is the whole problem; turning it over clears the park and
             * wakes it through park() instead. */
            if (moved && muse_state_asleep() && !s_parked && !s_wake_pending) {
                s_wake_pending = true;
                s_wake_check_ms = now_ms + WAKE_CONFIRM_MS;
            }
            if (s_wake_pending && now_ms >= s_wake_check_ms) {
                s_wake_pending = false;
                if (muse_state_asleep() && !s_parked) {
                    const int need = s_wake_suppressed ? WAKE_ANGLE_SUPPRESSED_DEG
                                                      : CONFIG_MUSE_IMU_WAKE_ANGLE_DEG;
                    const int deg = muse_imu_posture_delta_deg();
                    if (muse_imu_wake_is_deliberate(need)) {
                        s_stat_motion++;
                        set_asleep(false, "lifted");
                        s_wake_hold_ms = now_ms;
                        s_wake_interacted = false;
                        ESP_LOGI(TAG, "lift accepted (%d deg, need %d)", deg, need);
                    } else {
                        s_stat_ignored++;
                        ESP_LOGI(TAG, "lift ignored (%d deg, need %d)", deg, need);
                    }
                }
            }
            /* Trace every movement the IMU can see. bump is the motion beyond
             * gravity; when it is high and STATUS1 still says nothing, the
             * engine is the one missing it, not the reader. */
            int bump = muse_imu_bump_mg();
            if (bump > IMU_BUMP_LOG_MG
                && now_ms - s_bump_logged_ms >= IMU_BUMP_LOG_QUIET_MS) {
                s_bump_logged_ms = now_ms;
                ESP_LOGW(TAG, "imu bump %d mg: status1 motion=%d, %s",
                         bump, (int)moved, muse_state_asleep() ? "asleep" : "awake");
            }
            if (moved && !muse_state_asleep()) {
                /* Not an error in itself: the poll reads STATUS1 whether the
                 * screen is on or off, and that read is what drops INT1. Worth
                 * a counter all the same - on battery it is the difference
                 * between "the engine saw the movement" and "the wake path lost
                 * it". */
                s_stat_motion_awake++;
            }
            /* The watchdog runs whether or not the engine is known to be
             * configured: a session whose init failed used to skip this for
             * good, and that is the one state it exists to repair. */
            if (now_ms - s_imu_verified_ms >= IMU_VERIFY_MS) {
                s_imu_verified_ms = now_ms;
                bool full = now_ms - s_imu_full_ms >= IMU_FULL_VERIFY_MS;
                if (full) {
                    s_imu_full_ms = now_ms;
                }
                muse_imu_check(full);   /* re-inits the chip when it drifted */
            }
            if (muse_state_asleep()) {
                if (now_ms - s_imu_rearmed_ms >= IMU_REARM_MS) {
                    s_imu_rearmed_ms = now_ms;
                    muse_imu_rearm();
                }
            } else {
                s_imu_rearmed_ms = now_ms;   /* the window counts from the last wake */
            }
        }
        /* Anything the user does clears the record: the point of the counter is
         * "nobody was there", not "the gesture fired". One real interaction also
         * puts the gate back to its configured angle. */
        if (ev) {
            s_wake_interacted = true;
            if (s_false_wakes || s_wake_suppressed) {
                s_false_wakes = 0;
                if (s_wake_suppressed) {
                    s_wake_suppressed = false;
                    ESP_LOGI(TAG, "lift-to-wake back to %d deg: the user is here",
                             CONFIG_MUSE_IMU_WAKE_ANGLE_DEG);
                }
            }
        }
        /* A lift nobody answered: the bag case, and the reason the screen used
         * to stay lit for minutes at a time in one. */
        if (s_wake_hold_ms && !muse_state_asleep()) {
            const int window = s_wake_suppressed ? WAKE_IDLE_RECHECK_SUPPRESSED_MS
                                                 : WAKE_IDLE_RECHECK_MS;
            if (now_ms - s_wake_hold_ms >= window) {
                s_wake_hold_ms = 0;
                if (!s_wake_interacted) {
                    if (++s_false_wakes >= FALSE_WAKE_LIMIT && !s_wake_suppressed) {
                        s_wake_suppressed = true;
                        ESP_LOGW(TAG, "lift-to-wake suppressed: %d idle wakes in a row, "
                                      "now needs %d deg and %d ms",
                                 s_false_wakes, WAKE_ANGLE_SUPPRESSED_DEG,
                                 WAKE_IDLE_RECHECK_SUPPRESSED_MS);
                    }
                    ESP_LOGI(TAG, "no interaction in %d ms: back to sleep (#%d)",
                             window, s_false_wakes);
                    set_asleep(true, "no interaction");
                }
            }
        }
        if (now - checked >= pdMS_TO_TICKS(SLEEP_CHECK_MS)) {
            checked = now;
            check_sleep();
        }

        if (now - powered >= pdMS_TO_TICKS(paused ? REST_POWER_MS : POWER_MS)) {
            powered = now;
            muse_power_t p = { .battery_pct = -1 };
            if (muse_board->read_power && muse_board->read_power(&p) == ESP_OK) {
                muse_state_set_power(&p);
                muse_battery_note_power(&p, muse_state_on_battery());
            }
        }

        paused = update_power();
        bool napping = update_wifi_nap(now, paused);
        muse_battery_note_state(muse_state_asleep(), s_cpu_low);
        /* Paused, anything but a button (a pairing prompt, an image) is
         * noticed within REST_WAIT_MS. Napping, nothing comes over the
         * network; USB power arriving is noticed within NAP_WAIT_MS. */
        if (paused && muse_board->wait_buttons) {
            int64_t before_ms = esp_timer_get_time() / 1000;
            TickType_t before_tick = xTaskGetTickCount();
            /* Every wake line this board has interrupts, so a 1 s rest wait
             * would only wake the CPU to look at nothing. A board whose wake
             * lines aren't reliable keeps the short wait (P1-4 in the code
             * review). */
            int wait_ms = napping || muse_board->wake_lines_reliable ? NAP_WAIT_MS
                                                                    : REST_WAIT_MS;
            if (s_wake_pending) {
                /* A lift is waiting on its posture check. Sleeping the full nap
                 * here would light the screen seconds after the hand stopped,
                 * which reads as "the gesture does not work": come back for the
                 * check instead. */
                const int64_t left = s_wake_check_ms - esp_timer_get_time() / 1000;
                wait_ms = left > 0 && left < wait_ms ? (int)left : 1;
            }
            muse_board->wait_buttons(wait_ms);
            /* Woken by a movement: STATUS1 latches until it is read, and the IMU
             * holds INT1 high until then - which means the line the next sleep
             * arms on is already satisfied, so a missed read costs every later
             * wake. Poll before going back to sleep instead of waiting for the
             * 200 ms window to come round. */
            s_imu_checked_ms = 0;
            /* A light sleep is also the moment the IMU's own configuration can
             * come back changed (a bus glitch while the chip is not listening),
             * so verify it at once rather than at the next 1 s tick. */
            s_imu_verified_ms = 0;
            s_stat_sleeps++;
            if (muse_gpio_wake_was_line()) {
                s_stat_wgpio++;
            } else if (muse_gpio_wake_was_timer()) {
                s_stat_wtimer++;
            } else {
                s_stat_wother++;
            }
            s_stat_slept_ms = esp_timer_get_time() / 1000 - before_ms;
            s_stat_tick_ms = (int64_t)(xTaskGetTickCount() - before_tick) * portTICK_PERIOD_MS;
            s_stat_int1 = gpio_get_level(MUSE_IMU_INT_GPIO);
            /* The same snapshot for the two lines that are the other candidates
             * behind "nothing woke it": the button's and the touch panel's.
             * muse_board.c read them before disarming, and the cause says
             * whether a line ended the sleep or the nap simply ran out. */
            s_stat_btn_level = muse_gpio_button_level();
            int touch = muse_gpio_wake_line_touch_index();
            s_stat_touch_level = touch < 0 ? -1 : muse_gpio_wake_line_seen(touch);
            s_stat_touch_armed = touch < 0 ? -1 : muse_gpio_wake_line_armed_level(touch);
            bool btn_down = s_stat_btn_level == 0;
            /* A wait that returns after ~no sleep never entered light sleep: the
             * wake level was already satisfied as it went in. The touch
             * controller does exactly that once when the panel is switched off -
             * a pulse of its own, with nobody touching the glass - and reading
             * that as a touch relit the screen the moment it went dark, once per
             * sleep, forever (last_slept_ms=0 in the counters). A hand that is
             * really on the glass is still there 50 ms later; a pulse is not, so
             * ask again before believing it. */
            bool rejected = s_stat_slept_ms < 80;
            if (rejected && touch >= 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                s_stat_touch_level = gpio_get_level(muse_gpio_wake_line_gpio(touch));
            }
            if (btn_down) {
                s_stat_wake_btn++;   /* still down: a press that ended the sleep */
            }
            /* The touch line is not a wake source on this board (the board file
             * says why), so a line wake here can only be the button or the
             * IMU's motion line, and both are already counted above. */
            check_touch_line();
            if (s_stat_slept_ms >= 100) {
                /* Only when the sleep was a real one: on battery this is a
                 * handful of lines a minute, not the 1.5 wakes/s the power
                 * counters report for shorter waits. */
                if (s_stat_int1) {
                    s_stat_line_high++;
                    ESP_LOGW(TAG, "imu line woke the cpu after %lld ms (tick %lld ms)",
                             (long long)s_stat_slept_ms, (long long)s_stat_tick_ms);
                } else if (s_stat_slept_ms >= 5000) {
                    ESP_LOGI(TAG, "slept %lld ms (tick %lld ms), line low",
                             (long long)s_stat_slept_ms, (long long)s_stat_tick_ms);
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(paused ? REST_POLL_MS : POLL_MS));
        }
    }
}

/* Reads the rest of a line into buf; false if it was too long (the rest is dropped). */
static bool read_line(char *buf, size_t cap)
{
    size_t len = 0;
    bool whole = true;
    uint8_t c;
    while (muse_console_getc(&c) && c != '\n') {
        if (c == '\r') {
            continue;
        }
        if (len < cap - 1) {
            buf[len++] = (char)c;
        } else {
            whole = false;
        }
    }
    buf[len] = '\0';
    return whole;
}

#if CONFIG_MUSE_HATCH
#define CHAT_OVER_SERIAL "true"

/*
 * "chat+=TEXT" adds a piece of a message and "chat=TEXT" adds the last piece
 * and sends it (TEXT escaped: \n \r \t \\). Every line is acknowledged, and the
 * host waits for that before the next: the driver drops what it has no room for.
 */
static char *s_chat;        /* the message so far */
static size_t s_chat_len;

static void chat_line(char *piece, bool last, bool whole)
{
    size_t n = muse_hatch_unescape(piece);
    if (!s_chat) {
        s_chat = heap_caps_malloc(CHAT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_chat_len = 0;
    }
    const char *err = !whole ? "LINE TOO LONG" : !s_chat ? "OUT OF MEMORY" : s_chat_len + n >= CHAT_MAX ? "TOO LONG" : NULL;
    if (err) {
        free(s_chat);
        s_chat = NULL;
        muse_hatch_console("error", err, NULL);
        return;
    }
    memcpy(s_chat + s_chat_len, piece, n);
    s_chat_len += n;
    s_chat[s_chat_len] = '\0';
    muse_hatch_console("ack", NULL, "\"bytes\":%u", (unsigned)s_chat_len);
    if (last) {
        muse_hatch_text_turn(s_chat);   /* frees it */
        s_chat = NULL;
    }
}

/* Drops a half-sent message and ends a typed turn. */
static void chat_cancel(void)
{
    free(s_chat);
    s_chat = NULL;
    muse_hatch_text_cancel();
}
#else
#define CHAT_OVER_SERIAL "false"   /* no PSRAM: replies come over Link, and only short ones */
#endif

/*
 * Bench: puts the face in a mode ("face=thinking") until the voice path or
 * another "face=" moves it on. "face=happy" goes back to idle with the happy
 * hop, as a finished turn does.
 */
static void set_face(const char *name)
{
    static const char *const modes[MUSE_MODE_COUNT] = {
        [MUSE_MODE_BOOT] = "boot",
        [MUSE_MODE_IDLE] = "idle",
        [MUSE_MODE_LISTENING] = "listening",
        [MUSE_MODE_THINKING] = "thinking",
        [MUSE_MODE_SPEAKING] = "speaking",
        [MUSE_MODE_ERROR] = "error",
        [MUSE_MODE_OFF] = "off",
    };
    muse_state_poke();
    bool happy = !strcmp(name, "happy");
    if (happy) {
        name = "idle";   /* where a finished turn hops to */
    }
    for (int m = 0; m < MUSE_MODE_COUNT; m++) {
        if (!strcmp(name, modes[m])) {
            muse_state_set_mode(MUSE_MODE_IDLE);   /* only idle leaves "off" */
            muse_state_set_mode((muse_mode_t)m);
            if (happy) {
                muse_state_make_happy();
            }
            return;
        }
    }
    printf("@face.error unknown face \"%s\": boot idle listening thinking speaking error off happy\n", name);
    fflush(stdout);
}

/*
 * Console-only commands; false for setup commands. Their buffers are taken
 * per command: without PSRAM, static ones would hold internal RAM for good.
 */
static bool console_command(char *line, bool whole)
{
    if (whole && s_console_hook && s_console_hook(line)) {
        return true;
    }
    if (!strcmp(line, "status")) {
        size_t cap = 1024;   /* long SSID, host and VM names escaped: past 512 */
        char *json = heap_caps_malloc(cap, MUSE_BIG_CAPS);
        if (json) {
            muse_ble_status_json(json, cap);
            printf("@status {\"board\":\"%s\",\"chat\":%s,\"device\":%s}\n", muse_board->name,
                   CHAT_OVER_SERIAL, json);
            fflush(stdout);
            free(json);
        }
        return true;
    }
    bool reset = !strcmp(line, "power.reset");
    if (reset || !strcmp(line, "power")) {
        if (reset) {
            muse_battery_reset();
        }
        const char *saved = muse_battery_saved_json();
        if (saved) {
            printf("@power.saved %s\n", saved);
        }
        size_t cap = 2048;   /* two dozen power locks */
        char *json = heap_caps_malloc(cap, MUSE_BIG_CAPS);
        if (json) {
            muse_battery_json(json, cap);
            printf("@power %s\n", json);
            free(json);
        }
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "ota")) {
        /* The Update page needs a finger; this is the same call from a cable. */
        bool started = muse_link_ota_check();
        char buf[128];
        muse_link_ota_status(buf, sizeof(buf));
        printf("@ota started=%d %s\n", (int)started, buf);
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "time") || !strncmp(line, "time=", 5)) {
        if (line[4] == '=') {
            /* ">time=YYYY-MM-DD HH:MM:SS" (UTC) for a bench with no network.
             * Normal operation never needs it: SNTP sets both clocks as soon as
             * Wi-Fi associates. */
            int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
            if (sscanf(line + 5, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6 &&
                sscanf(line + 5, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) {
                printf("@time bad format, want time=YYYY-MM-DD HH:MM:SS (UTC)\n");
            } else {
                esp_err_t err = muse_rtc_set_utc_fields(y, mo, d, h, mi, s);
                char stamp[48] = "unknown";
                (void)muse_rtc_stamp_str(stamp, sizeof(stamp));
                printf("@time set %s: %s\n", esp_err_to_name(err), stamp);
            }
        } else {
            char stamp[48] = "unknown";
            bool ok = muse_rtc_stamp_str(stamp, sizeof(stamp));
            printf("@time %s chip=%d\n", ok ? stamp : "unknown", (int)muse_rtc_present());
        }
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "batt")) {
        if (muse_board->power_debug) {
            muse_board->power_debug();
        } else {
            ESP_LOGI(TAG, "this board has no fuel gauge to dump");
        }
        return true;
    }
    if (!strcmp(line, "imu")) {
        muse_imu_log_diag("console");
        /* Why a movement did or did not wake the screen on battery, where the
         * USB cable that hides the state cannot be attached: sleeps is how often
         * the task slept; line_high and btn how often each line was asserted
         * when it came back (the button is active low, the IMU's line active
         * high); motion how many motion events the poll turned into a wake;
         * cause what the last light-sleep exit reported (0 = nothing slept, the
         * wait was notified from inside - a USB host or a wake request;
         * 4 = timer, the nap ran out; 7 = GPIO, a line did it). */
        printf("@imu.wakes sleeps=%lu wgpio=%lu wtimer=%lu wother=%lu line_high=%lu btn=%lu "
               "motion=%lu motion_awake=%lu ignored=%lu parked=%d false_wakes=%d "
               "suppressed=%d last_slept_ms=%lld "
               "last_tick_ms=%lld int1=%d btn_level=%d touch_level=%d touch_armed=%d cause=%d "
               "btn_irqs=%u\n",
               (unsigned long)s_stat_sleeps, (unsigned long)s_stat_wgpio,
               (unsigned long)s_stat_wtimer, (unsigned long)s_stat_wother,
               (unsigned long)s_stat_line_high, (unsigned long)s_stat_wake_btn,
               (unsigned long)s_stat_motion, (unsigned long)s_stat_motion_awake,
               (unsigned long)s_stat_ignored,
               (int)s_parked, s_false_wakes, (int)s_wake_suppressed,
               (long long)s_stat_slept_ms,
               (long long)s_stat_tick_ms, s_stat_int1, s_stat_btn_level, s_stat_touch_level,
               s_stat_touch_armed,
               muse_gpio_wake_cause(), muse_gpio_button_irqs());
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "imu.check")) {
        muse_imu_check(true);
        muse_imu_log_diag("console check");
        return true;
    }
    if (!strcmp(line, "imu.rearm")) {
        muse_imu_rearm();
        muse_imu_log_diag("console rearm");
        return true;
    }
    if (!strcmp(line, "tp")) {
        /* The touch line, the level it is armed on and what it has done so far:
         * the one place that answers "did a touch even reach the chip while the
         * screen was dark on battery". */
        int ti = muse_gpio_wake_line_touch_index();
        printf("@tp lines=%d touch=%d line=%d armed=%d live=%d seen=%d irqs=%u "
               "touch_wakes=%lu btn_irqs=%u btn_level=%d cause=%d touch_level=%d\n",
               muse_gpio_wake_line_count(), ti,
               ti < 0 ? -1 : (int)muse_gpio_wake_line_gpio(ti),
               ti < 0 ? -1 : muse_gpio_wake_line_armed(ti),
               ti < 0 ? -1 : muse_gpio_wake_line_live(ti),
               ti < 0 ? -1 : muse_gpio_wake_line_seen(ti),
               ti < 0 ? 0u : muse_gpio_wake_line_irqs(ti),
               (unsigned long)s_stat_touch_wakes, muse_gpio_button_irqs(),
               muse_gpio_button_level(), muse_gpio_wake_cause(), s_stat_touch_level);
        for (int i = 0; i < muse_gpio_wake_line_count(); i++) {
            /* Both lines, so the IMU's is read next to the touch's. */
            printf("@tp.line[%d] gpio=%d armed=%d live=%d seen=%d irqs=%u\n", i,
                   (int)muse_gpio_wake_line_gpio(i), muse_gpio_wake_line_armed(i),
                   muse_gpio_wake_line_live(i), muse_gpio_wake_line_seen(i),
                   muse_gpio_wake_line_irqs(i));
        }
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "tpwake=", 7)) {
        /* 0: not armed at all, 1: low level (the CST816S's INT is active low -
         * the driver configures a falling edge), 2: high level. One build, both
         * polarities: a wrong one costs a battery cycle to find out, and the
         * chip refuses to stay in light sleep while the condition is already
         * satisfied. */
        int mode = atoi(line + 7);
        int ti = muse_gpio_wake_line_touch_index();
        if (ti < 0) {
            printf("@tpwake no touch wake line registered\n");
        } else {
            esp_err_t err = muse_gpio_wake_line_set_level(muse_gpio_wake_line_gpio(ti),
                                                          mode == 0 ? -1 : (mode == 2 ? 1 : 0));
            printf("@tpwake mode=%d armed=%d live=%d (%s)\n", mode,
                   muse_gpio_wake_line_armed(ti), muse_gpio_wake_line_live(ti),
                   esp_err_to_name(err));
        }
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "i2c")) {
        /* The shared bus, its pad levels and what it has been doing. A bus that
         * went quiet during a battery session takes the touch controller down
         * with it, and that is what "the screen is slow" turns out to be. */
        int sda, scl;
        muse_i2c_levels(&sda, &scl);
        printf("@i2c registered=%d sda=%d scl=%d fails=%u streak=%u resets=%u\n",
               (int)muse_i2c_registered(), sda, scl, muse_i2c_fails(),
               muse_i2c_streak(), muse_i2c_recovers());
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "i2c.reset")) {
        /* Manual run of the same recovery the failure counter triggers: IDF's
         * bus reset (nine clocks, FSM reset, pads re-routed) plus the pull-ups.
         * The levels on both sides of it say which of the two it was. */
        int sda_before, scl_before, sda_after, scl_after;
        muse_i2c_levels(&sda_before, &scl_before);
        esp_err_t err = muse_i2c_recover();
        muse_i2c_levels(&sda_after, &scl_after);
        printf("@i2c.reset %s: sda %d->%d scl %d->%d fails=%u resets=%u\n",
               esp_err_to_name(err), sda_before, sda_after, scl_before, scl_after,
               muse_i2c_fails(), muse_i2c_recovers());
        fflush(stdout);
        return true;
    }
    /* Network commands live in main (sdk_token_console.c): the address is NVS
     * state, which this component cannot reach. */
    if (!strncmp(line, "pmlow=", 6)) {
        /* Bench aid: on battery the chip light-sleeps once the screen is dark,
         * but a USB host keeps it out of light sleep - so the one state that
         * misbehaves is the one a serial cable hides. This forces it back on,
         * battery pretence included, since the pause itself needs it. */
        s_force_low = line[6] == '1';
        muse_state_set_as_if_battery(s_force_low);
        printf("@pmlow forced=%d (battery pretence %s)\n", (int)s_force_low,
               s_force_low ? "on" : "off");
        fflush(stdout);
        return true;
    }
    if (!strncmp(line, "imuwatch=", 9)) {
        /* Bench aid for pick-up-to-wake: prints each motion event the chip
         * latches and the acceleration that came with it, so the threshold in
         * CONFIG_MUSE_IMU_WAKE_MG can be settled without a battery run. */
        int secs = atoi(line + 9);
        if (secs <= 0 || secs > 60) {
            secs = 15;
        }
        printf("@imuwatch %ds, move the gadget now\n", secs);
        fflush(stdout);
        int events = 0;
        int64_t until = esp_timer_get_time() / 1000 + (int64_t)secs * 1000;
        while (esp_timer_get_time() / 1000 < until) {
            if (muse_imu_moved()) {
                events++;
                printf("@imuwatch motion=%d bump=%d mg\n", events, muse_imu_bump_mg());
                fflush(stdout);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        printf("@imuwatch done events=%d\n", events);
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "nap")) {
        muse_state_set_as_if_battery(true);
        set_asleep(true, "serial");
        s_nap_now = true;
        return true;
    }
#if LV_USE_SNAPSHOT
    if (!strcmp(line, "ui.demo=1") || !strcmp(line, "ui.demo=0")) {
        muse_ui_bench_demo(!strcmp(line, "ui.demo=1"));
        return true;
    }
    if (!strcmp(line, "ui=settings") || !strcmp(line, "ui=face")) {
        muse_ui_bench_page(!strcmp(line, "ui=settings"));
        return true;
    }
#endif
    if (!strncmp(line, "face=", 5)) {
        set_face(line + 5);
        return true;
    }
    if (strncmp(line, "chat", 4) != 0) {
        return false;
    }
#if CONFIG_MUSE_HATCH
    if (!strcmp(line, "chat.cancel")) {
        chat_cancel();
        return true;
    }
    bool last = !strncmp(line, "chat=", 5);
    if (last || !strncmp(line, "chat+=", 6)) {
        chat_line(line + (last ? 5 : 6), last, whole);
        return true;
    }
    return false;
#else
    (void)whole;
    muse_hatch_console("error", "THIS BOARD CAN'T CHAT OVER SERIAL", NULL);
    return true;
#endif
}

/*
 * Bench testing over the USB cable: 'd' / 'u' act as the talk button going
 * down / up, so the voice path can be driven without a finger on the button;
 * 'm' plays a built-in MP3 through the reply decoder; 'a' / 's' press the
 * menu's Down / Select; 'p' sends a screenshot; 'z' / 'w' sleep and wake.
 * A line starting with '>' is a setup command, the same "key=value" text as
 * the BLE CMD characteristic, or one of the console's own: "status" prints
 * the device's state, "power" the battery meter (muse_battery.h) and
 * "power.reset" starts it over, "nap" sleeps and leaves Wi-Fi at once (as
 * two minutes asleep on battery would; 'w' rejoins), "imu" dumps the QMI8658A's
 * tap registers (plus "imu.check" and "imu.rearm" to force the repair paths),
 * "batt" dumps the fuel gauge's raw registers, "ota" starts the same GitHub
 * update check the settings page's Update button does, "face=" shows a face
 * (see set_face), and "chat=" sends a typed message to Hatch (see chat_line
 * and tools/muse/chat.py).
 */
static void serial_task(void *arg)
{
    if (muse_console_install(SERIAL_RX) != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        uint8_t c;
        if (!muse_console_getc(&c)) {
            continue;
        }
        if (c == 'm') {
            muse_voice_request_mp3test();
        } else if (c == 'a' || c == 's') {
            muse_state_poke();
            muse_menu_key(c == 'a' ? MUSE_MENU_DOWN : MUSE_MENU_SELECT);
        } else if (c == 'p') {
            muse_ui_request_snapshot();
        } else if (c == 'z' || c == 'w') {
            set_asleep(c == 'z', "serial");
        } else if (c == '>') {
            char *line = heap_caps_malloc(SERIAL_LINE, MUSE_BIG_CAPS);
            char none[1];   /* no room: the line is read and dropped */
            bool whole = read_line(line ? line : none, line ? SERIAL_LINE : sizeof(none));
            if (line && !console_command(line, whole)) {
                muse_ble_command(line);
            }
            free(line);
        } else if (c == 'd' || c == 'u') {
            muse_state_poke();
            post(c == 'd' ? MUSE_PTT_DOWN : MUSE_PTT_UP, false);
        }
    }
}

void muse_input_set_console_hook(bool (*hook)(const char *line))
{
    s_console_hook = hook;
}

esp_err_t muse_input_start(QueueHandle_t queue)
{
    s_queue = queue;
    if (xTaskCreate(input_task, "muse_input", 4096, NULL, 6, &s_input) != pdPASS) {
        return ESP_FAIL;
    }
    /* Bench-test and setup console; the input still works if it can't start. */
    xTaskCreate(serial_task, "muse_serial", 3584, NULL, 5, NULL);
    return ESP_OK;
}

void muse_input_request_power_off(void)
{
    s_power_off_requested = true;
}
