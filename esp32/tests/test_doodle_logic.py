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
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class DoodleLogicTest(unittest.TestCase):
    def test_logic_rules(self) -> None:
        # muse_doodle.c has no LVGL or ESP-IDF dependency, so it
        # builds with plain cc on the host (same pattern as
        # test_goldrain_logic.py).
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "doodle_logic"
            built = subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "components" / "muse"),
                 str(ROOT / "tests" / "doodle_logic_harness.c"),
                 str(ROOT / "components" / "muse" / "muse_doodle.c"),
                 "-o", str(binary), "-lm"],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            # ASan's shadow setup is flaky under WSL: roughly one run in
            # three aborts at startup with a repeating
            # "AddressSanitizer:DEADLYSIGNAL" and never exits, while the
            # very same binary passes in ~12 ms on the next try (measured
            # 2026-11-07). That is the environment, not the logic, so one
            # retry is allowed on exactly that signature; a real logic
            # failure still fails the test.
            ran = None
            try:
                ran = subprocess.run([str(binary)], capture_output=True,
                                     text=True, timeout=20)
            except subprocess.TimeoutExpired:
                pass  # the DEADLYSIGNAL loop never exits; retry below
            if ran is None or (ran.returncode != 0
                               and "DEADLYSIGNAL" in ran.stderr):
                ran = subprocess.run([str(binary)], capture_output=True,
                                     text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("ok", ran.stdout)


if __name__ == "__main__":
    unittest.main()
