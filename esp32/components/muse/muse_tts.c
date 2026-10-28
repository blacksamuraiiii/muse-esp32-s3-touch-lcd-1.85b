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

#include "muse_sentence.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
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
/* The request line must fit the client's TX buffer, whose default is only 512
 * bytes (DEFAULT_HTTP_BUF_SIZE): percent-encoding turns one 3-byte Chinese
 * character into 9 characters, so a 180-byte chunk of Chinese needs ~540 for
 * the text plus ~53 of method/path/version. 512 overflows -> "Out of buffer"
 * from esp_http_client and the chunk is lost (ESP_FAIL). */
#define TTS_HTTP_TX_BUF 2048
/* Per request. Short enough that any proxy in the path is happy with the URL,
 * and that playback starts before a long reply has finished synthesising. */
#define TTS_TEXT_CHUNK MUSE_SENTENCE_CHUNK
/* The TLS handshake runs on this task, and mbedtls wants more than the 8 KB
 * the endpoint itself needs; a short stack shows up as an unexplained
 * "TTS request failed". */
#define TTS_TASK_STACK 12288
#define TTS_TASK_PRIO 4
#define TTS_SEND_TIMEOUT_MS 3000
#define TTS_HTTP_TIMEOUT_MS 8000
/* The reply's first request is the one the listener is waiting for: give up
 * on it this soon instead, so a stalled connect or a service that has not
 * started answering costs three seconds before the retry, not eight. Once
 * the headers are in, the rest of the body gets the full timeout again. */
#define TTS_HTTP_FIRST_MS 3000
#define TTS_RETRY_DELAY_MS 150

static StreamBufferHandle_t s_ring;   /* storage in PSRAM: internal RAM is the scarce one */
static TaskHandle_t s_task;           /* one long-lived worker; its stack lives in PSRAM */
static volatile bool s_fetch_done;    /* the worker has finished every request */
static volatile bool s_abort;
/* Bumped by every muse_tts_speak() and muse_tts_cancel(). A fetch remembers the
 * generation it started in; once that no longer matches, its bytes are dropped
 * instead of being pushed into the ring of whatever is playing now. */
static volatile uint32_t s_gen;
static volatile uint32_t s_fetch_gen;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;  /* s_pending/s_busy handoff */
static volatile bool s_busy;          /* a reply is being fetched right now */
static bool s_short_wait;            /* this fetch started on TTS_HTTP_FIRST_MS */
static bool s_reply_first;           /* the next fetch_reply() is a reply's first */
static char *volatile s_pending;      /* text handed to the worker by muse_tts_speak */
static volatile size_t s_bytes;       /* bytes of the request being read */
static volatile int64_t s_first_byte_us; /* when the first body byte arrived */

/* Strict payload gate. The decoder plays noise out of anything it is handed, so
 * an error page (HTML) or a truncated body must never reach the ring. The first
 * bytes are held back until they look like MPEG audio: an MPEG frame sync
 * (0xFF) or an ID3v2 tag. Matching only 0xFF would reject plenty of valid
 * ID3-prefixed files. */
#define TTS_PREFIX_BYTES 8
static uint8_t s_prefix[TTS_PREFIX_BYTES];
static size_t s_prefix_n;
static bool s_payload_open;   /* the prefix passed: bytes go to the ring now */
static bool s_payload_bad;    /* the prefix failed: drop the rest, do not retry */

static bool mp3_prefix_ok(const uint8_t *p, size_t n)
{
    if (n < 4) {
        return false;
    }
    if (p[0] == 0xFF) {
        return true;                            /* MPEG frame sync */
    }
    return memcmp(p, "ID3", 3) == 0;            /* ID3v2 tag */
}

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

static size_t chunk_len(const char *p, size_t left)
{
    if (left <= TTS_TEXT_CHUNK) return left;
    size_t cut = TTS_TEXT_CHUNK;
    while (cut > 1 && ((unsigned char)p[cut] & 0xC0) == 0x80) cut--;
    for (size_t i = cut; i > cut / 2; i--) {
        if (muse_sentence_break_before(p, i)) return i;
    }
    return cut;
}

/* Bytes on their way to the decoder. False when nobody is draining. */
static bool ring_send(const uint8_t *d, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t n = xStreamBufferSend(s_ring, d + off, len - off, pdMS_TO_TICKS(TTS_SEND_TIMEOUT_MS));
        if (n == 0) {
            /* Nobody is draining: the turn moved on. Stop rather than wedge a
             * socket, and let the task wind down. */
            ESP_LOGW(TAG, "audio not being read, stopping the fetch");
            s_abort = true;
            return false;
        }
        off += n;
    }
    return true;
}

/* Holds the first TTS_PREFIX_BYTES back until they look like MPEG audio, then
 * streams everything through. A failing prefix marks the whole reply bad so the
 * rest is dropped and the caller does not retry. */
static void gate_feed(const uint8_t *d, size_t len)
{
    size_t off = 0;
    if (!s_payload_open) {
        size_t room = TTS_PREFIX_BYTES - s_prefix_n;
        size_t take = len < room ? len : room;
        memcpy(s_prefix + s_prefix_n, d, take);
        s_prefix_n += take;
        off = take;
        if (s_prefix_n < TTS_PREFIX_BYTES) {
            return;                 /* not enough to judge yet */
        }
        if (!mp3_prefix_ok(s_prefix, s_prefix_n)) {
            ESP_LOGW(TAG, "muse.tts: bad MP3 payload len=%u first=0x%02x", (unsigned)s_bytes, s_prefix[0]);
            s_payload_bad = true;
            s_abort = true;
            return;
        }
        s_payload_open = true;
        if (!ring_send(s_prefix, TTS_PREFIX_BYTES)) {
            return;
        }
    }
    if (off < len && !s_abort) {
        ring_send(d + off, len - off);
    }
}

static esp_err_t on_http_data(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_HEADER || ev->event_id == HTTP_EVENT_ON_DATA) {
        if (s_short_wait && ev->client) {
            /* The wait that matters is over: the body may stream for as long as
             * synthesising it takes. */
            s_short_wait = false;
            esp_http_client_set_timeout_ms(ev->client, TTS_HTTP_TIMEOUT_MS);
        }
        if (ev->event_id == HTTP_EVENT_ON_HEADER) {
            return ESP_OK;
        }
    }
    if (ev->event_id != HTTP_EVENT_ON_DATA || ev->data_len <= 0) {
        return ESP_OK;
    }
    /* Cancelled (barge-in) or taken over by a newer reply: drop these bytes.
     * Returning an error here would not help - esp_http_client's http_on_body()
     * ignores what the handler returns - so the socket is left to drain on its
     * own, in the background, while the next turn speaks over it. */
    if (s_abort || s_fetch_gen != s_gen) {
        return ESP_OK;
    }
    if (s_first_byte_us == 0) {
        s_first_byte_us = esp_timer_get_time();
    }
    s_bytes += (size_t)ev->data_len;
    gate_feed((const uint8_t *)ev->data, (size_t)ev->data_len);
    return ESP_OK;
}

/* A body shorter than the gate: judge it on what arrived and be done. */
static void gate_finish(void)
{
    if (s_abort || s_fetch_gen != s_gen) {
        return;
    }
    if (s_payload_open || s_payload_bad || !s_prefix_n) {
        return;
    }
    if (mp3_prefix_ok(s_prefix, s_prefix_n)) {
        s_payload_open = true;
        ring_send(s_prefix, s_prefix_n);
    } else {
        ESP_LOGW(TAG, "muse.tts: bad MP3 payload len=%u first=0x%02x", (unsigned)s_bytes, s_prefix[0]);
        s_payload_bad = true;
    }
}

static bool fetch_once(const char *url, int timeout_ms)
{
    /* Some TTS endpoints refuse a request with no browser User-Agent. */
    const esp_app_desc_t *app = esp_app_get_description();
    char user_agent[48];
    snprintf(user_agent, sizeof(user_agent), "MuseGadget/%s", app->version);

    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .buffer_size = TTS_HTTP_BUF,
        .buffer_size_tx = TTS_HTTP_TX_BUF,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = user_agent,
        .event_handler = on_http_data,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGW(TAG, "http client init failed");
        return false;
    }
    s_bytes = 0;
    s_first_byte_us = 0;
    s_prefix_n = 0;
    s_payload_open = false;
    s_payload_bad = false;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    int64_t body = esp_http_client_get_content_length(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TTS request failed: %s (url %u bytes, budget %u)",
                 esp_err_to_name(err), (unsigned)strlen(url),
                 (unsigned)TTS_HTTP_TX_BUF);
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "TTS endpoint answered %d", status);
        return false;
    }
    gate_finish();
    if (s_payload_bad) {
        /* Nothing went to the decoder: the reply stays on screen as text. */
        return false;
    }
    /* A streamed reply has no Content-Length (-1), so report what we read and
     * how long the first byte took: that is the number the stream change moves. */
    ESP_LOGI(TAG, "TTS got %u bytes (content-length %lld, first byte %d ms, total %d ms)",
             (unsigned)s_bytes, (long long)body,
             s_first_byte_us ? (int)((s_first_byte_us - t0) / 1000) : -1,
             (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

/* One retry, but only when nothing reached the decoder. A 502 carries an error
 * page that counts as bytes yet never opened the payload, so it is retried;
 * re-requesting after a partial stream instead would feed the decoder the same
 * audio twice. */
static bool fetch(const char *url, int timeout_ms)
{
    s_short_wait = timeout_ms < TTS_HTTP_TIMEOUT_MS;
    bool ok = fetch_once(url, timeout_ms);
    s_short_wait = false;
    if (ok) {
        return true;
    }
    if (!s_payload_open && !s_abort && s_fetch_gen == s_gen) {
        ESP_LOGW(TAG, "nothing decoded (%u bytes), retrying once", (unsigned)s_bytes);
        vTaskDelay(pdMS_TO_TICKS(TTS_RETRY_DELAY_MS));
        if (fetch_once(url, TTS_HTTP_TIMEOUT_MS)) {
            return true;
        }
    }
    return false;
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
    int timeout_ms = s_reply_first ? TTS_HTTP_FIRST_MS : TTS_HTTP_TIMEOUT_MS;
    s_reply_first = false;
    s_fetch_gen = s_gen;   /* from here on, bytes of an older generation stop here */
    while (pos < total && !s_abort && s_fetch_gen == s_gen) {
        size_t n = chunk_len(text + pos, total - pos);
        if (url_encode(encoded, sizeof(encoded), text + pos, n) == 0) {
            ESP_LOGW(TAG, "TTS text chunk does not fit the URL");
            break;
        }
        size_t need = prefix_len + strlen(encoded) + strlen(suffix) + 1;
        char *u = (char *)malloc(need);
        if (!u) break;
        snprintf(u, need, "%.*s%s%s", (int)prefix_len, url, encoded, suffix);
        bool ok = fetch(u, timeout_ms);
        timeout_ms = TTS_HTTP_TIMEOUT_MS;
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
        /* Drain whatever was queued: a barge-in leaves the old fetch in flight,
         * so the reply that replaces it waits here rather than go silent. */
        for (;;) {
            taskENTER_CRITICAL(&s_lock);
            char *text = s_pending;
            s_pending = NULL;
            taskEXIT_CRITICAL(&s_lock);
            if (!text) {
                break;
            }
            fetch_reply(text);
            free(text);
        }
        taskENTER_CRITICAL(&s_lock);
        if (!s_pending) {   /* a reply queued meanwhile keeps the worker busy */
            s_busy = false;
            s_fetch_done = true;
        }
        taskEXIT_CRITICAL(&s_lock);
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
    s_reply_first = true;
    char *copy = strdup(text);
    if (!copy) return false;
    taskENTER_CRITICAL(&s_lock);
    char *stale = s_pending;   /* a cancelled fetch may have left one behind */
    s_pending = copy;
    s_abort = false;
    s_fetch_done = false;
    s_busy = true;
    s_gen++;
    taskEXIT_CRITICAL(&s_lock);
    free(stale);
    xTaskNotifyGive(s_task);
    return true;
}

/* Queue one more request behind the one in flight: the next sentence's audio
 * arrives while this one is still being read out, so the gap between sentences
 * is only the service's first byte, not its whole synthesis time. The ring is
 * deliberately not reset - the bytes of the sentence playing are in it. */
bool muse_tts_enqueue(const char *text)
{
    if (!CONFIG_MUSE_TTS_URL[0] || s_busy || !s_ring || !s_task || !text || !text[0]) {
        return false;
    }
    char *copy = strdup(text);
    if (!copy) return false;
    s_reply_first = false;     /* queued behind audio already playing: no rush */
    taskENTER_CRITICAL(&s_lock);
    char *stale = s_pending;   /* the queue is one deep: this is a leftover */
    s_pending = copy;
    s_fetch_done = false;
    s_busy = true;
    taskEXIT_CRITICAL(&s_lock);
    free(stale);
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
    /* Barge-in. The fetch already on the wire cannot be stopped from here (the
     * IDF event handler's return value is ignored), but bumping the generation
     * makes it stop feeding the ring, and clearing s_busy lets the reply that
     * replaces it speak at once instead of coming out as a silent turn. */
    s_abort = true;
    s_gen++;
    taskENTER_CRITICAL(&s_lock);
    s_busy = false;
    s_fetch_done = true;
    taskEXIT_CRITICAL(&s_lock);
    if (s_ring) xStreamBufferReset(s_ring);
}
