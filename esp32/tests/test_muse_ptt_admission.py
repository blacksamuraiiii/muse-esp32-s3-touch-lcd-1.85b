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

"""The press-qualification rules from upstream d0a65ab, as ported: a voice
note needs a 600 ms hold and 500 ms of audio captured while held, the
pre-roll is kept, and a waking press shows nothing until it qualifies.

Byte-identical production unit (muse_voice.c) against deterministic,
hardware-free fakes (tests/ptt_admission_fakes/). Scenarios 58/59
(shutdown, call epoch) have no equivalent in this fork and are not run.
The audio levels are the fork's own: quiet pre-roll is the room, loud
notes are speech, so the fork's energy gate passes them."""

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
FAKES = ROOT / 'tests' / 'ptt_admission_fakes'
SANITIZE = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
ENV = {**os.environ,
       'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
       'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}

# (name, spiram, hatch, passport): our board, a no-PSRAM board, and the
# AI Passport's 120 ms pre-roll.
CONFIGS = {
    'psram': (1, 1, 0),
    'lowmem': (0, 0, 0),
    'passport': (0, 0, 1),
}

ADMISSION = list(range(50, 58))
ORDER = (60, 61, 62, 63)
WAKE = (70, 71, 72, 73, 74, 78, 79, 82, 83, 84)


class AdmissionCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binaries = {}
        for name, (spiram, hatch, passport) in CONFIGS.items():
            stage = Path(cls.tmp.name) / name
            stage.mkdir()
            for source in MUSE.glob('*.h'):
                shutil.copyfile(source, stage / source.name)
            shutil.copyfile(MUSE / 'muse_voice.c', stage / 'muse_voice.c')
            shutil.copytree(FAKES, stage, dirs_exist_ok=True)
            (stage / 'sdkconfig.h').write_text(
                '#pragma once\n'
                f'#define CONFIG_SPIRAM {spiram}\n'
                f'#define CONFIG_MUSE_HATCH {hatch}\n'
                '#define CONFIG_MUSE_BUILD_RELEASE 1\n'
                '#define CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV 0\n'
                f'#define CONFIG_MUSE_BOARD_AI_PASSPORT {passport}\n'
                '#define CONFIG_MUSE_BOARD_SENSECAP_WATCHER 0\n'
                '#define CONFIG_MUSE_BARGE_IN_SETTLE_MS 200\n'
                '#define CONFIG_MUSE_VOICE_MIN_PEAK_DBFS -40\n'
                '#define CONFIG_FREERTOS_UNICORE 0\n')
            out = stage / 'admission'
            command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                       '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-unused-function',
                       '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                       *SANITIZE, '-no-pie',
                       '-include', str(ROOT / 'tests' / 'host_compat.h'),
                       '-I', str(stage),
                       str(ROOT / 'tests' / 'muse_ptt_admission_harness.c'),
                       str(MUSE / 'muse_adpcm.c')]
            result = subprocess.run([*command, '-lm', '-o', str(out)],
                                    capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[name] = out

    def run_case(self, scenario, config):
        result = subprocess.run([str(self.binaries[config]), str(scenario)],
                                capture_output=True, text=True, timeout=10, env=ENV)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_notes_require_real_hold_and_new_capture_before_any_upload(self):
        for config in ('psram', 'lowmem'):
            for scenario in ADMISSION:
                with self.subTest(config=config, scenario=scenario):
                    self.run_case(scenario, config)

    def test_note_preserves_buffered_order_and_streams_fresh_pcm_exactly(self):
        # PSRAM PCM, low-memory 320 ms pre-roll, low-memory 120 ms pre-roll.
        for config in ('psram', 'lowmem', 'passport'):
            for scenario in ORDER:
                with self.subTest(config=config, scenario=scenario):
                    self.run_case(scenario, config)

    def test_waking_press_shows_nothing_until_it_qualifies(self):
        for config in ('psram', 'lowmem'):
            for scenario in WAKE:
                with self.subTest(config=config, scenario=scenario):
                    self.run_case(scenario, config)


if __name__ == '__main__':
    unittest.main()
