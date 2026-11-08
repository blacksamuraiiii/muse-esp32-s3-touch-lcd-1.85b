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

"""The game sound effects (muse_audio_sfx_render in muse_audio.h)."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class SfxTest(unittest.TestCase):
    def test_sfx(self) -> None:
        # muse_audio_sfx_render is a static inline in muse_audio.h, which
        # needs nothing but <math.h> and a stub esp_err.h (tests/sfx_fakes),
        # so it builds with plain cc on the host (same pattern as
        # test_speaking_face.py).
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "sfx"
            built = subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "components" / "muse"),
                 "-I", str(ROOT / "tests" / "sfx_fakes"),
                 str(ROOT / "tests" / "sfx_harness.c"),
                 "-lm", "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("ok", ran.stdout)


if __name__ == "__main__":
    unittest.main()
