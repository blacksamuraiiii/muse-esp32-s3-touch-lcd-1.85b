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

# Shared setup for the standalone application and an enclosing internal project.
# Include before project(); source lists and dependencies stay in the components.
get_filename_component(GADGET_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Keep each build's generated configuration beside its build artifacts.
# Explicit -DSDKCONFIG paths, including the flash helper's, still take priority.
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig" CACHE FILEPATH "Project configuration")
endif()

if(NOT CMAKE_SOURCE_DIR STREQUAL GADGET_SOURCE_DIR)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${GADGET_SOURCE_DIR}/main"
        "${GADGET_SOURCE_DIR}/components"
    )
endif()

# ESP-Mosaico: Espressif's ESP-Iris and the app side of its retained Vibe Mode
# firmware, from an esp-mosaico-utils checkout (tools/muse/board.sh passes it).
if(MOSAICO_UTILS)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${MOSAICO_UTILS}/ESP-Iris/components/esp_iris"
        "${MOSAICO_UTILS}/esp-mosaico-recovery/components/esp_mosaico_app_recovery"
        "${GADGET_SOURCE_DIR}/components_mosaico/muse_mosaico_iris"
    )
endif()

# ---- build type -> version suffix -------------------------------------------
# A Test image must never be mistaken for a release one, and the version is
# baked into esp_app_desc during project(), so the suffix has to be decided
# before that call. The build type lives in the sdkconfig defaults chain
# (devices/sdkconfig.muse-test sets CONFIG_MUSE_BUILD_TEST=y), which CMake
# cannot read as CONFIG_* this early - so parse those files instead. The last
# file naming a build type wins, matching Kconfig's later-file precedence. The
# result is then checked against the generated sdkconfig in validate_config.cmake,
# so a build whose suffix and build type disagree fails instead of shipping quietly.
set(GADGET_BUILD_TYPE "")
foreach(GADGET_DEFAULTS_FILE IN LISTS SDKCONFIG_DEFAULTS)
    if(IS_ABSOLUTE "${GADGET_DEFAULTS_FILE}")
        set(GADGET_DEFAULTS_PATH "${GADGET_DEFAULTS_FILE}")
    else()
        set(GADGET_DEFAULTS_PATH "${GADGET_SOURCE_DIR}/${GADGET_DEFAULTS_FILE}")
    endif()
    if(EXISTS "${GADGET_DEFAULTS_PATH}")
        file(STRINGS "${GADGET_DEFAULTS_PATH}" GADGET_BUILD_LINES
             REGEX "^CONFIG_MUSE_BUILD_(TEST|RELEASE)=y")
        foreach(GADGET_BUILD_LINE IN LISTS GADGET_BUILD_LINES)
            if(GADGET_BUILD_LINE MATCHES "^CONFIG_MUSE_BUILD_TEST=y")
                set(GADGET_BUILD_TYPE "test")
            else()
                set(GADGET_BUILD_TYPE "release")
            endif()
        endforeach()
    endif()
endforeach()
if(GADGET_BUILD_TYPE STREQUAL "test" AND NOT DEFINED PROJECT_VER)
    file(STRINGS "${GADGET_SOURCE_DIR}/version.txt" GADGET_VERSION)
    if(GADGET_VERSION)
        # A version that already ends in a letter (1.4a, 1.4d) is a test version by
        # itself: the letter is the test marker, so no "-test" is appended. A plain
        # numeric version (1.4) is a release version, and its test image says so.
        if(GADGET_VERSION MATCHES "[a-z]$")
            set(PROJECT_VER "${GADGET_VERSION}")
        else()
            set(PROJECT_VER "${GADGET_VERSION}-test")
        endif()
    endif()
endif()

# ESP-IDF reads version.txt from the active project root unless PROJECT_VER is set.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
