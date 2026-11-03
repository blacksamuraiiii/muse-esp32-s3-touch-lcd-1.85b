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

#include "muse_audio_play.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "minimp3.h"

#include "muse_audio.h"

static const char *TAG = "muse.audio_url";

/* The decoder needs the next frame's header before it will give up the current
 * one, so this much has to stay behind until the download ends. */
#define MP3_HOLD (1441 + 4)
/* A few seconds of 48 kbps audio: the HTTP reads are ~1 KB, so this only has
 * to cover the gap between two of them plus a decode pass. */
#define MP3_BUF (32 * 1024)
#define HTTP_BUF 1024
#define TASK_STACK 12288
#define TASK_PRIO 4
#define HTTP_TIMEOUT_MS 30000

typedef struct {
    uint32_t step;   /* Q16 input samples per output sample */
    uint32_t pos;
    int16_t prev;
} down_rs_t;

static TaskHandle_t s_task;
static volatile bool s_abort;
/* esp_http_client ignores the event handler's return value, so a decoder that
 * falls behind cannot abort the download; remember it and report failure. */
static bool s_dropped;

static uint8_t *s_mp3;
static size_t s_len;
/* Decoder state is 6.7 KB and is touched only by the decode path (no DMA,
 * no ISR), so it lives in PSRAM and leaves that much internal RAM to the
 * I2S rings that need it. */
static mp3dec_t *s_dec;
static int16_t *s_pcm;      /* MINIMP3_MAX_SAMPLES_PER_FRAME */
static int16_t *s_pcm16;    /* resampled, worst case one output per input */
static down_rs_t s_rs;
static int s_in_rate;

static bool ensure_buffers(void)
{
    if (s_dec && s_mp3 && s_pcm && s_pcm16) return true;
    s_dec = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM);
    s_mp3 = (uint8_t *)heap_caps_malloc(MP3_BUF, MALLOC_CAP_SPIRAM);
    s_pcm = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_pcm16 = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_dec || !s_mp3 || !s_pcm || !s_pcm16) {
        /* Free whatever landed. Leaving a partial set behind would pass the
         * s_mp3 sentinel on the next call with s_pcm/s_pcm16 still NULL and
         * crash the decoder on the first frame. */
        free(s_dec);
        s_dec = NULL;
        free(s_mp3);
        s_mp3 = NULL;
        free(s_pcm);
        s_pcm = NULL;
        free(s_pcm16);
        s_pcm16 = NULL;
        ESP_LOGE(TAG, "no buffer for playback (need %d KB of PSRAM)", (MP3_BUF + 3 * 4096) / 1024);
        return false;
    }
    mp3dec_init(s_dec);
    return true;
}

static void resampler_init(down_rs_t *r, int in_rate, int out_rate)
{
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    r->pos = 0;
    r->prev = 0;
}

/* Linear interpolation; state carries across calls, so out must hold
 * n * out/in + 2 samples. */
static size_t resample(down_rs_t *r, const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    if (!n) return 0;
    while ((r->pos >> 16) < n) {
        size_t i = r->pos >> 16;
        int32_t a = i ? in[i - 1] : r->prev;
        int32_t b = in[i];
        out[o++] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)(r->pos & 0xffff)) >> 16));
        r->pos += r->step;
    }
    r->pos -= (uint32_t)n << 16;
    r->prev = in[n - 1];
    return o;
}

/* An ID3v2 tag ahead of the audio would be read as a frame; the bundled
 * minimp3 has no ID3 handling. Returns how many bytes to drop. */
static size_t id3_len(const uint8_t *p, size_t n)
{
    if (n < 10 || memcmp(p, "ID3", 3) != 0) return 0;
    size_t sz = ((size_t)(p[6] & 0x7f) << 21) | ((size_t)(p[7] & 0x7f) << 14)
                | ((size_t)(p[8] & 0x7f) << 7) | (size_t)(p[9] & 0x7f);
    return 10 + sz + ((p[5] & 0x10) ? 10 : 0);   /* + footer, if flagged */
}

/* Decodes what is buffered and writes it to the speaker. `last` is set once
 * nothing more is coming, which is what lets the decoder flush its tail. */
static void drain(bool last)
{
    if (!s_len) return;
    size_t dropped = id3_len(s_mp3, s_len);
    if (dropped) {
        if (dropped >= s_len) {
            s_len = 0;
            return;
        }
        memmove(s_mp3, s_mp3 + dropped, s_len - dropped);
        s_len -= dropped;
        ESP_LOGI(TAG, "skipped a %u-byte ID3 tag", (unsigned)dropped);
    }
    const size_t hold = last ? 0 : MP3_HOLD;
    size_t off = 0;
    mp3dec_frame_info_t info;
    while (s_len - off > hold) {
        int samples = mp3dec_decode_frame(s_dec, s_mp3 + off, s_len - off, s_pcm, &info);
        if (!info.frame_bytes) break;
        off += info.frame_bytes;
        if (!samples) continue;
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                s_pcm[k] = (int16_t)((s_pcm[2 * k] + s_pcm[2 * k + 1]) / 2);
            }
        }
        if (s_in_rate != info.hz) {
            ESP_LOGI(TAG, "playing %d Hz, %d ch, %d kbps", info.hz, info.channels, info.bitrate_kbps);
            s_in_rate = info.hz;
            resampler_init(&s_rs, info.hz, MUSE_AUDIO_RATE);
        }
        size_t n = resample(&s_rs, s_pcm, samples, s_pcm16);
        if (n && !s_abort) {
            muse_audio_write(s_pcm16, n);
        }
    }
    if (off) {
        memmove(s_mp3, s_mp3 + off, s_len - off);
        s_len -= off;
    }
}

static esp_err_t on_http_data(esp_http_client_event_t *ev)
{
    if (ev->event_id != HTTP_EVENT_ON_DATA || ev->data_len <= 0 || s_abort) {
        return ESP_OK;
    }
    size_t off = 0;
    while (off < (size_t)ev->data_len && !s_abort) {
        size_t room = MP3_BUF - s_len;
        if (room < 1024) {
            drain(false);          /* makes room; frame-sized consumption */
            room = MP3_BUF - s_len;
            if (room < 1024) {
                ESP_LOGW(TAG, "decoder is not keeping up");
                s_dropped = true;
                return ESP_FAIL;
            }
        }
        size_t take = (size_t)ev->data_len - off < room ? (size_t)ev->data_len - off : room;
        memcpy(s_mp3 + s_len, (const uint8_t *)ev->data + off, take);
        s_len += take;
        off += take;
        drain(false);
    }
    return ESP_OK;
}

typedef struct {
    char *url;
    muse_audio_play_done_cb_t done;
    void *user;
} play_req_t;

static void play_task(void *arg)
{
    play_req_t *req = (play_req_t *)arg;
    bool ok = false;
    const char *code = "unreachable";
    char message[128] = "download failed";

    muse_audio_power(true);   /* left open afterwards: the voice task owns closing it */
    if (ensure_buffers()) {
        s_len = 0;
        s_in_rate = 0;
        mp3dec_init(s_dec);
        const esp_http_client_config_t cfg = {
            .url = req->url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = HTTP_TIMEOUT_MS,
            .buffer_size = HTTP_BUF,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = on_http_data,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) {
            code = "out_of_memory";
            snprintf(message, sizeof(message), "could not create the HTTP client");
        } else {
            esp_err_t err = esp_http_client_perform(c);
            int status = esp_http_client_get_status_code(c);
            int64_t body = esp_http_client_get_content_length(c);
            esp_http_client_cleanup(c);
            if (s_abort) {
                code = "cancelled";
                snprintf(message, sizeof(message), "stopped");
            } else if (err != ESP_OK) {
                code = "unreachable";
                snprintf(message, sizeof(message), "%s", esp_err_to_name(err));
                ESP_LOGW(TAG, "fetch failed: %s", esp_err_to_name(err));
            } else if (status != 200) {
                code = "bad_status";
                snprintf(message, sizeof(message), "endpoint answered %d", status);
                ESP_LOGW(TAG, "endpoint answered %d", status);
            } else {
                drain(true);       /* flush the tail minimp3 was holding back */
                if (s_dropped) {
                    /* The fetch answered 200 but chunks were discarded when
                     * the decoder fell behind; reporting ok would claim a
                     * complete playback that never happened. */
                    ok = false;
                    code = "audio_dropped";
                    snprintf(message, sizeof(message), "decoder fell behind, part of the audio was dropped");
                    ESP_LOGW(TAG, "reporting failure: audio was dropped");
                } else {
                    ok = true;
                    code = "ok";
                    snprintf(message, sizeof(message), "played %lld bytes", (long long)body);
                    ESP_LOGI(TAG, "played %lld bytes", (long long)body);
                }
            }
        }
    } else {
        code = "out_of_memory";
        snprintf(message, sizeof(message), "no PSRAM for the decode buffers");
    }

    /* The callback reports back to the command layer, which is what frees the
     * request context. Clearing s_task after it keeps a second request from
     * reusing the decode buffers while this task is still on them. */
    if (req->done) {
        req->done(ok, code, message, req->user);
    }
    free(req->url);
    free(req);
    s_task = NULL;
    vTaskDelete(NULL);
}

bool muse_audio_play_url(const char *url, muse_audio_play_done_cb_t done, void *user)
{
    if (s_task || !url || !url[0]) {
        return false;
    }
    play_req_t *req = (play_req_t *)calloc(1, sizeof(*req));
    if (!req) return false;
    req->url = strdup(url);
    req->done = done;
    req->user = user;
    if (!req->url) {
        free(req);
        return false;
    }
    s_abort = false;
    s_dropped = false;
    if (xTaskCreate(play_task, "muse_aplay", TASK_STACK, req, TASK_PRIO, &s_task) != pdPASS) {
        free(req->url);
        free(req);
        s_task = NULL;
        ESP_LOGW(TAG, "no task for playback");
        return false;
    }
    return true;
}

void muse_audio_play_stop(void)
{
    s_abort = true;
}

bool muse_audio_play_busy(void)
{
    return s_task != NULL;
}
