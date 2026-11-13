"""Bridge streaming tests (C1 first-sentence cut; C3 pacing; F16 coverage
for speakable(), error codes, token, fallback, word replies, the B2
truncation rule and the F10/F11 frame and cache edges).

Runs the Flask app on a local port with Azure stubbed out: the fake yields
canned MP3 bytes after a delay that grows with the text, the way Azure's time
to first audio grows with the sentence, so the first-byte timing of the cut
is measurable without a key.

The environment is pinned before importing tts_server, so a real .env beside
tts_server.py (or TTS_* variables in the shell) can never change what the
tests see: BACKEND=azure, no token, no fallback, streaming and pacing on.

Run from tts-server/:  python3 -m unittest test_tts_stream -v
"""

from __future__ import annotations

import os
import shutil
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

# Pin the server's environment before importing it: the module reads its
# config (and a neighbouring .env) at import time, so anything left to the
# ambient environment would change backend, token or pacing under the tests.
os.environ["TTS_BACKEND"] = "azure"
os.environ["TTS_FALLBACK"] = ""
os.environ["TTS_TOKEN"] = ""
os.environ["TTS_STREAM"] = "1"
os.environ["TTS_PACING"] = "1"
os.environ["TTS_TEST_HOOKS"] = ""
os.environ["AZURE_SPEECH_KEY"] = ""

HERE = Path(__file__).resolve().parent
import tts_server as t  # noqa: E402  (the module under test)

CANNED = (HERE.parent / "esp32" / "components" / "muse" / "test_reply.mp3").read_bytes()
assert t.is_mp3(CANNED), "canned MP3 must pass the bridge's own gate"


class _FakeAzureResponse:
    """Canned MP3, first read delayed by 0.05s + 0.03s per character: the
    stand-in for Azure's length-dependent time to first audio."""

    def __init__(self, text: str):
        self._data = CANNED
        self._off = 0
        self._delay = 0.05 + 0.03 * len(text)
        self._slept = False

    def read(self, n=-1):
        if not self._slept:
            time.sleep(self._delay)
            self._slept = True
        if self._off >= len(self._data):
            return b""
        chunk = self._data[self._off : self._off + (n if n > 0 else len(self._data))]
        self._off += len(chunk)
        return chunk

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def _fake_azure_request(text, voice, rate, pitch):
    return _FakeAzureResponse(text)


class _TruncatedFakeResponse:
    """Canned MP3 for the first read, then a mid-stream failure: the shape
    of an Azure connection that dies after the first audio (B2)."""

    def __init__(self, text: str):
        self._data = CANNED
        self._off = 0

    def read(self, n=-1):
        if self._off >= 600:
            raise RuntimeError("mid-stream failure (test stub)")
        chunk = self._data[self._off : self._off + 600]
        self._off += len(chunk)
        return chunk

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class _ServerCase(unittest.TestCase):
    """A local bridge with Azure stubbed out (shared server setup)."""

    @classmethod
    def setUpClass(cls):
        cls._real = t._azure_request
        t._azure_request = _fake_azure_request
        # A fresh cache per run: a stale hit would hide the split entirely.
        t._cache.clear()
        t._cache_bytes = 0
        from werkzeug.serving import make_server

        cls.server = make_server("127.0.0.1", 0, t.app, threaded=True)
        cls.port = cls.server.server_port
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.thread.join(timeout=5)
        t._azure_request = cls._real

    def get(self, query):
        url = "http://127.0.0.1:%d/tts?%s" % (self.port, query)
        t0 = time.monotonic()
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                first = r.read(1)
                ttfb = time.monotonic() - t0
                body = first + r.read()
                return r.status, dict(r.headers), ttfb, body
        except urllib.error.HTTPError as e:
            # Error statuses (400/403/502) are answers too: return them
            # instead of raising, so the error-code tests can read them.
            return e.code, dict(e.headers), time.monotonic() - t0, e.read()

class BridgeTest(_ServerCase):
    def test_split_first_sentence(self):
        self.assertEqual(
            t.split_first_sentence("今天天气不错，适合出门走走，不过要注意保暖。"),
            ("今天天气不错，", "适合出门走走，不过要注意保暖。"),
        )
        self.assertEqual(
            t.split_first_sentence("Hello, world. Bye."), ("Hello,", " world. Bye.")
        )
        self.assertIsNone(t.split_first_sentence("今天天气不错。"))
        self.assertIsNone(t.split_first_sentence("，今天天气不错。"))  # 1-char head
        self.assertIsNone(t.split_first_sentence("你好，"))  # empty tail

    def test_first_cut_is_faster_to_first_byte(self):
        text = "今天天气不错，适合出门走走，不过要注意保暖。"
        q = "text=" + urllib.request.quote(text)
        # Full sentence: one synthesis, delay grows with the whole text.
        status, _h, ttfb_full, body_full = self.get(q)
        self.assertEqual(status, 200)
        # Same text again would be a cache hit, so clear it: the cut must be
        # measured on a fresh synthesis, not on the cached bytes.
        t._cache.clear()
        t._cache_bytes = 0
        # First request: only the head is synthesised before the first byte.
        status, headers, ttfb_cut, body_cut = self.get(q + "&first=1")
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("X-TTS-First-Cut"), "1")
        # The head (7 chars) is far quicker than the whole sentence (22).
        self.assertLess(ttfb_cut, 0.5, "cut first byte %s" % ttfb_cut)
        self.assertGreater(ttfb_full, 0.5, "full first byte %s" % ttfb_full)
        # Both responses still carry the whole reply: head audio + tail audio.
        self.assertEqual(body_cut, CANNED + CANNED)
        self.assertEqual(body_full, CANNED)

    def test_no_comma_means_no_cut(self):
        q = "text=" + urllib.request.quote("今天天气不错。")
        status, headers, _ttfb, body = self.get(q + "&first=1")
        self.assertEqual(status, 200)
        self.assertIsNone(headers.get("X-TTS-First-Cut"))
        self.assertEqual(body, CANNED)

    def test_cut_response_is_cached_whole(self):
        text = "出门记得带伞，今天有雨。"
        q = "text=" + urllib.request.quote(text) + "&first=1"
        status, headers, _ttfb, body = self.get(q)
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("X-TTS-First-Cut"), "1")
        # The next request for the same text is a cache hit with the same bytes.
        status, headers, _ttfb, body2 = self.get(q)
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("X-TTS-Cache"), "hit")
        self.assertEqual(body2, body)

    def test_last_chunk_marker(self):
        # C2: ?last=1 is echoed as X-TTS-Last: 1 on a streaming response, a
        # buffered one (stream=0), and a cache hit; without it, no header.
        q = "text=" + urllib.request.quote("尾巴来了。")
        _s, h, _t, _b = self.get(q + "&last=1")
        self.assertEqual(h.get("X-TTS-Last"), "1")
        _s, h, _t, _b = self.get(q)
        self.assertIsNone(h.get("X-TTS-Last"))
        t._cache.clear()
        t._cache_bytes = 0
        _s, h, _t, _b = self.get(q + "&last=1&stream=0")
        self.assertEqual(h.get("X-TTS-Last"), "1")
        _s, h, _t, _b = self.get(q + "&last=1")
        self.assertEqual(h.get("X-TTS-Cache"), "hit")
        self.assertEqual(h.get("X-TTS-Last"), "1")

    def test_empty_and_unspeakable_text_are_400(self):
        status, _h, _t, _b = self.get("text=")
        self.assertEqual(status, 400)
        status, _h, _t, _b = self.get("text=" + urllib.request.quote("😀"))
        self.assertEqual(status, 400)
        status, _h, _t, _b = self.get("text=" + urllib.request.quote("%*"))
        self.assertEqual(status, 400)

    def test_short_text_is_served_but_not_cached(self):
        text = "好的"   # 2 chars < CACHE_MIN_CHARS (3): served, not remembered
        q = "text=" + urllib.request.quote(text) + "&stream=0"
        key = (t.BACKEND, t.DEFAULT_VOICE, t.DEFAULT_RATE, t.DEFAULT_PITCH, text)
        t._cache.clear()
        t._cache_bytes = 0
        status, headers, _tt, body = self.get(q)
        self.assertEqual(status, 200)
        self.assertEqual(body, CANNED)
        self.assertEqual(headers.get("X-TTS-Cache"), "miss")
        status, headers, _tt, body2 = self.get(q)
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("X-TTS-Cache"), "miss")
        self.assertEqual(body2, body)
        self.assertNotIn(key, t._cache)

    def test_token_gate(self):
        real = t.TTS_TOKEN
        t.TTS_TOKEN = "test-only-token"
        try:
            q = "text=" + urllib.request.quote("你好。")
            status, _h, _t, _b = self.get(q)
            self.assertEqual(status, 403)
            status, _h, _t, body = self.get(q + "&k=test-only-token")
            self.assertEqual(status, 200)
            self.assertEqual(body, CANNED)
        finally:
            t.TTS_TOKEN = real

    def test_upstream_failure_is_502(self):
        real = t._azure_request

        def _boom(text, voice, rate, pitch):
            raise RuntimeError("boom (test stub)")

        t._azure_request = _boom
        try:
            t._cache.clear()
            t._cache_bytes = 0
            q = "text=" + urllib.request.quote("你好。")
            status, _h, _t, _b = self.get(q + "&stream=0")
            self.assertEqual(status, 502)
            status, _h, _t, _b = self.get(q)
            self.assertEqual(status, 502)
        finally:
            t._azure_request = real

    def test_fallback_serves_when_primary_fails(self):
        real_synth = t.SYNTH
        real_fb = t.FALLBACK

        def _boom(text, voice, rate, pitch):
            raise RuntimeError("primary down (test stub)")

        t.SYNTH = {"azure": _boom, "edge": lambda *a: CANNED}
        t.FALLBACK = "edge"
        try:
            t._cache.clear()
            t._cache_bytes = 0
            q = "text=" + urllib.request.quote("你好。") + "&stream=0"
            status, headers, _t, body = self.get(q)
            self.assertEqual(status, 200)
            self.assertEqual(body, CANNED)
            self.assertEqual(headers.get("X-TTS-Backend"), "edge")
        finally:
            t.SYNTH = real_synth
            t.FALLBACK = real_fb

    def test_truncated_stream_is_not_cached(self):
        # B2: a stream that dies mid-way answers truncated and must not
        # leave the prefix in the cache for the next identical request.
        real = t._azure_request
        t._azure_request = lambda text, voice, rate, pitch: _TruncatedFakeResponse(text)
        try:
            text = "截断测试文本，中途失败。"
            q = "text=" + urllib.request.quote(text)
            key = (t.BACKEND, t.DEFAULT_VOICE, t.DEFAULT_RATE, t.DEFAULT_PITCH, text)
            t._cache.clear()
            t._cache_bytes = 0
            status, headers, _tt, body = self.get(q)
            self.assertEqual(status, 200)
            self.assertLess(len(body), len(CANNED), "response is the truncated prefix")
            self.assertNotIn(key, t._cache)
            # The next identical request streams again (a miss): it does not
            # replay the truncated prefix from the cache.
            status, headers, _tt, body2 = self.get(q)
            self.assertEqual(status, 200)
            self.assertEqual(headers.get("X-TTS-Cache"), "miss")
            self.assertEqual(body2, body)
            self.assertNotIn(key, t._cache)
        finally:
            t._azure_request = real

    def test_truncated_stream_is_not_cached_unpaced(self):
        # B2 again on the gen_raw() path (TTS_PACING=0): same rule, the
        # truncated head is served once and never cached.
        real = t._azure_request
        t._azure_request = lambda text, voice, rate, pitch: _TruncatedFakeResponse(text)
        t.PACING_DEFAULT = False
        try:
            text = "截断测试文本，无 pacing。"
            q = "text=" + urllib.request.quote(text)
            key = (t.BACKEND, t.DEFAULT_VOICE, t.DEFAULT_RATE, t.DEFAULT_PITCH, text)
            t._cache.clear()
            t._cache_bytes = 0
            status, headers, _tt, body = self.get(q)
            self.assertEqual(status, 200)
            self.assertEqual(len(body), 600, "unpaced: exactly the truncated head")
            self.assertLess(len(body), len(CANNED))
            self.assertNotIn(key, t._cache)
            status, headers, _tt, body2 = self.get(q)
            self.assertEqual(headers.get("X-TTS-Cache"), "miss")
            self.assertEqual(body2, body)
            self.assertNotIn(key, t._cache)
        finally:
            t._azure_request = real
            t.PACING_DEFAULT = True


class WordTest(_ServerCase):
    """F12/F16: the ?word=1 path -- speakable gate, synth-on-miss, disk hit."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls._real_dir = t.WORD_AUDIO_DIR
        cls._tmp = tempfile.mkdtemp(prefix="tts-word-test-")
        t.WORD_AUDIO_DIR = cls._tmp

    @classmethod
    def tearDownClass(cls):
        t.WORD_AUDIO_DIR = cls._real_dir
        shutil.rmtree(cls._tmp, ignore_errors=True)
        super().tearDownClass()

    def test_word_speakable_gate_returns_400(self):
        # An emoji "word" must 400 here, not burn one synthesis and a file:
        # the directory gains nothing from these two requests.
        before = os.listdir(t.WORD_AUDIO_DIR)
        status, _h, _t, _b = self.get("word=1&text=" + urllib.request.quote("😀"))
        self.assertEqual(status, 400)
        self.assertEqual(os.listdir(t.WORD_AUDIO_DIR), before)
        status, _h, _t, _b = self.get("word=1&text=" + urllib.request.quote("%*"))
        self.assertEqual(status, 400)
        self.assertEqual(os.listdir(t.WORD_AUDIO_DIR), before)

    def test_word_empty_is_400(self):
        status, _h, _t, _b = self.get("word=1&text=")
        self.assertEqual(status, 400)

    def test_word_miss_synth_then_disk_hit(self):
        q = "word=1&text=apple"
        status, headers, _t, body = self.get(q)
        self.assertEqual(status, 200)
        self.assertEqual(body, CANNED)
        self.assertEqual(headers.get("X-TTS-Word"), "1")
        self.assertEqual(headers.get("X-TTS-Cache"), "miss")
        self.assertEqual(len(os.listdir(t.WORD_AUDIO_DIR)), 1)
        status, headers, _t, body2 = self.get(q)
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("X-TTS-Cache"), "hit")
        self.assertEqual(body2, CANNED)


class SpeakableTest(unittest.TestCase):
    """F12: speakable() -- the three review probes plus the edges around them."""

    def test_kaomoji_with_a_letter_inside_is_stripped(self):
        self.assertEqual(t.speakable("(o･ω･o)"), "")
        self.assertEqual(t.speakable("(╯°□°)╯"), "")
        self.assertEqual(t.speakable("(｡･ω･｡)"), "")

    def test_zero_width_chars_are_stripped(self):
        self.assertEqual(t.speakable("你好\u200b"), "你好")
        self.assertEqual(t.speakable("\ufeff你好"), "你好")

    def test_cyrillic_greek_and_accented_latin_are_words(self):
        self.assertEqual(t.speakable("Привет"), "Привет")
        self.assertEqual(t.speakable("Γεια σου"), "Γεια σου")
        self.assertEqual(t.speakable("café"), "café")

    def test_symbol_expansion_is_capped_at_max_text_len(self):
        self.assertEqual(len(t.speakable("℃" * 180)), t.MAX_TEXT_LEN)

    def test_real_parentheticals_survive(self):
        self.assertEqual(t.speakable("(hello)"), "(hello)")
        self.assertEqual(t.speakable("(你好)"), "(你好)")
        self.assertEqual(t.speakable("(a)"), "(a)")
        self.assertEqual(t.speakable("(ab)"), "(ab)")

    def test_unspeakable_still_empty(self):
        self.assertEqual(t.speakable("%*"), "")
        self.assertEqual(t.speakable("……"), "")
        self.assertEqual(t.speakable("😀"), "")


class CacheUnitTest(unittest.TestCase):
    """F11: cache_put overwriting a key must not double-count its bytes."""

    def setUp(self):
        t._cache.clear()
        t._cache_bytes = 0

    def tearDown(self):
        t._cache.clear()
        t._cache_bytes = 0

    def _key(self, text):
        return (t.BACKEND, t.DEFAULT_VOICE, t.DEFAULT_RATE, t.DEFAULT_PITCH, text)

    def test_overwrite_replaces_bytes_instead_of_adding(self):
        key = self._key("hello world")
        t.cache_put(key, b"x" * 1000)
        t.cache_put(key, b"y" * 1500)
        self.assertEqual(len(t._cache), 1)
        self.assertEqual(t._cache_bytes, 1500)
        self.assertEqual(t._cache[key], b"y" * 1500)

    def test_overwrite_moves_key_to_most_recent(self):
        k1 = self._key("first text here")
        k2 = self._key("second text here")
        t.cache_put(k1, b"a" * 10)
        t.cache_put(k2, b"b" * 10)
        t.cache_put(k1, b"c" * 10)
        self.assertEqual(list(t._cache.keys())[-1], k1)
        self.assertEqual(t._cache_bytes, 20)

    def test_cache_delete_drops_key_and_bytes(self):
        key = self._key("hello world")
        t.cache_put(key, b"x" * 100)
        t.cache_delete(key)
        self.assertNotIn(key, t._cache)
        self.assertEqual(t._cache_bytes, 0)
        t.cache_delete(key)   # deleting a missing key is a no-op
        self.assertEqual(t._cache_bytes, 0)


class SplitterTest(unittest.TestCase):
    """C3: the MP3 frame splitter (no server, no network)."""

    def frames_of(self, data, step=None):
        sp = t._Mp3Frames()
        if step is None:
            sp.feed(data)
            return sp.take(final=True)
        for i in range(0, len(data), step):
            sp.feed(data[i : i + step])
        return sp.take(final=True)

    def test_canned_file_splits_into_tag_plus_frames(self):
        frames = self.frames_of(CANNED)
        tag, rest = frames[0], frames[1:]
        self.assertEqual(tag[1], 0.0)  # the ID3 tag is not audio, never paced
        self.assertEqual(len(tag[0]), 45)
        self.assertEqual(len(rest), 153)
        self.assertTrue(all(d == 0.024 for _b, d in rest))  # 576 @ 24 kHz
        self.assertEqual(b"".join(b for b, _d in frames), CANNED)  # no byte lost

    def test_small_feeds_give_the_same_frames(self):
        whole = self.frames_of(CANNED)
        self.assertEqual(self.frames_of(CANNED, step=100), whole)
        self.assertEqual(self.frames_of(CANNED, step=7), whole)

    def test_junk_between_frames_is_dropped_and_sync_recovers(self):
        # A frame boundary after the tag + two frames, found, not assumed.
        sp = t._Mp3Frames()
        sp.feed(CANNED)
        frames = sp.take(final=True)
        cut = len(frames[0][0]) + len(frames[1][0]) + len(frames[2][0])
        junked = CANNED[:cut] + b"\x00\x00\x00" + CANNED[cut:]
        self.assertEqual(self.frames_of(junked), self.frames_of(CANNED))

    def test_trailing_partial_frame_flushes_only_at_the_end(self):
        part = CANNED[:-10]  # the last frame arrives short
        sp = t._Mp3Frames()
        sp.feed(part)
        frames = sp.take(final=False)
        self.assertLess(sum(len(b) for b, _d in frames), len(part))  # held back
        rest = sp.take(final=True)
        self.assertEqual(len(rest), 1)  # the short tail, flushed once
        self.assertEqual(b"".join(b for b, _d in frames) + rest[0][0], part)

    def test_reserved_sample_rate_index_returns_none(self):
        # F10: MPEG-1 Layer III, bitrate index 9, sri==3 (reserved) -- the
        # old code indexed _MP3_SR with it and raised IndexError mid-stream.
        self.assertIsNone(t._Mp3Frames._frame_at(bytes([0xFF, 0xFA, 0x9C, 0x00])))
        # Free-format (bri 0) and bad bitrate (bri 15) already returned None.
        self.assertIsNone(t._Mp3Frames._frame_at(bytes([0xFF, 0xFA, 0x00, 0x00])))
        self.assertIsNone(t._Mp3Frames._frame_at(bytes([0xFF, 0xFA, 0xF0, 0x00])))

    def test_sri3_frame_is_skipped_and_stream_recovers(self):
        # F10: a reserved-sri frame at the head of the stream is dropped as
        # lost sync and the real frames behind it still come out whole.
        bad = bytes([0xFF, 0xFA, 0x9C, 0x00])
        frames = self.frames_of(bad + CANNED)
        self.assertEqual(frames, self.frames_of(CANNED)[1:])

    def test_id3_tag_split_across_feeds(self):
        # The tag arrives in pieces: nothing is emitted until all 45 bytes
        # are in, then the tag comes out exactly once, before the frames.
        sp = t._Mp3Frames()
        sp.feed(CANNED[:5])
        self.assertEqual(sp.take(), [])
        sp.feed(CANNED[5:10])
        self.assertEqual(sp.take(), [])
        sp.feed(CANNED[10:45])
        tag = sp.take()
        self.assertEqual(tag, [(CANNED[:45], 0.0)])
        sp.feed(CANNED[45:])
        self.assertEqual(sp.take(final=True), self.frames_of(CANNED)[1:])


class PacerTest(unittest.TestCase):
    """C3: _FramePacer re-anchoring (no server, no network)."""

    def test_stall_reanchors_instead_of_bursting(self):
        pacer = t._FramePacer()
        pacer.wait(0.024)
        pacer.wait(0.024)
        pacer._next_t = time.monotonic() - 1.0   # upstream stalled, backlog due
        t0 = time.monotonic()
        pacer.wait(0.024)
        self.assertLess(time.monotonic() - t0, 0.2, "re-anchor must not replay the stall")
        self.assertGreater(pacer._next_t, t0, "clock re-anchored to now, not left behind")

    def test_steady_frames_are_metered(self):
        pacer = t._FramePacer()
        pacer.wait(0.1)
        pacer.wait(0.1)
        t0 = time.monotonic()
        pacer.wait(0.1)
        self.assertGreaterEqual(time.monotonic() - t0, 0.07, "third frame waits its turn")


class PacingTest(_ServerCase):
    """C3: the burst arrives at once, the board receives it in real time."""

    def test_burst_is_metered_out_in_real_time(self):
        text = "今天天气不错，适合出门走走。"
        q = "text=" + urllib.request.quote(text)
        t0 = time.monotonic()
        with urllib.request.urlopen(
            "http://127.0.0.1:%d/tts?%s" % (self.port, q), timeout=30
        ) as r:
            first = r.read(1)
            ttfb = time.monotonic() - t0
            gaps = []
            prev = time.monotonic()
            got = first
            while True:
                chunk = r.read(288)  # two frames a read
                now = time.monotonic()
                if not chunk:
                    break
                gaps.append(now - prev)
                prev = now
                got += chunk
            total = time.monotonic() - t0
        self.assertEqual(got, CANNED)  # paced, but no byte lost or duplicated
        # The fake's synthesis delay for this text is 0.44 s; pacing adds
        # nothing to the first byte (the first two frames go out at once).
        self.assertLess(ttfb, 0.7, "first frames go out at once")
        # 153 frames at 24 ms is ~3.7 s of audio: a burst would take < 0.5 s.
        self.assertGreater(total, 3.0, "paced total %s" % total)
        self.assertLess(total, 8.0, "paced total %s" % total)
        # After the first two frames, reads arrive about two frames apart.
        gaps = sorted(gaps[2:])
        self.assertGreater(gaps[len(gaps) // 2], 0.030, "median gap %s" % gaps)

    def test_pacing_off_is_a_burst_again(self):
        t.PACING_DEFAULT = False
        try:
            text = "今天天气不错，适合出门走走。"
            q = "text=" + urllib.request.quote(text)
            t._cache.clear()
            t._cache_bytes = 0
            t0 = time.monotonic()
            _s, _h, _ttfb, body = self.get(q)
            total = time.monotonic() - t0
            self.assertEqual(body, CANNED)
            # The fake's synthesis delay for this text is 0.44 s; with pacing
            # off the whole file follows at once, so the total stays well
            # under a second -- the mirror of the > 3.0 s paced assertion.
            self.assertLess(total, 1.0, "unpacing total %s" % total)
        finally:
            t.PACING_DEFAULT = True


if __name__ == "__main__":
    unittest.main()
