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

#include "muse_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "muse.tts";

/* Sized to cover the gap between one HTTP read and the next turn iteration,
 * not the whole reply: the chat task moves audio into the turn's own buffer as
 * fast as it goes out of the speaker. */
#define TTS_RING_BYTES 16384
#define TTS_HTTP_BUF 1024
/* Per request. Short enough that any proxy in the path is happy with the URL,
 * and that playback starts before a long reply has finished synthesising. */
#define TTS_TEXT_CHUNK 180
/* The TLS handshake runs on this task, and mbedtls wants more than the 8 KB
 * the endpoint itself needs; a short stack shows up as an unexplained
 * "TTS request failed". */
#define TTS_TASK_STACK 12288
#define TTS_TASK_PRIO 4
#define TTS_SEND_TIMEOUT_MS 3000
#define TTS_HTTP_TIMEOUT_MS 15000

static StreamBufferHandle_t s_ring;   /* storage in PSRAM: internal RAM is the scarce one */
static TaskHandle_t s_task;           /* one long-lived worker; its stack lives in PSRAM */
static volatile bool s_fetch_done;    /* the worker has finished every request */
static volatile bool s_abort;
static volatile bool s_busy;          /* a reply is being fetched right now */
static char *volatile s_pending;      /* text handed to the worker by muse_tts_speak */

static size_t url_encode(char *out, size_t cap, const char *in, size_t in_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                     || (c >= '0' && c <= '9')
                     || c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            if (n + 1 >= cap) return 0;
            out[n++] = (char)c;
        } else {
            if (n + 3 >= cap) return 0;
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 0xf];
        }
    }
    out[n] = '\0';
    return n;
}

/* True when a cut just before `i` lands after punctuation, so a request does
 * not end mid-clause. ASCII punctuation, plus the three-byte CJK marks. */
static bool break_before(const char *p, size_t i)
{
    if (i == 0) return false;
    unsigned char prev = (unsigned char)p[i - 1];
    if (prev == ' ' || prev == '\n' || prev == '\t' || prev == ',' || prev == '.'
        || prev == ';' || prev == ':' || prev == '!' || prev == '?') {
        return true;
    }
    /* 。！？ and their fullwidth forms end in 0x82 / 0x81 / 0x9F. */
    if ((prev == 0x82 || prev == 0x81 || prev == 0x9f) && i >= 3
        && (unsigned char)p[i - 3] == 0xE0 + ((unsigned char)p[i - 2] >> 4)) {
        return true;
    }
    return false;
}

static size_t chunk_len(const char *p, size_t left)
{
    if (left <= TTS_TEXT_CHUNK) return left;
    size_t cut = TTS_TEXT_CHUNK;
    while (cut > 1 && ((unsigned char)p[cut] & 0xC0) == 0x80) cut--;
    for (size_t i = cut; i > cut / 2; i--) {
        if (break_before(p, i)) return i;
    }
    return cut;
}

static esp_err_t on_http_data(esp_http_client_event_t *ev)
{
    if (ev->event_id != HTTP_EVENT_ON_DATA || ev->data_len <= 0) {
        return ESP_OK;
    }
    size_t off = 0;
    while (off < (size_t)ev->data_len) {
        size_t n = xStreamBufferSend(s_ring, (const uint8_t *)ev->data + off,
                                     (size_t)ev->data_len - off,
                                     pdMS_TO_TICKS(TTS_SEND_TIMEOUT_MS));
        if (n == 0) {
            /* Nobody is draining: the turn moved on. Stop rather than wedge a
             * socket, and let the task wind down. */
            ESP_LOGW(TAG, "audio not being read, stopping the fetch");
            s_abort = true;
            break;
        }
        off += n;
    }
    return ESP_OK;
}

static bool fetch(const char *url)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = TTS_HTTP_TIMEOUT_MS,
        .buffer_size = TTS_HTTP_BUF,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_data,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGW(TAG, "http client init failed");
        return false;
    }
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    int64_t body = esp_http_client_get_content_length(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TTS request failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "TTS endpoint answered %d", status);
        return false;
    }
    ESP_LOGI(TAG, "TTS got %lld bytes", (long long)body);
    return true;
}

static void fetch_reply(char *text)
{
    const char *url = CONFIG_MUSE_TTS_URL;
    const char *ph = strstr(url, "{text}");
    size_t prefix_len = ph ? (size_t)(ph - url) : strlen(url);
    const char *suffix = ph ? ph + 6 : "";
    char encoded[TTS_TEXT_CHUNK * 3 + 8];

    size_t pos = 0;
    size_t total = strlen(text);
    while (pos < total && !s_abort) {
        size_t n = chunk_len(text + pos, total - pos);
        if (url_encode(encoded, sizeof(encoded), text + pos, n) == 0) {
            ESP_LOGW(TAG, "TTS text chunk does not fit the URL");
            break;
        }
        size_t need = prefix_len + strlen(encoded) + strlen(suffix) + 1;
        char *u = (char *)malloc(need);
        if (!u) break;
        snprintf(u, need, "%.*s%s%s", (int)prefix_len, url, encoded, suffix);
        bool ok = fetch(u);
        free(u);
        if (!ok) break;
        pos += n;
    }

}

/* One long-lived worker: created once with its stack in PSRAM, then woken per
 * reply. It never exits, so there is no caps-aware self-delete to get wrong
 * (and vStreamBufferDeleteWithCaps() double-frees on IDF 6.0.1, esp-idf#18855). */
static void tts_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        char *text = s_pending;
        s_pending = NULL;
        if (text) {
            fetch_reply(text);
            free(text);
        }
        s_fetch_done = true;
        s_busy = false;
    }
}

bool muse_tts_speak(const char *text)
{
    if (!CONFIG_MUSE_TTS_URL[0] || s_busy || !text || !text[0]) {
        return false;
    }
    if (!s_ring) {
        /* PSRAM: a 16 KB stream buffer out of internal RAM starves the rest. */
        s_ring = xStreamBufferCreateWithCaps(TTS_RING_BYTES, 1, MALLOC_CAP_SPIRAM);
        if (!s_ring) {
            ESP_LOGW(TAG, "no stream buffer for TTS audio");
            return false;
        }
    }
    if (!s_task) {
        /* 12 KB of stack does not fit internal RAM here (largest free block was
         * 9 KB on the 185B), so the worker's stack comes from PSRAM. */
        if (xTaskCreateWithCaps(tts_task, "muse_tts", TTS_TASK_STACK, NULL,
                                TTS_TASK_PRIO, &s_task,
                                MALLOC_CAP_SPIRAM) != pdPASS) {
            s_task = NULL;
            ESP_LOGW(TAG, "no task for TTS (PSRAM stack)");
            return false;
        }
    }
    xStreamBufferReset(s_ring);
    s_abort = false;
    s_fetch_done = false;
    char *copy = strdup(text);
    if (!copy) return false;
    s_pending = copy;
    s_busy = true;
    xTaskNotifyGive(s_task);
    return true;
}

size_t muse_tts_read(uint8_t *out, size_t cap)
{
    if (!s_ring) return 0;
    return xStreamBufferReceive(s_ring, out, cap, 0);
}

bool muse_tts_drained(void)
{
    return s_fetch_done && (!s_ring || xStreamBufferIsEmpty(s_ring));
}

bool muse_tts_active(void)
{
    return s_busy;
}

void muse_tts_cancel(void)
{
    s_abort = true;
    s_fetch_done = true;
    if (s_ring) xStreamBufferReset(s_ring);
}
