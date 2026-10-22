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

#include "muse_board.h"

#include "esp_check.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "muse_board";

#define DEBOUNCE_SAMPLES 3
#define DEBOUNCE_POLL_MS 10     /* the input task's awake poll */

static TaskHandle_t s_waiter;

/* Extra wake lines a board registered (see muse_gpio_wake_line_add). */
#define WAKE_LINES_MAX 2
static struct {
    gpio_num_t gpio;
    int level;       /* armed level: -1 not armed, 0 low, 1 high */
    bool own_intr;   /* we installed the handler and drive the interrupt enable */
    bool touch;      /* the panel's INT: a hand on the glass is what it reports */
    unsigned irqs;   /* interrupts this line has raised since boot */
    int seen;        /* level when the last wait returned, -1 unknown */
    int armed;       /* level when the wait armed it, -1 unknown */
    unsigned skips;  /* cycles left out of the arming: level already satisfied */
    gpio_int_type_t normal_intr;  /* trigger type the pin's owner wants back */
    /* Borrowed interrupt: enabled for the armed window, restored *enabled* on
     * disarm - the owning driver registered its ISR for a pin that interrupts,
     * and leaving it disabled killed the touch input after the first sleep. */
    bool borrow_intr;
    esp_err_t (*drain)(void);      /* releases a line its part holds asserted */
} s_wake_lines[WAKE_LINES_MAX];
static int s_wake_line_n;

/* Forensics for the wait (muse_board.h): the button is ours to count, and the
 * cause is what the last light-sleep exit reported. */
static volatile unsigned s_button_irqs;
static int s_button_seen = -1;
static int s_wake_cause;

static bool s_isr_service;

static esp_err_t ensure_isr_service(void)
{
    if (s_isr_service) {
        return ESP_OK;
    }
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;   /* already installed */
    }
    s_isr_service = err == ESP_OK;
    return err;
}

/* A button reached the level muse_gpio_buttons_wait() armed. Level-triggered,
 * so off until armed again. The same handler serves a wake line we own (the
 * IMU's), and the two counters answer different questions: a button interrupt
 * that never reached the input task, against an IMU line that never rose. */
static void on_button_line(void *arg)
{
    gpio_num_t gpio = (gpio_num_t)(intptr_t)arg;
    gpio_intr_disable(gpio);
    bool ours = false;
    for (int i = 0; i < s_wake_line_n; i++) {
        if (s_wake_lines[i].gpio == gpio) {
            s_wake_lines[i].irqs++;
            ours = true;
            break;
        }
    }
    if (!ours) {
        s_button_irqs++;
    }
    BaseType_t woken = pdFALSE;
    if (s_waiter) {
        vTaskNotifyGiveFromISR(s_waiter, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static esp_err_t button_init(muse_gpio_button_t *b, gpio_num_t gpio, bool active_high)
{
    *b = (muse_gpio_button_t){ .gpio = gpio, .active_high = active_high };
    /* Input-only pads (the classic ESP32's 34-39) have no pull-up; their
     * boards wire one. */
    bool pull_up = !active_high && GPIO_IS_VALID_OUTPUT_GPIO(gpio);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio %d", gpio);
    /* For muse_gpio_buttons_wait(); the line doesn't interrupt until then.
     * Installing twice works but logs an error. */
    ESP_RETURN_ON_ERROR(ensure_isr_service(), TAG, "gpio isr");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(gpio, on_button_line, (void *)(intptr_t)gpio), TAG, "gpio %d isr", gpio);
    return gpio_intr_disable(gpio);
}

static esp_err_t wake_line_add(gpio_num_t gpio, int level, bool own_intr, bool pull_up)
{
    if (s_wake_line_n == WAKE_LINES_MAX) {
        return ESP_ERR_NO_MEM;
    }
    if (own_intr) {
        /* Ours to drive: the line belongs to a part that does not interrupt on
         * its own (the IMU), so the handler here is what tells the input task
         * the sleep is over. The pull keeps the pin defined while that part is
         * still in high-Z; `pull_up` is for an open-drain output such as the
         * RTC's INT, where a pull-down would hold the line asserted. */
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = pull_up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "wake gpio %d", gpio);
        ESP_RETURN_ON_ERROR(ensure_isr_service(), TAG, "gpio isr");
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(gpio, on_button_line,
                                                 (void *)(intptr_t)gpio),
                            TAG, "gpio %d isr", gpio);
        ESP_RETURN_ON_ERROR(gpio_intr_disable(gpio), TAG, "gpio %d intr", gpio);
    }
    /* A line another driver already interrupts on (the touch controller) is
     * left exactly as that driver configured it: direction, pull and interrupt
     * type. It used to be forced to an input with a pull-down, which fights an
     * active-low IRQ (the CST816S idles high and is pulled low by a touch), and
     * gpio_config() here would reset the interrupt type to none and take the
     * touch driver with it. Only the light-sleep wake source is armed, and only
     * around the wait. */
    s_wake_lines[s_wake_line_n].gpio = gpio;
    s_wake_lines[s_wake_line_n].level = level;
    s_wake_lines[s_wake_line_n].own_intr = own_intr;
    s_wake_lines[s_wake_line_n].touch = false;
    s_wake_lines[s_wake_line_n].irqs = 0;
    s_wake_lines[s_wake_line_n].seen = -1;
    s_wake_lines[s_wake_line_n].normal_intr = GPIO_INTR_DISABLE;
    s_wake_lines[s_wake_line_n].armed = -1;
    s_wake_lines[s_wake_line_n].skips = 0;
    s_wake_lines[s_wake_line_n].borrow_intr = false;
    s_wake_lines[s_wake_line_n].drain = NULL;
    s_wake_line_n++;
    return ESP_OK;
}

esp_err_t muse_gpio_wake_line_add(gpio_num_t gpio, bool wake_high)
{
    return wake_line_add(gpio, wake_high ? 1 : 0, true, false);
}

/* Same, for a line that idles high through an external pull-up (the RTC's INT
 * is open drain): the pin gets the internal pull-up instead of the pull-down
 * wake_line_add() uses for a part in high-Z. */
esp_err_t muse_gpio_wake_line_add_pullup(gpio_num_t gpio, bool wake_high)
{
    return wake_line_add(gpio, wake_high ? 1 : 0, true, true);
}

/* For a part whose own driver owns the pin and its interrupt - the touch
 * controller: the LVGL adapter registers its ISR there. Only the sleep wake
 * source is armed, so a tap on the glass ends light sleep; the driver's own
 * handler then does the notifying. */
esp_err_t muse_gpio_wake_line_add_wake_only(gpio_num_t gpio, bool wake_high)
{
    return wake_line_add(gpio, wake_high ? 1 : 0, false, false);
}

static int wake_line_index(gpio_num_t gpio)
{
    for (int i = 0; i < s_wake_line_n; i++) {
        if (s_wake_lines[i].gpio == gpio) {
            return i;
        }
    }
    return -1;
}

/* Take the pin's interrupt enable for the armed window, and give it back on
 * disarm. A line whose driver stops interrupting while the display is paused
 * (the LVGL adapter's touch input does exactly that) never wakes the chip:
 * ESP-IDF wants the pin's interrupt *enabled* for a light-sleep GPIO wake -
 * gpio_wakeup_enable() only sets the wake source. `normal_intr` is the trigger
 * type that driver configured, restored on disarm so an armed level wake does
 * not leave the pin interrupting by level for the rest of the session. */
esp_err_t muse_gpio_wake_line_take_intr(gpio_num_t gpio, gpio_int_type_t normal_intr)
{
    int i = wake_line_index(gpio);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    s_wake_lines[i].borrow_intr = true;
    s_wake_lines[i].normal_intr = normal_intr;
    return ESP_OK;
}

/* Read/touch whatever releases a line its part holds asserted - the CST816S
 * keeps INT low after a touch until the host reads it, and while the display
 * is paused nothing else does. Called just before the line is armed. */
esp_err_t muse_gpio_wake_line_set_drain(gpio_num_t gpio, esp_err_t (*drain)(void))
{
    int i = wake_line_index(gpio);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    s_wake_lines[i].drain = drain;
    return ESP_OK;
}

/* The line's level when the last wait armed it: a wake counts as a *new*
 * assertion of the line only if it was high then. Without this a line that a
 * part holds low (nobody read the touch controller) relights the screen the
 * moment it goes dark, once per sleep, forever. */
int muse_gpio_wake_line_armed_level(int index)
{
    if (index < 0 || index >= s_wake_line_n) {
        return -1;
    }
    return s_wake_lines[index].armed;
}

unsigned muse_gpio_wake_line_skips(int index)
{
    if (index < 0 || index >= s_wake_line_n) {
        return 0;
    }
    return s_wake_lines[index].skips;
}

esp_err_t muse_gpio_wake_line_set_level(gpio_num_t gpio, int level)
{
    int i = wake_line_index(gpio);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    s_wake_lines[i].level = level > 0 ? 1 : (level < 0 ? -1 : 0);
    return ESP_OK;
}

void muse_gpio_wake_line_mark_touch(gpio_num_t gpio)
{
    int i = wake_line_index(gpio);
    if (i >= 0) {
        s_wake_lines[i].touch = true;
    }
}

int muse_gpio_wake_cause(void)
{
    return s_wake_cause;
}

bool muse_gpio_wake_was_line(void)
{
    return s_wake_cause == ESP_SLEEP_WAKEUP_GPIO;
}

bool muse_gpio_wake_was_timer(void)
{
    return s_wake_cause == ESP_SLEEP_WAKEUP_TIMER;
}

unsigned muse_gpio_button_irqs(void)
{
    return s_button_irqs;
}

int muse_gpio_button_level(void)
{
    return s_button_seen;
}

int muse_gpio_wake_line_count(void)
{
    return s_wake_line_n;
}

gpio_num_t muse_gpio_wake_line_gpio(int i)
{
    return (i >= 0 && i < s_wake_line_n) ? s_wake_lines[i].gpio : GPIO_NUM_NC;
}

int muse_gpio_wake_line_armed(int i)
{
    return (i >= 0 && i < s_wake_line_n) ? s_wake_lines[i].level : -1;
}

int muse_gpio_wake_line_live(int i)
{
    return (i >= 0 && i < s_wake_line_n) ? gpio_get_level(s_wake_lines[i].gpio) : -1;
}

int muse_gpio_wake_line_seen(int i)
{
    return (i >= 0 && i < s_wake_line_n) ? s_wake_lines[i].seen : -1;
}

unsigned muse_gpio_wake_line_irqs(int i)
{
    return (i >= 0 && i < s_wake_line_n) ? s_wake_lines[i].irqs : 0;
}

int muse_gpio_wake_line_touch_index(void)
{
    for (int i = 0; i < s_wake_line_n; i++) {
        if (s_wake_lines[i].touch) {
            return i;
        }
    }
    return -1;
}

/* --------------------------------------------------------------- the I2C bus
 * See muse_board.h for why this exists. The recovery is IDF's own bus reset -
 * it clocks nine pulses out of a slave that is holding SDA down and re-routes
 * the pads back to the peripheral - followed by the pull-ups the bus was built
 * with, in case light sleep left the pads without them. */
#define I2C_FAIL_STREAK 2          /* failures in a row before the bus is reset */
#define I2C_RECOVER_MIN_MS 10000   /* ... no more often than this */

static struct {
    i2c_master_bus_handle_t bus;
    gpio_num_t sda, scl;
    unsigned fails, recovers, streak;
    int64_t last_recover_ms;
} s_i2c;

void muse_i2c_register(i2c_master_bus_handle_t bus, gpio_num_t sda, gpio_num_t scl)
{
    s_i2c.bus = bus;
    s_i2c.sda = sda;
    s_i2c.scl = scl;
}

bool muse_i2c_registered(void)
{
    return s_i2c.bus != NULL;
}

void muse_i2c_levels(int *sda, int *scl)
{
    if (sda) {
        *sda = s_i2c.bus ? gpio_get_level(s_i2c.sda) : -1;
    }
    if (scl) {
        *scl = s_i2c.bus ? gpio_get_level(s_i2c.scl) : -1;
    }
}

esp_err_t muse_i2c_recover(void)
{
    if (!s_i2c.bus) {
        return ESP_ERR_INVALID_STATE;
    }
    int sda_before = gpio_get_level(s_i2c.sda);
    int scl_before = gpio_get_level(s_i2c.scl);
    esp_err_t err = i2c_master_bus_reset(s_i2c.bus);
    /* The bus was created with the internal pull-ups on; light sleep is the
     * one thing that can take them away without anybody noticing, and without
     * them a floating bus times out exactly like a held one. */
    gpio_set_pull_mode(s_i2c.sda, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(s_i2c.scl, GPIO_PULLUP_ONLY);
    int sda_after = gpio_get_level(s_i2c.sda);
    int scl_after = gpio_get_level(s_i2c.scl);
    s_i2c.recovers++;
    s_i2c.last_recover_ms = esp_timer_get_time() / 1000;
    ESP_LOGW(TAG, "i2c bus reset (%s): sda %d->%d scl %d->%d, %u failures, %u resets",
             esp_err_to_name(err), sda_before, sda_after, scl_before, scl_after,
             s_i2c.fails, s_i2c.recovers);
    return err;
}

void muse_i2c_report(bool ok)
{
    if (ok) {
        s_i2c.streak = 0;
        return;
    }
    s_i2c.fails++;
    s_i2c.streak++;
    if (s_i2c.streak < I2C_FAIL_STREAK) {
        return;
    }
    if (s_i2c.recovers && esp_timer_get_time() / 1000 - s_i2c.last_recover_ms < I2C_RECOVER_MIN_MS) {
        return;   /* a bus that dies faster than this is not healed by a reset */
    }
    (void)muse_i2c_recover();
}

unsigned muse_i2c_fails(void)
{
    return s_i2c.fails;
}

unsigned muse_i2c_recovers(void)
{
    return s_i2c.recovers;
}

unsigned muse_i2c_streak(void)
{
    return s_i2c.streak;
}

esp_err_t muse_gpio_button_init(muse_gpio_button_t *b, gpio_num_t gpio)
{
    return button_init(b, gpio, false);
}

esp_err_t muse_gpio_button_init_high(muse_gpio_button_t *b, gpio_num_t gpio)
{
    return button_init(b, gpio, true);
}

static bool raw_pressed(const muse_gpio_button_t *b)
{
    return gpio_get_level(b->gpio) == b->active_high;
}

/* What ended the last light sleep, as one of the esp_sleep_source_t values the
 * callers print and the RTC diary keeps (0 = nothing slept, 4 = timer, 7 = a
 * GPIO line, 1 = something else). esp_sleep_get_wakeup_causes() is the bitmap
 * form of the deprecated single-source call, so GPIO wins when both fired. */
static int wake_cause(void)
{
    uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        return ESP_SLEEP_WAKEUP_GPIO;
    }
    if (causes & (1u << ESP_SLEEP_WAKEUP_TIMER)) {
        return ESP_SLEEP_WAKEUP_TIMER;
    }
    return causes ? ESP_SLEEP_WAKEUP_ALL : ESP_SLEEP_WAKEUP_UNDEFINED;
}

unsigned muse_gpio_button_poll(muse_gpio_button_t *b)
{
    bool raw = raw_pressed(b);
    if (raw == b->pressed) {
        b->stable = 0;
        return 0;
    }
    /* Asleep, a press is what woke the input task: it counts on its first sample. */
    if (++b->stable < DEBOUNCE_SAMPLES && !(raw && muse_state_asleep())) {
        return 0;
    }
    b->stable = 0;
    b->pressed = raw;
    return raw ? MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_RELEASE;
}

void muse_gpio_buttons_wait(muse_gpio_button_t *const *b, int n, int timeout_ms)
{
    for (int i = 0; i < n; i++) {
        if (raw_pressed(b[i]) != b[i]->pressed) {
            /* Mid-change: debounce at the awake poll rate. */
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_POLL_MS));
            return;
        }
    }
    /* Not cleared first: a stale notification (a line that changed back
     * before the last wait ended) only makes this return early. */
    s_waiter = xTaskGetCurrentTaskHandle();
    esp_sleep_enable_gpio_wakeup();
    for (int i = 0; i < n; i++) {
        /* Wake on the level the button would change to. */
        bool high = b[i]->pressed != b[i]->active_high;
        gpio_wakeup_enable(b[i]->gpio, high ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
        gpio_intr_enable(b[i]->gpio);
    }
    for (int i = 0; i < s_wake_line_n; i++) {
        /* A board line: the IMU's tap interrupt, or the touch controller's.
         * "tpwake=0" leaves one out, so a line that turns out to keep the chip
         * awake can be isolated without a new build. */
        if (s_wake_lines[i].level < 0) {
            continue;
        }
        /* Whatever holds the line asserted (the touch controller keeps INT low
         * until it is read) is released first: a stale low level would satisfy
         * the wake condition the instant the sleep starts. */
        if (s_wake_lines[i].drain) {
            int before = gpio_get_level(s_wake_lines[i].gpio);
            esp_err_t err = s_wake_lines[i].drain();
            int after = gpio_get_level(s_wake_lines[i].gpio);
            if (err != ESP_OK || before != after) {
                ESP_LOGW(TAG, "wake line %d drain: %s, level %d -> %d", (int)s_wake_lines[i].gpio,
                         esp_err_to_name(err), before, after);
            }
        }
        s_wake_lines[i].armed = gpio_get_level(s_wake_lines[i].gpio);
        if (s_wake_lines[i].armed == s_wake_lines[i].level) {
            /* Already at the level this line would wake on: arming it here only
             * makes IDF refuse the sleep, over and over. Leave it out for this
             * cycle - the button, the IMU line and the timer still end it - and
             * count it, since a line stuck at its armed level is a fault of the
             * part that owns it, not of the sleep. */
            s_wake_lines[i].skips++;
            gpio_wakeup_disable(s_wake_lines[i].gpio);
            if (s_wake_lines[i].own_intr || s_wake_lines[i].borrow_intr) {
                gpio_intr_disable(s_wake_lines[i].gpio);
            }
            continue;
        }
        gpio_wakeup_enable(s_wake_lines[i].gpio,
                           s_wake_lines[i].level ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
        if (s_wake_lines[i].own_intr || s_wake_lines[i].borrow_intr) {
            gpio_intr_enable(s_wake_lines[i].gpio);
        }
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
    /* Read the lines before disarming them, and the cause of the last sleep
     * exit: on battery this is the only moment anybody can see whether a line
     * woke the chip, which line it was, and whether the button was down. */
    s_wake_cause = wake_cause();
    s_button_seen = n > 0 ? gpio_get_level(b[0]->gpio) : -1;
    for (int i = 0; i < s_wake_line_n; i++) {
        s_wake_lines[i].seen = s_wake_lines[i].level < 0
                                   ? -1
                                   : gpio_get_level(s_wake_lines[i].gpio);
    }
    for (int i = 0; i < n; i++) {
        gpio_intr_disable(b[i]->gpio);
        gpio_wakeup_disable(b[i]->gpio);
    }
    for (int i = 0; i < s_wake_line_n; i++) {
        if (s_wake_lines[i].own_intr) {
            gpio_intr_disable(s_wake_lines[i].gpio);
        }
        if (s_wake_lines[i].normal_intr != GPIO_INTR_DISABLE) {
            /* A borrowed interrupt goes back *enabled*: its owner registered an
             * ISR for a pin that interrupts, and this firmware sleeps and wakes
             * many times an hour - disabling it here is what made the touch
             * input stop working after the first sleep. */
            gpio_set_intr_type(s_wake_lines[i].gpio, s_wake_lines[i].normal_intr);
            if (s_wake_lines[i].borrow_intr) {
                gpio_intr_enable(s_wake_lines[i].gpio);
            }
        }
        gpio_wakeup_disable(s_wake_lines[i].gpio);
    }
}
