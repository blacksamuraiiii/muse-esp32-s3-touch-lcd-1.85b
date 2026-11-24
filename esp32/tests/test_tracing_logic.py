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


class TracingLogicTest(unittest.TestCase):
    def test_logic_rules(self) -> None:
        # muse_tracing.c has no LVGL or ESP-IDF dependency, so it
        # builds with plain cc on the host (same pattern as
        # test_mathpractice_logic.py).
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "tracing_logic"
            built = subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "components" / "muse"),
                 str(ROOT / "tests" / "tracing_logic_harness.c"),
                 str(ROOT / "components" / "muse" / "muse_tracing.c"),
                 "-o", str(binary), "-lm"],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = None
            # ASan's shadow setup is flaky under WSL: roughly one run in
            # three aborts at startup with a repeating
            # "AddressSanitizer:DEADLYSIGNAL" and never exits, while the
            # same binary passes in ~6 ms (the thirty-character harness)
            # on the next try. That is the environment, not the logic, so
            # retries are allowed on exactly those two signatures; a real
            # logic failure (non-zero exit without DEADLYSIGNAL) still
            # fails the test. The 5 s timeout is ~800x the real run time:
            # a process still alive after it is the hang, so killing it
            # quickly beats waiting out a 20 s timeout per attempt.
            for _ in range(3):
                try:
                    ran = subprocess.run([str(binary)], capture_output=True,
                                         text=True, timeout=5)
                except subprocess.TimeoutExpired:
                    ran = None
                    continue
                if ran.returncode == 0 or "DEADLYSIGNAL" not in ran.stderr:
                    break
                ran = None
            self.assertIsNotNone(ran, "tracing harness hung in all three attempts (WSL ASan flake)")
            assert ran is not None
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout.strip(), "ok")


if __name__ == "__main__":
    unittest.main()
