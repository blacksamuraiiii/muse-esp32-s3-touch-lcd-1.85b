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
 * Speaking replies out loud.
 *
 * The SDK ships the playback half of this (the turn's MP3 buffer, minimp3 and
 * the resampler) but no speech service, so by default replies are shown at
 * reading pace and the speaker stays quiet. This fills that in: the reply text
 * goes to CONFIG_MUSE_TTS_URL and the MP3 that comes back is handed to the
 * turn, which decodes and plays it.
 *
 * The URL carries the text in a {text} placeholder, so any HTTP service that
 * answers with MP3 works without touching this file:
 *
 *   https://fanyi.baidu.com/gettts?lan=zh&text={text}&spd=5&source=web
 *   http://192.168.1.1:5050/tts?voice=zh-CN-XiaoxiaoNeural&text={text}
 *
 * That second form is how an Edge TTS bridge slot in: the device side does not
 * care who synthesises, only that MP3 comes back.
 *
 * One fetch runs at a time, on its own task, feeding a stream buffer that the
 * chat session drains into the turn on its own task (the turn's MP3 buffer is
 * not shared). Long replies are split into several requests so the URL stays a
 * sane length and playback can start before the rest has been fetched.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Expand the template with one short text, for callers that fetch one
// pre-generated word audio file instead of a reply sentence (the
// word-cards prefetch). The bridge answers /tts with the cached MP3
// when it has one, so this is the same endpoint, host, key and voice
// the replies already use, and no second server is needed. The request
// is marked word=1, so the bridge answers with the word voice
// (en-US-AriaNeural, -10%) from its disk cache instead of the reply
// voice. Returns false when there is no template, no {text}
// placeholder, the text is longer than one TTS chunk, or the result
// would not fit in `out`.
bool muse_tts_url_for_text(const char *text, char *out, size_t cap);

// A new turn started: audio fetched for the old turn is stale, whatever state
// the previous turn ended in. Bumping the generation makes the worker drop
// those bytes instead of feeding them into the ring of the reply that
// replaces it (C4; the word prefetch already does this when a set restarts).
// The next muse_tts_speak() bumps it again, so this never affects the new
// turn's own audio.
void muse_tts_new_turn(void);

// Open the reply connection now, while the model is still writing: the TLS
// handshake (3-4 s on this board) then costs nothing the listener waits for.
// Fire and forget -- it returns at once, does nothing when a reply is already
// fetching or when the socket from the last turn is still fresh (under 45 s
// idle), and drops a stale one so the reconnect happens here, not on the first
// word of the reply. Needs no text, so it spends no synthesis quota.
void muse_tts_prewarm(void);

// Start speaking `text`. Returns false when a fetch is already running or the
// task could not be started. The text is copied, so the caller may free it.
// `last` is the message's final unit (C2): its last request carries &last=1,
// so the bridge can mark the last chunk and the turn can end on drained audio.
bool muse_tts_speak(const char *text, bool last);

// Queue `text` behind the request in flight, so its audio starts arriving while
// the current one is still being read out. Meant for the streaming speaker's
// next sentence. The buffered audio is kept, unlike muse_tts_speak(), which
// starts a fresh reply. The queue is one deep: false while a request is in
// flight (muse_tts_active()) or when nothing has fetched yet. `last` is the
// message's final unit, as for muse_tts_speak().
bool muse_tts_enqueue(const char *text, bool last);

// True when the most recent completed fetch carried the bridge's last-chunk
// marker (X-TTS-Last: 1): the audio of the reply's final unit really arrived.
// False after a new message starts, after a cancel, and for every fetch whose
// response did not carry the marker (including an old bridge that never sends
// one), so a missing marker is a slow finish, never a stuck one.
bool muse_tts_last_chunk(void);

// Copy up to `cap` bytes of MP3 out of the fetch, without blocking. Returns how
// many were copied, 0 when nothing is ready.
size_t muse_tts_read(uint8_t *out, size_t cap);

// True once every request has finished and the buffer has been drained. Until
// then the fetch may still add more audio.
bool muse_tts_drained(void);

// True while a fetch is in flight (including while its audio is still buffered).
bool muse_tts_active(void);

// Abandon the fetch and drop whatever it buffered. Safe to call when idle.
void muse_tts_cancel(void);

/* Raw fetches over the reply connection. The word and quiz cards pull their
 * audio from the same bridge with `&word=1` and want the bytes untouched, so
 * they reuse the keep-alive client instead of opening a second one (a fresh
 * handshake costs 3.5-4.3 s on this board). No lock: the cards and the chat are
 * never used at the same time, so the reply path is idle whenever a card fetch
 * runs. Returns 1 = HTTP 200 with the body delivered to `sink`, 0 = another
 * status code, -1 = transport failure (the socket has been dropped; the caller
 * may retry once and will pay one handshake). */
typedef void (*muse_tts_raw_sink_t)(const uint8_t *data, size_t len, void *ctx);
int muse_tts_fetch_raw(const char *url, int timeout_ms,
                       muse_tts_raw_sink_t sink, void *ctx);

/* Drop the kept-alive socket. A fetch that stopped mid-body leaves unread bytes
 * on it, so whoever knows that happened calls this; the next request
 * re-handshakes. */
void muse_tts_release_client(void);

/* Reachability probe, so the settings rows can tell the truth instead of
 * claiming "Connected" the moment a host is filled in. The probe asks the
 * service for one very short line through the same template the replies use,
 * so host, certificate, key and voice are all exercised; the state is what the
 * UI shows, and it is never inferred from the configuration alone. */
typedef enum {
    MUSE_SPEECH_NOT_SET = 0,   /* no endpoint in the settings or the build */
    MUSE_SPEECH_OFFLINE,       /* saved, but there is no Wi-Fi to try it on */
    MUSE_SPEECH_UNTESTED,      /* saved, not probed since it was last changed */
    MUSE_SPEECH_TESTING,       /* the probe's request is in flight */
    MUSE_SPEECH_REACHABLE,     /* it answered with audio */
    MUSE_SPEECH_UNREACHABLE,   /* it did not answer, or answered without audio */
} muse_speech_state_t;

typedef struct {
    muse_speech_state_t state;
    char detail[48];   /* one short line, e.g. "HTTP 403" or "Waiting for Wi-Fi" */
} muse_speech_status_t;

// Probe now. Does nothing without an endpoint, without Wi-Fi, or while a probe
// is already running.
void muse_speech_test(void);

// Snapshot of the last probe. No Wi-Fi overrides a stored result, the way
// muse_hatch_status() does, so a row cannot keep saying "Connected" on a
// network that went away.
void muse_speech_status(muse_speech_status_t *out);

const char *muse_speech_state_name(muse_speech_state_t state);

// The speech settings changed: whatever was probed no longer describes them.
void muse_speech_config_changed(void);
