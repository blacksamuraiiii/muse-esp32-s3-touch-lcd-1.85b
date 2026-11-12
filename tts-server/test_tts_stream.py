"""Bridge streaming tests (C1 first-sentence cut; C3 pacing to follow).

Runs the Flask app on a local port with Azure stubbed out: the fake yields
canned MP3 bytes after a delay that grows with the text, the way Azure's time
to first audio grows with the sentence, so the first-byte timing of the cut
is measurable without a key.

Run from tts-server/:  python3 -m unittest test_tts_stream -v
"""

from __future__ import annotations

import os
import threading
import time
import unittest
import urllib.request
from pathlib import Path

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
        with urllib.request.urlopen(url, timeout=30) as r:
            first = r.read(1)
            ttfb = time.monotonic() - t0
            body = first + r.read()
            return r.status, dict(r.headers), ttfb, body

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
            _s, _h, _ttfb, body = self.get(q)
            self.assertEqual(body, CANNED)
        finally:
            t.PACING_DEFAULT = True


if __name__ == "__main__":
    unittest.main()
