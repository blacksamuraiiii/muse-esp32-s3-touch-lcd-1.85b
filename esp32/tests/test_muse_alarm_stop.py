# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Host checks for the alarm ring's stop path (1.5.4b review A-11).

Compiles the real `alarm_start()`, `muse_input_alarm_stop()` and the
ring-stop branch of `input_task()` out of `muse_input.c` against a fake
clock and a fake board, and checks:

- the Stop button waits out the same 6 s panel-settling grace as the
  keys (refused inside the grace, stops at exactly 6 s);
- only a NEW key press stops a ring: a press that was already down when
  the ring started, or one that started inside the grace and stays held
  past it, never stops it -- it must be released and pressed again;
- a fresh talk edge after the grace stops the ring, and so does a
  non-talk key (bezel path) even inside the grace, as written.

`alarm_stop()` itself is stubbed (it counts calls and clears the ring):
the point here is the stop *decision*, not the re-arm that follows it.
The A-10 ordering (ring timestamp written before the ringing flag) is
pinned by a source assertion, since the race it closes is not
observable from the host.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INPUT = ROOT / "components" / "muse" / "muse_input.c"

GRACE_US = 6_000_000


def function(source, ret, name):
    """The `ret name(...) { ... }` definition, braces matched."""
    match = re.search(r"(?:static )?" + ret + r" " + name + r"\([^)]*\)\s*\{", source)
    start, pos, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


def ring_branch(source):
    """The `if (s_alarm_ringing) { ... }` stop branch of input_task()."""
    anchor = source.index("/* Waking the display makes the CST816S report one phantom press")
    start = source.rindex("if (s_alarm_ringing) {", 0, anchor)
    pos, depth = source.index("{", start) + 1, 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


class AlarmStopTest(unittest.TestCase):
    def test_alarm_stop_paths(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C compiler unavailable")
        source = INPUT.read_text()

        # A-10: the ring timestamp is written before the ringing flag, so
        # the LVGL thread can never read a fresh flag with a stale time.
        start = function(source, "void", "alarm_start")
        self.assertLess(start.index("s_alarm_ring_us = "),
                        start.index("s_alarm_ringing = true;"))
        branch = ring_branch(source)
        self.assertIn("const bool held_edge = held_now && !s_talk_down_seen;", branch)
        self.assertIn("(held_edge && !grace)", branch)

        code = r"""
#define _POSIX_C_SOURCE 200809L   /* localtime_r, under -std=c11 */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- fakes: a controllable clock, board and RTC ---- */
static int64_t fake_now_us;
static int64_t esp_timer_get_time(void) { return fake_now_us; }
static void esp_logi(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI esp_logi
#define TAG "muse_input"

static bool fake_talk_down;
static bool fake_talk_down_fn(void) { return fake_talk_down; }
typedef struct { bool (*talk_down)(void); } muse_board_t;
static muse_board_t fake_board = { fake_talk_down_fn };
#define muse_board (&fake_board)

static bool muse_rtc_now(time_t *out) { (void)out; return false; }
static void set_asleep(bool asleep, const char *why) { (void)asleep; (void)why; }
static void muse_state_poke(void) {}
static void muse_state_set_caption(const char *fmt, ...) { (void)fmt; }
static int muse_settings_alarm_hour(void) { return 7; }
static int muse_settings_alarm_min(void) { return 0; }

/* ---- the alarm state the extracted code shares ---- */
#define GRACE_US 6000000
#define ALARM_TOUCH_GRACE_US (6LL * 1000 * 1000)
#define ALARM_RING_US (30LL * 1000 * 1000)
#define MUSE_BTN_TALK_PRESS   (1u << 0)
#define MUSE_BTN_TALK_RELEASE (1u << 1)
#define MUSE_BTN_AUX_PRESS    (1u << 2)
#define MUSE_BTN_AUX_RELEASE  (1u << 3)
#define MUSE_BTN_UP           (1u << 4)
#define MUSE_BTN_DOWN         (1u << 5)
#define MUSE_BTN_LEFT         (1u << 6)
#define MUSE_BTN_RIGHT        (1u << 7)
#define MUSE_BTN_ENTER        (1u << 8)
#define MUSE_BTN_ESCAPE       (1u << 9)
#define MUSE_BTN_POWER_PRESS  (1u << 10)
#define MUSE_BTN_POWER_RELEASE (1u << 11)
#define MUSE_BTN_POWER_LONG   (1u << 12)
#define MUSE_BTN_SPEAKER_PRESS (1u << 13)
#define MUSE_BTN_SPEAKER_RELEASE (1u << 14)
#define ALARM_STOP_BITS (MUSE_BTN_TALK_PRESS | MUSE_BTN_AUX_PRESS | MUSE_BTN_UP | MUSE_BTN_DOWN | \
                         MUSE_BTN_LEFT | MUSE_BTN_RIGHT | MUSE_BTN_ENTER | MUSE_BTN_ESCAPE | \
                         MUSE_BTN_POWER_PRESS | MUSE_BTN_SPEAKER_PRESS)

static bool s_alarm_ringing;
static int64_t s_alarm_ring_us;
static int64_t s_alarm_sfx_us;
static int64_t s_alarm_quiet_us;
static int s_alarm_last_rung_year = -1;
static int s_alarm_last_rung_yday = -1;
static bool s_talk_down_seen;
static int alarm_stop_calls;

/* alarm_stop() stubbed: the decision is what this pins, not the re-arm. */
static void alarm_stop(const char *why) { (void)why; alarm_stop_calls++; s_alarm_ringing = false; }
static void reset_alarm(void) {
    s_alarm_ringing = false;
    s_alarm_ring_us = 0;
    s_alarm_sfx_us = 0;
    s_alarm_quiet_us = 0;
    s_talk_down_seen = false;
    alarm_stop_calls = 0;
}
"""
        code += function(source, "void", "alarm_start") + "\n"
        code += function(source, "bool", "muse_input_alarm_stop") + "\n"
        code += function(source, "bool", "muse_input_alarm_ringing") + "\n"
        code += """
static void ring_key_poll(unsigned ev, unsigned touch, int64_t now_us)
{
""" + branch + """
}
"""
        code += r"""
int main(void) {
    /* The Stop button: refused inside the grace, stops at exactly 6 s. */
    reset_alarm();
    fake_now_us = 1000000; fake_talk_down = false;
    alarm_start("test");
    assert(muse_input_alarm_ringing() && s_alarm_ring_us == 1000000 && alarm_stop_calls == 0);
    fake_now_us = 1000000 + 5999999;
    assert(!muse_input_alarm_stop() && muse_input_alarm_ringing() && alarm_stop_calls == 0);
    fake_now_us = 1000000 + 6000000;
    assert(muse_input_alarm_stop() && !muse_input_alarm_ringing() && alarm_stop_calls == 1);
    assert(!muse_input_alarm_stop() && alarm_stop_calls == 1);

    /* A press already down when the ring starts (the wake-up phantom):
     * held past the grace, it never stops the ring. */
    reset_alarm();
    fake_now_us = 10000000; fake_talk_down = true;
    alarm_start("test");
    assert(s_talk_down_seen);
    ring_key_poll(0, 0, 17000000);
    assert(muse_input_alarm_ringing() && alarm_stop_calls == 0);

    /* A press that starts inside the grace and stays held past it: still
     * no stop. Only a release followed by a fresh press stops the ring. */
    reset_alarm();
    fake_now_us = 20000000; fake_talk_down = false;
    alarm_start("test");
    fake_talk_down = true;
    ring_key_poll(0, 0, 22000000);
    assert(muse_input_alarm_ringing() && alarm_stop_calls == 0);
    ring_key_poll(0, 0, 27000000);
    assert(muse_input_alarm_ringing() && alarm_stop_calls == 0);
    fake_talk_down = false;
    ring_key_poll(0, 0, 27500000);
    assert(muse_input_alarm_ringing() && alarm_stop_calls == 0);
    fake_talk_down = true;
    ring_key_poll(0, 0, 28000000);
    assert(!muse_input_alarm_ringing() && alarm_stop_calls == 1);

    /* A fresh talk edge after the grace stops the ring; the same edge
     * inside the grace does not. */
    reset_alarm();
    fake_now_us = 30000000; fake_talk_down = false;
    alarm_start("test");
    ring_key_poll(MUSE_BTN_TALK_PRESS, 0, 37000000);
    assert(!muse_input_alarm_ringing() && alarm_stop_calls == 1);
    reset_alarm();
    fake_now_us = 40000000; fake_talk_down = false;
    alarm_start("test");
    ring_key_poll(MUSE_BTN_TALK_PRESS, 0, 42000000);
    assert(muse_input_alarm_ringing() && alarm_stop_calls == 0);

    /* A non-talk key counts at once, even inside the grace (bezel path). */
    reset_alarm();
    fake_now_us = 50000000; fake_talk_down = false;
    alarm_start("test");
    ring_key_poll(MUSE_BTN_AUX_PRESS, 0, 51000000);
    assert(!muse_input_alarm_ringing() && alarm_stop_calls == 1);

    printf("alarm stop paths OK (grace=%d us)\n", GRACE_US);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            (path / "check.c").write_text(code)
            subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path / "check.c"), "-o", str(path / "check")],
                           check=True)
            subprocess.run([str(path / "check")], check=True)


if __name__ == "__main__":
    unittest.main()
