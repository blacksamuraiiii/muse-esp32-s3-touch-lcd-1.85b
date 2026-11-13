# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise production PSRAM reply handlers with host-side event sinks."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    'CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'
))


class ChatSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()
        constants = source[source.index('#define MIC_RATE'):source.index('/* ---- Voice task')]
        types = source[source.index('enum phase_t'):source.index('/* 10 KB')]
        handlers = source[source.index('static int find_msg('):source.index('static void on_chat_ack(')]
        reset = source[source.index('static bool turn_start('):source.index('static void turn_begin(')]
        # decode() (with pace_silently() and the C2 drain defines) for the
        # F15 drain-grace tests below; nothing else in the file needs it.
        decode = source[source.index('/* Speaker off: queues'):source.index('/* Ends the turn once the reply is complete')]
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "host_compat.h"
#include "cJSON.h"
#include "minimp3.h"
#include "muse_chat_priv.h"
#define ESP_LOGI(...) ((void)0)
/* The streaming drain path under test. */
#define CONFIG_MUSE_STREAMING_TTS 1
/* The repo's fake cJSON (tests/link_fakes) has cJSON_ParseWithLength but no
 * cJSON_Parse macro; the real one defines it, so only fill it in when it is
 * missing. */
#ifndef cJSON_Parse
#define cJSON_Parse(v) cJSON_ParseWithLength((v), strlen(v))
#endif
''' + constants + types + r'''
static turn_t s_turn;
static char s_reply_shown[EV_TEXT];
static int64_t s_last_seq, s_marks[4];
static int captions, console_events;
enum mark_t { M_RELEASE, M_SENT, M_ACK, M_TEXT, M_DONE, M_TTS, M_MP3, M_AUDIO, M_COUNT };
static void mark(mark_t) {}
static int64_t s_now_us = 12345;
static int64_t now_us() { return s_now_us; }
static void emit(muse_hatch_ev_t type, const char *) {
    if (type == MUSE_HATCH_EV_REPLY) captions++;
}
void muse_hatch_console(const char *, const char *, const char *, ...) { console_events++; }
void muse_hatch_tail_words(const char *text, char *out, size_t cap) { strlcpy(out, text, cap); }
bool muse_hatch_caption_at(const char *text, size_t, char *out, size_t cap) {
    strlcpy(out, text, cap); return text[0];
}
static void turn_finish() { s_turn.phase = P_IDLE; }
static void turn_fail(const char *) { turn_finish(); }
static bool ensure_connected() { return true; }
/* turn_start() also reads the connection state; the harness never connects,
 * so the stale-session drop is skipped and the busy streak stays zero. */
static bool s_connected;
static struct { int64_t last_rx_us; } s_conn;
static int s_busy_streak;
static void drop_connection(const char *) {}
bool muse_hatch_configured() { return true; }
static void resampler_init(resampler_t *, int, int) {}
static size_t resample(resampler_t *, const int16_t *, size_t, int16_t *) { return 0; }
/* turn_start() calls these two (prewarm since 1.5.2c, new_turn since 1.5.3a
 * C4); the harness does not link muse_tts.c, so they are no-ops here. */
void muse_tts_prewarm() {}
void muse_tts_new_turn() {}
/* Controllable stand-ins for the TTS fetch state decode() reads. */
static bool s_tts_active, s_tts_last_chunk;
bool muse_tts_active() { return s_tts_active; }
bool muse_tts_last_chunk() { return s_tts_last_chunk; }
/* decode() drains into a fake playback queue: len is what the voice task
 * has not read yet. The decoder itself is stubbed out (the drain tests run
 * with an empty MP3 buffer, or with no room to decode into);
 * mp3dec_decode_frame only reports that there is no frame to take. */
typedef struct fake_streambuf { size_t len, cap; } fake_streambuf_t;
typedef fake_streambuf_t *StreamBufferHandle_t;
static fake_streambuf_t s_out_buf = {0, 65536};
static StreamBufferHandle_t s_out = &s_out_buf;
static size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t h) { return h ? h->cap - h->len : 0; }
static int xStreamBufferIsEmpty(StreamBufferHandle_t h) { return !h || h->len == 0; }
static size_t xStreamBufferSend(StreamBufferHandle_t h, const void *d, size_t n, unsigned t) {
    (void)d; (void)t;
    if (!h) return 0;
    size_t room = h->cap - h->len;
    size_t w = n < room ? n : room;
    h->len += w;
    return w;
}
static std::atomic<uint32_t> s_gen{0};
static int16_t s_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t s_pcm16[MINIMP3_MAX_SAMPLES_PER_FRAME];
void mp3dec_init(mp3dec_t *dec) { (void)dec; }
int mp3dec_decode_frame(mp3dec_t *dec, const uint8_t *mp3, int mp3_bytes, mp3d_sample_t *pcm, mp3dec_frame_info_t *info) {
    (void)dec; (void)mp3; (void)mp3_bytes; (void)pcm;
    info->frame_bytes = 0;
    return 0;
}
''' + reset + handlers + decode + r'''
static void begin(bool typed = false) {
    assert(turn_start(s_turn.gen + 1, typed));
    s_turn.phase = P_WAIT_REPLY;
    s_turn.acked = true;
    strlcpy(s_turn.user_ids[0], "note", sizeof(s_turn.user_ids[0]));
    strlcpy(s_turn.user_ids[1], "parent", sizeof(s_turn.user_ids[1]));
    captions = console_events = 0;
}
static void event(const char *kind, const char *id, const char *parent = "", const char *text = "") {
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", kind);
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "message_id", id);
    if (parent[0]) cJSON_AddStringToObject(payload, "reply_to_message_id", parent);
    cJSON_AddStringToObject(payload, "text", text);
    cJSON_AddStringToObject(payload, "display_text", text);
    on_event(root);
    cJSON_Delete(root);
}
static void rejected_deltas() {
    for (bool typed : {false, true}) {
        begin(typed);
        event("delta.message_start", "other", "elsewhere");
        event("delta.text_append", "other", "", "Wrong reply");
        event("delta.message_done", "other");
        event("message.assistant", "other", "", "Wrong final");
        assert(!s_turn.nmsgs && !captions && !console_events);
        assert(!s_turn.last_content_us && !s_turn.last_event_us);
        event("delta.message_start", "reply", "note");
        event("delta.text_append", "reply", "", "Our reply");
        event("delta.message_done", "reply");
        assert(s_turn.nmsgs == 1 && s_turn.msgs[0].done && s_turn.msgs[0].len == 9);
        assert(typed ? console_events == 2 : captions == 1);
        begin(typed);
        event("message.assistant", "other", "", "New turn");
        assert(s_turn.nmsgs == 1 && s_turn.msgs[0].done);
    }
}
static void valid_parents() {
    begin();
    event("message.assistant", "first", "note", "First");
    event("message.assistant", "second", "parent", "Second");
    event("message.assistant", "third", "first", "Third");
    event("message.assistant", "fourth", "", "Live");
    assert(s_turn.nmsgs == 4);
    for (int i = 0; i < s_turn.nmsgs; i++) assert(s_turn.msgs[i].done);
    cJSON *payload = cJSON_Parse("{\"parent_message_id\":\"elsewhere\"}");
    assert(bind_msg("fallback", payload) == -1);
    cJSON_Delete(payload);
    event("message.assistant", "fallback", "", "Wrong final");
    assert(s_turn.nmsgs == 4);
}
static void bounded_rejections() {
    begin();
    event("delta.message_start", "reply", "note");
    char id[20];
    for (int i = 0; i < 9; i++) { /* exceed the eight rejected IDs retained per turn */
        snprintf(id, sizeof(id), "other%d", i);
        event("delta.message_start", id, "elsewhere");
    }
    event("message.assistant", "other0", "", "Wrong final");
    event("message.assistant", id, "", "Overflow final");
    assert(s_turn.nmsgs == 1 && !captions && !console_events);
    event("delta.text_append", "reply", "", "Our reply");
    event("delta.message_done", "reply");
    event("message.assistant", "second", "note", "Second");
    assert(s_turn.nmsgs == 2 && s_turn.msgs[0].done && s_turn.msgs[1].done);
}
/* F15 drain-grace tests: one finished message whose MP3 has all arrived
 * (mp3_ended, mp3_len 0) and whose fetch is done, so decode() is in the
 * C2 drain wait. The clock is s_now_us; the fake playback queue is
 * s_out_buf (len = audio the voice task has not read yet). */
static char s_texts[MAX_MSGS * TEXT_MAX];
static uint8_t s_mp3buf[4096];
static bool drain_finished() { return s_turn.tts_msg < 0 && s_turn.msgs[0].tts == TTS_FINISHED; }
static void begin_drain(bool last_chunk, bool out_queued) {
    memset(&s_turn, 0, sizeof(s_turn));
    s_turn.texts = s_texts;
    strlcpy(s_turn.texts, "hello", TEXT_MAX);
    s_turn.nmsgs = 1;
    s_turn.tts_msg = 0;
    s_turn.msgs[0].done = true;
    s_turn.msgs[0].len = 5;
    s_turn.msgs[0].spoken = 5;
    s_turn.msgs[0].tts = TTS_ACTIVE;
    s_turn.mp3 = s_mp3buf;
    s_turn.mp3_len = 0;
    s_turn.mp3_ended = true;
    s_turn.kbps = 48;
    s_turn.gen = s_gen.load();
    s_tts_active = false;
    s_tts_last_chunk = last_chunk;
    s_now_us = 1000000;
    s_out_buf.len = out_queued ? 1024 : 0;
}
static void drain_short_grace() {
    /* No marker, queue already dry when the stream ends: finishes 500 ms
     * after the wait arms, not 2 s (F15). */
    begin_drain(false, false);
    decode();
    assert(!drain_finished());
    s_now_us += 499999;
    decode();
    assert(!drain_finished());
    s_now_us += 1;
    decode();
    assert(drain_finished());
}
static void drain_long_grace() {
    /* No marker, output still queued when the stream ends: keeps the 2 s
     * grace from when the wait armed, so it must not finish at 1.5 s. */
    begin_drain(false, true);
    decode();
    assert(!drain_finished());
    s_out_buf.len = 0;   /* the voice task drained the queue */
    s_now_us += 1500000;
    decode();
    assert(!drain_finished());
    s_now_us += 500000;
    decode();
    assert(drain_finished());
}
static void drain_marker_margin() {
    /* The last-chunk marker arrived: the 120 ms margin is unchanged. */
    begin_drain(true, false);
    decode();
    assert(!drain_finished());
    s_now_us += 119999;
    decode();
    assert(!drain_finished());
    s_now_us += 1;
    decode();
    assert(drain_finished());
}
static void drain_never_hangs_while_draining() {
    /* No marker, and the queue stays full for 10 s: the message must not
     * finish while output is still queued -- and must finish once it drains,
     * the long grace having expired during the drain. */
    begin_drain(false, true);
    decode();
    assert(!drain_finished());
    s_now_us += 10000000;
    decode();
    assert(!drain_finished());
    s_out_buf.len = 0;
    decode();
    assert(drain_finished());
}
static void drain_undecoded_blocks() {
    /* MP3 still undecoded (and no room in the queue to decode into): the
     * message must not finish, however much time passes. */
    begin_drain(false, false);
    s_turn.mp3_len = 100;
    s_out_buf.len = s_out_buf.cap;   /* no room: the decode loop cannot run */
    s_now_us += 10000000;
    decode();
    assert(!drain_finished());
    assert(s_turn.mp3_len == 100);
}
static void drain_silent_when_no_frames() {
    /* Not one frame came back: the message falls back to reading pace
     * instead of waiting out a drain grace. */
    begin_drain(false, false);
    s_turn.kbps = 0;
    decode();
    assert(s_turn.silent);
    assert(!drain_finished());
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: rejected_deltas(); break;
    case 1: valid_parents(); break;
    case 2: bounded_rejections(); break;
    case 3: drain_short_grace(); break;
    case 4: drain_long_grace(); break;
    case 5: drain_marker_margin(); break;
    case 6: drain_never_hangs_while_draining(); break;
    case 7: drain_undecoded_blocks(); break;
    case 8: drain_silent_when_no_frames(); break;
    default: return 2;
    }
}
'''
        (out / 'session.cpp').write_text(code)
        flags = ['-Wall', '-Wextra', '-Werror',
                 # silent_frames() is extracted with the handlers but its
                 # callers (hand_sentence/start_tts) are not, so it is
                 # unused in this sliced TU; it is used in production.
                 '-Wno-unused-function',
                 '-I', str(JSON),
                 '-I', str(ROOT / 'tests'), '-I', str(ROOT / 'components/muse'),
                 '-I', str(ROOT / 'components/minimp3/include')]
        cc = shlex.split(os.environ.get('CC', 'cc'))
        cxx = shlex.split(os.environ.get('CXX', 'c++'))
        for tool in (cc, cxx):
            if not tool or shutil.which(tool[0]) is None:
                raise unittest.SkipTest('C/C++ compiler not available: install gcc/g++ or set CC/CXX')
        commands = [
            [*cc, '-std=c11', *flags,
             '-c', str(JSON / 'cJSON.c'), '-o', str(out / 'cjson.o')],
            [*cxx, '-std=gnu++17', *flags,
             str(out / 'session.cpp'), str(out / 'cjson.o'), '-o', str(out / 'session')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'session'

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejected_deltas_do_not_emit_or_complete_voice_or_typed_replies(self):
        self.run_case(0)

    def test_ack_parent_reply_chain_and_parentless_messages_still_work(self):
        self.run_case(1)

    def test_rejection_capacity_preserves_correlation(self):
        self.run_case(2)

    def test_drain_short_grace_without_marker(self):
        self.run_case(3)

    def test_drain_long_grace_while_output_queued(self):
        self.run_case(4)

    def test_drain_marker_margin_unchanged(self):
        self.run_case(5)

    def test_drain_never_hangs_while_draining(self):
        self.run_case(6)

    def test_drain_undecoded_mp3_blocks_finish(self):
        self.run_case(7)

    def test_drain_silent_when_no_frames(self):
        self.run_case(8)
