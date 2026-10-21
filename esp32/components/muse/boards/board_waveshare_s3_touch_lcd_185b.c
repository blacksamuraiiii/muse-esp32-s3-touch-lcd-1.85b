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
#include "muse_imu.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "muse_rtc.h"
#include "muse_ui.h"

static const char *TAG = "board";

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

#define I2C_SDA GPIO_NUM_11    /* touch, ES8311, ES7210, BQ27220, IMU, RTC */
#define I2C_SCL GPIO_NUM_10

#define I2S_MCLK GPIO_NUM_2
#define I2S_BCLK GPIO_NUM_48
#define I2S_WS GPIO_NUM_38
#define I2S_DOUT GPIO_NUM_47   /* playback -> ES8311 */
#define I2S_DIN GPIO_NUM_39    /* record <- ES7210 */
#define PA_EN GPIO_NUM_9       /* amp on while HIGH */

#define BQ27220_ADDR 0x55      /* fuel gauge, 7-bit */
#define QMI8658_ADDR 0x6B      /* IMU, 7-bit; SDO tied low on this board */

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
static i2c_master_bus_handle_t s_i2c;  /* its inner handle: touch, IMU, PMU */
static bq27220_handle_t s_gauge;       /* espressif/bq27220 fuel gauge */
static i2c_master_dev_handle_t s_bq;   /* BQ27220 fuel gauge, added to s_i2c */
static i2c_master_dev_handle_t s_imu;  /* QMI8658A: tap-to-wake only */
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
 * docs/20261018待办.md. */
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
     * it - touch, IMU, PMU - takes the inner i2c_master handle. One owner, one
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
    /* i2c_master_transmit_receive() takes a device handle, not a bus handle:
     * register the fuel gauge as one device on the shared bus. */
    const i2c_device_config_t bq_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BQ27220_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &bq_cfg, &s_bq), TAG, "bq27220 device");
    /* QMI8658A at 0x6B: picking the gadget up wakes the screen, and turning it
     * face down mutes playback (muse_imu.c). A board whose IMU doesn't answer
     * still runs - the screen then only wakes on the button. */
    const i2c_device_config_t imu_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(s_i2c, &imu_cfg, &s_imu) == ESP_OK) {
        esp_err_t err = muse_imu_init(s_imu);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "pick-up-to-wake off: %s", esp_err_to_name(err));
        } else if ((err = muse_gpio_wake_line_add(MUSE_IMU_INT_GPIO, true)) != ESP_OK) {
            ESP_LOGW(TAG, "IMU wake line not armed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "no IMU device handle");
    }
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

/* The CST816S reports a touch as a *low pulse* on IRQ rather than a held level.
 * The touch chip's own IRQ settings, read-only, logged for the record:
 * 0xA7 chip id, 0xED IrqPluseWidth (the low pulse a touch makes - the thing a
 * light-sleep wake has to catch), 0xFA IrqCtl, 0xFE DisAutoSleep. An earlier
 * round *wrote* 0xEE here on the assumption it was IrqPluseWidth: the vendor
 * SDK's register table is shifted by one in places and 0xEE is not that
 * register. The write cost a night of the chip waking the screen as soon as it
 * went dark, so the settings are read, never poked, until a datasheet settles
 * their meaning. */
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
    /* A tap on the glass is a short low pulse - shorter than the level the
     * chip's low-power wake sampler can catch, which is why a *held* finger
     * wakes the board and a tap does not. IrqPluseWidth (0xED, per the CST816S
     * register manual) stretches that pulse; the unit is not documented in the
     * tables available here, so this is a middle setting, read back so the boot
     * log carries what the chip ended up with. Held touches behave as before.
     * The register is not in the public tables, so it is behind a Kconfig
     * switch: it is on because a tap did not wake the board without it. */
#if CONFIG_MUSE_TOUCH_IRQ_PULSE_WIDEN
    uint8_t width = 0;
    const uint8_t widen = 0x14;
    if (esp_lcd_panel_io_rx_param(s_tp_io, 0xED, &width, 1) == ESP_OK) {
        esp_err_t err = esp_lcd_panel_io_tx_param(s_tp_io, 0xED, &widen, 1);
        uint8_t after = 0;
        (void)esp_lcd_panel_io_rx_param(s_tp_io, 0xED, &after, 1);
        ESP_LOGI(TAG, "touch: IrqPluseWidth 0x%02X -> 0x%02X (%s)", width, after,
                 esp_err_to_name(err));
    }
#endif
    /* Auto-sleep lets the controller stop sampling between touches, and the
     * first touch after a long pause then only wakes it without reporting
     * coordinates. The INT line is one of this board's light-sleep wake lines,
     * so the controller has to stay responsive: turn auto-sleep off and log
     * what the chip says. */
    uint8_t autosleep = 0;
    if (esp_lcd_panel_io_rx_param(s_tp_io, 0xFE, &autosleep, 1) == ESP_OK && autosleep != 0x01) {
        const uint8_t off = 0x01;
        esp_err_t err = esp_lcd_panel_io_tx_param(s_tp_io, 0xFE, &off, 1);
        uint8_t after = 0;
        (void)esp_lcd_panel_io_rx_param(s_tp_io, 0xFE, &after, 1);
        ESP_LOGI(TAG, "touch: DisAutoSleep 0x%02X -> 0x%02X (%s)", autosleep, after,
                 esp_err_to_name(err));
    }
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
    /* The touch line is deliberately NOT a wake source. A hand resting near the
     * glass, a stray pulse from the controller and a screen that never stayed
     * dark were all traced back to it, and the same touches produced empty
     * voice notes. The screen now wakes on the BOOT button or on being picked
     * up (the IMU's motion line), and the touch panel only reports while the
     * display is running. To bring it back, register TP_INT with
     * muse_gpio_wake_line_add_wake_only(TP_INT, false) here and the input
     * task's check_touch_line() picks it up again unchanged. */
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
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

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
        .pa_pin = PA_EN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
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

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk }, 1, timeout_ms);
}

static esp_err_t bq27220_read_word(uint8_t reg, uint16_t *out)
{
    uint8_t data[2];
    esp_err_t err = i2c_master_transmit_receive(s_bq, &reg, 1, data, 2, 50);
    muse_i2c_report(err == ESP_OK);
    ESP_RETURN_ON_ERROR(err, TAG, "bq27220 read");
    *out = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return ESP_OK;
}

/* Single-cell Li-ion, open-circuit volts to percent: the last-resort fallback
 * for a gauge that answers out of range (see read_power). A cell under load
 * reads a few tens of mV low, so the bottom of the curve is deliberately a
 * little generous: the estimate errs towards "not empty yet". */
static int pct_from_mv(int mv)
{
    static const struct {
        uint16_t mv;
        uint8_t pct;
    } curve[] = {
        { 4200, 100 }, { 4150, 95 }, { 4100, 90 }, { 4050, 85 }, { 4000, 80 },
        { 3950, 74 },  { 3900, 68 }, { 3850, 62 }, { 3800, 56 }, { 3750, 50 },
        { 3700, 42 },  { 3650, 34 }, { 3600, 26 }, { 3550, 19 }, { 3500, 13 },
        { 3450, 8 },   { 3400, 5 },  { 3350, 3 },  { 3300, 1 },  { 3200, 0 },
    };
    if (mv >= curve[0].mv) {
        return 100;
    }
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
        if (mv >= curve[i].mv) {
            int span_mv = curve[i - 1].mv - curve[i].mv;
            int span_pct = curve[i - 1].pct - curve[i].pct;
            return curve[i].pct + (mv - curve[i].mv) * span_pct / span_mv;
        }
    }
    return 0;
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

/* BQ27220 fuel gauge. Addresses from the TRM's Table 2-1 (Standard Commands),
 * cross-checked against esp-iot-solution's bq27220 driver:
 *   Voltage 0x08 (mV), BatteryStatus 0x0A, Current 0x0C (mA, + = charging),
 *   RemainingCapacity 0x10, FullChargeCapacity 0x12, AverageCurrent 0x14,
 *   TimeToFull 0x18 (min), CycleCount 0x2A, StateOfCharge 0x2C (%),
 *   StateOfHealth 0x2E (%), InternalTemperature 0x28 (0.1 K).
 * 0x1C is StandbyTimeToEmpty, not the state of charge: reading it as a
 * percentage is what pinned the gauge at 100% (1947 clamped to 100). */
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
    /* The gauge's own numbers, from espressif/bq27220. Its model is written from
     * this pack's capacity at init (s_cedv), so the percentage is the gauge's
     * computation, not a voltage estimate: a gauge that answers out of range is
     * reported as unknown (-1) rather than papered over with millivolts. */
    uint16_t soc = 0, mv = 0, status = 0;
    if (s_gauge != NULL) {
        soc = bq27220_get_state_of_charge(s_gauge);
        mv = bq27220_get_voltage(s_gauge);
        /* The component's getters swallow the transfer error and return 0, and
         * no real cell reads 0 mV: both zero means the bus, not the battery.
         * Report it so the caller keeps the last valid reading instead of
         * showing 0%. */
        if (soc == 0 && mv == 0) {
            muse_i2c_report(false);
            ESP_LOGW(TAG, "gauge read 0 mV / 0%%: bus glitch, keeping the last value");
            return ESP_ERR_INVALID_RESPONSE;
        }
        battery_status_t bs;
        if (bq27220_get_battery_status(s_gauge, &bs) == ESP_OK) {
            status = bs.full;
        }
    } else {
        /* Component handle missing (profile write failed): the same standard
         * commands, read straight off the bus, so the UI still has numbers. */
        ESP_RETURN_ON_ERROR(bq27220_read_word(0x2c, &soc), TAG, "state of charge");
        ESP_RETURN_ON_ERROR(bq27220_read_word(0x08, &mv), TAG, "voltage");
        if (bq27220_read_word(0x0a, &status) != ESP_OK) {
            status = 0;
        }
    }
    /* The gauge is the source of truth - its model is written from this pack's
     * capacity at init (s_cedv). The cell-voltage curve stays only as a
     * last-resort fallback for when the gauge gives nothing usable (out of
     * range, or its handle is missing), never as an override of the gauge. */
    int pct = soc <= 100 ? (int)soc : -1;
    if (pct < 0) {
        pct = pct_from_mv((int)mv);
        static bool warned;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "gauge reads %u, not a percentage: falling back to %d mV (~%d%%)",
                     soc, (int)mv, pct);
        }
    }
    out->battery_pct = pct;
    out->battery_mv = (int)mv;
    /* BatteryStatus bit0 is DSG: clear while the charger is pushing current in. */
    out->charging = (status & 0x0001) == 0;
    out->usb = usb_serial_jtag_is_connected();
    return ESP_OK;
}

/* Every standard command that fits in one word, 0x08..0x2E, named from the
 * TRM so the dump reads without a datasheet at hand. Printed raw as well: a
 * value that does not fit its name is the fastest way to spot a wrong address. */
static void dump_gauge(void)
{
    static const struct {
        uint8_t reg;
        const char *name;
        const char *unit;
    } regs[] = {
        { 0x08, "Voltage", "mV" },
        { 0x0a, "BatteryStatus", "bits" },
        { 0x0c, "Current", "mA +chg" },
        { 0x10, "RemainingCapacity", "mAh" },
        { 0x12, "FullChargeCapacity", "mAh" },
        { 0x14, "AverageCurrent", "mA" },
        { 0x16, "TimeToEmpty", "min" },
        { 0x18, "TimeToFull", "min" },
        { 0x1a, "StandbyCurrent", "mA" },
        { 0x1c, "StandbyTimeToEmpty", "min" },
        { 0x1e, "MaxLoadCurrent", "mA" },
        { 0x20, "MaxLoadTimeToEmpty", "min" },
        { 0x22, "RawCoulombCount", "count" },
        { 0x24, "AveragePower", "mW" },
        { 0x28, "InternalTemperature", "0.1 K" },
        { 0x2a, "CycleCount", "cycles" },
        { 0x2c, "StateOfCharge", "%" },
        { 0x2e, "StateOfHealth", "%" },
    };
    ESP_LOGI(TAG, "BQ27220 standard commands (TRM Table 2-1):");
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint16_t v = 0;
        if (bq27220_read_word(regs[i].reg, &v) != ESP_OK) {
            ESP_LOGW(TAG, "  0x%02x %-20s no answer", regs[i].reg, regs[i].name);
            continue;
        }
        ESP_LOGI(TAG, "  0x%02x %-20s %6d (0x%04x) %s", regs[i].reg, regs[i].name,
                 (int16_t)v, v, regs[i].unit);
    }
    uint16_t dcap = 0;
    if (bq27220_read_word(0x3c, &dcap) == ESP_OK) {
        ESP_LOGI(TAG, "  0x3c %-20s %6u        mAh", "DesignCapacity", dcap);
    }
}


static void send_disp_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
}

static esp_err_t power_off(void)
{
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
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0), TAG, "wake button");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.85B",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = true,
    /* Touch INT, BOOT and the IMU's INT1 all interrupt a light sleep, so with
     * no knock polling to do there is nothing for a 1 s rest wait to notice:
     * the pause can wait as long as a Wi-Fi nap does (P1-4 in the code review). */
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
