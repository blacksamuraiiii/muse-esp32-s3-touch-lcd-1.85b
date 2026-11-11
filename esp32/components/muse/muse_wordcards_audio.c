/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_wordcards_audio.c -- word-cards pronunciation, see
 * muse_wordcards_audio.h.
 *
 * The fetch is one HTTP GET per word against the reply TTS's /tts
 * URL (muse_tts_url_for_text, with word=1 appended): the bridge
 * answers a word=1 request from its word-audio cache on disk in the
 * word voice (en-US-AriaNeural, -10%), so no Azure synthesis happens
 * for a flip and no Chinese reply voice ever reads an English word. The playback is minimp3 from the cached bytes, the same
 * decode shape as muse_audio_play.c (ID3 skip, stereo mix-down,
 * linear resample to 16 kHz) but from memory instead of HTTP, so a
 * flip plays at once with no network in the path.
 *
 * Both tasks keep their stacks and decode buffers in PSRAM: a 12 KB
 * stack does not fit internal RAM on the 185B (largest free block was
 * 9 KB), the same reason muse_tts.c gives its worker a PSRAM stack.
 */
#include "muse_wordcards_audio.h"

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
#include "muse_tts.h"

static const char *TAG = "muse.wordcards_audio";

/* One attempt per word (user decision: no retry beyond the one the
 * reply path already does -- and this fetch has none at all, the file
 * either exists on the server or it does not). */
#define WC_HTTP_TIMEOUT_MS 8000
#define WC_HTTP_BUF 1024
#define WC_TASK_STACK 12288
/* The playback task needs more than the fetch task: measured on the 185B it
 * overflowed 12288 (panic: "A stack overflow in task muse_wc_play") while
 * decoding + resampling + muse_audio_write ran. 24 KB still fits PSRAM
 * comfortably and leaves headroom for a deeper codec call path; the
 * high-water mark logged at the end of each playback is the number to tune
 * against, not this guess. */
#define WC_PLAY_STACK 24576
#define WC_TASK_PRIO 4

/* Trailing-silence trim. Azure pads a one-word MP3 to a fixed length: a
 * 11232-byte file is 1.872 s for ~0.5 s of speech, the rest silence. The
 * playback used to spend that tail in the task, holding the speaker and
 * the word's slot, so the next flip found a busy task and (before this
 * change) made the LVGL thread wait for it. Stop writing once the tail
 * has been silent this long. Silence inside a word is shorter, and the
 * trim only arms after the word has actually spoken. */
#define WC_SPEECH_PEAK     300      /* below this peak counts as silence */
#define WC_SPEECH_MIN_MS   200u     /* audible this long before trimming */
#define WC_TAIL_SILENCE_MS 200u     /* silent this long: the word is done */

/* PSRAM slots: one per word of the current set. Allocated on the
 * first prefetch and kept: a round reuses them, a restart refills
 * them. 32 KB each is the MUSE_WORDCARDS_MAX_MP3_BYTES cap. */
static uint8_t *s_slot[MUSE_WORDCARDS_WORDS];
static size_t s_slot_len[MUSE_WORDCARDS_WORDS];

static TaskHandle_t s_fetch_task;
static volatile bool s_fetch_abort;
static volatile uint32_t s_fetch_gen;
/* A prefetch asked for while an older one is still running: the old
 * task is told to stop, and this one starts when it exits. */
static muse_wordcards_t *s_fetch_wanted_g;
static bool s_fetch_wanted;

static TaskHandle_t s_play_task;
static volatile bool s_play_abort;
/* A word asked for while another is still playing: the running task is
 * told to stop, and this one starts as it exits. Queued here instead of
 * waited for, because the caller is the LVGL thread (see
 * muse_wordcards_audio_play). */
static muse_wordcards_t *s_play_wanted_g;
static int s_play_wanted_idx = -1;

/* Decode state, PSRAM like muse_audio_play.c (6.7 KB decoder, touched
 * only by the decode path). */
static mp3dec_t *s_dec;
static int16_t *s_pcm;
static int16_t *s_pcm16;

typedef struct {
    uint32_t step; /* Q16 input samples per output sample */
    uint32_t pos;
    int16_t prev;
} wc_rs_t;

static void rs_init(wc_rs_t *r, int in_rate, int out_rate)
{
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    r->pos = 0;
    r->prev = 0;
}

static size_t rs_resample(wc_rs_t *r, const int16_t *in, size_t n,
                          int16_t *out)
{
    size_t o = 0;
    if (!n) {
        return 0;
    }
    while ((r->pos >> 16) < n) {
        size_t i = r->pos >> 16;
        int32_t a = i ? in[i - 1] : r->prev;
        int32_t b = in[i];
        out[o++] = (int16_t)(a + (int32_t)(((int64_t)(b - a) *
                                            (int64_t)(r->pos & 0xffff)) >> 16));
        r->pos += r->step;
    }
    r->pos -= (uint32_t)n << 16;
    r->prev = in[n - 1];
    return o;
}

/* An ID3v2 tag ahead of the audio would be read as a frame; the
 * bundled minimp3 has no ID3 handling. Same helper as
 * muse_audio_play.c. Returns how many bytes to drop. */
static size_t id3_len(const uint8_t *p, size_t n)
{
    if (n < 10 || memcmp(p, "ID3", 3) != 0) {
        return 0;
    }
    size_t sz = ((size_t)(p[6] & 0x7f) << 21) |
                ((size_t)(p[7] & 0x7f) << 14) |
                ((size_t)(p[8] & 0x7f) << 7) | (size_t)(p[9] & 0x7f);
    return 10 + sz + ((p[5] & 0x10) ? 10 : 0);
}

static bool ensure_slots(void)
{
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        if (!s_slot[i]) {
            s_slot[i] = (uint8_t *)heap_caps_malloc(
                MUSE_WORDCARDS_MAX_MP3_BYTES, MALLOC_CAP_SPIRAM);
            if (!s_slot[i]) {
                ESP_LOGE(TAG, "no PSRAM for the word-audio cache");
                return false;
            }
        }
    }
    return true;
}

static bool ensure_decode_buffers(void)
{
    if (s_dec && s_pcm && s_pcm16) {
        return true;
    }
    s_dec = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM);
    s_pcm = (int16_t *)heap_caps_malloc(
        MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_pcm16 = (int16_t *)heap_caps_malloc(
        MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t),
        MALLOC_CAP_SPIRAM);
    if (!s_dec || !s_pcm || !s_pcm16) {
        free(s_dec);
        s_dec = NULL;
        free(s_pcm);
        s_pcm = NULL;
        free(s_pcm16);
        s_pcm16 = NULL;
        ESP_LOGE(TAG, "no PSRAM for the decode buffers");
        return false;
    }
    return true;
}

/* --- prefetch ----------------------------------------------------- */

/* One fetch's write target, set by the task before each GET. The
 * event handler runs on the HTTP client's task, but only this fetch
 * task starts a GET, so there is no concurrent writer. */
static int s_fetch_idx;
static size_t s_fetch_len;
static bool s_fetch_overflow;

static esp_err_t on_fetch_data(esp_http_client_event_t *ev)
{
    if (ev->event_id != HTTP_EVENT_ON_DATA || ev->data_len <= 0) {
        return ESP_OK;
    }
    if (s_fetch_abort) {
        return ESP_OK; /* drained, not stored */
    }
    size_t room = MUSE_WORDCARDS_MAX_MP3_BYTES - s_fetch_len;
    size_t take = (size_t)ev->data_len < room ? (size_t)ev->data_len
                                              : room;
    if (take) {
        memcpy(s_slot[s_fetch_idx] + s_fetch_len,
               (const uint8_t *)ev->data, take);
        s_fetch_len += take;
    }
    if ((size_t)ev->data_len > take) {
        s_fetch_overflow = true; /* file bigger than the cap: reject it */
    }
    return ESP_OK;
}

/* The one connection all four words of a set travel on. A fresh TLS
 * handshake is the whole cost of a word (measured 2026-11-10: 3.5-4.3 s
 * per word on the board, while the server answers in 18 ms once the
 * socket is up), so the client is created once with keep-alive and
 * reused; only the URL changes per word. */
static esp_http_client_handle_t s_client;

static bool ensure_client(const char *url)
{
    if (s_client) {
        return true;
    }
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = WC_HTTP_TIMEOUT_MS,
        .buffer_size = WC_HTTP_BUF,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_fetch_data,
        .keep_alive_enable = true,
    };
    s_client = esp_http_client_init(&cfg);
    return s_client != NULL;
}

static void drop_client(void)
{
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

/* One GET on the shared connection. Returns 1 = cached READY, 0 = failed
 * (mark FAILED), -1 = transport failure (mark FAILED and abandon the
 * rest of the set). One retry on a fresh connection: a kept-alive socket
 * the server closed while it was idle fails exactly like a real outage,
 * and that retry is the difference between a silent word and a word. */
static int fetch_one(muse_wordcards_t *g, int idx, uint32_t gen)
{
    const muse_wordcards_set_t *set =
        &MUSE_WORDCARDS_SETS_TABLE[g->set];
    char url[512];
    if (!muse_tts_url_for_text(set->words[idx].en, url, sizeof(url))) {
        return 0; /* no speech service configured */
    }
    esp_err_t err = ESP_FAIL;
    int status = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ensure_client(url)) {
            return -1;
        }
        esp_http_client_set_url(s_client, url);
        s_fetch_idx = idx;
        s_fetch_len = 0;
        s_fetch_overflow = false;
        err = esp_http_client_perform(s_client);
        status = esp_http_client_get_status_code(s_client);
        if (s_fetch_abort || gen != g->set_gen) {
            return 0; /* superseded: the bookkeeping ignores the mark */
        }
        if (err == ESP_OK) {
            break;
        }
        /* The socket is suspect: start the retry from a new one. */
        drop_client();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "word %s: fetch failed: %s", set->words[idx].en,
                 esp_err_to_name(err));
        return -1;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "word %s: endpoint answered %d", set->words[idx].en,
                 status);
        return 0;
    }
    if (s_fetch_overflow ||
        !muse_wordcards_mp3_ok(s_slot[idx], s_fetch_len)) {
        ESP_LOGW(TAG, "word %s: bad MP3 payload (%u bytes)",
                 set->words[idx].en, (unsigned)s_fetch_len);
        return 0;
    }
    s_slot_len[idx] = s_fetch_len;
    muse_wordcards_cache_ready(g, idx, gen, s_fetch_len);
    ESP_LOGI(TAG, "word %s: cached %u bytes", set->words[idx].en,
             (unsigned)s_fetch_len);
    return 1;
}
typedef struct {
    muse_wordcards_t *g;
    uint32_t gen;
} fetch_req_t;

/* The task's only exit: clear the handle, then start a prefetch that
 * was asked for while this one was running (a restart during a slow
 * fetch). Starting it here, rather than in the caller, is what keeps
 * a restart from stranding the new generation with FETCHING marks and
 * no task to resolve them. */
static void fetch_task_exit(void)
{
    s_fetch_task = NULL;
    if (s_fetch_wanted) {
        muse_wordcards_t *g = s_fetch_wanted_g;
        s_fetch_wanted = false;
        muse_wordcards_audio_prefetch(g);
    }
    vTaskDelete(NULL);
}

static void fetch_task(void *arg)
{
    fetch_req_t *req = (fetch_req_t *)arg;
    muse_wordcards_t *g = req->g;
    uint32_t gen = req->gen;
    free(req);
    if (!ensure_slots()) {
        for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
            muse_wordcards_cache_failed(g, i, gen);
        }
        fetch_task_exit();
    }
    /* A playback in flight owns a slot's bytes: wait for it to finish
     * (a word is ~1 s) rather than overwrite audio being decoded. */
    for (int i = 0; i < 20 && s_play_task && !s_fetch_abort; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    muse_wordcards_cache_begin(g);
    for (int i = 0; i < MUSE_WORDCARDS_WORDS; i++) {
        if (s_fetch_abort || gen != g->set_gen) {
            break;
        }
        int r = fetch_one(g, i, gen);
        if (r == 0) {
            muse_wordcards_cache_failed(g, i, gen);
        } else if (r < 0) {
            /* Transport failure: the host is unreachable, so the rest
             * of the set would fail the same way -- mark them FAILED
             * now instead of paying three more timeouts. */
            muse_wordcards_cache_failed(g, i, gen);
            for (int j = i + 1; j < MUSE_WORDCARDS_WORDS; j++) {
                muse_wordcards_cache_failed(g, j, gen);
            }
            break;
        }
    }
    fetch_task_exit();
}

static void start_fetch(muse_wordcards_t *g)
{
    fetch_req_t *req = (fetch_req_t *)calloc(1, sizeof(*req));
    if (!req) {
        return;
    }
    req->g = g;
    req->gen = g->set_gen;
    s_fetch_abort = false;
    if (xTaskCreateWithCaps(fetch_task, "muse_wc_fetch", WC_TASK_STACK,
                            req, WC_TASK_PRIO, &s_fetch_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        free(req);
        s_fetch_task = NULL;
        ESP_LOGW(TAG, "no task for word-audio prefetch");
    }
}

void muse_wordcards_audio_prefetch(muse_wordcards_t *g)
{
    if (!g) {
        return;
    }
    if (s_fetch_task) {
        /* A fetch for the old generation is still running (a restart
         * during a slow fetch): ask it to stop and remember to start
         * this one when it exits -- fetch_task_exit() picks it up.
         * The old task may have exited between the check and the
         * flag, so check again: if it is already gone, start now
         * instead of waiting for an exit that already happened. */
        s_fetch_wanted_g = g;
        s_fetch_wanted = true;
        s_fetch_abort = true;
        if (!s_fetch_task && s_fetch_wanted) {
            s_fetch_wanted = false;
            start_fetch(g);
        }
        return;
    }
    start_fetch(g);
}

void muse_wordcards_audio_prefetch_abort(void)
{
    s_fetch_abort = true;
}

/* --- playback ----------------------------------------------------- */

typedef struct {
    int idx;
    size_t len;
    const char *en;   /* the word, for the log only (static table string) */
} play_req_t;

bool muse_wordcards_audio_play(muse_wordcards_t *g, int i);

/* The task's only exit: clear the handle, then start a word that was asked
 * for while this one was playing (a fast second flip), then delete. The
 * queued start must happen AFTER the handle is cleared, or the new call
 * sees a busy task and queues itself again. */
static void play_task_exit(void)
{
    s_play_task = NULL;
    if (s_play_wanted_idx >= 0) {
        muse_wordcards_t *g = s_play_wanted_g;
        int idx = s_play_wanted_idx;
        s_play_wanted_g = NULL;
        s_play_wanted_idx = -1;
        if (g && idx >= 0) {
            (void)muse_wordcards_audio_play(g, idx);
        }
    }
    vTaskDelete(NULL);
}

static void play_task(void *arg)
{
    play_req_t *req = (play_req_t *)arg;
    int idx = req->idx;
    size_t len = req->len;
    const char *en = req->en;
    free(req);
    ESP_LOGI(TAG, "play %s: %u bytes (stack %u left at start)",
             en, (unsigned)len,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    muse_audio_power(true); /* left open: the voice task owns closing */
    if (!ensure_decode_buffers()) {
        play_task_exit();
    }
    /* The slot belongs to the current round; copy the length, not the
     * pointer, into this task -- the bytes stay put until the next
     * prefetch, which waits for this task to finish. */
    mp3dec_init(s_dec);
    uint8_t *p = s_slot[idx];
    size_t n = len;
    size_t drop = id3_len(p, n);
    if (drop && drop < n) {
        p += drop;
        n -= drop;
    }
    /* Initialised at declaration, not left to rs_init(): gcc cannot prove the
     * first rs_resample() comes after it and -Werror=maybe-uninitialized
     * rejects the build. A 1:1 step is also the safe default -- a zero step
     * would spin rs_resample() forever if it ever ran uninitialised. */
    wc_rs_t rs = {.step = 1u << 16, .pos = 0, .prev = 0};
    int in_rate = 0;
    size_t off = 0;
    mp3dec_frame_info_t info;
    uint32_t spoke_ms = 0;
    uint32_t silent_ms = 0;
    while (n - off > 0 && !s_play_abort) {
        int samples =
            mp3dec_decode_frame(s_dec, p + off, n - off, s_pcm, &info);
        if (!info.frame_bytes) {
            break;
        }
        off += info.frame_bytes;
        if (!samples) {
            continue;
        }
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                s_pcm[k] =
                    (int16_t)((s_pcm[2 * k] + s_pcm[2 * k + 1]) / 2);
            }
        }
        if (in_rate != info.hz) {
            in_rate = info.hz;
            rs_init(&rs, info.hz, MUSE_AUDIO_RATE);
        }
        size_t out_n = rs_resample(&rs, s_pcm, samples, s_pcm16);
        if (out_n && !s_play_abort) {
            muse_audio_write(s_pcm16, out_n);
            /* Trailing-silence trim (see WC_TAIL_SILENCE_MS): a silent
             * tail is not worth holding the speaker for, and the next
             * flip would have to wait for it. */
            int16_t peak = 0;
            for (size_t k = 0; k < out_n; k++) {
                int16_t v = s_pcm16[k] < 0 ? (int16_t)-s_pcm16[k] : s_pcm16[k];
                if (v > peak) {
                    peak = v;
                }
            }
            uint32_t ms = (uint32_t)((out_n * 1000u) / MUSE_AUDIO_RATE);
            if (peak > WC_SPEECH_PEAK) {
                spoke_ms += ms;
                silent_ms = 0;
            } else if (spoke_ms >= WC_SPEECH_MIN_MS) {
                silent_ms += ms;
                if (silent_ms >= WC_TAIL_SILENCE_MS) {
                    ESP_LOGI(TAG, "play %s: tail silence trimmed at %u ms",
                             en, (unsigned)(spoke_ms + silent_ms));
                    break;
                }
            }
        }
    }
    ESP_LOGI(TAG, "play %s done: stack %u left", en,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    play_task_exit();
}

bool muse_wordcards_audio_play(muse_wordcards_t *g, int i)
{
    if (!g || i < 0 || i >= MUSE_WORDCARDS_WORDS) {
        return false;
    }
    if (!muse_wordcards_cache_is_ready(g, i)) {
        return false;
    }
    if (s_play_task) {
        /* Supersede, never wait: this is called from the LVGL tick
         * (drain_pending_audio), and waiting here for the running word
         * froze the whole UI for up to a second on every fast tap. Queue
         * this one and let the running task's exit start it. The caller
         * clears its pending mark on true, so the latest tap is the one
         * that plays. */
        s_play_wanted_g = g;
        s_play_wanted_idx = i;
        s_play_abort = true;
        if (!s_play_task && s_play_wanted_idx >= 0) {
            /* It exited between the check and the queue: start now
             * instead of waiting for an exit that already happened. */
            play_req_t *r = (play_req_t *)calloc(1, sizeof(*r));
            if (r) {
                r->idx = i;
                r->len = s_slot_len[i];
                r->en = MUSE_WORDCARDS_SETS_TABLE[g->set].words[i].en;
                s_play_wanted_idx = -1;
                s_play_wanted_g = NULL;
                s_play_abort = false;
                if (xTaskCreateWithCaps(play_task, "muse_wc_play", WC_PLAY_STACK,
                                        r, WC_TASK_PRIO, &s_play_task,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
                    free(r);
                    s_play_task = NULL;
                    return false;
                }
            }
        }
        return true;
    }
    play_req_t *req = (play_req_t *)calloc(1, sizeof(*req));
    if (!req) {
        return false;
    }
    req->idx = i;
    req->len = s_slot_len[i];
    req->en = MUSE_WORDCARDS_SETS_TABLE[g->set].words[i].en;
    s_play_abort = false;
    if (xTaskCreateWithCaps(play_task, "muse_wc_play", WC_PLAY_STACK,
                            req, WC_TASK_PRIO, &s_play_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        free(req);
        s_play_task = NULL;
        ESP_LOGW(TAG, "no task for word-audio playback");
        return false;
    }
    return true;
}

void muse_wordcards_audio_stop(void)
{
    s_play_abort = true;
}

bool muse_wordcards_audio_busy(void)
{
    return s_play_task != NULL;
}
