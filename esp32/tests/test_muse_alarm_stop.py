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

R-3 (2026-11-22) is pinned behaviourally too, against a controllable
clock: a console test ring (`alarm_start("console")`) stamps nothing,
so the day's real alarm still arms; a real ring stamps the date and
the (h, m), and moving the alarm later the same day re-arms today,
while an unchanged time keeps the F-A1 skip (a clock jump back over
a ring that already fired still cannot ring twice).
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
#define ESP_LOGW esp_logi
#define TAG "muse_input"

typedef int esp_err_t;
#define ESP_OK 0
static const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "OK";
}
#define ALARM_DAYS_DAILY 0x7F
static const char *const WEEKDAY_NAMES[7] =
    {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static bool fake_talk_down;
static bool fake_talk_down_fn(void) { return fake_talk_down; }
typedef struct { bool (*talk_down)(void); } muse_board_t;
static muse_board_t fake_board = { fake_talk_down_fn };
#define muse_board (&fake_board)

/* Controllable clock for the R-3 arm checks (the stop-path tests
 * above leave it off, so muse_rtc_now() stays false there). */
static bool fake_rtc_ok;
static time_t fake_rtc_now;
static bool muse_rtc_now(time_t *out)
{
    if (!fake_rtc_ok) {
        return false;
    }
    *out = fake_rtc_now;
    return true;
}
static bool muse_rtc_present(void) { return true; }
static int fake_alarm_h = 7;
static int fake_alarm_m = 0;
static int muse_rtc_arm_wakeup_in(uint32_t secs)
{
    (void)secs;
    return ESP_OK;
}
static int muse_rtc_arm_wakeup_weekly(uint32_t secs)
{
    (void)secs;
    return ESP_OK;
}
static void set_asleep(bool asleep, const char *why) { (void)asleep; (void)why; }
static void muse_state_poke(void) {}
static void muse_state_set_caption(const char *fmt, ...) { (void)fmt; }
static int muse_settings_alarm_hour(void) { return fake_alarm_h; }
static int muse_settings_alarm_min(void) { return fake_alarm_m; }
static int muse_settings_alarm_days(void) { return 0x7F; }

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
static time_t s_alarm_target;
static int s_alarm_last_rung_year = -1;
static int s_alarm_last_rung_yday = -1;
static int s_alarm_last_rung_h = -1;
static int s_alarm_last_rung_m = -1;
static bool s_talk_down_seen;
static int alarm_stop_calls;

/* alarm_stop() stubbed: the decision is what this pins, not the re-arm. */
static void alarm_stop(const char *why) { (void)why; alarm_stop_calls++; s_alarm_ringing = false; }
static void reset_alarm(void) {
    s_alarm_ringing = false;
    s_alarm_ring_us = 0;
    s_alarm_sfx_us = 0;
    s_alarm_quiet_us = 0;
    s_alarm_target = 0;
    s_alarm_last_rung_year = -1;
    s_alarm_last_rung_yday = -1;
    s_alarm_last_rung_h = -1;
    s_alarm_last_rung_m = -1;
    s_talk_down_seen = false;
    alarm_stop_calls = 0;
}

/* 2026-11-22 (a Sunday) at h:m local -- the R-3 scenarios all run on
 * one day, so mktime/localtime_r round-trip in whatever TZ the host
 * runs in. */
static time_t mk_time(int day, int h, int m) {
    struct tm tm = {0};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 10;
    tm.tm_mday = day;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_isdst = -1;
    return mktime(&tm);
}
"""
        code += function(source, "void", "alarm_start") + "\n"
        code += function(source, "void", "alarm_arm_next") + "\n"
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

    /* R-3 (2026-11-22): a console test ring stamps nothing, so the
     * day's real alarm still arms. */
    reset_alarm();
    fake_rtc_ok = true; fake_rtc_now = mk_time(22, 6, 30);
    fake_alarm_h = 7; fake_alarm_m = 0;
    alarm_start("console");
    assert(s_alarm_last_rung_yday == -1 && s_alarm_last_rung_h == -1 &&
           s_alarm_last_rung_m == -1);
    alarm_arm_next();
    assert(s_alarm_target == mk_time(22, 7, 0));

    /* A real 07:00 ring stamps date + time; moving the alarm to 09:00
     * at 08:00 re-arms today 09:00. */
    reset_alarm();
    fake_rtc_now = mk_time(22, 6, 30); fake_alarm_h = 7; fake_alarm_m = 0;
    alarm_start("clock");
    assert(s_alarm_last_rung_yday >= 0);
    assert(s_alarm_last_rung_h == 7 && s_alarm_last_rung_m == 0);
    fake_rtc_now = mk_time(22, 8, 0); fake_alarm_h = 9; fake_alarm_m = 0;
    alarm_arm_next();
    assert(s_alarm_target == mk_time(22, 9, 0));

    /* Same ring, no time change: 08:00 re-arm skips today and arms
     * tomorrow 07:00. */
    reset_alarm();
    fake_rtc_now = mk_time(22, 6, 30); fake_alarm_h = 7; fake_alarm_m = 0;
    alarm_start("clock");
    fake_rtc_now = mk_time(22, 8, 0);
    alarm_arm_next();
    assert(s_alarm_target == mk_time(23, 7, 0));

    /* And the F-A1 protection itself still holds with the time
     * unchanged: after the 07:00 ring, the clock jumping back to
     * 06:00 the same day must NOT re-arm today 07:00. */
    reset_alarm();
    fake_rtc_now = mk_time(22, 6, 30); fake_alarm_h = 7; fake_alarm_m = 0;
    alarm_start("clock");
    fake_rtc_now = mk_time(22, 6, 0);
    alarm_arm_next();
    assert(s_alarm_target == mk_time(23, 7, 0));

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
