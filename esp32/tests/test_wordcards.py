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

from __future__ import annotations

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# A8 masters, 128x128 each: byte sums measured at generation time
# (~/workspace/wordcards-art-gen.py). The generator also verifies each
# master against the v4 coloured render (IoU >= 0.999); this test pins
# the shipped bytes so a later regeneration or a bad merge is caught
# here instead of on the board.
EXPECTED_ART = {
    "apple": 2143255,
    "banana": 856669,
    "cherry": 1133453,
    "peach": 1793543,
    "cat": 1734876,
    "dog": 1487589,
    "bird": 783611,
    "fish": 1250943,
}


class WordCardsTest(unittest.TestCase):
    def test_wordcards_logic(self) -> None:
        # muse_wordcards.c is pure logic (no LVGL, no ESP-IDF), so it
        # builds with plain cc on the host (same pattern as
        # test_tictactoe.py).
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "wordcards"
            built = subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "components" / "muse"),
                 str(ROOT / "tests" / "wordcards_harness.c"),
                 str(ROOT / "components" / "muse" / "muse_wordcards.c"),
                 "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("ok", ran.stdout)

    def test_wordcards_art_bytes(self) -> None:
        # The shipped A8 masters must be the generated ones: 16 KB
        # each, a real picture (some fully opaque pixels, some
        # transparent), and pairwise different (no two words sharing
        # one master by accident).
        art_dir = ROOT / "components" / "muse"
        seen = {}
        for name, expected_sum in EXPECTED_ART.items():
            text = (art_dir / f"muse_wordcards_art_{name}.c").read_text()
            body = text.split(f"muse_wordcards_art_{name}_map[] = {{")[1]
            body = body.split("};")[0]
            values = [int(x, 16) for x in re.findall(r"0x[0-9a-f]{2}", body)]
            self.assertEqual(len(values), 128 * 128, name)
            self.assertEqual(sum(values), expected_sum, name)
            self.assertGreater(sum(1 for v in values if v == 255), 1000, name)
            self.assertGreater(sum(1 for v in values if v == 0), 1000, name)
            self.assertNotIn(sum(values), seen, name)
            seen[sum(values)] = name


if __name__ == "__main__":
    unittest.main()
