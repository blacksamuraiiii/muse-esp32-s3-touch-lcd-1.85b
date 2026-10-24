/*
 * Bench-only power attribution sweeps.
 *
 * Purpose: hold one hardware configuration at a time so the night recorder
 * (muse_telemetry) can attribute the resting current to a single load. It is
 * a measurement aid, not user-facing behaviour: the command exists only in
 * Test images and every call is paired with a telemetry event so the CSV
 * carries the transitions.
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"

/* Applies the named configuration and writes a one-line summary to `out`.
 * Names: base, nolightsleep, cpu240, panel_awake, codec_rail, codec_open.
 * "base" restores the resting state (panel SLPIN, backlight 0, codecs closed,
 * no PM locks). Unknown names return ESP_ERR_INVALID_ARG. */
esp_err_t muse_powcfg_apply(const char *name, char *out, size_t capacity);
