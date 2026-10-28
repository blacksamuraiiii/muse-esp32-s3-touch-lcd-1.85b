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
#include "muse_interact_ui.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
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
#include "muse_rtc.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_menu.h"
#include "muse_pet.h"
#include "muse_pet_text.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_telemetry.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif
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
#define WIFI_NAP_MS (10 * 60 * 1000)  /* low power this long: Wi-Fi off until it ends */
#define DOUBLE_TICKS 35        /* 350 ms: a second aux press within this toggles phone setup */

#define GOODBYE_MS 1500        /* let the goodbye animation play */
#define HINT_TICKS 60          /* 0.6 s: warn that holding powers off */
#define LONG_TICKS 150         /* 1.5 s: power off */
#define SLEEP_CHECK_MS 100
#define SERIAL_RX 1024         /* the driver drops what doesn't fit, so a console line must */
#define SERIAL_LINE 1024
#define CHAT_MAX (192 * 1024)  /* a typed message, assembled from "chat+=" lines */

static QueueHandle_t s_queue;
static bool (*s_console_hook)(const char *line);
static TaskHandle_t s_input;
static bool s_talk_down;
static bool s_cpu_low;      /* display stopped and the CPU allowed to sleep */
static volatile bool s_power_off_requested;
static volatile bool s_bench_hold;

void muse_input_bench_hold(bool on) { s_bench_hold = on; }
static bool s_cpu_pd;       /* muse_input_power_init() holds the CPU's retention memory */
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_awake;   /* with s_cpu_pd: held whenever !s_cpu_low */
static bool s_awake_held;
#endif
static volatile bool s_nap_now;   /* ">nap": asleep, as if on battery, nap without waiting WIFI_NAP_MS */
static bool s_force_low;          /* ">pmlow=1": light-sleep even with a USB host attached */

/* Wake forensics, printed by "tp": they separate "the line never rose" from
 * "the line rose and nobody read it" when a button wake fails on battery. */
static uint32_t s_stat_sleeps;      /* wait_buttons() returns */
static uint32_t s_stat_wgpio;       /* ... ended by a line (cause GPIO) */
static uint32_t s_stat_wtimer;      /* ... ended by the nap running out */
static uint32_t s_stat_wother;      /* ... ended any other way */
static uint32_t s_stat_wake_btn;    /* ... of which the button's line was down */
static uint32_t s_stat_touch_wakes; /* wakes this task made from the touch line */
static int64_t s_stat_slept_ms = 1000;  /* before the first wait: not a rejected sleep */
static int64_t s_stat_tick_ms;      /* ... and what the FreeRTOS tick made of it */
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
    if (s_bench_hold) {
        ESP_LOGW(TAG, "power-off blocked: bench sweep in progress");
        muse_state_set_mode(MUSE_MODE_IDLE);
        return;
    }
    ESP_LOGI(TAG, "shutting down");
    muse_state_set_asleep(false);
    update_power();
    muse_state_set_progress(0);
    muse_state_set_level(0);
    muse_state_set_mode(MUSE_MODE_OFF);
    muse_state_set_caption("GOODBYE!");
    /* The last line before the lights go out: without it a night's recording
     * ends in mid-air and "it stopped at 23:40" has no cause attached. */
#if CONFIG_MUSE_TELEMETRY
    muse_telemetry_event("shutdown", muse_rtc_wake_armed() ? "alarm armed" : "no alarm");
#endif
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
        if (s_input) {
            xTaskNotifyGive(s_input);   /* out of wait_buttons() */
        }
        if (!asleep && !muse_wifi_connected()) {
            muse_wifi_apply();   /* the screen says reconnecting: don't sit out the backoff */
        }
    }
}

/* --------------------------------------------------------------- the alarm */
/*
 * A daily alarm, and the RTC chip owns the trigger: muse_rtc_arm_wakeup_in() is
 * armed with the distance to the next HH:MM, and the chip raises INT whether
 * the device is in deep sleep (the board folds GPIO6 into the ext1 mask, then
 * this boot rings), in light sleep (the GPIO6 wake line ends the nap), or
 * awake (alarm_poll() compares the clock). One ring, then the next day's arm:
 * re-arming lives only here, which is what keeps a fired alarm from ringing
 * twice.
 */
#define ALARM_RING_US (60LL * 1000 * 1000)   /* 60 s, then it gives up */
#define ALARM_CHIRP_US (2LL * 1000 * 1000)   /* one chirp every 2 s */
#define ALARM_TOUCH_GRACE_US (6LL * 1000 * 1000)   /* panel settles: see input_task */
#define ALARM_QUIET_US (2LL * 1000 * 1000)         /* after a stop: the same press */
#define ALARM_CHECK_US (500LL * 1000)              /* how often the chip's AF is read */
/* Press edges that stop a ring: everything a finger can do on this class of
 * board (the menu keys included, for boards that have them). */
#define ALARM_STOP_BITS (MUSE_BTN_TALK_PRESS | MUSE_BTN_AUX_PRESS | MUSE_BTN_UP | MUSE_BTN_DOWN | \
                         MUSE_BTN_LEFT | MUSE_BTN_RIGHT | MUSE_BTN_ENTER | MUSE_BTN_ESCAPE | \
                         MUSE_BTN_POWER_PRESS | MUSE_BTN_SPEAKER_PRESS)

static bool s_alarm_ringing;
static int64_t s_alarm_ring_us;
static int64_t s_alarm_chirp_us;
static int64_t s_alarm_quiet_us;   /* talk edges ignored until this moment */
static int64_t s_alarm_check_us;   /* last time the chip's flag was read */
static time_t s_alarm_target;   /* local epoch the current arm is for; 0 unknown */

/* Repeat masks name days the same way tm_wday and the chip's weekday counter
 * do: bit0 Sunday .. bit6 Saturday. 0 is a once-only alarm, 0x7F every day. */
#define ALARM_DAYS_DAILY 0x7F

static const char *const WEEKDAY_NAMES[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static void alarm_arm_next(void)
{
    if (!muse_rtc_present()) {
        return;
    }
    time_t now;
    if (!muse_rtc_now(&now)) {
        ESP_LOGW(TAG, "alarm: no time to count from yet");
        return;
    }
    const int h = muse_settings_alarm_hour();
    const int m = muse_settings_alarm_min();
    const uint8_t days = (uint8_t)muse_settings_alarm_days();
    /* Once and every-day need no weekday test; anything else has to land on a
     * day the mask names, so walk forward at most a week
     * (docs/20261023-待办.md §2.1). mktime normalises the date and fills
     * tm_wday, so the search and the weekday agree by construction. */
    const bool filter = days != 0 && days != ALARM_DAYS_DAILY;
    struct tm at;
    localtime_r(&now, &at);
    time_t target = 0;
    for (int add = 0; add <= 7; add++) {
        struct tm cand = at;
        cand.tm_mday += add;
        cand.tm_hour = h;
        cand.tm_min = m;
        cand.tm_sec = 0;
        cand.tm_isdst = -1;
        const time_t t = mktime(&cand);
        if (t <= now + 1) {
            continue;   /* today's time is already gone */
        }
        if (filter && !(days & (1u << cand.tm_wday))) {
            continue;
        }
        target = t;
        break;
    }
    if (target == 0) {
        s_alarm_target = 0;
        ESP_LOGW(TAG, "alarm %02d:%02d: no day of the mask within a week", h, m);
        return;
    }
    long secs = (long)(target - now);
    if (secs < 2) {
        secs = 2;
    }
    /* A filtered alarm also compares the weekday in the chip, so a target days
     * away arms exactly and the device stays asleep until that day. The same
     * call covers the corner where tomorrow is a full 86400 s away, which the
     * daily form refuses. */
    const bool weekly = filter || secs > 86399;
    esp_err_t err = weekly ? muse_rtc_arm_wakeup_weekly((uint32_t)secs)
                           : muse_rtc_arm_wakeup_in((uint32_t)secs);
    if (err == ESP_OK) {
        s_alarm_target = target;
        struct tm tt;
        localtime_r(&target, &tt);
        ESP_LOGI(TAG, "alarm %02d:%02d armed in %ld s (%s)", h, m, secs,
                 days == 0 ? "once" : filter ? WEEKDAY_NAMES[tt.tm_wday] : "every day");
#if CONFIG_MUSE_TELEMETRY
        char detail[64];
        snprintf(detail, sizeof(detail), "%02d:%02d in %lds mask=0x%02x %s", h, m, secs,
                 (unsigned)days, WEEKDAY_NAMES[tt.tm_wday]);
        muse_telemetry_event("alarm.arm", detail);
#endif
    } else {
        s_alarm_target = 0;
        ESP_LOGW(TAG, "alarm %02d:%02d not armed: %s", h, m, esp_err_to_name(err));
    }
}

/* Settings changed, or boot: on arms the next occurrence, off leaves no wake
 * source at all - and clears AF, or a stale flag would hold INT low. */
void muse_input_alarm_sync(void)
{
    if (s_alarm_ringing) {
        return;   /* the ring's own stop re-arms */
    }
    if (muse_settings_alarm_on()) {
        alarm_arm_next();
    } else {
        s_alarm_target = 0;
        (void)muse_rtc_disarm();
    }
}

static void alarm_start(const char *why)
{
    if (s_alarm_ringing) {
        return;
    }
    s_alarm_ringing = true;
    s_alarm_ring_us = esp_timer_get_time();
    s_alarm_chirp_us = s_alarm_ring_us;
    ESP_LOGI(TAG, "alarm ringing (%s)", why);
#if CONFIG_MUSE_TELEMETRY
    muse_telemetry_event("alarm.fire", why);
#endif
    set_asleep(false, "alarm");
    muse_state_poke();
    muse_state_set_caption("ALARM %02d:%02d\nPRESS TO STOP", muse_settings_alarm_hour(),
                           muse_settings_alarm_min());
}

/* Any key, or the minute is up. §3.5: re-set for the same time tomorrow. */
static void alarm_stop(const char *why)
{
    if (!s_alarm_ringing) {
        return;
    }
    s_alarm_ringing = false;
    s_alarm_quiet_us = esp_timer_get_time() + ALARM_QUIET_US;
    ESP_LOGI(TAG, "alarm stopped (%s)", why);
#if CONFIG_MUSE_TELEMETRY
    muse_telemetry_event("alarm.stop", why);
#endif
    muse_state_set_caption("ALARM OFF");
    if (muse_settings_alarm_days() == 0) {
        /* A once-only alarm is exactly that: the ring is the whole of it, so
         * turn it off instead of arming the same time again
         * (docs/20261023-待办.md §2.1). Turning it off notifies, and the sync
         * that runs from the listener disarms the chip. */
        s_alarm_target = 0;
        muse_settings_set_alarm_on(false);
        ESP_LOGI(TAG, "once alarm finished: turned off");
    } else {
        alarm_arm_next();
    }
}

bool muse_input_alarm_ringing(void)
{
    return s_alarm_ringing;
}

/* Every poll: start the ring when the chip's flag says it fired or the clock
 * reached the arm; chirp on a timer while ringing; give up after a minute. */
static void alarm_poll(void)
{
    if (!s_alarm_ringing) {
        if (!muse_settings_alarm_on()) {
            return;
        }
        /* The flag lives in the chip, so reading it costs an I2C transaction:
         * twice a second is plenty against a minute-resolution alarm, and the
         * input task runs this every 10 ms. */
        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_alarm_check_us < (int64_t)ALARM_CHECK_US) {
            return;
        }
        s_alarm_check_us = now_us;
        const bool chip = muse_rtc_alarm_pending();
        bool clock = false;
        if (!chip && s_alarm_target) {
            time_t now;
            clock = muse_rtc_now(&now) && now >= s_alarm_target;
        }
        if (chip || clock) {
            alarm_start(chip ? "rtc" : "clock");
        }
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_alarm_ring_us >= ALARM_RING_US) {
        alarm_stop("60 s");
        return;
    }
    if (now >= s_alarm_chirp_us) {
        muse_voice_request_chirp();
        muse_state_poke();   /* and auto-sleep does not walk over the ring */
        s_alarm_chirp_us = now + ALARM_CHIRP_US;
    }
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
    static char saved_caption[64];

    if (sleep_in && --sleep_in == 0) {
        set_asleep(true, muse_board->aux_button);
    }
    if (edge && pressed) {
        held = 0;
        hinted = false;
        /* Asleep, the press only lights the screen. */
        swallow = muse_state_asleep();
        if (swallow) {
            set_asleep(false, muse_board->aux_button);
        } else if (sleep_in) {
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
    if (!edge || !pressed) {
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
    muse_ble_status_t ble;
    muse_ble_status(&ble);
    bool prompt = ble.passkey || muse_link_state() == MUSE_LINK_CONFIRM;
    if (prompt) {
        set_asleep(false, "pairing");
        return;
    }
    int after = muse_settings_sleep_s();
    float mode_t;
    if (after && !muse_state_asleep() && !s_alarm_ringing
        && muse_state_mode(&mode_t) == MUSE_MODE_IDLE
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
    if (low && err == ESP_OK) {
        /* 1.5b: entering light sleep with the modem fully awake (PS_NONE)
         * leaves the radio up while the CPU sleeps. REST lets it sleep
         * between beacons. The voice task re-applies its own level
         * (FULL while talking, DOZE/REST while idle) on its next pass,
         * and the 10-minute nap disconnect path is untouched. */
        muse_wifi_power(MUSE_WIFI_REST);
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
        s_power_swallow = muse_state_asleep();
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
        if (muse_state_asleep()) {
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
    /* An alarm that fired while the chip was in deep sleep woke this boot, and
     * its flag is still set - nothing has cleared it. Ring first; otherwise arm
     * the next occurrence (or leave no wake source at all). */
    ESP_LOGI(TAG, "boot: reset=%d wake=0x%" PRIx32 " alarm_pending=%d",
             (int)esp_reset_reason(), (uint32_t)esp_sleep_get_wakeup_causes(),
             (int)muse_rtc_alarm_pending());
#if CONFIG_MUSE_TELEMETRY
    {
        /* The line a night's recording is read against: what woke this boot. */
        char why[64];
        snprintf(why, sizeof(why), "reset=%d wake=0x%lx pending=%d", (int)esp_reset_reason(),
                 (unsigned long)esp_sleep_get_wakeup_causes(), (int)muse_rtc_alarm_pending());
        muse_telemetry_event("boot", why);
    }
#endif
    if (muse_settings_alarm_on() && muse_rtc_alarm_pending()) {
        alarm_start("boot");
    } else {
        muse_input_alarm_sync();
    }
    bool aux_down = false;
    bool paused = false;
    TickType_t checked = xTaskGetTickCount() - pdMS_TO_TICKS(SLEEP_CHECK_MS);
    TickType_t powered = xTaskGetTickCount() - pdMS_TO_TICKS(POWER_MS);

    for (;;) {
        unsigned ev = muse_board->poll_buttons();
        unsigned touch = atomic_exchange(&s_touch_talk_events, 0);
        const int64_t now_us = esp_timer_get_time();
        if (s_alarm_ringing) {
            /* Waking the display makes the CST816S report one phantom press
             * (its INT pulses while the panel is still coming back and the
             * adapter turns the hold into a talk edge). A talk press therefore
             * only counts once the panel has settled; the bezel button counts
             * at once. Press edges only: the release of a press taken before
             * the ring started used to end it. §3.5: any key stops the ring. */
            const bool grace = now_us - s_alarm_ring_us < ALARM_TOUCH_GRACE_US;
            const bool talk = (ev & MUSE_BTN_TALK_PRESS) || (touch & MUSE_BTN_TALK_PRESS);
            /* The live hardware read is the same talk button whose pulse the
             * panel's wake-up produces, so it waits out the grace window as
             * well. Without that a ring ended about four seconds in on the
             * bench: the phantom pulse arrived as a held button, the edge test
             * never saw it as an edge, and the "bezel counts at once" path took
             * it anyway (measured 2026-10-23, 85B). */
            const bool held = muse_board->talk_down && muse_board->talk_down();
            const bool bezel = (ev & (ALARM_STOP_BITS & ~MUSE_BTN_TALK_PRESS)) || (held && !grace);
            if (bezel || (talk && !grace)) {
                ESP_LOGI(TAG, "alarm stopped by key (ev=0x%x touch=0x%x grace=%d)", ev, touch,
                         (int)grace);
                alarm_stop("key");
            }
        }
        if (s_alarm_ringing || now_us < s_alarm_quiet_us) {
            /* The press that stops the ring does nothing else, and while the
             * alarm owns the speaker a long press on the glass must not start
             * a conversation either. Held for a moment past the stop too, so
             * the release of that same press cannot leak into the voice
             * pipeline. */
            ev &= ~(MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE);
            touch = 0;
        }
        ev |= touch;
#if CONFIG_MUSE_VIDEO
        /* Any press leaves the video view (its release then does nothing). */
        const unsigned presses = MUSE_BTN_TALK_PRESS | MUSE_BTN_AUX_PRESS | MUSE_BTN_POWER_PRESS |
                                 MUSE_BTN_SPEAKER_PRESS | MUSE_BTN_UP | MUSE_BTN_DOWN | MUSE_BTN_LEFT |
                                 MUSE_BTN_RIGHT | MUSE_BTN_ENTER | MUSE_BTN_ESCAPE;
        if ((ev & presses) && muse_video_is_open()) {
            muse_video_close();
            ev &= ~(presses | MUSE_BTN_POWER_LONG);
        }
#endif
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
        /* A hand on the glass is the other thing that wakes the screen, and the
         * only reader of that line lives here: see check_touch_line(). Boards
         * that register no touch wake line - this one does not - pay a cheap
         * index lookup for it. */
        check_touch_line();
        alarm_poll();
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
            muse_board->wait_buttons(wait_ms);
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
            /* The same snapshot for the lines behind "nothing woke it": the
             * button's and, on a board that registers one, the touch panel's.
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
             * says why), so a line wake here can only be the button, and that
             * is already counted above. */
            check_touch_line();
            if (s_stat_slept_ms >= 5000) {
                ESP_LOGI(TAG, "slept %lld ms (tick %lld ms), line low",
                         (long long)s_stat_slept_ms, (long long)s_stat_tick_ms);
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

#if CONFIG_MUSE_BUILD_TEST
/* 1.5b: the mutating pet.* console commands are Test-gated like pet.reset,
 * so these helpers are too (otherwise a Release build warns about them
 * being unused). pet_console_snapshot() below stays always-compiled: the
 * read-only pet.status command uses it. */
static const char *pet_result_name(muse_pet_result_t r)
{
    switch (r) {
    case PET_OK: return "OK";
    case PET_COIN: return "COIN";
    case PET_CAP: return "CAP";
    case PET_POOR: return "POOR";
    case PET_FORTUNE_NEW: return "FORTUNE_NEW";
    case PET_FORTUNE_SAME: return "FORTUNE_SAME";
    case PET_STORAGE: return "STORAGE";
    default: return "BUSY";
    }
}

/* Wait up to ~2 s for the feedback of one action. Feedback for other actions
 * is pushed back to the front so the console loop never swallows feedback
 * meant for the UI (see muse_pet_feedback_requeue()). */
static bool pet_console_wait(muse_pet_action_t action, muse_pet_feedback_t *out)
{
    for (int i = 0; i < 200; i++) {
        muse_pet_feedback_t fb;
        if (muse_pet_feedback(&fb)) {
            if (fb.action == action) {
                *out = fb;
                return true;
            }
            if (!muse_pet_feedback_requeue(&fb)) {
                printf("@pet feedback dropped\n");
                fflush(stdout);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}
#endif

static void pet_console_snapshot(void)
{
    muse_pet_model_t s;
    if (!muse_pet_snapshot(&s)) {
        printf("@pet snapshot unavailable ready=%d storage_error=%d\n",
               (int)muse_pet_ready(), (int)muse_pet_storage_error());
        fflush(stdout);
        return;
    }
    printf("@pet coins=%u touch=%u feed_mask=0x%x fortune_day=%d fortune=%u "
           "decay_day=%d decay_notice=%u\n",
           (unsigned)s.coins, (unsigned)s.daily_touch_coins,
           (unsigned)s.feed_mask, (int)s.fortune_day,
           (unsigned)s.fortune, (int)s.decay_day, (unsigned)s.decay_notice);
    fflush(stdout);
}

#if CONFIG_MUSE_BUILD_TEST
/* Send one action and print its result + snapshot. Returns false on
 * not-ready / queue-full / timeout, so the caller prints nothing further. */
static bool pet_console_do(muse_pet_action_t action, const char *what)
{
    if (!muse_pet_request(action)) {
        printf("@pet %s error not-ready storage_error=%d\n", what,
               (int)muse_pet_storage_error());
        fflush(stdout);
        return false;
    }
    muse_pet_feedback_t fb;
    if (!pet_console_wait(action, &fb)) {
        printf("@pet %s error timeout\n", what);
        fflush(stdout);
        return false;
    }
    printf("@pet %s result=%s\n", what, pet_result_name(fb.result));
    fflush(stdout);
    pet_console_snapshot();
    return true;
}
#endif /* CONFIG_MUSE_BUILD_TEST: mutating pet.* helpers */

/*
 * Console-only commands; false for setup commands. Their buffers are taken
 * per command: without PSRAM, static ones would hold internal RAM for good.
 */
static bool console_command(char *line, bool whole)
{
    if (whole && s_console_hook && s_console_hook(line)) {
        return true;
    }
#if CONFIG_MUSE_VIDEO
    /* The Phone camera item, for a bench without fingers: "video" opens the
     * view (asking the phone for a stream), "video=0" closes it. */
    if (!strcmp(line, "video") || !strcmp(line, "video=0")) {
        muse_state_poke();
        if (line[5]) {
            muse_video_close();
        } else {
            muse_video_open();
        }
        printf("@video %s\n", muse_video_is_open() ? "open" : "closed");
        fflush(stdout);
        return true;
    }
#endif
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
#if CONFIG_MUSE_BUILD_TEST
    if (!strncmp(line, "ui.game", 7)) {
        /* Drives one game round from the console. The request is queued and
         * started by the LVGL tick; the outcome is visible in the "muse_ic"
         * log lines, not in this reply. */
        int which = (line[7] == ' ') ? atoi(line + 8) : 0;
        bool queued = muse_interact_ui_test_game(which);
        printf("@ui game choice=%d queued=%d\n", which, (int)queued);
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "pmdump")) {
        /* Who is holding a power-management lock right now, and for how long
         * each one has. This is the first thing to read when the chip will not
         * stay in light sleep: a lock taken at boot and never released explains
         * a REST state that never arrives, and nothing else on the device
         * reports it. IDF's own dump, so there is no new code to trust
         * (docs/20261023-测试版遥测设计.md §3.1). */
        printf("@pmdump\n");
        esp_pm_dump_locks(stdout);
        fflush(stdout);
        return true;
    }
#endif
#if CONFIG_MUSE_TELEMETRY
    if (!strncmp(line, "telemetry", 9)) {
        /* ">telemetry" reports whether the recorder is up (and is the first
         * thing to try when a night produced no file), ">telemetry.dump" walks
         * the CSV out of the port for console.py to save, ">telemetry.stats"
         * summarises it on the device, ">telemetry.clear" starts a fresh night. */
        if (!strcmp(line, "telemetry.dump")) {
            muse_telemetry_dump();
        } else if (!strcmp(line, "telemetry.stats")) {
            muse_telemetry_stats();
        } else if (!strcmp(line, "telemetry.clear")) {
            muse_telemetry_clear();
        } else if (!strcmp(line, "telemetry")) {
            printf("@telemetry active=%d\n", (int)muse_telemetry_active());
        } else {
            printf("@telemetry unknown command\n");
        }
        fflush(stdout);
        return true;
    }
#endif
#if CONFIG_MUSE_BUILD_TEST
    if (!strncmp(line, "alarm", 5)) {
        /* Bench hook, Test build only: ">alarm=HH:MM:mask" sets and arms it,
         * ">alarm=off" turns it off, ">alarm" reports the state. The panel rows
         * are the product path; this exists so the acceptance cases in
         * docs/20261023-待办.md §2.4 can be run from a cable. Mask: bit0 Sunday
         * .. bit6 Saturday, 0 once, 127 every day, 62 weekdays. */
        if (line[5] == '=') {
            const char *v = line + 6;
            if (!strcmp(v, "off")) {
                muse_settings_set_alarm_on(false);
            } else {
                int hh = 0, mm = 0, mask = 0;
                if (sscanf(v, "%d:%d:%d", &hh, &mm, &mask) == 3 && hh >= 0 && hh <= 23 &&
                    mm >= 0 && mm <= 59 && mask >= 0 && mask <= 0x7F) {
                    muse_settings_set_alarm_hour(hh);
                    muse_settings_set_alarm_min(mm);
                    muse_settings_set_alarm_days(mask);
                    muse_settings_set_alarm_on(true);
                } else {
                    printf("@alarm bad value, want HH:MM:mask (mask 0..127)\n");
                    fflush(stdout);
                    return true;
                }
            }
        }
        printf("@alarm %02d:%02d %s days 0x%02x ringing=%d\n", muse_settings_alarm_hour(),
               muse_settings_alarm_min(), muse_settings_alarm_on() ? "on" : "off",
               (unsigned)muse_settings_alarm_days(), (int)muse_input_alarm_ringing());
        fflush(stdout);
        return true;
    }
#endif
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
    if (!strcmp(line, "alarm") || !strncmp(line, "alarm=", 6) || !strcmp(line, "alarm.off") ||
        !strcmp(line, "alarm.test") || !strncmp(line, "alarm.arm=", 10) ||
        !strcmp(line, "alarm.stop")) {
        /* The alarm from a cable: "alarm" reports, "alarm=07:30" sets and enables,
         * "alarm.off" disables, "alarm.test" rings now (ring UI, chirp, the key
         * that stops it), "alarm.stop" stops a ring without a finger, and
         * "alarm.arm=N" arms N seconds out - how the three wake paths are tested
         * without waiting for a real time. */
        if (line[5] == '=') {
            int h = 0, m = 0;
            if (sscanf(line + 6, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
                printf("@alarm bad format, want alarm=HH:MM\n");
            } else {
                muse_settings_set_alarm_hour(h);
                muse_settings_set_alarm_min(m);
                muse_settings_set_alarm_on(true);   /* notifies, which arms */
                printf("@alarm set %02d:%02d on\n", h, m);
            }
        } else if (!strcmp(line, "alarm.off")) {
            muse_settings_set_alarm_on(false);
            printf("@alarm off\n");
        } else if (!strcmp(line, "alarm.test")) {
            alarm_start("console");
            printf("@alarm ringing\n");
        } else if (!strcmp(line, "alarm.stop")) {
            alarm_stop("console");
            printf("@alarm stopped\n");
        } else if (!strncmp(line, "alarm.arm=", 10)) {
            esp_err_t err = muse_rtc_arm_wakeup_in((uint32_t)atol(line + 10));
            printf("@alarm arm %s\n", esp_err_to_name(err));
        } else {
            time_t now = 0;
            bool have = muse_rtc_now(&now);
            printf("@alarm on=%d at=%02d:%02d armed=%d pending=%d ringing=%d target=%lld "
                   "now=%lld chip=%d reset=%d wake=0x%x\n",
                   (int)muse_settings_alarm_on(), muse_settings_alarm_hour(),
                   muse_settings_alarm_min(), (int)muse_rtc_wake_armed(),
                   (int)muse_rtc_alarm_pending(), (int)s_alarm_ringing,
                   (long long)s_alarm_target, (long long)(have ? now : 0),
                   (int)muse_rtc_present(), (int)esp_reset_reason(),
                   (unsigned)esp_sleep_get_wakeup_causes());
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
    if (!strcmp(line, "tp")) {
        /* The wake lines, the level each is armed on and what they have done so
         * far: the one place that answers "did a line even reach the chip while
         * the screen was dark on battery". With the IMU gone this board has one
         * line (BOOT) and no touch line; the counters still say why a wake
         * happened, which is what a battery-only failure needs. */
        int ti = muse_gpio_wake_line_touch_index();
        printf("@tp lines=%d touch=%d line=%d armed=%d live=%d seen=%d irqs=%u "
               "touch_wakes=%lu btn_irqs=%u btn_level=%d cause=%d touch_level=%d "
               "sleeps=%lu wgpio=%lu wtimer=%lu wother=%lu btn=%lu "
               "last_slept_ms=%lld last_tick_ms=%lld touch_armed=%d\n",
               muse_gpio_wake_line_count(), ti,
               ti < 0 ? -1 : (int)muse_gpio_wake_line_gpio(ti),
               ti < 0 ? -1 : muse_gpio_wake_line_armed(ti),
               ti < 0 ? -1 : muse_gpio_wake_line_live(ti),
               ti < 0 ? -1 : muse_gpio_wake_line_seen(ti),
               ti < 0 ? 0u : muse_gpio_wake_line_irqs(ti),
               (unsigned long)s_stat_touch_wakes, muse_gpio_button_irqs(),
               muse_gpio_button_level(), muse_gpio_wake_cause(), s_stat_touch_level,
               (unsigned long)s_stat_sleeps, (unsigned long)s_stat_wgpio,
               (unsigned long)s_stat_wtimer, (unsigned long)s_stat_wother,
               (unsigned long)s_stat_wake_btn, (long long)s_stat_slept_ms,
               (long long)s_stat_tick_ms, s_stat_touch_armed);
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
    if (!strcmp(line, "off")) {
        /* Bench: the power-off the 1.5 s hold runs, so a deep sleep - and the
         * wake that ends it, button or RTC alarm - can be driven from a cable. */
        muse_input_request_power_off();
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
    if (!strncmp(line, "pet.", 4)) {
        if (!strcmp(line, "pet.status")) {
            /* Read-only, always compiled. Since 1.5b every mutating pet.*
             * command (touch/feed/fortune/game/reset) is Test-gated;
             * status is the only pet.* command a Release build answers.
             * Never invents numbers: without a snapshot it prints
             * ready/storage_error and nothing else. */
            if (!muse_pet_ready()) {
                printf("@pet status ready=0 storage_error=%d\n",
                       (int)muse_pet_storage_error());
            } else {
                pet_console_snapshot();
            }
            fflush(stdout);
            return true;
        }
#if CONFIG_MUSE_BUILD_TEST
        /* 1.5b: mutating commands, Test only. In a Release build these
         * fall through to "@pet unknown command" below and move no coins. */
        if (!strcmp(line, "pet.touch")) {
            pet_console_do(PET_TOUCH, "touch");
            return true;
        }
        if (!strncmp(line, "pet.feed", 8)) {
            /* Each food pays its own 10 now, so name it: "pet.feed 0/1/2"
             * = rice / cake / coffee, bare "pet.feed" = rice. */
            int which = (line[8] == ' ') ? atoi(line + 9) : 0;
            muse_pet_action_t act = PET_FEED_RICE;
            if (which == 1) act = PET_FEED_CAKE;
            if (which == 2) act = PET_FEED_COFFEE;
            pet_console_do(act, "feed");
            return true;
        }
        if (!strcmp(line, "pet.fortune")) {
            if (pet_console_do(PET_FORTUNE, "fortune")) {
                muse_pet_model_t s;
                if (muse_pet_snapshot(&s) && s.fortune_day >= 0) {
                    const char *g = muse_pet_fortune_grade(s.fortune);
                    const char *t = muse_pet_fortune_text(s.fortune);
                    printf("@pet fortune grade=%s text=%s\n",
                           g ? g : "?", t ? t : "?");
                    fflush(stdout);
                }
            }
            return true;
        }
        if (!strncmp(line, "pet.game", 8)) {
            /* 1.5a settles by result, so the bench has to say which one:
             * "pet.game 1" win (+5), "pet.game -1" loss (-5), no argument or
             * "pet.game 0" draw (no change). */
            int outcome = (line[8] == ' ') ? atoi(line + 9) : 0;
            if (!muse_pet_request_game(outcome)) {
                printf("@pet game error not-ready storage_error=%d\n",
                       (int)muse_pet_storage_error());
                fflush(stdout);
                return true;
            }
            muse_pet_feedback_t fb;
            if (!pet_console_wait(PET_GAME, &fb)) {
                printf("@pet game error timeout\n");
                fflush(stdout);
                return true;
            }
            printf("@pet game outcome=%d result=%s\n", outcome,
                   pet_result_name(fb.result));
            fflush(stdout);
            pet_console_snapshot();
            return true;
        }
#endif /* CONFIG_MUSE_BUILD_TEST: mutating pet.* commands */
#if CONFIG_MUSE_BUILD_TEST
        if (!strcmp(line, "pet.reset")) {
            /* Destructive, Test only (design §3.3). Two paths:
             *  - worker running: PET_RESET through the queue (race-free,
             *    the worker is the sole NVS writer);
             *  - worker dead (e.g. corrupt blob): erase directly, then a
             *    reboot rebuilds fresh state (the worker re-inits on
             *    NOT_FOUND). Never erase behind a live worker. */
            if (muse_pet_ready()) {
                muse_pet_feedback_t fb;
                if (muse_pet_request(PET_RESET) &&
                    pet_console_wait(PET_RESET, &fb) && fb.result == PET_OK) {
                    printf("@pet reset ok\n");
                } else {
                    printf("@pet reset error\n");
                }
            } else if (muse_pet_erase_storage()) {
                printf("@pet reset erased, reboot to rebuild\n");
            } else {
                printf("@pet reset erase failed\n");
            }
            fflush(stdout);
            return true;
        }
#endif
        /* In a Release build, the Test-gated pet.* commands fall through
         * to here (the #if blocks above are compiled out) instead of
         * silently doing nothing. */
        printf("@pet unknown command\n");
        fflush(stdout);
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
 * ten minutes asleep on battery would; 'w' rejoins), "tp" dumps the wake
 * lines and the light-sleep counters, "batt" dumps the fuel gauge through the
 * bq27220 component's getters, "ota" starts the same GitHub
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
