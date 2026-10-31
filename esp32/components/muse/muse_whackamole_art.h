/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_whackamole_art.h -- whack-a-mole images.
 *
 * Mole sources (user decision 2026-10-31, personal use):
 *   Elecrow / Horion donor whack-a-mole artwork, the same
 *   images in both repos (byte-identical C arrays):
 *   dishu.png (normal) and dishu_hit.png (hit, starburst),
 *   175x147 RGBA, Lanczos-resized to the 56x47 display size
 *   of mockup v9. The donor code is NOT reused (licence
 *   unclear, buffer-overflow bug); the artwork is the only
 *   donor exception, explicitly approved by the user.
 * Hole and front lip are generated, not copied: soil rim
 * ellipse outer 36x20, black pit inner 29x15 (v9 mockup),
 * lip = bottom-half rim band with the EXACT same radii,
 * so it coincides with the rim and never double-edges
 * (v8 mockup flaw: a separate uniform-width arc).
 *
 * Format mirrors the hand assets (LVGLImage.py, ARGB8888):
 * lv_image_dsc_t, stride = w * 4, straight alpha.
 * Generated once, offline, by
 * ~/workspace/whackamole-art-gen.py; do not hand-edit the
 * .c file -- rerun the script.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t muse_whackamole_mole;
extern const lv_image_dsc_t muse_whackamole_mole_hit;
extern const lv_image_dsc_t muse_whackamole_hole;
extern const lv_image_dsc_t muse_whackamole_lip;

#ifdef __cplusplus
}
#endif
