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

"""The real lcd185b board callbacks with host I2C and LVGL fakes: the update
windows round_area() aligns, and read_power() against the BQ27220 decision
table (upstream be49f02's rules, through our espressif/bq27220 getters)."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^static (?:void|esp_err_t) " + name + r"\([^)]*\)\n\{", source, re.M)
    assert match, name
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class Lcd185bBoard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        temp = Path(cls.tmp.name)
        cls.source = (ROOT / "components/muse/boards/board_waveshare_s3_touch_lcd_185b.c").read_text()
        (temp / "lcd185b_board_functions.h").write_text(
            function(cls.source, "round_area") + "\n" + function(cls.source, "read_power")
        )
        cls.binary = temp / "lcd185b_board"
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            raise unittest.SkipTest("C compiler not available")
        built = subprocess.run([
            *cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-I", str(temp), "-I", str(ROOT / "tests/lcd185b_board_fakes"),
            "-I", str(ROOT / "components/muse"),
            str(ROOT / "tests/lcd185b_board_harness.c"), "-o", str(cls.binary),
        ], capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stderr)

    def samples(self, *samples):
        defaults = dict(usb=1, probe=1, attached=1, status=8, operation=32,
                        soc=57, mv=3876, ma=0, fail=0)
        lines = []
        for sample in samples:
            values = defaults | sample
            lines.append(" ".join(str(values[key]) for key in defaults))
        proc = subprocess.run([str(self.binary), "power"], input="\n".join(lines) + "\n",
                              check=True, capture_output=True, text=True)
        return [tuple(map(int, line.split())) for line in proc.stdout.splitlines()]

    def test_update_windows_cover_pixels_without_crossing_panel_edges(self):
        self.assertIn("lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL)",
                      self.source)
        subprocess.run([str(self.binary), "alignment"], check=True, capture_output=True, text=True)

    def test_percent_and_charging_direction(self):
        # (err, pct, mv, charging, usb, ma, temp_c10, warnings, infos)
        rows = self.samples({}, dict(ma=123), dict(ma=-123, usb=0), dict(ma=0),
                            dict(ma=123, status=9))
        self.assertEqual([row[3] for row in rows], [0, 1, 0, 0, 0])
        self.assertEqual(rows[0], (0, 57, 3876, 0, 1, 0, 250, 0, 1))
        self.assertEqual(rows[1][5], 123)
        self.assertEqual(rows[2][4], 0)

    def test_zero_and_full_are_valid_percentages(self):
        rows = self.samples(dict(soc=0, mv=3000), dict(soc=100, mv=4200, status=0x208))
        self.assertEqual([row[1] for row in rows], [0, 100])
        self.assertEqual([row[3] for row in rows], [0, 0])

    def test_absence_does_not_publish_gauge_voltage(self):
        self.assertEqual(self.samples(dict(status=0)), [(0, -1, 0, 0, 1, 0, 0, 0, 1)])

    def test_uninitialized_or_config_mode_is_unknown(self):
        for operation in (0, 0x400, 0x420):
            with self.subTest(operation=operation):
                self.assertEqual(self.samples(dict(operation=operation)),
                                 [(0, -1, 0, 0, 1, 0, 0, 1, 0)])

    def test_every_failure_clears_previous_sample_and_recovers(self):
        # A failed read publishes unknown (never the stale 57%) and the next
        # good read recovers; the log fires once per outcome, not per poll.
        cases = (dict(fail=1), dict(fail=2), dict(probe=0), dict(attached=0))
        for fail in cases:
            with self.subTest(fail=fail):
                rows = self.samples({}, fail, {})
                self.assertEqual(rows[0], (0, 57, 3876, 0, 1, 0, 250, 0, 1))
                self.assertEqual(rows[1], (0, -1, 0, 0, 1, 0, 0, 1, 1))
                self.assertEqual(rows[2], (0, 57, 3876, 0, 1, 0, 250, 1, 2))

    def test_invalid_samples_are_unknown_instead_of_clamped(self):
        for values in (dict(soc=101), dict(soc=65535), dict(mv=0), dict(mv=1999), dict(mv=5001)):
            with self.subTest(values=values):
                self.assertEqual(self.samples(values)[0], (0, -1, 0, 0, 1, 0, 0, 1, 0))

    def test_missing_handle_still_updates_usb(self):
        self.assertEqual(self.samples(dict(attached=0, usb=0)),
                         [(0, -1, 0, 0, 0, 0, 0, 1, 0)])

    def test_probe_failure_still_updates_usb(self):
        rows = self.samples(dict(probe=0, usb=0), dict(probe=0, usb=1))
        self.assertEqual(rows[0], (0, -1, 0, 0, 0, 0, 0, 1, 0))
        self.assertEqual(rows[1], (0, -1, 0, 0, 1, 0, 0, 1, 0))

    def test_log_fires_once_per_outcome(self):
        rows = self.samples(dict(operation=0), dict(operation=0), {},
                            dict(), dict(probe=0), dict(probe=0))
        self.assertEqual([row[7:] for row in rows],
                         [(1, 0), (1, 0), (1, 1), (1, 1), (2, 1), (2, 1)])


if __name__ == "__main__":
    unittest.main()
