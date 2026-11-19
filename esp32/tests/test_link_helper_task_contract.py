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
#
# Ported from upstream facebookincubator/muse-gadget-sdk 7b302e3
# (esp32/tests/test_link_helper_task_contract.py), trimmed to the local
# R2 subset (review 1.5.4a S2/S3):
#
# - Local helper tasks are only three, all in main/app.c:
#   vm_auth_refresh_task ("vm_auth_rfsh"), scan_refresh_task
#   ("scan_rfsh") and hatch_vm_task ("muse_vm"). Upstream's
#   start_confirm_timeout workers (confirm_timeout_task,
#   confirmed_idle_timeout_task) and ble_server.c's provision_task are
#   NOT helper tasks here: they still use plain xTaskCreate /
#   plain vTaskDelete, paired, with no leak (review S3). The upstream
#   assertions that those go through helper_task_create() are deleted,
#   not weakened silently; test_plain_setup_tasks_stay_plain pins the
#   current plain/plain pairing so a half-migration (creation moved to
#   the helper, cancel left plain) fails loudly instead of leaking a
#   WithCaps stack.
# - vm_auth_refresh_task's early exit returns after its
#   helper_task_delete(NULL), so the no-bare-return check is scoped to
#   "no return before the first delete" rather than the upstream
#   blanket ban.
"""Setup tasks whose stacks may live in PSRAM pair creation with deletion.

A task created WithCaps keeps its stack and TCB after a plain vTaskDelete(),
so every exit and cancellation goes through helper_task_delete(). The stacks
go to PSRAM only where flash writes leave the cache on: provisioning writes
NVS, which asserts on a PSRAM stack otherwise.

"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HELPER_H = ROOT / "main" / "helper_task.h"
APP_C = ROOT / "main" / "app.c"
BLE_SERVER_C = ROOT / "main" / "ble_server.c"
KCONFIG = ROOT / "main" / "Kconfig.projbuild"

# The local R2 subset: function name -> task name passed to helper_task_create.
HELPER_TASKS = {
    "vm_auth_refresh_task": "vm_auth_rfsh",
    "scan_refresh_task": "scan_rfsh",
    "hatch_vm_task": "muse_vm",
}


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : i]
    raise AssertionError(f"could not find end of {signature}")


def _strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


class HelperTaskContractTest(unittest.TestCase):
    def test_psram_stacks_only_where_flash_writes_keep_the_cache(self) -> None:
        header = _strip_comments(HELPER_H.read_text())
        gate = re.search(r"#if (.*)\n#include \"esp_heap_caps.h\"", header)
        self.assertIsNotNone(gate, "PSRAM gate not found")
        condition = gate.group(1)
        self.assertIn("CONFIG_HOMEHUB_SETUP_TASKS_IN_PSRAM", condition)
        self.assertIn("CONFIG_SPIRAM_FETCH_INSTRUCTIONS", condition)
        self.assertIn("CONFIG_SPIRAM_RODATA", condition)
        self.assertNotIn("||", condition)

        # Opt-in per board, and only where flash writes keep the cache on.
        kconfig = KCONFIG.read_text()
        option = kconfig[kconfig.index("config HOMEHUB_SETUP_TASKS_IN_PSRAM") :]
        option = option[: option.index("\n    config ", 1)]
        self.assertIn("depends on SPIRAM && SPIRAM_FETCH_INSTRUCTIONS && SPIRAM_RODATA", option)
        self.assertIn("default n", option)

        create = _function_body(header, "static inline BaseType_t helper_task_create(")
        delete = _function_body(header, "static inline void helper_task_delete(")
        for body, caps, plain in (
            (create, "xTaskCreateWithCaps(", "xTaskCreate("),
            (delete, "vTaskDeleteWithCaps(", "vTaskDelete("),
        ):
            psram, _, internal = body.partition("#else")
            self.assertIn("#if HELPER_TASK_STACK_IN_PSRAM", psram)
            self.assertIn(caps, psram)
            self.assertIn(plain, internal)
            self.assertNotIn(caps, internal)

    def test_setup_tasks_do_not_choose_psram_stacks_themselves(self) -> None:
        for path in (APP_C, BLE_SERVER_C):
            source = _strip_comments(path.read_text())
            for name in ("scan_refresh_task", "provision_task",
                         "confirm_timeout_task", "confirmed_idle_timeout_task"):
                self.assertNotRegex(
                    source, rf"xTaskCreate(?:PinnedToCore)?WithCaps\(\s*{name}\b",
                    f"{path.name}: {name} bypasses helper_task_create()",
                )
            self.assertNotIn("xTaskCreateWithCaps(task,", source)

    def test_every_helper_task_exit_uses_helper_task_delete(self) -> None:
        app = _strip_comments(APP_C.read_text())
        started = {
            name
            for name in re.findall(r"helper_task_create\(\s*(\w+)\s*,", app)
            if name != "task"
        }
        self.assertEqual(started, set(HELPER_TASKS))
        for name in sorted(started):
            body = _function_body(app, f"static void {name}(void *arg) {{")
            self.assertIn("helper_task_delete(NULL);", body,
                          f"{name} must free its WithCaps stack")
            self.assertNotIn("vTaskDelete(", body, f"{name} leaks a WithCaps stack")
            # No exit before the first delete: a return after the delete is
            # dead code (the task is already gone), not a leak.
            head, _, _ = body.partition("helper_task_delete(NULL);")
            self.assertNotRegex(head, r"\breturn\s*;",
                                f"{name} exits without deleting itself")

    def test_plain_setup_tasks_stay_plain(self) -> None:
        # S3 scope, pinned: start_confirm_timeout() and provision_task still
        # use plain xTaskCreate / plain vTaskDelete. If either creation moves
        # to helper_task_create(), its cancel/delete must move to
        # helper_task_delete() in the same change, or the WithCaps stack leaks.
        app = _strip_comments(APP_C.read_text())
        confirm = _function_body(app, "static BaseType_t start_confirm_timeout(")
        self.assertIn("xTaskCreate(task,", confirm)
        self.assertNotIn("helper_task_create(task,", confirm)
        cancel = _function_body(app, "static uint32_t advance_confirm_generation_locked(")
        self.assertIn("vTaskDelete(s_confirm_timeout_task);", cancel)
        self.assertNotIn("helper_task_delete(s_confirm_timeout_task)", app)
        ble = _strip_comments(BLE_SERVER_C.read_text())
        self.assertIn('xTaskCreate(provision_task, "prov", 8192,', ble)
        self.assertNotIn("helper_task_create(provision_task,", ble)


if __name__ == "__main__":
    unittest.main()
