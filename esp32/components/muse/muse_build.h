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

#include "sdkconfig.h"

/*
 * The build type, and the one place a release build is allowed to refuse to
 * build.
 *
 * MUSE_BUILD (Kconfig, "Muse > Build type") has two values and defaults to
 * Release, so forgetting the Test overlay yields a clean release image rather
 * than test code in one. The build type is the switch a person turns; the
 * facilities below are what the code tests.
 *
 * Kconfig can drop a value whose dependencies are unmet, which is fine when a
 * facility is merely not selected - but a hand-edited sdkconfig that turns a
 * facility on *inside* a Release build is a mixture nobody asked for and nobody
 * would notice: a release image that samples the battery every minute and
 * writes to a FAT partition that the release partition table does not have.
 * That case must fail loudly, here, at compile time. Release is only worth the
 * name if it is actually enforced.
 *
 * Included from muse_board.h, which every board file and the application
 * translate unit reach, so the checks run in a normal build without anyone
 * having to remember them.
 */

#if !defined(CONFIG_MUSE_BUILD_TEST) && !defined(CONFIG_MUSE_BUILD_RELEASE)
#error "No build type selected: pick Muse > Build type (Test or Release). A sdkconfig from before the build type existed needs regenerating."
#endif

#if defined(CONFIG_MUSE_BUILD_TEST) && defined(CONFIG_MUSE_BUILD_RELEASE)
#error "Both MUSE_BUILD_TEST and MUSE_BUILD_RELEASE are set: they are one Kconfig choice and cannot both be on."
#endif

#ifdef CONFIG_MUSE_BUILD_RELEASE
#  ifdef CONFIG_MUSE_POWER_TEST
#    error "Release build with CONFIG_MUSE_POWER_TEST: the ptest firmware replaces app_main and is test-only. Build it with the Test overlay, or turn it off."
#  endif
#  ifdef CONFIG_MUSE_TELEMETRY
#    error "Release build with CONFIG_MUSE_TELEMETRY: the night-telemetry sampler is test-only. It also needs the Test partition table's FAT partition, which a Release image does not have."
#  endif
#endif

/* 1 in a Test build, 0 in a Release build: for code that has to branch rather
 * than be compiled out (a menu entry, a string). Prefer #if CONFIG_<facility>
 * where a facility has its own switch - this is the build type itself, not a
 * facility. */
#ifdef CONFIG_MUSE_BUILD_TEST
#  define MUSE_BUILD_IS_TEST 1
#else
#  define MUSE_BUILD_IS_TEST 0
#endif
