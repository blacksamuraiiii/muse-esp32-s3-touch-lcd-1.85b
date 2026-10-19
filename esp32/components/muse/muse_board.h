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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "lvgl.h"

#include "muse_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What Muse needs from the hardware. Each file in boards/ fills one of these in
 * and returns it from muse_board_get(). Everything above this line (UI, voice
 * loop, settings, Hatch) is shared; Wi-Fi and BLE belong to Home Link.
 */

/* Button edges reported by poll_buttons(). */
#define MUSE_BTN_TALK_PRESS   (1u << 0)
#define MUSE_BTN_TALK_RELEASE (1u << 1)
#define MUSE_BTN_AUX_PRESS    (1u << 2)
#define MUSE_BTN_AUX_RELEASE  (1u << 3)
/* Keyboard navigation presses, independent of the two-button controls. */
#define MUSE_BTN_UP           (1u << 4)
#define MUSE_BTN_DOWN         (1u << 5)
#define MUSE_BTN_LEFT         (1u << 6)
#define MUSE_BTN_RIGHT        (1u << 7)
#define MUSE_BTN_ENTER        (1u << 8)
#define MUSE_BTN_ESCAPE       (1u << 9)
/* Dedicated controls; legacy two-button boards keep AUX's existing role.
 * POWER_LONG is a validated hold/PMIC long event, never a fabricated level. */
#define MUSE_BTN_POWER_PRESS  (1u << 10)
#define MUSE_BTN_POWER_RELEASE (1u << 11)
#define MUSE_BTN_POWER_LONG   (1u << 12)
#define MUSE_BTN_SPEAKER_PRESS (1u << 13)
#define MUSE_BTN_SPEAKER_RELEASE (1u << 14)

/* Where a button's icon goes on screen: beside the button, inside the panel. */
typedef struct {
    lv_align_t align;
    int16_t x, y;
} muse_button_hint_t;

typedef struct {
    const char *name;
    int width, height;
    bool round;             /* circular panel: keep content inside the circle */
    bool no_ring;           /* square panel: progress as a bar along the bottom edge, no ring around Muse */
    bool touch;             /* no touch: no settings screen, set up over BLE */
    float diagonal_in;      /* screen size; under 2" typing uses a keypad with bigger keys */
    bool touch_talk;        /* on-screen mic supplies talk edges; no physical talk button */
    bool keyboard;          /* dedicated menu navigation keys */
    const char *talk_button;    /* where the buttons are, for captions: "top" */
    const char *aux_button;     /* "bottom" */
    muse_button_hint_t talk_hint;   /* mic icon; the menu's hints follow both */
    muse_button_hint_t aux_hint;    /* power or menu icon; left out, there's none */
    /* Optional dedicated POWER and speaker controls, independent of AUX. */
    const char *power_button;
    const char *speaker_button;
    muse_button_hint_t power_hint;
    muse_button_hint_t speaker_hint;
    /* 466 px round-panel layout: dedicated 26-36 px rim icons, safe header chord
     * and central read-reply column. Requires all three hint positions/names. */
    bool rim_controls;
    /* The menu turns a right-edge aux button's hint on end, which LVGL draws
     * through a ~14 KB layer; set this where that's too much RAM (the C3) and
     * the hint lies flat on the menu's bar, left of the talk button's. */
    bool flat_menu_hints;
    int frame_ms;           /* face animation period */
    int avatar_px;          /* optional avatar canvas size; 0 uses the UI default */

    /* Power rails, buses, expanders. Runs first. */
    esp_err_t (*init)(void);

    /* Starts the panel, LVGL and its task; *touch may be left NULL. */
    lv_display_t *(*display_start)(lv_indev_t **touch);
    bool (*display_lock)(int timeout_ms);   /* -1 waits forever */
    void (*display_unlock)(void);
    void (*set_brightness)(int pct);        /* 0 = off */
    void (*panel_sleep)(bool sleep);        /* screen off; NULL: backlight only */
    /* Screen off with nothing to draw: stops LVGL's task and tick, and touch,
     * so the chip can light-sleep. Buttons still wake it. NULL: LVGL keeps
     * running. */
    void (*display_pause)(bool pause);

    /* Codec handles for one duplex, 2-slot I2S bus, not yet opened. */
    esp_err_t (*audio_init)(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic);
    int mic_slot;           /* slot carrying the mic (0/1), or -1 to mix both */
    void (*set_mic_gain)(esp_codec_dev_handle_t mic, int db);   /* NULL: esp_codec_dev_set_in_gain */
    /* Audio power the board can switch, such as an amp's enable: on once
     * the codecs are open, off before they close to rest. NULL: none. */
    void (*audio_power)(bool on);
    /* The codecs' own supply, where the board switches it: on before they
     * open (the first time too, before audio_init), off once both have
     * closed to rest. Off, nothing reaches them: muse_audio skips the volume
     * and gain setters until they open again, and reapplies the settings
     * then. NULL: always powered. */
    void (*codec_power)(bool on);

    /* Called every 10 ms from the input task (50 ms while the display is
     * paused, unless wait_buttons is set); returns MUSE_BTN_* edges. */
    unsigned (*poll_buttons)(void);
    /* Display paused: returns once a button changes (waking the chip from
     * light sleep), the task is notified or timeout_ms passes, so the
     * buttons needn't be polled. NULL: polled. */
    void (*wait_buttons)(int timeout_ms);
    esp_err_t (*read_power)(muse_power_t *out);
    /* Optional: print the fuel gauge's raw registers to the log, for
     * calibrating it against the cell that is actually fitted. NULL: the
     * board has no gauge worth dumping. */
    void (*power_debug)(void);
    /* Turns the board off; returns only on failure. NULL: it can't from
     * software (a physical switch does), and Settings has no Power off. */
    esp_err_t (*power_off)(void);
} muse_board_t;

/* The running board, set by muse_app_run(). */
extern const muse_board_t *muse_board;

/* Starts everything and returns; Home Link's app_main() calls it. */
void muse_app_run(const muse_board_t *board);

/* The board selected by CONFIG_MUSE_BOARD_* (boards/). */
const muse_board_t *muse_board_get(void);

/* Debounced GPIO button, for poll_buttons() implementations. */
typedef struct {
    gpio_num_t gpio;
    bool active_high;
    bool pressed;
    uint8_t stable;
} muse_gpio_button_t;

/* Active low, with the internal pull-up where the pad has one. */
esp_err_t muse_gpio_button_init(muse_gpio_button_t *b, gpio_num_t gpio);
/* Active high and driven both ways by the board, so no internal pull. */
esp_err_t muse_gpio_button_init_high(muse_gpio_button_t *b, gpio_num_t gpio);
/* Returns MUSE_BTN_TALK_PRESS / MUSE_BTN_TALK_RELEASE on an edge; shift as needed. */
unsigned muse_gpio_button_poll(muse_gpio_button_t *b);
/* For wait_buttons(): blocks until one of the n buttons leaves its debounced
 * state, the calling task is notified (xTaskNotifyGive) or timeout_ms passes. */
void muse_gpio_buttons_wait(muse_gpio_button_t *const *b, int n, int timeout_ms);

/* An extra level-triggered line that should wake the chip too, on top of the
 * buttons: an IMU interrupt, an RTC alarm. `wake_high` is the level the line
 * takes when the event happens. Registered at board init, armed and disarmed
 * around muse_gpio_buttons_wait(), and the wait returns when it fires. */
esp_err_t muse_gpio_wake_line_add(gpio_num_t gpio, bool wake_high);

/* Same, for a line another driver already interrupts on: the pin is armed as a
 * light-sleep wake source, and its interrupt is left alone (see muse_board.c). */
esp_err_t muse_gpio_wake_line_add_wake_only(gpio_num_t gpio, bool wake_high);

/* Take this line's interrupt enable for the armed window (restores
 * `normal_intr` on disarm). Needed for a line whose own driver stops
 * interrupting while the display is paused, e.g. the touch controller. */
esp_err_t muse_gpio_wake_line_take_intr(gpio_num_t gpio, gpio_int_type_t normal_intr);

/* Whatever releases a line its part keeps asserted (the touch controller holds
 * INT low until it is read); called just before that line is armed. */
esp_err_t muse_gpio_wake_line_set_drain(gpio_num_t gpio, esp_err_t (*drain)(void));

/* The line's level when the last wait armed it (-1 unknown): a low line is a
 * new touch only if it was high when the sleep started. */
int muse_gpio_wake_line_armed_level(int index);

/* Cycles where the line was already at its armed level and was left unarmed. */
unsigned muse_gpio_wake_line_skips(int index);

/* The level a registered line is armed on, changed while running: < 0 stops
 * arming it, 0 arms a low level, 1 a high level. One build can therefore test
 * both polarities of a line that a datasheet does not settle ("tpwake=" in the
 * console). ESP_ERR_NOT_FOUND for a gpio that is not registered. */
esp_err_t muse_gpio_wake_line_set_level(gpio_num_t gpio, int level);

/* Marks a line as the touch panel's: a touch pulls it to the level it is armed
 * on and holds it until the controller is read. The input task reads the line
 * to turn a touch into a wake - while the screen is dark on battery the display
 * is paused, so the LVGL task that owns on_cover_event() is not running. */
void muse_gpio_wake_line_mark_touch(gpio_num_t gpio);

/* Wake forensics, filled by every muse_gpio_buttons_wait(). These separate the
 * four candidates behind "the screen did not come on": the line never woke the
 * chip, it woke it and nobody read it, the button never interrupted, or the
 * state did change and the display did not resume. */
int muse_gpio_wake_cause(void);         /* esp_sleep_get_wakeup_cause(); 0 = timeout */
bool muse_gpio_wake_was_line(void);     /* the last sleep ended on a GPIO line, not a timer */
bool muse_gpio_wake_was_timer(void);    /* ... and this one ended when the nap ran out */
unsigned muse_gpio_button_irqs(void);   /* button interrupts since boot */
int muse_gpio_button_level(void);       /* button level when the wait returned, -1 unknown */
int muse_gpio_wake_line_count(void);
gpio_num_t muse_gpio_wake_line_gpio(int i);
int muse_gpio_wake_line_armed(int i);   /* -1 not armed, 0 low level, 1 high level */
int muse_gpio_wake_line_live(int i);    /* level read now, -1 unknown */
int muse_gpio_wake_line_seen(int i);    /* level when the wait returned, -1 unknown */
unsigned muse_gpio_wake_line_irqs(int i);
int muse_gpio_wake_line_touch_index(void);   /* -1 when no line is marked as the touch */

/* ------------------------------------------------------- the shared I2C bus
 *
 * Every part on the board's I2C bus - the touch controller above all - goes
 * quiet together when the bus itself stops working, which is what a battery
 * session can leave behind: the console then shows probe timeouts on the fuel
 * gauge and no answer from the IMU, and the touch reads are just slow. The two
 * candidates are a slave holding a line low (recoverable with the nine clocks
 * IDF's bus reset emits) and a pad that lost its configuration during light
 * sleep (recoverable by re-applying the pull-ups), so the recovery below does
 * both and reports the pad levels before and after.
 *
 * Boards call muse_i2c_register() once, report every transaction result with
 * muse_i2c_report(), and get a self-healing bus: two failures in a row and the
 * recovery runs, rate-limited. The console reaches the same path as "i2c". */
void muse_i2c_register(i2c_master_bus_handle_t bus, gpio_num_t sda, gpio_num_t scl);
void muse_i2c_report(bool ok);
esp_err_t muse_i2c_recover(void);       /* reset the bus, re-apply the pull-ups */
bool muse_i2c_registered(void);
void muse_i2c_levels(int *sda, int *scl);   /* pad levels read as GPIO, -1 unknown */
unsigned muse_i2c_fails(void);          /* failed transactions since boot */
unsigned muse_i2c_recovers(void);       /* recoveries run since boot */
unsigned muse_i2c_streak(void);         /* failures in a row right now */

#ifdef __cplusplus
}
#endif
