# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Sentence units for streaming TTS (muse_sentence.c): a unit ends at ASCII
.!?;: or CJK 。！？, commas and spaces never split one, and a long run
without a boundary is cut at 180 bytes on a UTF-8 character boundary."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class SentenceTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_sentence_harness"
        proc = subprocess.run(
            [
                *cc,
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "components" / "muse"),
                str(ROOT / "tests" / "muse_sentence_harness.c"),
                str(ROOT / "components" / "muse" / "muse_sentence.c"),
                "-o",
                str(cls.binary),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def units(self, text: str) -> tuple[list[bytes], bytes]:
        proc = subprocess.run(
            [str(self.binary)],
            input=text.encode(),
            capture_output=True,
            check=True,
        )
        lines = proc.stdout.decode().splitlines()
        self.assertTrue(lines[-1].startswith("rest "))
        rest = int(lines[-1].split()[1])
        data = text.encode()
        out: list[bytes] = []
        off = 0
        for line in lines[:-1]:
            n = int(line)
            out.append(data[off : off + n])
            off += n
        self.assertEqual(data[off:], data[len(data) - rest :] if rest else b"")
        return out, data[off:]

    def test_ascii_sentences(self) -> None:
        units, rest = self.units("Hello world. How are you? Fine!")
        self.assertEqual(units, [b"Hello world.", b" How are you?", b" Fine!"])
        self.assertEqual(rest, b"")

    def test_cjk_sentences(self) -> None:
        units, rest = self.units("今天天气不错。我们去公园吧！")
        self.assertEqual(units, ["今天天气不错。".encode(), "我们去公园吧！".encode()])
        self.assertEqual(rest, b"")

    def test_comma_and_space_do_not_split(self) -> None:
        units, rest = self.units("Hello, world. Bye.")
        self.assertEqual(units, [b"Hello, world.", b" Bye."])
        self.assertEqual(rest, b"")
        units, rest = self.units("你好，今天天气不错，明天见。")
        self.assertEqual(units, ["你好，今天天气不错，明天见。".encode()])
        self.assertEqual(rest, b"")

    def test_semicolon_and_colon(self) -> None:
        units, rest = self.units("A; B: C.")
        self.assertEqual(units, [b"A;", b" B:", b" C."])
        self.assertEqual(rest, b"")

    def test_mixed_cjk_and_ascii(self) -> None:
        units, rest = self.units("第一句。第二句！Third one? Yes;")
        self.assertEqual(
            units,
            ["第一句。".encode(), "第二句！".encode(), b"Third one?", b" Yes;"],
        )
        self.assertEqual(rest, b"")

    def test_no_boundary_short_waits(self) -> None:
        units, rest = self.units("hello world")
        self.assertEqual(units, [])
        self.assertEqual(rest, b"hello world")

    def test_no_boundary_long_cuts_at_180(self) -> None:
        units, rest = self.units("a" * 200)
        self.assertEqual(units, [b"a" * 180])
        self.assertEqual(rest, b"a" * 20)

    def test_forced_cut_never_splits_utf8(self) -> None:
        text = "ab" + "中" * 70
        units, rest = self.units(text)
        self.assertEqual(len(units), 1)
        self.assertEqual(len(units[0]), 179)
        units[0].decode("utf-8")
        self.assertEqual(rest, text.encode()[179:])

    def test_empty_and_whitespace_only(self) -> None:
        units, rest = self.units("")
        self.assertEqual(units, [])
        self.assertEqual(rest, b"")
        units, rest = self.units("   ")
        self.assertEqual(units, [])
        self.assertEqual(rest, b"   ")


if __name__ == "__main__":
    unittest.main()
