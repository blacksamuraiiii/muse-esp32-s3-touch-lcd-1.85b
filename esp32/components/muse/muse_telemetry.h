/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The night recorder (docs/20261023-测试版遥测设计.md §3.2).
 *
 * What it is for: the power work needs to know what the device was *doing*
 * during the hours it drained 16% overnight, not just how much it drained.
 * ptest answers "what should each state cost" on a bench with a meter; this
 * answers "which state was it actually in, and when" on the device, in the
 * user's pocket, with no cable attached.
 *
 * One CSV file in the FAT partition of the Test partition table:
 *
 *   P,<utc>,<pct>,<avg_mA>,<volt_mV>,<temp_C>,<ps>,<asleep>,<rest>,<alarm_armed>,<lightsleep_pct>,<wakes_delta>
 *   E,<utc>,<what>,<detail>
 *
 * P lines every 60 s. E lines when something changes (boot, an alarm arming,
 * firing or stopping, Wi-Fi power save switching, shutdown), written as it
 * happens rather than inferred from the next sample.
 *
 * rest and lightsleep_pct are cumulative since the battery measurement started
 * (muse_battery.h), not per-interval: the per-mille accumulator behind them
 * resolves to 60 ms over a one-minute window, and the question being answered
 * ("was it ever resting at all?") is about the night, not the minute.
 *
 * Compiled only under CONFIG_MUSE_TELEMETRY (the Test build type). In a Release
 * build the file is not built, the partition does not exist, and every call
 * site here compiles away.
 */

/* Mounts the FAT partition and starts the sampler. Safe to call once, after
 * NVS and the board are up; a board whose partition is missing or will not
 * mount leaves the recorder off and says so in the log. */
void muse_telemetry_start(void);

/* Whether the recorder is running (mounted and sampling). */
bool muse_telemetry_active(void);

/* One line for something that happened now. Cheap enough to call from the
 * input task; a no-op when the recorder is off. */
void muse_telemetry_event(const char *what, const char *detail);

/* Console: >telemetry.dump prints the file, >telemetry.stats summarises it,
 * >telemetry.clear deletes it. They print their own "@..." result lines. */
void muse_telemetry_dump(void);
void muse_telemetry_stats(void);
void muse_telemetry_clear(void);

#ifdef __cplusplus
}
#endif
