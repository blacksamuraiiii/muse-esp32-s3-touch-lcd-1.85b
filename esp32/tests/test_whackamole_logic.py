# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
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


class WhackamoleLogicTest(unittest.TestCase):
    def test_logic_rules(self) -> None:
        # muse_whackamole.c has no LVGL or ESP-IDF dependency, so it
        # builds with plain cc on the host (same pattern as
        # test_sudoku_logic.py).
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "whackamole_logic"
            built = subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "components" / "muse"),
                 str(ROOT / "tests" / "whackamole_logic_harness.c"),
                 str(ROOT / "components" / "muse" / "muse_whackamole.c"),
                 "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("ok", ran.stdout)


if __name__ == "__main__":
    unittest.main()
