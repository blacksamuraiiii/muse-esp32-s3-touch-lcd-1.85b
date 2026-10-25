/* SPDX-License-Identifier: Apache-2.0 */
/*
 * muse_interact_hand_assets.h -- TEMPLATE, NOT A BUILDABLE ASSET FILE.
 *
 * The game subpage needs the three Game-icons hand images as LVGL image
 * descriptors. Do NOT hand-draw replacements here: the user has already
 * rejected two rounds of hand drawings, a pixel hand, a Font Awesome
 * silhouette, a self-drawn object set, Adafruit sprites and Twemoji.
 * The only approved art is the Game-icons set below.
 *
 * Source (user decision 2026-10-25, design §5.5):
 *   game-icons/icons repository, john-redman/rock.svg, paper.svg,
 *   scissors.svg -- John Redman, CC BY 3.0, 512x512 grid, single colour.
 *   Keep the Game-icons attribution in the 1.5a release notes (CC BY 3.0).
 *   Order in the game subpage is rock, paper, scissors (left to right).
 *
 * Conversion, to be done ONCE offline before the first Test build, then
 * this file is replaced by the generated one (same symbol names):
 *   1. Take each SVG, rotate the whole drawing 90 degrees
 *      counter-clockwise (hands point UP, not right). Rotation is part of
 *      the approved design (mockup v17); unrotated hands are a bug, not
 *      a style choice.
 *   2. Render to PNG with transparency, white hand on transparent
 *      background. Choice size ~100px, result size ~130px; generate ONE
 *      master (e.g. 256px) and let LVGL scale it to both sizes with
 *      smoothing -- do NOT generate two different pixel grids.
 *   3. Convert with the repo's LVGL image converter to C arrays
 *      (true-colour + alpha, or the repo's usual image format -- check
 *      what muse_ui.c's lv_image_create() expects in the repo's LVGL
 *      version before choosing; do not guess the format from another
 *      LVGL major version).
 *   4. Name the descriptors exactly:
 *        muse_interact_hand_rock, muse_interact_hand_paper,
 *        muse_interact_hand_scissors  (type lv_image_dsc_t).
 *      The loser-dim effect is NOT a second asset: the code recolours
 *      the same image to COLOR_DIM at runtime (see muse_interact_ui.c).
 *      The winner border is a container border, also not an asset.
 *   5. Verify on device at 100px and 130px before calling the game UI
 *      done: fist / open palm / V-hand must each read on their own,
 *      not only as a trio. The Adafruit lesson applies here too --
 *      a hand that only reads in a trio is not good enough.
 *
 * Until step 4 is done, muse_interact_ui.c refuses to build: it includes
 * this file, and this file deliberately stops the build with #error.
 * Delete the #error below ONLY when the three descriptors above exist.
 * Do not silence it by drawing a placeholder hand.
 */
#pragma once

#error "Generate the Game-icons hand assets first (see the conversion steps in this file); do not build the game subpage with placeholder art."
