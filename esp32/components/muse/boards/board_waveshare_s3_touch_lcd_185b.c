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

/*
 * Waveshare ESP32-S3-Touch-LCD-1.85B: ESP32-S3R8 (16 MB flash, 8 MB octal
 * PSRAM) with a 1.85" 360x360 round TFT on an ST77916 over QSPI, a CST816S
 * capacitive touch controller, an ES8311 codec for the speaker and an ES7210
 * ADC for the dual microphones, a BQ27220 fuel gauge, and native USB.
 *
 * Pin/fact sources (per devices/AGENTS.md, do not guess pins):
 *  - Waveshare official docs: https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B
 *    (LCD QSPI CS=21 PCLK=40 D0=46 D1=45 D2=42 D3=41 RST=3 BL=5;
 *     touch CST816S I2C addr 0x15: SCL=10 SDA=11 RST=1 INT=4;
 *     I2S MCLK=2 BCLK=48 LRCK=38 DOUT=47 DIN=39;
 *     ES8311 I2C 7-bit 0x18, ES7210 I2C 7-bit 0x40 (docs print the 8-bit
 *     forms 0x30/0x80); amp enable PA_CTRL=9;
 *     BOOT=GPIO0; BQ27220 I2C 7-bit 0x55; QMI8658 IMU 0x6B; PCF85063 RTC 0x51)
 *  - Vendor repo (clone it, do not read on the web):
 *    https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B
 *  - Community firmware for this exact board (display/touch/audio/RTC/IMU):
 *    https://github.com/jamesblashill/pocket-watch
 *
 * Strapping pins in use: GPIO0 (BOOT), GPIO3 (LCD RST), GPIO45/46 (LCD
 * D1/D0). GPIO45 selects VDD_SPI at reset: no external pull-ups on the LCD
 * data lines, and the panel must stay high-Z through reset.
 *
 * ST77916 bring-up note: espressif/esp_lcd_st77916 2.x ships a single default
 * init table (vendor_specific_init_default). The "probes register 0x04 to pick
 * one of two tables" behaviour described in older write-ups is gone; if the
 * panel needs a different sequence, pass it via
 * st77916_vendor_config_t.init_cmds (see the component's README).
 *
 * Self-interruption note: Muse records only while the TALK button is held;
 * there is no always-on VAD and no full-duplex voice path, so the speaker
 * can never false-trigger recording the way it does on always-listening
 * firmware. Pressing TALK mid-reply (barge-in) first discards
 * CONFIG_MUSE_BARGE_IN_SETTLE_MS of capture (muse_voice.c).
 */
#include "bq27220.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "esp_wifi.h"   /* power_off() stops Wi-Fi before deep sleep (1.5b) */
#include "i2c_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_MUSE_LCD185B_INIT_ECHOEAR
/* The third ST77916 360x360 table, borrowed from the OSTB EchoEar-2ST port
 * (xiaozhi's esp-vocat sequence, MIT - the notice stays in that header). Only
 * compiled when forced from menuconfig, so an AUTO build carries no copy. */
#include "ostb_echoear_display_init.h"
#endif

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "muse_rtc.h"
#include "muse_ui.h"

static const char *TAG = "board";

#if CONFIG_MUSE_POWER_TEST
#include "boards/muse_power_test_board.h"
#include <string.h>

/* Board-owned descriptors: host tests compile this exact block as well.
 * This board has NO expander and no switchable rails - LCD, touch, codecs,
 * gauge, RTC and IMU all hang off always-on 3V3 - so the honest state axis
 * is backlight duty, panel SLPIN, codec open/suspend, the amp enable
 * (PA_EN GPIO9) and pad parking, not rail switching. Every entry below
 * names what is really controllable; things this firmware cannot switch or
 * that were never verified from a schematic are MUSE_PTEST_UNSUPPORTED with
 * the reason in the note. Pin facts: Waveshare official docs for this board
 * (same list as the file header). */
#define STATE(id, note, load, mhz, bl, amp) { id, note, load, mhz, bl, amp, 0, 1000, MUSE_PTEST_CODEC_KEEP, "other", "" }
static const muse_ptest_state_t s_matrix[] = {
    STATE("baseline_pre", "radio-off, codecs uninitialized/closed, panel SLPIN + backlight 0 (no rails exist here), DFS + automatic light sleep", MUSE_PTEST_IDLE, 0, -1, false),
    STATE("cpu_fixed_40_idle", "fixed CPU clock, idle (not a busy loop)", MUSE_PTEST_IDLE, 40, -1, false),
    STATE("cpu_fixed_80_idle", "fixed CPU clock, idle", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("cpu_fixed_160_idle", "fixed CPU clock, idle", MUSE_PTEST_IDLE, 160, -1, false),
    STATE("cpu_fixed_240_idle", "fixed CPU clock, idle", MUSE_PTEST_IDLE, 240, -1, false),
    STATE("baseline_cpu_post", "paired DFS baseline after fixed clocks", MUSE_PTEST_IDLE, 0, -1, false),
    STATE("lcd_black_bl_0", "ST77916 init + static black, backlight 0%, no LVGL/touch task; panel+touch sit on always-on 3V3 (nothing to rail off)", MUSE_PTEST_LCD, 80, 0, false),
    STATE("lcd_black_bl_25", "static black, backlight 25%", MUSE_PTEST_LCD, 80, 25, false),
    STATE("lcd_black_bl_50", "static black, backlight 50%", MUSE_PTEST_LCD, 80, 50, false),
    STATE("lcd_black_bl_100", "static black, backlight 100%", MUSE_PTEST_LCD, 80, 100, false),
    STATE("lcd_sleep_post", "paired: SLPIN + backlight 0 (panel charge pump off); CST816S left at its auto-sleep power-on default", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("codecs_closed_pre", "always-on codec rail, no initialized I2S yet or codecs closed; amp GPIO9 low", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("codecs_open_amp_off", "16kHz stereo-slot codecs open, I2S clocks on, no drain, PA_EN driven low", MUSE_PTEST_CODECS_IDLE, 80, -1, false),
    STATE("codecs_open_amp_on", "same codecs and I2S, PA_EN high, no playback", MUSE_PTEST_CODECS_IDLE, 80, -1, true),
    STATE("mic_capture_16k", "16kHz stereo I2S drained/discarded, ES7210 MIC1 slot 0, 0dB gain, amp off", MUSE_PTEST_MIC, 80, -1, false),
    STATE("sine_1k_minus18dbfs", "1kHz sine -18dBFS peak, volume 25/100, 16kHz stereo slots, amp on", MUSE_PTEST_SINE, 80, -1, true),
    STATE("codecs_closed_post", "paired baseline after codec close and I2S stop", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("battery_divider_unsupported", "skipped: no switchable battery-divider rail - this board gauges over I2C (BQ27220, always powered)", MUSE_PTEST_UNSUPPORTED, 80, -1, false),
    STATE("gauge_sample_1hz", "BQ27220 gauge read over I2C once/second (this board's real battery-read path; no ADC divider exists)", MUSE_PTEST_ADC_SAMPLE, 80, -1, false),
    STATE("gauge_off_post", "paired gauge-sample-off baseline", MUSE_PTEST_IDLE, 80, -1, false),
    STATE("wifi_scanning", "unassociated active scans, no credentials/NVS; completed scans counted", MUSE_PTEST_WIFI_SCAN, 80, -1, false),
    STATE("wifi_associated_idle", "skipped: no known AP credentials; never load production NVS", MUSE_PTEST_UNSUPPORTED, 80, -1, false),
    STATE("ble_advertising", "skipped: standalone NimBLE lifecycle not characterized; controller never initialized", MUSE_PTEST_UNSUPPORTED, 80, -1, false),
    STATE("imu_off_unsupported", "skipped: QMI8658 at 0x6B is always powered and nothing in this firmware drives it; no verified enable path", MUSE_PTEST_UNSUPPORTED, 80, -1, false),
    STATE("deep_sleep_timer", "skipped: no deep sleep in the peripheral sweep; the sleep matrix owns the timer-deep rows", MUSE_PTEST_UNSUPPORTED, 0, -1, false),
    STATE("baseline_post", "radio-off resting, panel SLPIN + backlight 0, amp low, codecs closed, DFS + automatic light sleep", MUSE_PTEST_IDLE, 0, -1, false),
};
#define MATRIX_COUNT (sizeof(s_matrix) / sizeof(s_matrix[0]))
_Static_assert(MATRIX_COUNT * MUSE_PTEST_MAX_REPEATS <= MUSE_PTEST_MAX_RECORDS, "result capacity");

#define SLEEP_STATE(id, note, knobs, codec, role, group, poll, load) \
    { id, note, load, 0, -1, false, knobs, poll, codec, role, group }
/* No rgb_low / unused_hiz rows: there is no RGB LED on this board, and no
 * GPIO has been verified genuinely unconnected from the schematic, so
 * floating a guessed "unused" pad list would be fabrication. No cpu_pd rows
 * either: the board overlay keeps CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP
 * off (its retention state does not fit beside this board's PSRAM/WiFi
 * budget), so a cpu_pd label would advertise a power-down that cannot
 * happen. PULLS_OFF is kept with an explicit caveat: the production i2c_bus
 * config enables internal pull-ups on SDA/SCL, but whether the board has
 * external ones is unverified - if the bus dies the record fails honestly. */
#define BEST_KNOBS (MUSE_PTEST_I2S_LOW | MUSE_PTEST_UART_HIZ | MUSE_PTEST_PULLS_OFF | MUSE_PTEST_GPIO_ISOLATE)
static const muse_ptest_state_t s_sleep_matrix[] = {
    SLEEP_STATE("cold_ref", "untouched codec and I2S state; cold means no audio constructor has run", 0, MUSE_PTEST_CODEC_COLD, "other", "", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("cold_i2s_low", "cold codec, I2S outputs low and DIN pulldown; reference for hiz", MUSE_PTEST_I2S_LOW, MUSE_PTEST_CODEC_COLD, "ref", "cold", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("cold_i2s_hiz", "cold codec, I2S pads floating", MUSE_PTEST_I2S_HIZ, MUSE_PTEST_CODEC_COLD, "variant", "cold", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("cold_i2s_low_b", "paired cold I2S-low reference", MUSE_PTEST_I2S_LOW, MUSE_PTEST_CODEC_COLD, "ref", "cold", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("codec_initialized", "audio constructors only, never stream-opened; I2S stopped", 0, MUSE_PTEST_CODEC_INITIALIZED, "other", "", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("codec_suspended", "16kHz codecs opened then closed; codec drivers' own shutdown writes; always-on codec rail", 0, MUSE_PTEST_CODEC_SUSPENDED, "other", "", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref", "warm baseline_post policy; no parking knobs", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("uart_hiz", "UART0 pads 43/44 floating only without VBUS; rerouted immediately on USB", MUSE_PTEST_UART_HIZ, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_2", "paired warm reference", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("pulls_off", "internal pulls off I2C SDA/SCL (production enables both); external board pull-ups NOT schematic-verified - a dead bus fails the record honestly", MUSE_PTEST_PULLS_OFF, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_3", "paired warm reference", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("gpio_isolate", "sleep-pad isolation; exempted to stay latched: I2S-low, backlight 0, PA_EN low, panel reset high", MUSE_PTEST_GPIO_ISOLATE, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_4", "paired warm reference", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("best_combined", "all safe sleep knobs, I2S low", BEST_KNOBS, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_5", "paired warm reference", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("best_combined_b", "repeat best-combined variant", BEST_KNOBS, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("best_poll_5s", "best-combined with five-second VBUS poll; boundary timers remain", BEST_KNOBS, MUSE_PTEST_CODEC_KEEP, "variant", "warm", 5000, MUSE_PTEST_IDLE),
    SLEEP_STATE("warm_ref_6", "final paired warm reference for best-combined-b and five-second polling", 0, MUSE_PTEST_CODEC_KEEP, "ref", "warm", 1000, MUSE_PTEST_IDLE),
    SLEEP_STATE("deep_sleep_timer", "timer-only reset, sleep-pad isolation; no held outputs", MUSE_PTEST_GPIO_ISOLATE, MUSE_PTEST_CODEC_SUSPENDED, "other", "", 0, MUSE_PTEST_DEEP_SLEEP),
    SLEEP_STATE("deep_sleep_timer_held", "timer-only reset; I2S outputs, backlight(0) and PA_EN low held across deep sleep; LCD data/reset never held (strapping)", MUSE_PTEST_GPIO_ISOLATE | MUSE_PTEST_I2S_LOW | MUSE_PTEST_GPIO_HOLD, MUSE_PTEST_CODEC_SUSPENDED, "other", "", 0, MUSE_PTEST_DEEP_SLEEP),
};
#define SLEEP_MATRIX_COUNT (sizeof(s_sleep_matrix) / sizeof(s_sleep_matrix[0]))
_Static_assert(SLEEP_MATRIX_COUNT <= MUSE_PTEST_MAX_RECORDS, "sleep result capacity");

const char *muse_ptest_board_name(void) { return "Waveshare ESP32-S3-Touch-LCD-1.85B"; }

const muse_ptest_state_t *muse_ptest_board_matrix(const char *name, size_t *count)
{
    if (count) { *count = 0; }
    if (name && !strcmp(name, "peripheral")) {
        if (count) { *count = MATRIX_COUNT; }
        return s_matrix;
    }
    if (name && !strcmp(name, "sleep")) {
        if (count) { *count = SLEEP_MATRIX_COUNT; }
        return s_sleep_matrix;
    }
    return NULL;
}
/* End board-owned power-test descriptors. */
#endif


#define LCD_H_RES 360
#define LCD_V_RES 360
#define LCD_HOST SPI3_HOST
#define LCD_CS GPIO_NUM_21
#define LCD_PCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_45
#define LCD_D2 GPIO_NUM_42
#define LCD_D3 GPIO_NUM_41
#define LCD_RST GPIO_NUM_3
#define LCD_BL GPIO_NUM_5       /* LEDC PWM dimming */
/* Four bands to the screen (muse_lcd_bands.h). The SPI DMA cannot read PSRAM
 * fast enough while both cores draw: ESP-IDF falls back to a private internal
 * bounce buffer per PSRAM flush, and that shows up as "DMA TX underflow
 * detected" the moment Wi-Fi and BLE are up. Bands draw in PSRAM but send
 * through two fixed internal buffers instead. Same as the 1.75C and 2.16. */
#define DRAW_BUF_LINES (LCD_V_RES / 4)
#define LCD_CHUNK_BYTES (LCD_H_RES * 8 * 2)

#define TP_ADDR 0x15           /* CST816S, 7-bit */
#define TP_RST GPIO_NUM_1
#define TP_INT GPIO_NUM_4

#define I2C_SDA GPIO_NUM_11    /* touch, ES8311, ES7210, BQ27220, RTC */
#define I2C_SCL GPIO_NUM_10

#define I2S_MCLK GPIO_NUM_2
#define I2S_BCLK GPIO_NUM_48
#define I2S_WS GPIO_NUM_38
#define I2S_DOUT GPIO_NUM_47   /* playback -> ES8311 */
#define I2S_DIN GPIO_NUM_39    /* record <- ES7210 */
#define PA_EN GPIO_NUM_9       /* amp on while HIGH */

#define BQ27220_ADDR 0x55      /* fuel gauge, 7-bit */

#define TALK_GPIO GPIO_NUM_0   /* BOOT; the PWR key is hardware-only */

/*
 * This board ships with two ST77916 panel revisions. The panel identifies
 * itself through register 0x04 (read over QSPI with opcode 0x0B):
 *   00 7F 7F 7F -> version 1      00 02 7F 7F -> version 2
 * esp_lcd_st77916 only ever sends its own built-in default table, which
 * matches neither revision - the screen comes up as coloured stripes. Both
 * tables below are copied verbatim from Waveshare's own BSP for this board
 * (Examples/ESP-IDF-V5.5.3/01_comprehensive_example/components/
 *  waveshare__esp32_s3_touch_lcd_1_85B/esp32_s3_touch_lcd_1_85B.c,
 *  vendor_specific_init_version_1 / _2), probe logic included.
 */
#define ST77916_OPCODE_WRITE_CMD 0x02
#define ST77916_OPCODE_READ_CMD  0x0B
#define ST77916_REG_VERSION      0x04
#define ST77916_PROBE_HZ         (3 * 1000 * 1000)     /* the probe runs slowly */

static const st77916_lcd_init_cmd_t s_init_version_1[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x49}, 1, 0},
    {0xB1, (uint8_t []){0x4A}, 1, 0},
    {0xB2, (uint8_t []){0x1F}, 1, 0},
    {0xB4, (uint8_t []){0x46}, 1, 0},
    {0xB5, (uint8_t []){0x34}, 1, 0},
    {0xB6, (uint8_t []){0xD5}, 1, 0},
    {0xB7, (uint8_t []){0x30}, 1, 0},
    {0xB8, (uint8_t []){0x04}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x70, 0x09, 0x12, 0x0C, 0x0B, 0x27, 0x38, 0x54, 0x4E, 0x19, 0x15, 0x15, 0x2C, 0x2F}, 14, 0},
    {0xE1, (uint8_t []){0x70, 0x08, 0x11, 0x0C, 0x0B, 0x27, 0x38, 0x43, 0x4C, 0x18, 0x14, 0x14, 0x2B, 0x2D}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x08}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x0B}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x00}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x82}, 1, 0},
    {0xEA, (uint8_t []){0xDF}, 1, 0},
    {0xEB, (uint8_t []){0x89}, 1, 0},
    {0xEC, (uint8_t []){0x20}, 1, 0},
    {0xED, (uint8_t []){0x14}, 1, 0},
    {0xEE, (uint8_t []){0xFF}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0xFF}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x30}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x42}, 1, 0},
    {0x61, (uint8_t []){0xE0}, 1, 0},
    {0x62, (uint8_t []){0x40}, 1, 0},
    {0x63, (uint8_t []){0x40}, 1, 0},
    {0x64, (uint8_t []){0x02}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x40}, 1, 0},
    {0x67, (uint8_t []){0x03}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x42}, 1, 0},
    {0x71, (uint8_t []){0xE0}, 1, 0},
    {0x72, (uint8_t []){0x40}, 1, 0},
    {0x73, (uint8_t []){0x40}, 1, 0},
    {0x74, (uint8_t []){0x02}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x40}, 1, 0},
    {0x77, (uint8_t []){0x03}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x38}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x04}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xDC}, 1, 0},
    {0x85, (uint8_t []){0x00}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x38}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x06}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xDE}, 1, 0},
    {0x8D, (uint8_t []){0x00}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x38}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x08}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xE0}, 1, 0},
    {0x95, (uint8_t []){0x00}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x38}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0A}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xE2}, 1, 0},
    {0x9D, (uint8_t []){0x00}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x38}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x03}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xDB}, 1, 0},
    {0xA5, (uint8_t []){0x00}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x38}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x05}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xDD}, 1, 0},
    {0xAD, (uint8_t []){0x00}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x38}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x07}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xDF}, 1, 0},
    {0xB5, (uint8_t []){0x00}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x38}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x09}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xE1}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x22}, 1, 0},
    {0xC1, (uint8_t []){0xAA}, 1, 0},
    {0xC2, (uint8_t []){0x65}, 1, 0},
    {0xC3, (uint8_t []){0x74}, 1, 0},
    {0xC4, (uint8_t []){0x47}, 1, 0},
    {0xC5, (uint8_t []){0x56}, 1, 0},
    {0xC6, (uint8_t []){0x00}, 1, 0},
    {0xC7, (uint8_t []){0x88}, 1, 0},
    {0xC8, (uint8_t []){0x99}, 1, 0},
    {0xC9, (uint8_t []){0x33}, 1, 0},
    {0xD0, (uint8_t []){0x11}, 1, 0},
    {0xD1, (uint8_t []){0xAA}, 1, 0},
    {0xD2, (uint8_t []){0x65}, 1, 0},
    {0xD3, (uint8_t []){0x74}, 1, 0},
    {0xD4, (uint8_t []){0x47}, 1, 0},
    {0xD5, (uint8_t []){0x56}, 1, 0},
    {0xD6, (uint8_t []){0x00}, 1, 0},
    {0xD7, (uint8_t []){0x88}, 1, 0},
    {0xD8, (uint8_t []){0x99}, 1, 0},
    {0xD9, (uint8_t []){0x33}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    /* TEON. Inert on this board: no TE pin in the map above and
     * tear_avoid_mode = NONE, so nothing reads the signal. Kept because it is
     * part of the vendor's fixed Sitronix tail (F0/00 -> 35/00 -> 21/00 -> 11
     * -> 29) and sits in the correct place, before DISPON. Dropping it would
     * only make this table byte-identical to the official 1.85C's. */
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 0},
};

static const st77916_lcd_init_cmd_t s_init_version_2[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},   /* TEON, inert - see s_init_version_1 */
    {0x21, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 0},
};

static i2c_bus_handle_t s_i2c_bus;     /* espressif/i2c_bus: the one bus owner */
static i2c_master_bus_handle_t s_i2c;  /* its inner handle: touch, PMU */
static bq27220_handle_t s_gauge;       /* espressif/bq27220 fuel gauge */
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_tp_io;   /* CST816S register access */
static esp_lcd_panel_io_handle_t s_panel_io; /* kept for SLPIN/SLPOUT */
static esp_lcd_touch_handle_t s_tp;         /* read to release its INT */
static lv_indev_t *s_touch_indev;           /* LVGL's input device for the face */
static muse_gpio_button_t s_talk;

/* The 500 mAh single-cell LiPo this board ships with, written as the CEDV model
 * the BQ27220 gauges from. The capacity terms are this pack's; the
 * voltage/temperature terms are the reference cell the component carries in
 * test_apps/main/bq27220_test.c (they describe cell chemistry, not pack size).
 * They are a sane starting model, not a GPCCEDV-calibrated one - see
 * notes/20261018待办.md. */
static const parameter_cedv_t s_cedv = {
    .full_charge_cap = 500,
    .design_cap = 500,
    .reserve_cap = 0,
    .near_full = 150,
    .self_discharge_rate = 20,
    .EDV0 = 3490,
    .EDV1 = 3511,
    .EDV2 = 3535,
    .EMF = 3670,
    .C0 = 115,
    .R0 = 968,
    .T0 = 4547,
    .R1 = 4764,
    .TC = 11,
    .C1 = 0,
    .DOD0 = 4147,
    .DOD10 = 4002,
    .DOD20 = 3969,
    .DOD30 = 3938,
    .DOD40 = 3880,
    .DOD50 = 3824,
    .DOD60 = 3794,
    .DOD70 = 3753,
    .DOD80 = 3677,
    .DOD90 = 3574,
    .DOD100 = 3490,
};

static const gauging_config_t s_gauging = {
    .CCT = 1,
    .CSYNC = 0,
    .EDV_CMP = 0,
    .SC = 1,
    .FIXED_EDV0 = 0,
    .FCC_LIM = 1,
    .FC_FOR_VDQ = 1,
    .IGNORE_SD = 1,
    .SME0 = 0,
};

static esp_err_t init(void)
{
    /* The fuel gauge is espressif/bq27220, which drives its own device through
     * espressif/i2c_bus: that component creates the bus, and everything else on
     * it - touch, RTC, PMU - takes the inner i2c_master handle. One owner, one
     * bus, no second install on the same port. */
    const i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    s_i2c_bus = i2c_bus_create(I2C_NUM_0, &i2c_cfg);
    ESP_RETURN_ON_FALSE(s_i2c_bus != NULL, ESP_FAIL, TAG, "i2c bus");
    s_i2c = i2c_bus_get_internal_bus_handle(s_i2c_bus);
    ESP_RETURN_ON_FALSE(s_i2c != NULL, ESP_FAIL, TAG, "i2c master handle");
    /* Everything on this bus - the touch controller above all - goes quiet
     * together when the bus itself stops working, which is what a battery
     * session can leave behind (muse_board.h has the recovery). */
    muse_i2c_register(s_i2c, I2C_SDA, I2C_SCL);
    /* The clock at 0x51. Graceful when the chip is absent: the board then runs
     * on the system clock alone and shows no time until SNTP has run. */
    (void)muse_rtc_init(s_i2c);
    /* Light sleep: the RTC's INT is a GPIO wake source too, so an armed alarm
     * ends a nap as well as a deep sleep. Registered once here - the table is
     * fixed at boot, and the line only asserts while the chip has an alarm
     * enabled, so a standing registration costs nothing while it is off. Open
     * drain, so this is the pull-up variant of add(): the plain one pulls the
     * pin down and would hold INT asserted. */
    esp_err_t rtc_line = muse_gpio_wake_line_add_pullup(MUSE_RTC_INT_GPIO, false);
    if (rtc_line != ESP_OK) {
        ESP_LOGW(TAG, "RTC alarm wake line: %s", esp_err_to_name(rtc_line));
    }
    /* Hand the gauge its model. bq27220_create() checks what the chip already
     * stores and writes only when it differs, so this lands once and then logs
     * "Skip battery profile update" on later boots. That write is what fixes the
     * flat 33%: the chip shipped with a 3000 mAh model on a 500 mAh pack.
     * It reads the device number before touching anything, and on a cold boot
     * (pack rail just powered) that first MAC read has come back garbage and
     * refused the model once: retry, since the write is idempotent anyway. */
    for (int attempt = 0; attempt < 2 && s_gauge == NULL; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        s_gauge = bq27220_create(&(bq27220_config_t){
            .i2c_bus = s_i2c_bus,
            .cfg = &s_gauging,
            .cedv = &s_cedv,
        });
    }
    if (s_gauge == NULL) {
        ESP_LOGW(TAG, "gauge model not written: bq27220_create failed");
    }
    /* No separate device handle for the gauge any more: the component owns its
     * own on this bus, and read_power() only needs i2c_master_probe() to tell
     * "present" from "gone". The hand-rolled register path that needed one went
     * with the voltage fallback (1.4, notes/20261022-1.4正式版待办.md §4). */
    /* No IMU: the QMI8658A at 0x6B is fitted and answers, but nothing in 1.4
     * drives it. Pick-up-to-wake, the posture gate, face-down mute and the tap
     * engine were removed with their wake line (notes/20261022-1.4正式版待办.md
     * §1), which also retires GPIO8 as a light-sleep source: the only wake line
     * left on this board is BOOT. */
    esp_err_t err = muse_gpio_button_init(&s_talk, TALK_GPIO);
    if (err == ESP_OK) {
        /* BOOT is usually held while flashing or resetting, and button_init()
         * leaves pressed = false: the first debounced sample would then read as
         * a fresh press, and holding it 5 s reaches "reset setup" (which forgets
         * the pairing and Wi-Fi). Seed the state instead, as 1.75C/1.85C do. */
        s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    }
    return err;
}

/* CST816S register 0xFE, DisAutoSleep: 0x01 = never idle down (the controller
 * scans continuously), 0x00 = let it sleep between touches. The one write this
 * board makes to the controller, and it follows the screen - awake while the
 * panel is on so a tap on the settings list lands on the first try, asleep while
 * the panel is dark so the part stops drawing its 1.6~2.5 mA dynamic current.
 *
 * That 2 mA was the whole reason the touch wake line had to go: keeping the
 * controller responsive for a light-sleep wake meant leaving auto-sleep off
 * around the clock, which cost ~48 mAh/day out of a 500 mAh pack. With the line
 * gone (notes/20261022-1.4正式版待办.md §2) the controller is allowed to sleep
 * and the price is one swallowed touch after the screen goes dark - it wakes the
 * chip without reporting coordinates, which on a BOOT-first device is nothing. */
static void touch_autosleep(bool allow)
{
    if (s_tp_io == NULL) {
        return;
    }
    const uint8_t want = allow ? 0x00 : 0x01;
    uint8_t cur = 0xFF;
    const bool read_ok = esp_lcd_panel_io_rx_param(s_tp_io, 0xFE, &cur, 1) == ESP_OK;
    if (read_ok && cur == want) {
        return;   /* already in the state the screen asks for */
    }
    const esp_err_t err = esp_lcd_panel_io_tx_param(s_tp_io, 0xFE, &want, 1);
    ESP_LOGI(TAG, "touch: DisAutoSleep 0x%02X -> 0x%02X (%s, %s)", cur, want,
             allow ? "sleep allowed" : "kept awake", esp_err_to_name(err));
}

/* The CST816S reports a touch as a *low pulse* on IRQ rather than a held level.
 * The touch chip's own IRQ settings, read-only, logged for the record:
 * 0xA7 chip id, 0xED IrqPluseWidth (the low pulse a touch makes), 0xFA IrqCtl,
 * 0xFE DisAutoSleep. An earlier round *wrote* 0xEE here on the assumption it
 * was IrqPluseWidth: the vendor SDK's register table is shifted by one in
 * places and 0xEE is not that register. The write cost a night of the chip
 * waking the screen as soon as it went dark, so these are read, never poked,
 * until a datasheet settles their meaning - 0xFE is the one exception above. */
static void touch_report_config(void)
{
    static const struct {
        uint8_t reg;
        const char *name;
    } regs[] = {
        { 0xA7, "chip id" },
        { 0xED, "IrqPluseWidth" },
        { 0xFA, "IrqCtl" },
        { 0xFE, "DisAutoSleep" },
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint8_t v = 0;
        if (esp_lcd_panel_io_rx_param(s_tp_io, regs[i].reg, &v, 1) == ESP_OK) {
            ESP_LOGI(TAG, "touch: 0x%02X %s = 0x%02X", regs[i].reg, regs[i].name, v);
        } else {
            ESP_LOGW(TAG, "touch: 0x%02X %s not readable", regs[i].reg, regs[i].name);
        }
    }
    /* Auto-sleep (0xFE DisAutoSleep): 0x01 keeps the controller scanning all the
     * time, 0x00 lets it idle down between touches. It follows the screen, see
     * touch_autosleep() - here the panel is on, so the controller stays awake
     * and the first tap in the settings list lands. */
    touch_autosleep(false);
}

/* ST77916 windows must be even-aligned: QSPI carries two pixels per beat and
 * the driver's draw_bitmap() writes the window as given. Same callback the
 * official 1.75C/1.85C boards install. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        return NULL;
    }

    /* Reset the panel by hand first: the driver's own reset only happens once
     * the panel is created, which is after the 0x04 probe below. */
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    const spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }

    /* Ask the panel which revision it is, on a slow clock, then init with the
     * table that matches it (see the two tables above). */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    io_config.pclk_hz = ST77916_PROBE_HZ;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io) != ESP_OK) {
        return NULL;
    }
    uint8_t version[4] = { 0 };
    const int probe_cmd = ((ST77916_REG_VERSION & 0xff) << 8) | (ST77916_OPCODE_READ_CMD << 24);
    esp_err_t probe_err = esp_lcd_panel_io_rx_param(io, probe_cmd, version, sizeof(version));
    if (probe_err != ESP_OK) {
        /* One retry: the read is cheap and a silent fallback to the v2 table is
         * how a "garbled screen" report gets mistaken for normal. */
        vTaskDelay(pdMS_TO_TICKS(20));
        probe_err = esp_lcd_panel_io_rx_param(io, probe_cmd, version, sizeof(version));
    }
    const st77916_lcd_init_cmd_t *init_cmds = s_init_version_2;
    size_t init_cmds_size = sizeof(s_init_version_2) / sizeof(s_init_version_2[0]);
    const char *init_label = "v2";
    if (probe_err == ESP_OK && version[0] == 0x00 && version[1] == 0x7F &&
        version[2] == 0x7F && version[3] == 0x7F) {
        init_cmds = s_init_version_1;
        init_cmds_size = sizeof(s_init_version_1) / sizeof(s_init_version_1[0]);
        init_label = "v1";
    }
    /* Kconfig override: a third panel revision would read a 0x04 value this
     * probe has never seen, and neither known table would fit. Forcing one
     * (menuconfig > Muse > ST77916 init table) turns "garbled screen, no spare
     * board" into a one-line change and a reflash. AUTO keeps the probe. */
#if CONFIG_MUSE_LCD185B_INIT_V1
    init_cmds = s_init_version_1;
    init_cmds_size = sizeof(s_init_version_1) / sizeof(s_init_version_1[0]);
    init_label = "v1 (forced)";
#elif CONFIG_MUSE_LCD185B_INIT_V2
    init_label = "v2 (forced)";
#elif CONFIG_MUSE_LCD185B_INIT_ECHOEAR
    init_cmds = echoear_init_cmds;
    init_cmds_size = sizeof(echoear_init_cmds) / sizeof(echoear_init_cmds[0]);
    init_label = "echoear (forced)";
#endif
    if (probe_err != ESP_OK) {
        ESP_LOGW(TAG, "panel revision unknown (probe %s, readback %02X %02X %02X %02X): "
                      "using init table %s", esp_err_to_name(probe_err),
                 version[0], version[1], version[2], version[3], init_label);
    } else {
        ESP_LOGI(TAG, "ST77916 reg 0x%02X = %02X %02X %02X %02X -> init table %s", ST77916_REG_VERSION,
                 version[0], version[1], version[2], version[3], init_label);
    }
    esp_lcd_panel_io_del(io);

    /* Now put the device back on the fast clock for the draw path. */
    io_config.pclk_hz = 80 * 1000 * 1000;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io) != ESP_OK) {
        return NULL;
    }
    s_panel_io = io;   /* the sleep commands below go out through this one */
    const st77916_vendor_config_t vendor_config = {
        .init_cmds = init_cmds,
        .init_cmds_size = init_cmds_size,
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    if (esp_lcd_new_panel_st77916(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* If red and blue are swapped on first boot, flip rgb_ele_order to BGR. */
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    /* Bands, not esp_lv_adapter_register_display(): it sets the profile's
     * buffer_height/use_psram/double-buffer itself (muse_lcd_bands.h). This
     * runs in boot_task, pinned to MUSE_UI_CORE, so the panel's SPI interrupt
     * is set up beside the task that sends to it. */
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_cfg.dev_addr = TP_ADDR;
    tp_io_cfg.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t tp_io;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TP_RST,
        .int_gpio_num = TP_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
    };
    esp_lcd_touch_handle_t tp;
    if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp) != ESP_OK) {
        return NULL;
    }
    s_tp_io = tp_io;
    s_tp = tp;
    touch_report_config();
    const esp_lv_adapter_touch_config_t lv_tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    *touch = esp_lv_adapter_register_touch(&lv_tp_cfg);
    s_touch_indev = *touch;   /* poll_buttons() reads its state for hold-to-talk */
    /* The touch line is deliberately NOT a wake source, and 1.4 makes that
     * final: a hand resting near the glass, a stray pulse from the controller
     * and a screen that never stayed dark were all traced back to it, and the
     * same touches produced empty voice notes. The price was worse than that -
     * a wake line needs the controller responsive, which meant auto-sleep off
     * around the clock at 1.6~2.5 mA (touch_autosleep). The screen wakes on
     * BOOT alone, and the touch panel only reports while the display runs. To
     * bring it back, register TP_INT with muse_gpio_wake_line_add_wake_only()
     * here and the input task's check_touch_line() picks it up unchanged - but
     * then touch_autosleep(true) has to stop, or the line will not fire. */
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "backlight %d%%", pct);
}

/* Past muse_lcd_bands_register(), every use of the panel and its IO has to run
 * on the task that owns the bus (muse_lcd_bands.h), so wrap these. */
static void send_panel_sleep(void *sleep)
{
    /* Raw SLPIN (0x10) / SLPOUT (0x11) on the QSPI command path (opcode 0x02).
     * The driver's own esp_lcd_panel_disp_sleep() is a no-op here: esp_lcd_st77916
     * 2.0.2 registers del/reset/init/draw_bitmap/disp_on_off and no base.disp_sleep,
     * so the call came back ESP_ERR_NOT_SUPPORTED and the panel kept its charge
     * pump and bias running (the official 1.75C sends these two commands for the
     * same reason). */
    if (s_panel_io == NULL) {
        return;
    }
    const bool in = *(bool *)sleep;
    esp_lcd_panel_io_tx_param(s_panel_io, (0x02u << 24) | ((in ? 0x10u : 0x11u) << 8), NULL, 0);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_panel_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* SLPIN/SLPOUT settle, as 1.75C/1.85C do */
    /* The controller idles down with the panel and comes back with it: this is
     * the ~2 mA the touch wake line used to cost (touch_autosleep). Runs on the
     * UI task, the same one that owns the touch driver's reads. */
    touch_autosleep(sleep);
    if (!sleep) {
        /* Coming back, the CST816S answers the first transaction with garbage
         * (the "panel_io_i2c_*: i2c transaction failed" / 0xC0 readbacks in the
         * log) and pulses its INT on the way up. The adapter reads that pulse
         * as a finger, and a press left set in the indev shows up later as a
         * hold-to-talk - which started a conversation over the alarm ring.
         * Drain the controller and drop whatever press the indev is holding. */
        if (s_tp_io != NULL) {
            uint8_t status = 0;
            (void)esp_lcd_panel_io_rx_param(s_tp_io, 0x01, &status, 1);   /* status: clears INT */
        }
        if (s_touch_indev != NULL) {
            lv_indev_reset(s_touch_indev, NULL);
        }
    }
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

#if CONFIG_MUSE_POWER_TEST
static int s_ptest_apply_step; /* 0 = none; last apply step attempted */
static i2s_chan_handle_t s_ptest_tx, s_ptest_rx;
static bool s_ptest_i2s_stopped;
static const audio_codec_if_t *s_ptest_dac_if, *s_ptest_adc_if;
#endif

/* ES8311 out and ES7210 in on one duplex I2S bus. The ES7210's second
 * channel carries the speaker reference (xiaozhi uses it for echo
 * cancelling); Muse takes slot 0 (MIC1).
 *
 * Deliberate differences from the official 1.85C (same panel, same codecs),
 * checked line by line - do not "align" them without reading this:
 *
 *  - I2S_NUM_0 here, I2S_NUM_1 there. The S3's two ports are equivalent; this
 *    board took the port from board_vn_s3_183.c, which devices/AGENTS.md names
 *    as this port's audio reference, and 1.3 shipped on it.
 *  - use_mclk = true here, false there. This board wires MCLK to GPIO2, so the
 *    codec can run off it instead of deriving a clock from BCLK. 1.85C drives
 *    the same pin but tells the codec to ignore it; both lock, this one has
 *    the lower jitter.
 *  - hw_gain {5.0, 3.3} here, unset there. That pair is the library default,
 *    not a measurement: esp_codec_dev_vol.c:54-59 substitutes 5.0 V for
 *    pa_voltage when it is 0, so 1.85C's zeroed struct and this explicit one
 *    produce the same volume curve. Spelling it out keeps the assumption
 *    visible; it is NOT this board's PA rail (that one still needs measuring
 *    before anyone tunes the volume map).
 */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
#if CONFIG_MUSE_POWER_TEST
    /* Publish ownership BEFORE init/enable: partial TX/RX failures must still
     * be tracked and stopped by the standalone diagnostic cleanup path. */
    s_ptest_tx = tx;
    s_ptest_rx = rx;
    s_ptest_i2s_stopped = false;
#endif
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
#if CONFIG_MUSE_POWER_TEST
        /* ptest owns PA_EN as a plain GPIO so "codecs open, amp off" is a
         * real state; the ES8311 driver must not flip it on open/close. */
        .pa_pin = GPIO_NUM_NC,
#else
        .pa_pin = PA_EN,
#endif
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
#if CONFIG_MUSE_POWER_TEST
    s_ptest_dac_if = dac; /* own low-level disable even if wrapper allocation fails */
#endif
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
#if CONFIG_MUSE_POWER_TEST
    s_ptest_adc_if = adc;
#endif
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    /* One line that makes the audio path auditable from the boot log alone:
     * the first thing a "no sound" report needs is how this build is wired,
     * compared against the 1.85C's I2S1 / mclk=derived / no hw_gain. */
    ESP_LOGI(TAG, "audio: ES8311 spk + ES7210 mic, I2S%d %d Hz, mclk=external GPIO%d, "
                  "pa=GPIO%d, hw_gain=%.1f/%.1f V, mic slot 0",
             (int)I2S_NUM_0, (int)MUSE_AUDIO_RATE, (int)I2S_MCLK, (int)PA_EN, 5.0f, 3.3f);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* The settings page offers 3 dB steps; the ES7210's PGA moves in 3 dB steps and
 * esp_codec_dev rounds 33 dB down to 30, so snap to the step and hand 34.5 for
 * the 33 dB slot - otherwise the slider shows a gain the codec never got. */
static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    db = (db / 3) * 3;
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

/* Hold-to-talk on the face tile: a finger held on the avatar for TOUCH_HOLD_MS
 * acts as the talk button. A tap, or a press that starts on another tile (the
 * settings list), never talks. The state comes from LVGL's input device, which
 * owns the CST816S handle - reading the driver here would race LVGL's reads. */
#define TOUCH_HOLD_MS 500

static unsigned poll_buttons(void)
{
    unsigned ev = muse_gpio_button_poll(&s_talk);

    static bool tp_down = false;          /* finger currently on the screen */
    static bool tp_talking = false;       /* hold threshold reached */
    static TickType_t tp_press_tick = 0;  /* when the current press began */
    static bool tp_on_face = false;       /* the press started on the face tile */

    bool pressed = false;
    if (s_touch_indev != NULL) {
        pressed = (lv_indev_get_state(s_touch_indev) == LV_INDEV_STATE_PRESSED);
    }
    if (pressed && !tp_down) {
        /* New press: a hold that starts on the face tile talks - unless it
         * started on one of the face's own controls (the speaker button, the
         * power icon). Those own their presses: a hold on the volume button is
         * its long-press, and a tap on the power icon is a power-off. */
        lv_point_t p = { 0 };
        lv_indev_get_point(s_touch_indev, &p);
        tp_down = true;
        tp_on_face = muse_ui_face_active() && !muse_ui_face_control_at(p.x, p.y);
        tp_press_tick = xTaskGetTickCount();
        tp_talking = false;
    } else if (!pressed && tp_down) {
        /* Release: end the talk if one started. */
        if (tp_talking) {
            ev |= MUSE_BTN_TALK_RELEASE;
        }
        tp_down = false;
        tp_talking = false;
    } else if (pressed && tp_down && tp_on_face && !tp_talking) {
        /* Still held on the face: start talking once the threshold passes. */
        if ((xTaskGetTickCount() - tp_press_tick) >= pdMS_TO_TICKS(TOUCH_HOLD_MS)) {
            tp_talking = true;
            ev |= MUSE_BTN_TALK_PRESS;
        }
    }
    return ev;
}

/* The bezel button only - the glass is the adapter's business. The alarm ring
 * wants the difference: this one stops it at once, a touch press has to outlast
 * the panel's own wake noise first (muse_input.c, ALARM_TOUCH_GRACE_US). */
static bool talk_down(void)
{
    /* The debounced state, not the raw level: the pin is the BOOT strap and
     * glitches low for a sample now and then (one such glitch stopped a ring),
     * while s_talk.pressed only changes after DEBOUNCE_SAMPLES polls. */
    return s_talk.pressed;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk }, 1, timeout_ms);
}

/* 7-bit addresses for i2c_master_probe(). The vendor docs print the 8-bit
 * forms (0x30 / 0x80); the codec handles elsewhere in this file take those,
 * because audio_codec_new_i2c_ctrl() wants the shifted address. */
#define ES8311_ADDR7 0x18
#define ES7210_ADDR7 0x40

/* Both codecs share the bus with the gauge. An I2C bus reset only resets the
 * ESP32's own peripheral - the slaves keep their registers - so a recovery
 * normally leaves audio alone, which is why this does nothing most of the time.
 * What a reset cannot fix is a slave whose state machine latched mid-transfer:
 * it stops answering and every later volume or gain write fails silently, with
 * the UI still showing "speaking". So once per recovery, probe both and say
 * which one went quiet; if one did, re-open the audio path - esp_codec_dev_open
 * rewrites the whole register set - rather than leaving a dead speaker until
 * the next reboot.
 *
 * Only re-opens when a codec is already not answering: a live take is lost by
 * then anyway, and doing it on a healthy bus would cut a recording short.
 * Called from read_power(), which the input task runs periodically. */
static void check_codecs_after_recovery(void)
{
    static unsigned seen;
    const unsigned recovers = muse_i2c_recovers();
    if (recovers == seen) {
        return;
    }
    seen = recovers;
    const bool spk = i2c_master_probe(s_i2c, ES8311_ADDR7, 20) == ESP_OK;
    const bool mic = i2c_master_probe(s_i2c, ES7210_ADDR7, 20) == ESP_OK;
    ESP_LOGW(TAG, "after bus reset #%u: ES8311 %s, ES7210 %s", recovers,
             spk ? "answers" : "SILENT", mic ? "answers" : "SILENT");
    if (spk && mic) {
        return;
    }
    /* Asleep, the voice task has both codecs closed on purpose; reopening here
     * would power them back up behind its back. Waking re-opens them anyway. */
    if (muse_state_asleep()) {
        ESP_LOGW(TAG, "codec re-open deferred: asleep");
        return;
    }
    muse_audio_power(false);
    muse_audio_power(true);
    ESP_LOGW(TAG, "audio re-opened after bus reset #%u: ES8311 %s, ES7210 %s", recovers,
             i2c_master_probe(s_i2c, ES8311_ADDR7, 20) == ESP_OK ? "answers" : "SILENT",
             i2c_master_probe(s_i2c, ES7210_ADDR7, 20) == ESP_OK ? "answers" : "SILENT");
}

/* BQ27220 fuel gauge, read entirely through espressif/bq27220's getters. The
 * component owns the register map, the CEDV model and the byte order; the only
 * thing this file still decides is what a reading means for the UI. Its model is
 * written from this pack's capacity at init (s_cedv), so the percentage is the
 * gauge's computation, not a voltage estimate: a gauge that answers out of range
 * is reported as unknown (-1) rather than papered over with millivolts. */
static esp_err_t read_power(muse_power_t *out)
{
    check_codecs_after_recovery();
    if (i2c_master_probe(s_i2c, BQ27220_ADDR, 20) != ESP_OK) {
        /* The pad levels go in the line: a bus that is held low by a slave and
         * one whose pads lost their pull-ups look the same from here. */
        int sda, scl;
        muse_i2c_levels(&sda, &scl);
        ESP_LOGW(TAG, "BQ27220 not answering at 0x%02x (sda=%d scl=%d)", BQ27220_ADDR, sda, scl);
        muse_i2c_report(false);
        return ESP_ERR_NOT_FOUND;
    }
    muse_i2c_report(true);
    if (s_gauge == NULL) {
        /* bq27220_create() failed at init, so there is no model and no honest
         * percentage. Report nothing rather than a voltage guess - the same rule
         * the 0 mV case below follows. */
        ESP_LOGW(TAG, "gauge handle missing: battery unknown");
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t soc = bq27220_get_state_of_charge(s_gauge);
    const uint16_t mv = bq27220_get_voltage(s_gauge);
    /* The component's getters swallow the transfer error and return 0, and no
     * real cell reads 0 mV: both zero means the bus, not the battery. Report it
     * so the caller keeps the last valid reading instead of showing 0%. */
    if (soc == 0 && mv == 0) {
        muse_i2c_report(false);
        ESP_LOGW(TAG, "gauge read 0 mV / 0%%: bus glitch, keeping the last value");
        return ESP_ERR_INVALID_RESPONSE;
    }
    battery_status_t bs;
    if (bq27220_get_battery_status(s_gauge, &bs) != ESP_OK) {
        bs.full = 0;   /* unknown: read as "not charging" below, which is the
                        * safe direction - a charger that is there will be seen
                        * on the next poll. */
    }
    out->battery_pct = soc <= 100 ? (int)soc : -1;
    out->battery_mv = (int)mv;
    /* BatteryStatus bit0 is DSG: clear while the charger is pushing current in. */
    out->charging = !bs.DSG;
    out->usb = usb_serial_jtag_is_connected();
    /* The gauge's coulomb counter and thermistor, for the night recorder: mA
     * (negative while discharging), and 0.1 K which becomes 0.1 C. Neither is
     * shown on screen, so a gauge that returns nonsense costs nothing here -
     * the sampler records it as-is and the bench reads the trend. */
    out->battery_ma = (int)bq27220_get_avgcurrent(s_gauge);
    out->battery_temp_c10 = (int)bq27220_get_temperature(s_gauge) - 2732;
    return ESP_OK;
}

/* Every reading the component exposes, named and unit-tagged so the dump reads
 * without a datasheet at hand. It used to walk the TRM's standard-command table
 * by hand (bq27220_read_word, since deleted): the three rows with no getter -
 * StandbyTimeToEmpty, MaxLoadTimeToEmpty, RawCoulombCount - went with it, they
 * were diagnostics nobody acted on. */
static void dump_gauge(void)
{
    if (s_gauge == NULL) {
        ESP_LOGW(TAG, "BQ27220: no handle (bq27220_create failed at init)");
        return;
    }
    battery_status_t bs = { 0 };
    (void)bq27220_get_battery_status(s_gauge, &bs);
    ESP_LOGI(TAG, "BQ27220 via espressif/bq27220:");
    ESP_LOGI(TAG, "  Voltage              %5u mV", bq27220_get_voltage(s_gauge));
    ESP_LOGI(TAG, "  Current              %5d mA (+ charging)", bq27220_get_current(s_gauge));
    ESP_LOGI(TAG, "  AverageCurrent       %5d mA", bq27220_get_avgcurrent(s_gauge));
    ESP_LOGI(TAG, "  AveragePower         %5d mW", bq27220_get_average_power(s_gauge));
    ESP_LOGI(TAG, "  RemainingCapacity    %5u mAh", bq27220_get_remaining_capacity(s_gauge));
    ESP_LOGI(TAG, "  FullChargeCapacity   %5u mAh", bq27220_get_full_charge_capacity(s_gauge));
    ESP_LOGI(TAG, "  DesignCapacity       %5u mAh", bq27220_get_design_capacity(s_gauge));
    ESP_LOGI(TAG, "  StateOfCharge        %5u %%", bq27220_get_state_of_charge(s_gauge));
    ESP_LOGI(TAG, "  StateOfHealth        %5u %%", bq27220_get_state_of_health(s_gauge));
    ESP_LOGI(TAG, "  CycleCount           %5u", bq27220_get_cycle_count(s_gauge));
    ESP_LOGI(TAG, "  InternalTemperature  %5u 0.1K (%d.%d C)",
             bq27220_get_temperature(s_gauge),
             (int)(bq27220_get_temperature(s_gauge) / 10 - 273),
             (int)(bq27220_get_temperature(s_gauge) % 10));
    ESP_LOGI(TAG, "  TimeToEmpty          %5u min", bq27220_get_time_to_empty(s_gauge));
    ESP_LOGI(TAG, "  TimeToFull           %5u min", bq27220_get_time_to_full(s_gauge));
    ESP_LOGI(TAG, "  StandbyCurrent       %5d mA", bq27220_get_standby_current(s_gauge));
    ESP_LOGI(TAG, "  MaxLoadCurrent       %5d mA", bq27220_get_maxload_current(s_gauge));
    ESP_LOGI(TAG, "  BatteryStatus        0x%04X: DSG=%d FC=%d TCA=%d TDA=%d BATTPRES=%d "
                  "CHGINH=%d SLEEP=%d OTC=%d OTD=%d",
             bs.full, (int)bs.DSG, (int)bs.FC, (int)bs.TCA, (int)bs.TDA, (int)bs.BATTPRES,
             (int)bs.CHGINH, (int)bs.SLEEP, (int)bs.OTC, (int)bs.OTD);
}


static void send_disp_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
}

static esp_err_t power_off(void)
{
    /* 1.5b: leave Wi-Fi cleanly before deep sleep. The driver used to be
     * left associated when the chip folded, and the next boot's join could
     * then spin against the AP's stale association. A failure here only
     * logs: it must never block powering off. */
    esp_wifi_disconnect();
    esp_err_t wifi_err = esp_wifi_stop();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep: wifi stop failed: %s", esp_err_to_name(wifi_err));
    }
    set_brightness(0);
    /* SLPIN, not just DISPOFF: DISPOFF blanks the picture and leaves the
     * panel's charge pump and bias running. GRAM is lost either way and the
     * next boot re-inits the panel. */
    bool sleep = true;
    muse_lcd_bands_run(send_panel_sleep, &sleep);
    muse_lcd_bands_run(send_disp_off, NULL);
    /* The amp must not be left enabled: PA_EN high is tens of mA in "off". */
    gpio_set_direction(PA_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(PA_EN, 0);
    /* ext0 reads the pin through the RTC domain, where the digital pull-up is
     * gone. Without this a floating BOOT wakes the chip the moment it sleeps. */
    rtc_gpio_pullup_en(TALK_GPIO);
    rtc_gpio_pulldown_dis(TALK_GPIO);
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    /* ext1, not ext0, so the RTC alarm can share the deep-sleep mask with the
     * button. Both lines are active low, and the RTC_INT bit is only added when
     * an alarm is actually armed - with none armed the mask is the single BOOT
     * bit ext0 used to watch, so button behaviour is unchanged. */
    uint64_t wake_mask = BIT64(TALK_GPIO);
    if (muse_rtc_wake_armed()) {
        /* Open drain, and the RTC domain has no digital pull-up. */
        rtc_gpio_pullup_en(MUSE_RTC_INT_GPIO);
        rtc_gpio_pulldown_dis(MUSE_RTC_INT_GPIO);
        ESP_LOGI(TAG, "deep sleep: alarm armed, GPIO%d level %d", (int)MUSE_RTC_INT_GPIO,
                 gpio_get_level(MUSE_RTC_INT_GPIO));
        wake_mask |= BIT64(MUSE_RTC_INT_GPIO);
    } else {
        ESP_LOGI(TAG, "deep sleep: no alarm armed, BOOT only");
    }
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW),
                        TAG, "wake button");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

#if CONFIG_MUSE_POWER_TEST
/* Diagnostic-only hooks (boards/muse_power_test_board.h). Bench console: the
 * framework speaks UART on CONFIG_ESP_CONSOLE_UART_NUM - UART0 on this board,
 * GPIO43/44 - so the bench attaches a 3.3 V USB-UART to the TXD/RXD pads and
 * the sweep starts when VBUS drops. Everything below is isolated: no LVGL,
 * no touch polling, no voice/network, no production NVS. The panel brings
 * itself up per state through the same revision probe and init tables the
 * shipped display_start() uses; the touch controller is never initialized
 * and is left at its auto-sleep power-on default.
 *
 * Unverified here (no schematic read, so nothing is claimed): whether the
 * I2C bus has external pull-ups (pulls_off caveat in the matrix), and which
 * GPIOs are genuinely unconnected (no unused_hiz knob at all). */
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"

static bool s_ptest_lcd_on, s_ptest_pwm, s_ptest_audio_attempted, s_ptest_audio_failed;
static bool s_ptest_audio_poisoned, s_ptest_usb_dryrun, s_ptest_amp_on;
void muse_ptest_board_usb_dryrun(bool on) { s_ptest_usb_dryrun = on; }
static muse_ptest_codec_regs_t s_ptest_codec_regs;
static bool s_ptest_spk_open, s_ptest_mic_open;
static muse_ptest_codec_t s_ptest_codec_state = MUSE_PTEST_CODEC_COLD;
static uint16_t s_ptest_knobs;
static bool s_ptest_i2s_parked, s_ptest_uart_parked;
static uint64_t s_ptest_held_mask;
#define PTEST_HOLD_MAGIC 0x50544844u
#define PTEST_WARM_MAGIC 0x5054574du
static RTC_NOINIT_ATTR volatile struct { uint32_t magic, inverse, warm_magic, warm_inverse; } s_ptest_hold_owner;
_Static_assert(sizeof(s_ptest_hold_owner) == MUSE_PTEST_RTC_RECOVERY_BYTES, "recovery markers RTC budget");
static void ptest_mark_warm(void)
{
    s_ptest_hold_owner.warm_inverse = ~PTEST_WARM_MAGIC;
    s_ptest_hold_owner.warm_magic = PTEST_WARM_MAGIC;
    __sync_synchronize(); /* before ANY constructor/partial hardware write */
}
bool muse_ptest_board_warm_seen(void)
{
    return s_ptest_hold_owner.warm_magic == PTEST_WARM_MAGIC && s_ptest_hold_owner.warm_inverse == ~PTEST_WARM_MAGIC;
}
bool muse_ptest_board_deep_holds_owned(void)
{
    return s_ptest_hold_owner.magic == PTEST_HOLD_MAGIC && s_ptest_hold_owner.inverse == ~PTEST_HOLD_MAGIC;
}

/* Only the vendor's own audio pads are ever parked: I2S MCLK/BCLK/WS/DOUT/
 * DIN (Waveshare pin map, file header). The LCD data lines 45/46 and reset 3
 * are strapping-connected and must stay high-Z through reset, so they are
 * never parked or held. The I2C pair gets the pulls-off treatment only. */
static const gpio_num_t s_ptest_i2s_pins[] = { I2S_MCLK, I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN };
static const gpio_num_t s_ptest_pull_pins[] = { I2C_SDA, I2C_SCL };
/* Deep-sleep hold set: the four driven I2S outputs, the backlight pin (LEDC
 * at duty 0 latches low) and PA_EN (amp off = low). Never LCD data/reset. */
static const gpio_num_t s_ptest_hold_pins[] = { I2S_MCLK, I2S_BCLK, I2S_WS, I2S_DOUT, LCD_BL, PA_EN };
typedef struct { gpio_num_t pin; gpio_io_config_t io; uint32_t matrix_out; int level; } ptest_pin_save_t;
static ptest_pin_save_t s_ptest_i2s_save[5], s_ptest_pull_save[2];
static size_t s_ptest_i2s_saved, s_ptest_pull_saved;

static int ptest_latch(gpio_num_t pin)
{
    return pin < 32 ? (GPIO.out >> pin) & 1 : (GPIO.out1.val >> (pin - 32)) & 1;
}
static esp_err_t ptest_snapshot(const gpio_num_t *pins, size_t n, ptest_pin_save_t *saved, size_t *count)
{
    *count = 0;
    for (size_t i = 0; i < n; i++) {
        saved[i].pin = pins[i];
        ESP_RETURN_ON_ERROR(gpio_get_io_config(pins[i], &saved[i].io), TAG, "pin snapshot");
        saved[i].matrix_out = GPIO.func_out_sel_cfg[pins[i]].val;
        saved[i].level = ptest_latch(pins[i]);
        (*count)++;
    }
    return ESP_OK;
}
static esp_err_t ptest_pin(gpio_num_t pin, gpio_mode_t mode, bool down)
{
    if (mode & GPIO_MODE_OUTPUT) { ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), TAG, "pin low latch"); }
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(pin), .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "pin mode");
    if (rtc_gpio_is_valid_gpio(pin)) {
        ESP_RETURN_ON_ERROR(rtc_gpio_pullup_dis(pin), TAG, "RTC pullup off");
        ESP_RETURN_ON_ERROR(down ? rtc_gpio_pulldown_en(pin) : rtc_gpio_pulldown_dis(pin), TAG, "RTC pulldown");
    }
    return ESP_OK;
}
static esp_err_t ptest_restore_pins(ptest_pin_save_t *saved, size_t *count)
{
    for (size_t i = 0; i < *count; i++) {
        ptest_pin_save_t *s = &saved[i];
        gpio_mode_t mode = (s->io.ie ? GPIO_MODE_INPUT : 0) | (s->io.oe ? GPIO_MODE_OUTPUT : 0);
        if (s->io.od) { mode |= GPIO_MODE_DEF_OD; }
        ESP_RETURN_ON_ERROR(gpio_set_level(s->pin, s->level), TAG, "restore latch");
        const gpio_config_t cfg = { .pin_bit_mask = BIT64(s->pin), .mode = mode,
            .pull_up_en = s->io.pu, .pull_down_en = s->io.pd, .intr_type = GPIO_INTR_DISABLE };
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "restore pin");
        ESP_RETURN_ON_ERROR(gpio_set_drive_capability(s->pin, s->io.drv), TAG, "restore drive");
        /* gpio_config disconnects output matrices. Restore the exact stopped
         * I2S (or boot-default) routing, polarity and peripheral OE selection. */
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[s->pin], s->io.fun_sel);
        GPIO.func_out_sel_cfg[s->pin].val = s->matrix_out;
        ESP_RETURN_ON_ERROR(s->io.slp_sel ? gpio_sleep_sel_en(s->pin) : gpio_sleep_sel_dis(s->pin), TAG, "restore SLP_SEL");
        if (rtc_gpio_is_valid_gpio(s->pin)) {
            ESP_RETURN_ON_ERROR(s->io.pu ? rtc_gpio_pullup_en(s->pin) : rtc_gpio_pullup_dis(s->pin), TAG, "restore RTC PU");
            ESP_RETURN_ON_ERROR(s->io.pd ? rtc_gpio_pulldown_en(s->pin) : rtc_gpio_pulldown_dis(s->pin), TAG, "restore RTC PD");
        }
    }
    *count = 0;
    return ESP_OK;
}

esp_err_t muse_ptest_board_uart_restore(void)
{
    if (!s_ptest_uart_parked) { return ESP_OK; }
    /* Console default pins for UART0; the header pads are TXD(43)/RXD(44). */
    ESP_RETURN_ON_ERROR(uart_set_pin(CONFIG_ESP_CONSOLE_UART_NUM, GPIO_NUM_43, GPIO_NUM_44,
                                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "restore UART0");
    s_ptest_uart_parked = false;
    s_ptest_knobs &= ~MUSE_PTEST_UART_HIZ;
    return ESP_OK;
}

esp_err_t muse_ptest_board_release_deep_holds(void)
{
    /* Called on boot only when the valid journal owns these holds. Program
     * the same low state before release, avoiding an output glitch. */
    for (size_t i = 0; i < sizeof(s_ptest_hold_pins) / sizeof(s_ptest_hold_pins[0]); i++) {
        gpio_num_t pin = s_ptest_hold_pins[i];
        ESP_RETURN_ON_ERROR(ptest_pin(pin, GPIO_MODE_OUTPUT, false), TAG, "held pin low");
        ESP_RETURN_ON_ERROR(gpio_hold_dis(pin), TAG, "release held pin");
        /* Resting config: I2S pads float (no driver owns them before audio
         * init); amp and backlight stay driven low, as production's
         * power_off() leaves the amp. */
        if (pin == LCD_BL || pin == PA_EN) {
            ESP_RETURN_ON_ERROR(ptest_pin(pin, GPIO_MODE_OUTPUT, false), TAG, "safe low");
        } else {
            ESP_RETURN_ON_ERROR(ptest_pin(pin, GPIO_MODE_DISABLE, false), TAG, "released pin safe hiz");
        }
    }
    gpio_deep_sleep_hold_dis();
    __sync_synchronize();
    s_ptest_hold_owner.magic = 0; /* clear ownership only after every release */
    s_ptest_hold_owner.inverse = 0;
    s_ptest_held_mask = 0;
    s_ptest_knobs &= ~MUSE_PTEST_GPIO_HOLD;
    return ESP_OK;
}

static esp_err_t ptest_revert_knobs(void)
{
    esp_sleep_enable_gpio_switch(false);
    if (s_ptest_held_mask || muse_ptest_board_deep_holds_owned()) { ESP_RETURN_ON_ERROR(muse_ptest_board_release_deep_holds(), TAG, "release own holds"); }
    ESP_RETURN_ON_ERROR(muse_ptest_board_uart_restore(), TAG, "UART unpark");
    ESP_RETURN_ON_ERROR(ptest_restore_pins(s_ptest_i2s_save, &s_ptest_i2s_saved), TAG, "I2S unpark");
    s_ptest_i2s_parked = false;
    ESP_RETURN_ON_ERROR(ptest_restore_pins(s_ptest_pull_save, &s_ptest_pull_saved), TAG, "pull restore");
    s_ptest_knobs = 0;
    return ESP_OK;
}

static esp_err_t ptest_apply_knobs(uint16_t knobs)
{
    /* This board implements no RGB LED knob and no unused-pad knob (nothing
     * verified unconnected); reject rather than silently ignore. */
    const uint16_t known = MUSE_PTEST_I2S_LOW | MUSE_PTEST_I2S_HIZ | MUSE_PTEST_UART_HIZ |
                           MUSE_PTEST_PULLS_OFF | MUSE_PTEST_GPIO_ISOLATE | MUSE_PTEST_GPIO_HOLD;
    if (knobs & ~known) { return ESP_ERR_INVALID_ARG; }
    if ((knobs & (MUSE_PTEST_I2S_LOW | MUSE_PTEST_I2S_HIZ)) == (MUSE_PTEST_I2S_LOW | MUSE_PTEST_I2S_HIZ)) { return ESP_ERR_INVALID_ARG; }
    if (knobs & (MUSE_PTEST_I2S_LOW | MUSE_PTEST_I2S_HIZ)) {
        ESP_RETURN_ON_FALSE(!s_ptest_spk_open && !s_ptest_mic_open, ESP_ERR_INVALID_STATE, TAG, "park open codecs");
        ESP_RETURN_ON_ERROR(ptest_snapshot(s_ptest_i2s_pins, 5, s_ptest_i2s_save, &s_ptest_i2s_saved), TAG, "save I2S");
        s_ptest_i2s_parked = true;
        for (size_t i = 0; i < 5; i++) {
            bool low = (knobs & MUSE_PTEST_I2S_LOW) != 0;
            ESP_RETURN_ON_ERROR(ptest_pin(s_ptest_i2s_pins[i], !low ? GPIO_MODE_DISABLE : i == 4 ? GPIO_MODE_INPUT : GPIO_MODE_OUTPUT, low && i == 4), TAG, "park I2S");
        }
        s_ptest_knobs |= knobs & (MUSE_PTEST_I2S_LOW | MUSE_PTEST_I2S_HIZ);
    }
    if (knobs & MUSE_PTEST_UART_HIZ) {
        muse_ptest_readback_t p = {0};
        ESP_RETURN_ON_ERROR(muse_ptest_board_power(&p), TAG, "park UART VBUS");
        ESP_RETURN_ON_FALSE(!p.usb || s_ptest_usb_dryrun, ESP_ERR_INVALID_STATE, TAG, "UART on USB");
        /* uart_write_bytes can return with a FIFO tail still on the wire. */
        ESP_RETURN_ON_ERROR(uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(1000)), TAG, "UART TX drain before parking");
        s_ptest_uart_parked = true; /* publish before either pin changes */
        ESP_RETURN_ON_ERROR(ptest_pin(GPIO_NUM_43, GPIO_MODE_DISABLE, false), TAG, "UART TX hiz");
        ESP_RETURN_ON_ERROR(ptest_pin(GPIO_NUM_44, GPIO_MODE_DISABLE, false), TAG, "UART RX hiz");
        s_ptest_knobs |= MUSE_PTEST_UART_HIZ;
    }
    if (knobs & MUSE_PTEST_PULLS_OFF) {
        ESP_RETURN_ON_ERROR(ptest_snapshot(s_ptest_pull_pins, 2, s_ptest_pull_save, &s_ptest_pull_saved), TAG, "save pulls");
        for (size_t i = 0; i < 2; i++) {
            gpio_num_t pin = s_ptest_pull_pins[i];
            ESP_RETURN_ON_ERROR(gpio_pullup_dis(pin), TAG, "digital PU off");
            ESP_RETURN_ON_ERROR(gpio_pulldown_dis(pin), TAG, "digital PD off");
            if (rtc_gpio_is_valid_gpio(pin)) {
                ESP_RETURN_ON_ERROR(rtc_gpio_pullup_dis(pin), TAG, "RTC PU off");
                ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(pin), TAG, "RTC PD off");
            }
        }
        s_ptest_knobs |= MUSE_PTEST_PULLS_OFF;
    }
    if (knobs & MUSE_PTEST_GPIO_ISOLATE) {
        esp_sleep_config_gpio_isolate();
        esp_sleep_enable_gpio_switch(true);
        /* Exempt only pads this firmware deliberately keeps latched in
         * sleep: the parked I2S-low outputs, the backlight pin (LEDC duty 0
         * latches low), the amp enable (low) and the panel reset (high, so
         * SLPIN survives). Never the LCD data/strapping pads, never MSPI. */
        const gpio_num_t keep[] = { I2S_MCLK, I2S_BCLK, I2S_WS, I2S_DOUT, LCD_BL, PA_EN, LCD_RST };
        for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
            ESP_RETURN_ON_ERROR(gpio_sleep_sel_dis(keep[i]), TAG, "kept latch in sleep");
        }
        if (knobs & MUSE_PTEST_I2S_LOW) {
            ESP_RETURN_ON_ERROR(gpio_sleep_set_direction(I2S_DIN, GPIO_MODE_INPUT), TAG, "DIN sleep input");
            ESP_RETURN_ON_ERROR(gpio_sleep_set_pull_mode(I2S_DIN, GPIO_PULLDOWN_ONLY), TAG, "DIN sleep pulldown");
        }
        s_ptest_knobs |= MUSE_PTEST_GPIO_ISOLATE;
    }
    return ESP_OK;
}

esp_err_t muse_ptest_board_prepare_deep(bool held)
{
    if (!held) { return ESP_OK; }
    ESP_RETURN_ON_FALSE(s_ptest_knobs & MUSE_PTEST_I2S_LOW, ESP_ERR_INVALID_STATE, TAG, "deep hold requires low pins");
    /* Independent, tiny ownership bank survives a torn large run journal.
     * Claim every pad before acquiring any: releasing an unheld owned pad
     * is safe, missing ownership after acquisition is not. */
    ptest_mark_warm(); /* ownership recovery may outlive the large warm journal */
    s_ptest_hold_owner.inverse = ~PTEST_HOLD_MAGIC;
    s_ptest_hold_owner.magic = PTEST_HOLD_MAGIC;
    __sync_synchronize();
    for (size_t i = 0; i < sizeof(s_ptest_hold_pins) / sizeof(s_ptest_hold_pins[0]); i++) {
        gpio_num_t pin = s_ptest_hold_pins[i];
        /* Holding a pad that is not latched low would glitch whatever it
         * drives; the backlight is LEDC-driven, so ask the peripheral. */
        bool low = pin == LCD_BL ? !s_ptest_pwm || ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) == 0
                                 : ptest_latch(pin) == 0;
        ESP_RETURN_ON_FALSE(low, ESP_FAIL, TAG, "held pad not low");
        ESP_RETURN_ON_ERROR(gpio_hold_en(pin), TAG, "deep hold low");
        s_ptest_held_mask |= BIT64(pin);
    }
    gpio_deep_sleep_hold_en();
    s_ptest_knobs |= MUSE_PTEST_GPIO_HOLD;
    return ESP_OK;
}

unsigned muse_ptest_board_codec_history(void) { return s_ptest_audio_attempted && s_ptest_codec_state == MUSE_PTEST_CODEC_COLD ? MUSE_PTEST_CODEC_INITIALIZED : s_ptest_codec_state; }
void muse_ptest_board_restore_codec_history(unsigned history)
{
    if (history >= MUSE_PTEST_CODEC_INITIALIZED && history <= MUSE_PTEST_CODEC_SUSPENDED) { s_ptest_codec_state = (muse_ptest_codec_t)history; }
}

static esp_codec_dev_handle_t s_ptest_spk, s_ptest_mic;
static int s_ptest_volume, s_ptest_battery_mv, s_ptest_avg_ma, s_ptest_expected_duty;
static uint32_t s_ptest_frames, s_ptest_adc_samples;
static int64_t s_ptest_next_adc_us;
static muse_ptest_load_t s_ptest_load;
static esp_lcd_panel_handle_t s_ptest_panel;
static esp_lcd_panel_io_handle_t s_ptest_io;

static esp_err_t ptest_amp(bool on)
{
    /* The amp enable is this board's only switchable load. ptest owns it as
     * a plain GPIO (audio_init hands the ES8311 a NC pa_pin under ptest). */
    ESP_RETURN_ON_ERROR(gpio_set_level(PA_EN, on ? 1 : 0), TAG, "amp enable");
    s_ptest_amp_on = on;
    if (on) { vTaskDelay(pdMS_TO_TICKS(50)); } /* settle; bench value pending */
    return ESP_OK;
}

static esp_err_t ptest_codec_read(uint8_t addr, const uint8_t *regs, size_t n, uint8_t *values, uint8_t *valid)
{
    i2c_master_dev_handle_t device = NULL;
    const i2c_device_config_t cfg = { .device_address = addr, .scl_speed_hz = 400000 };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &cfg, &device), TAG, "codec snapshot handle");
    esp_err_t first = ESP_OK;
    for (size_t i = 0; i < n; i++) {
        esp_err_t e = i2c_master_transmit_receive(device, &regs[i], 1, &values[i], 1, 50);
        if (!e) { *valid |= (uint8_t)(1u << i); }
        else {
            ESP_LOGE(TAG, "codec @%02x reg %02x read error %x", addr, regs[i], (unsigned)e);
            if (!first) { first = e; }
        }
    }
    esp_err_t cleanup = i2c_master_bus_rm_device(device);
    return first ? first : cleanup;
}

/* Evidence only, never acceptance: these bytes let the bench see what the
 * codecs hold. No "expected" values are asserted - the ES8311/ES7210
 * constructor-final and shutdown bytes are not vendor-verified for this
 * board - and adc_variant stays 0 ("unknown"): the enum knows ES7243(E),
 * not ES7210. Reads are side-effect-free whatever the map turns out to be. */
static esp_err_t ptest_codec_snapshot(void)
{
    static const uint8_t dac_regs[] = { 0x00, 0x01, 0x02, 0x0d, 0x0e, 0x12, 0x14 };
    static const uint8_t adc_regs[] = { 0x00, 0x01, 0x02, 0x03 };
    memset(&s_ptest_codec_regs, 0, sizeof(s_ptest_codec_regs));
    esp_err_t first = ptest_codec_read(ES8311_ADDR7, dac_regs, 7, s_ptest_codec_regs.dac, &s_ptest_codec_regs.dac_mask);
    esp_err_t e = ptest_codec_read(ES7210_ADDR7, adc_regs, 4, s_ptest_codec_regs.adc, &s_ptest_codec_regs.adc_mask);
    if (!first) { first = e; }
    return first; /* values, including mismatches, are evidence, never ESP_FAIL */
}

void muse_ptest_board_codec_regs(muse_ptest_codec_regs_t *out)
{
    if (out) { *out = s_ptest_codec_regs; }
}

static esp_err_t ptest_audio_close(void)
{
    bool was_open = s_ptest_spk_open || s_ptest_mic_open;
    bool orphaned_warm = s_ptest_codec_state >= MUSE_PTEST_CODEC_INITIALIZED && (!s_ptest_spk || !s_ptest_mic);
    bool check_hw = was_open || s_ptest_audio_failed || orphaned_warm;
    if (s_ptest_audio_failed) { s_ptest_audio_poisoned = true; }
    esp_err_t first = ptest_amp(false);
    /* Wrapper open may mark its own flags before failing. Close every owned
     * wrapper, not only ones whose open returned success to this caller. */
    if (s_ptest_spk) {
        int e = esp_codec_dev_close(s_ptest_spk);
        if (!first && e != ESP_CODEC_DEV_OK) { first = ESP_FAIL; }
        if (e == ESP_CODEC_DEV_OK) { s_ptest_spk_open = false; }
    }
    if (s_ptest_mic) {
        int e = esp_codec_dev_close(s_ptest_mic);
        if (!first && e != ESP_CODEC_DEV_OK) { first = ESP_FAIL; }
        if (e == ESP_CODEC_DEV_OK) { s_ptest_mic_open = false; }
    }
    if (!s_ptest_i2s_stopped) {
        const i2s_chan_handle_t channels[] = { s_ptest_tx, s_ptest_rx };
        bool stopped = true;
        for (size_t i = 0; i < 2; i++) {
            if (channels[i]) {
                esp_err_t e = i2s_channel_disable(channels[i]);
                if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
                    stopped = false;
                    if (!first) { first = e; }
                }
            }
        }
        s_ptest_i2s_stopped = stopped;
    }
    if (first && s_ptest_codec_state >= MUSE_PTEST_CODEC_INITIALIZED) {
        check_hw = true;
        s_ptest_audio_poisoned = true;
    }
    if (check_hw) {
        /* close() ignores low-level errors. Explicit enable(false) both checks
         * errors and keeps the drivers' enabled flag consistent for reuse.
         * Never do this to the intentional constructor-only initialized state. */
        const audio_codec_if_t *codecs[] = { s_ptest_dac_if, s_ptest_adc_if };
        for (size_t i = 0; i < 2; i++) {
            if (codecs[i] && (!codecs[i]->enable || codecs[i]->enable(codecs[i], false) != ESP_CODEC_DEV_OK)) {
                if (!first) { first = ESP_FAIL; }
                s_ptest_audio_poisoned = true;
            }
        }
        /* Verify by register transport only. The ES8311/ES7210 shutdown byte
         * sequences are not vendor-verified here (sensecap had them for
         * ES8311 + ES7243(E)), so there is no forced register suspend: the
         * codec drivers' own disable() is the shutdown path under test. */
        esp_err_t e = ptest_codec_snapshot();
        if (e) {
            s_ptest_audio_poisoned = true;
            if (!first) { first = e; }
        }
        if (!first) {
            s_ptest_codec_state = MUSE_PTEST_CODEC_SUSPENDED;
            s_ptest_audio_failed = false;
        }
    }
    return first;
}

static esp_err_t ptest_audio_initialize(void)
{
    ESP_RETURN_ON_FALSE(!s_ptest_audio_poisoned, ESP_ERR_INVALID_STATE, TAG, "audio lifecycle poisoned");
    if (!s_ptest_spk || !s_ptest_mic) {
        ESP_RETURN_ON_FALSE(!s_ptest_i2s_parked, ESP_ERR_INVALID_STATE, TAG, "I2S parked");
        /* Do not retry a partially-created shared I2S bus on later commands. */
        ESP_RETURN_ON_FALSE(!s_ptest_audio_attempted, ESP_ERR_INVALID_STATE, TAG, "partial audio init");
        s_ptest_audio_attempted = s_ptest_audio_failed = true;
        ptest_mark_warm(); /* sticky across retained resets, even torn run journal */
        ESP_RETURN_ON_ERROR(audio_init(&s_ptest_spk, &s_ptest_mic), TAG, "diagnostic codecs");
        s_ptest_codec_state = MUSE_PTEST_CODEC_INITIALIZED;
        s_ptest_audio_failed = false;
    }
    return ESP_OK;
}

static esp_err_t ptest_audio_open(void)
{
    ESP_RETURN_ON_FALSE(!s_ptest_i2s_parked, ESP_ERR_INVALID_STATE, TAG, "audio open with parked I2S");
    ESP_RETURN_ON_ERROR(ptest_audio_initialize(), TAG, "audio initialized");
    s_ptest_audio_failed = true; /* Includes a failure after wrapper flags change. */
    s_ptest_i2s_stopped = false; /* wrapper open may enable DMA before failing */
    esp_codec_dev_sample_info_t fs = { .sample_rate = 16000, .channel = 2, .bits_per_sample = 16 };
    if (!s_ptest_spk_open) {
        s_ptest_spk_open = true;
        ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_ptest_spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "speaker open");
    }
    if (!s_ptest_mic_open) {
        s_ptest_mic_open = true;
        ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_ptest_mic, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "mic open");
    }
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_ptest_spk, 25) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "bounded volume");
    s_ptest_volume = 25;
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_ptest_mic, 0.0f) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "mic gain");
    /* Opening a codec may skip re-enabling an already-open shared data IF.
     * Ensure clocks, but accept already-enabled as the documented state. */
    s_ptest_i2s_stopped = false; /* enable may partially succeed */
    const i2s_chan_handle_t channels[] = { s_ptest_tx, s_ptest_rx };
    for (size_t i = 0; i < 2; i++) {
        esp_err_t e = i2s_channel_enable(channels[i]);
        ESP_RETURN_ON_FALSE(e == ESP_OK || e == ESP_ERR_INVALID_STATE, e, TAG, "I2S enable");
    }
    s_ptest_audio_failed = false;
    return ESP_OK;
}

static esp_err_t ptest_lcd(int percent)
{
    if (!s_ptest_lcd_on) {
        if (!s_ptest_pwm) {
            const ledc_timer_config_t timer = {
                .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_USE_XTAL_CLK,
            };
            ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "diagnostic PWM");
            s_ptest_pwm = true;
        }
        /* Reset by hand first, then the same revision probe and init table
         * selection display_start() uses: two ST77916 panel revisions exist
         * and neither matches the component's built-in default. */
        const gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << LCD_RST, .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "panel reset line");
        ESP_RETURN_ON_ERROR(gpio_set_level(LCD_RST, 0), TAG, "panel reset low");
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(gpio_set_level(LCD_RST, 1), TAG, "panel reset high");
        vTaskDelay(pdMS_TO_TICKS(10));
        const spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(
            LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
        ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "diagnostic LCD SPI");
        esp_lcd_panel_io_handle_t probe_io;
        esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
        io_config.pclk_hz = ST77916_PROBE_HZ;
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &probe_io), TAG, "probe IO");
        uint8_t version[4] = { 0 };
        const int probe_cmd = ((ST77916_REG_VERSION & 0xff) << 8) | (ST77916_OPCODE_READ_CMD << 24);
        esp_err_t probe_err = esp_lcd_panel_io_rx_param(probe_io, probe_cmd, version, sizeof(version));
        const st77916_lcd_init_cmd_t *init_cmds = s_init_version_2;
        size_t init_cmds_size = sizeof(s_init_version_2) / sizeof(s_init_version_2[0]);
        if (probe_err == ESP_OK && version[0] == 0x00 && version[1] == 0x7F &&
            version[2] == 0x7F && version[3] == 0x7F) {
            init_cmds = s_init_version_1;
            init_cmds_size = sizeof(s_init_version_1) / sizeof(s_init_version_1[0]);
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(probe_io), TAG, "probe IO del");
        io_config.pclk_hz = 80 * 1000 * 1000;
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_ptest_io), TAG, "diagnostic LCD IO");
        const st77916_vendor_config_t vendor_config = {
            .init_cmds = init_cmds,
            .init_cmds_size = init_cmds_size,
            .flags = { .use_qspi_interface = 1 },
        };
        const esp_lcd_panel_dev_config_t panel_cfg = {
            .reset_gpio_num = LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .vendor_config = (void *)&vendor_config,
        };
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(s_ptest_io, &panel_cfg, &s_ptest_panel), TAG, "diagnostic panel");
        s_ptest_lcd_on = true; /* cleanup must run even if setup fails below */
        ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_ptest_panel), TAG, "panel reset");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_ptest_panel), TAG, "panel init");
        /* Clear GRAM to black through the driver's blocking draw path, the
         * same way the shipped bring-up does: no LVGL, no color-done
         * callback, backpressure from the queue itself. */
        uint8_t *black = heap_caps_calloc(1, LCD_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(black, ESP_ERR_NO_MEM, TAG, "black stripe");
        esp_err_t e = ESP_OK;
        for (int y = 0; y < LCD_V_RES; y += 8) {
            e = esp_lcd_panel_draw_bitmap(s_ptest_panel, 0, y, LCD_H_RES,
                                          y + 8 < LCD_V_RES ? y + 8 : LCD_V_RES, black);
            if (e) { break; }
        }
        /* A draw error may mean a DMA buffer is still in flight. Keep it
         * alive (bounded diagnostic-only leak) rather than risk a UAF. */
        if (!e) { free(black); }
        ESP_RETURN_ON_ERROR(e, TAG, "black draw");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_ptest_panel, true), TAG, "panel display on");
    }
    /* Rebind the backlight channel every time: GPIO-mode parking disconnects
     * the LEDC matrix. */
    const ledc_channel_config_t ch = {
        .gpio_num = LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0,
        .duty = percent * 1023 / 100,
    };
    return ledc_channel_config(&ch);
}

static esp_err_t ptest_lcd_off(void)
{
    if (s_ptest_pwm) {
        ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0), TAG, "backlight zero");
        ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), TAG, "backlight update");
    }
    if (s_ptest_lcd_on && s_ptest_panel) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_ptest_panel, false), TAG, "panel off");
        /* Raw SLPIN on the QSPI command path; esp_lcd_st77916 2.x has no
         * base.disp_sleep, so the raw command is the only sleep (the note in
         * send_panel_sleep() above documents the same). */
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_ptest_io, (0x02u << 24) | (0x10u << 8), NULL, 0), TAG, "panel sleep");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    /* No LCD rail exists on this board: SLPIN + backlight 0 is the honest
     * "panel off" this hardware can do. */
    return ESP_OK;
}

esp_err_t muse_ptest_board_init(void)
{
    ESP_RETURN_ON_ERROR(init(), TAG, "board init");
    /* The amp enable becomes a plain ptest output here (audio_init hands the
     * ES8311 a NC pa_pin under ptest), low from the first moment. */
    ESP_RETURN_ON_ERROR(ptest_pin(PA_EN, GPIO_MODE_OUTPUT, false), TAG, "amp enable low");
    /* The CST816S is never brought up in the diagnostic. Put its DisAutoSleep
     * in the power-on default explicitly, the same value production writes
     * when the screen sleeps, so the controller idles between touches in
     * every state instead of scanning. Best effort: a missing touch panel
     * must not fail the diagnostic's init. */
    i2c_master_dev_handle_t tp = NULL;
    const i2c_device_config_t tp_cfg = { .device_address = TP_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(s_i2c, &tp_cfg, &tp) == ESP_OK) {
        const uint8_t cmd[2] = { 0xFE, 0x00 }; /* DisAutoSleep: allow auto sleep */
        if (i2c_master_transmit(tp, cmd, sizeof(cmd), 50) != ESP_OK) {
            ESP_LOGW(TAG, "ptest: touch DisAutoSleep write failed (panel absent?)");
        }
        i2c_master_bus_rm_device(tp);
    }
    return ESP_OK;
}

esp_err_t muse_ptest_board_power(muse_ptest_readback_t *out)
{
    if (!out || s_i2c == NULL) { return ESP_ERR_INVALID_STATE; }
    out->usb = usb_serial_jtag_is_connected();
    out->input = out->output = out->direction = 0; /* no expander on this board */
    /* Charging: the gauge's DSG bit is clear while a charger pushes current
     * in - the same reading read_power() uses. Gauge absent: not charging. */
    out->charging = false;
    battery_status_t bs = { 0 };
    if (s_gauge != NULL && bq27220_get_battery_status(s_gauge, &bs) == ESP_OK) { out->charging = !bs.DSG; }
    return ESP_OK;
}

esp_err_t muse_ptest_board_apply(const muse_ptest_state_t *state)
{
    /* Headless board: UART0 pads are not broken out, so ESP_LOG never reaches
     * the host. Report the failing step as 0x4000|n in apply_err instead. */
#define APPLY_STEP(n) do { s_ptest_apply_step = (n); } while (0)
#define APPLY_FAIL(n, e) do { s_ptest_apply_step = (n); return (esp_err_t)(0x4000 | (n)); } while (0)
    APPLY_STEP(1);
    if (!state || s_i2c == NULL) { APPLY_FAIL(1, ESP_ERR_INVALID_STATE); }
    if (state->load == MUSE_PTEST_UNSUPPORTED) { return ESP_ERR_NOT_SUPPORTED; }
    bool audio = state->load == MUSE_PTEST_CODECS_IDLE || state->load == MUSE_PTEST_MIC || state->load == MUSE_PTEST_SINE;
    APPLY_STEP(2);
    if (audio && (state->knobs & (MUSE_PTEST_I2S_LOW | MUSE_PTEST_I2S_HIZ))) { APPLY_FAIL(2, ESP_ERR_INVALID_STATE); }
    APPLY_STEP(3);
    if (ptest_revert_knobs() != ESP_OK) { APPLY_FAIL(3, ESP_ERR_INVALID_STATE); }
    APPLY_STEP(4);
    if (state->codec_policy == MUSE_PTEST_CODEC_COLD && (s_ptest_audio_attempted || s_ptest_codec_state != MUSE_PTEST_CODEC_COLD)) { APPLY_FAIL(4, ESP_ERR_INVALID_STATE); }
    APPLY_STEP(5);
    if (state->codec_policy == MUSE_PTEST_CODEC_INITIALIZED && s_ptest_codec_state == MUSE_PTEST_CODEC_SUSPENDED) { APPLY_FAIL(5, ESP_ERR_INVALID_STATE); }
    s_ptest_load = state->load;
    s_ptest_frames = s_ptest_adc_samples = 0;
    s_ptest_battery_mv = 0;
    s_ptest_avg_ma = 0;
    s_ptest_next_adc_us = 0;
    /* Amp off before closing/reprogramming codecs, no power-up pop. */
    APPLY_STEP(6);
    if (ptest_amp(false) != ESP_OK) { APPLY_FAIL(6, ESP_ERR_INVALID_STATE); }
    if (state->codec_policy == MUSE_PTEST_CODEC_INITIALIZED) {
        /* Constructors write ES8311/ES7210 setup. No esp_codec_dev_open:
         * wrapper close is a no-op, but stop I2S DMA. */
        APPLY_STEP(7);
        if (ptest_audio_initialize() != ESP_OK) { APPLY_FAIL(7, ESP_ERR_INVALID_STATE); }
        if (ptest_audio_close() != ESP_OK) { APPLY_FAIL(7, ESP_ERR_INVALID_STATE); }
    } else if (state->codec_policy == MUSE_PTEST_CODEC_SUSPENDED) {
        APPLY_STEP(8);
        if (ptest_audio_open() != ESP_OK) { APPLY_FAIL(8, ESP_ERR_INVALID_STATE); }
        if (ptest_audio_close() != ESP_OK) { APPLY_FAIL(8, ESP_ERR_INVALID_STATE); }
    } else {
        APPLY_STEP(9);
        if ((audio ? ptest_audio_open() : ptest_audio_close()) != ESP_OK) { APPLY_FAIL(9, ESP_ERR_INVALID_STATE); }
    }
    APPLY_STEP(10);
    if ((state->backlight_pct < 0 ? ptest_lcd_off() : ptest_lcd(state->backlight_pct)) != ESP_OK) { APPLY_FAIL(10, ESP_ERR_INVALID_STATE); }
    s_ptest_expected_duty = state->backlight_pct < 0 ? 0 : state->backlight_pct * 1023 / 100;
    APPLY_STEP(11);
    if (state->amplifier && !audio) { APPLY_FAIL(11, ESP_ERR_INVALID_STATE); }
    if (state->amplifier) {
        APPLY_STEP(12);
        if (ptest_amp(true) != ESP_OK) { APPLY_FAIL(12, ESP_ERR_INVALID_STATE); }
    }
    APPLY_STEP(13);
    if (ptest_apply_knobs(state->knobs) != ESP_OK) { APPLY_FAIL(13, ESP_ERR_INVALID_STATE); }
    s_ptest_apply_step = 0;
    return ESP_OK;
#undef APPLY_STEP
#undef APPLY_FAIL
}

static esp_err_t ptest_check_knobs(void)
{
    gpio_io_config_t io;
    if (s_ptest_knobs & (MUSE_PTEST_I2S_LOW | MUSE_PTEST_I2S_HIZ)) {
        for (size_t i = 0; i < 5; i++) {
            ESP_RETURN_ON_ERROR(gpio_get_io_config(s_ptest_i2s_pins[i], &io), TAG, "I2S pin readback");
            bool low = (s_ptest_knobs & MUSE_PTEST_I2S_LOW) != 0;
            bool match = low ? i == 4 ? io.ie && !io.oe && io.pd && !io.pu : io.oe && !io.pu && !io.pd && io.sig_out == SIG_GPIO_OUT_IDX && !ptest_latch(s_ptest_i2s_pins[i]) : !io.ie && !io.oe && !io.pu && !io.pd;
            ESP_RETURN_ON_FALSE(match, ESP_FAIL, TAG, "I2S parking mismatch");
        }
    }
    if (s_ptest_knobs & MUSE_PTEST_UART_HIZ) {
        const gpio_num_t pins[] = { GPIO_NUM_43, GPIO_NUM_44 };
        for (size_t i = 0; i < 2; i++) {
            ESP_RETURN_ON_ERROR(gpio_get_io_config(pins[i], &io), TAG, "UART pin readback");
            ESP_RETURN_ON_FALSE(!io.ie && !io.oe && !io.pu && !io.pd, ESP_FAIL, TAG, "UART parking mismatch");
        }
    }
    if (s_ptest_knobs & MUSE_PTEST_PULLS_OFF) {
        for (size_t i = 0; i < 2; i++) {
            ESP_RETURN_ON_ERROR(gpio_get_io_config(s_ptest_pull_pins[i], &io), TAG, "pull readback");
            ESP_RETURN_ON_FALSE(!io.pu && !io.pd, ESP_FAIL, TAG, "pull mismatch");
        }
    }
    if (s_ptest_knobs & MUSE_PTEST_GPIO_ISOLATE) {
        ESP_RETURN_ON_ERROR(gpio_get_io_config(I2C_SDA, &io), TAG, "sleep switch readback");
        ESP_RETURN_ON_FALSE(io.slp_sel, ESP_FAIL, TAG, "sleep switch mismatch");
    }
    return ESP_OK;
}

esp_err_t muse_ptest_board_readback(muse_ptest_readback_t *out)
{
    if (!out) { return ESP_ERR_INVALID_ARG; }
    memset(&s_ptest_codec_regs, 0, sizeof(s_ptest_codec_regs));
    out->codec_state = s_ptest_codec_state;
    if (muse_ptest_board_power(out) != ESP_OK) { return (esp_err_t)(0x4200 | 1); }
    out->backlight_duty = s_ptest_pwm ? (int)ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) : -1;
    if (s_ptest_pwm && out->backlight_duty != s_ptest_expected_duty) { return (esp_err_t)(0x4200 | 2); }
    out->codecs_open = s_ptest_spk_open && s_ptest_mic_open;
    out->configured_volume = s_ptest_volume;
    out->battery_mv = s_ptest_battery_mv;
    out->avg_ma = (int16_t)s_ptest_avg_ma;
    out->io_frames = s_ptest_frames;
    out->adc_samples = s_ptest_adc_samples;
    if (ptest_check_knobs() != ESP_OK) { return (esp_err_t)(0x4200 | 3); }
    out->knobs_applied = s_ptest_knobs;
    out->codec_state = s_ptest_codec_state;
    if (ptest_codec_snapshot() != ESP_OK) { return (esp_err_t)(0x4200 | 4); }
    return ESP_OK;
}

esp_err_t muse_ptest_board_service(muse_ptest_load_t load)
{
    if (load != s_ptest_load) { return ESP_ERR_INVALID_STATE; }
    if (load == MUSE_PTEST_MIC || load == MUSE_PTEST_SINE) {
        /* A 10ms chunk, bounded 100ms DMA timeout. No voice/network task and
         * no buffers saved. Sine is -18dBFS PEAK, not RMS, on both slots. */
        int16_t pcm[160 * 2];
        size_t transferred = 0;
        esp_err_t e;
        if (load == MUSE_PTEST_MIC) {
            e = i2s_channel_read(s_ptest_rx, pcm, sizeof(pcm), &transferred, 100);
        } else {
            /* 16 samples/period at 16kHz. Fixed table avoids per-sample libm
             * load: +/-4125 ~= 32767 * 10^(-18/20). */
            static const int16_t tone[16] = { 0,1579,2917,3811,4125,3811,2917,1579,0,-1579,-2917,-3811,-4125,-3811,-2917,-1579 };
            for (size_t i = 0; i < 160; i++) { pcm[2 * i] = pcm[2 * i + 1] = tone[i % 16]; }
            e = i2s_channel_write(s_ptest_tx, pcm, sizeof(pcm), &transferred, 100);
        }
        if (e) { return e; }
        if (transferred != sizeof(pcm)) { return ESP_FAIL; }
        s_ptest_frames += transferred / (2 * sizeof(int16_t));
    } else if (load == MUSE_PTEST_ADC_SAMPLE && esp_timer_get_time() >= s_ptest_next_adc_us) {
        /* No ADC divider rail on this board: sampling is the BQ27220 gauge
         * read the production firmware uses (read_power). */
        muse_power_t p = {0};
        ESP_RETURN_ON_ERROR(read_power(&p), TAG, "gauge sample");
        s_ptest_battery_mv = p.battery_mv;
        /* The same gauge read the production firmware makes: BQ27220
         * AverageCurrent, negative while discharging. Sampling it here (the
         * ADC_SAMPLE load) keeps every other state free of extra I2C traffic,
         * so that load's own current is the only thing being measured. */
        s_ptest_avg_ma = p.battery_ma;
        s_ptest_adc_samples += 8;
        s_ptest_next_adc_us = esp_timer_get_time() + 1000000;
    }
    return ESP_OK;
}
#endif

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.85B",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = true,
    /* BOOT is the only wake line left (1.4 dropped the IMU's GPIO8 and the
     * touch panel's GPIO4), and it interrupts a light sleep, so there is
     * nothing for a 1 s rest wait to notice: the pause can wait as long as a
     * Wi-Fi nap does (P1-4 in the code review). */
    .wake_lines_reliable = true,
    .touch = true,
    .diagonal_in = 1.85f,
    .talk_button = "boot",
    .aux_button = NULL,      /* single button board: aux never fires, and NULL is
                              * what makes the settings page offer no
                              * "press boot to turn the screen off" line */
    /* Both buttons sit on the bezel's right side (BOOT up at ~1:30, PWR down at
     * ~4:30), so the icons that name them go on the rim beside them: the mic at
     * the top right, the power symbol at the bottom right. The power glyph is
     * drawn at 192/256 (28 px font -> ~21 px) so it reads as a rim hint rather
     * than a second button. */
    .talk_hint = { LV_ALIGN_TOP_RIGHT, -68, 50 },
    .aux_hint = { LV_ALIGN_BOTTOM_RIGHT, -68, -46, 192 },
    .speaker_angle_deg = 180,   /* the dial's far left (9 o'clock) */
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .set_mic_gain = set_mic_gain,
    .mic_slot = 0,              /* MIC1; slot 1 is the speaker reference */
    .poll_buttons = poll_buttons,
    .talk_down = talk_down,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_debug = dump_gauge,

    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
