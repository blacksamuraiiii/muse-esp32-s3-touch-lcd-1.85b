/*
 * GitHub Release OTA for the 1.85B build.
 *
 * The official OTA path (ota.c) takes a URL and installs whatever image it
 * finds there. This adds the missing half: a small public manifest that says
 * which version is current, fetched from the repo's latest release, compared
 * against the running image, and only then handed to ota_start().
 *
 * Manifest shape (see ota/publish_github.sh):
 *   {"version":"1.1","url":"https://.../muse-gadget.bin","sha256":"...","size":1234,"notes":"..."}
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Kicks a manifest check on its own task. Returns false when a check is
 * already running or the task could not be created. Safe from any task,
 * including the LVGL one. */
bool ota_github_check(void);

/* Short state for the settings row / update page, e.g. "Checking...",
 * "Up to date (1.1)", "Downloading 42%", "Failed: no manifest".
 * LVGL task only (it is also called from the OTA task's completion path, but
 * only writes into the caller's buffer). */
void ota_github_status(char *buf, size_t n);

/* True while a check or download started by ota_github_check() is in flight. */
bool ota_github_busy(void);
