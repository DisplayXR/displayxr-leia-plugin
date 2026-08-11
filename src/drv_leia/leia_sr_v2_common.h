// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR v2 objects shared by every graphics arm: instance, display, lens.
 * @ingroup drv_leia
 *
 * The per-arm files (`leia_sr_d3d11.cpp`, `..._d3d12.cpp`, `..._gl.cpp`,
 * `leia_sr.cpp`) differ only in how the *weaver* is created and how the input
 * texture is bound. Everything else — waiting for the server, finding the SR
 * display, reading its geometry, the 2D/3D lens hint — is identical.
 *
 * Under v1 that commonality is duplicated four times, and the copies have
 * already drifted (the arms disagree on how long they wait and on what they log
 * when the display never arrives). Rather than carry that duplication forward
 * into a second API family, the v2 path shares one implementation.
 *
 * Nothing here is declared unless the plug-in was built with the v2 SDK; see
 * `leia_sr_api_select.h` for how the path is chosen at runtime.
 */

#pragma once

#ifdef DXR_LEIA_HAS_SR_V2

#include <sr/sr_display.h>
#include <sr/sr_instance.h>
#include <sr/sr_lens.h>
#include <sr/sr_result.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Display geometry, in the units the runtime wants rather than the units SR
 * reports (SR gives centimetres; Kooima wants metres).
 */
struct leia_sr_v2_display_info
{
	//! Physical panel size in metres, for the Kooima FOV calculation.
	float width_m;
	float height_m;

	//! Panel resolution and its origin in virtual-desktop coordinates.
	uint32_t pixel_width;
	uint32_t pixel_height;
	int32_t screen_left;
	int32_t screen_top;

	/*!
	 * Recommended render size, **per eye** — confirmed with the SDK team, who
	 * pointed out that `srDisplayGetRecommendedTextureSize` forwards directly to
	 * the same `getRecommendedViewsTextureWidth/Height()` the v1 path calls. For
	 * this value the two APIs are not merely equivalent, they are one
	 * implementation, so v1 and v2 cannot disagree.
	 *
	 * Two properties that are easy to get wrong:
	 *
	 * - **Never derive it.** `physicalResolutionWidth / 2` is the SDK's fallback
	 *   when the display config leaves it unset, *not* a derivation — the value
	 *   is overridable per display (`ScreenParameters/recommendedViewsTextureWidth`).
	 *   Computing it ourselves as a cross-check or fallback would override an
	 *   explicit per-panel recommendation with our own arithmetic. Read it.
	 * - **It is orientation-aware**: width and height swap on a portrait panel.
	 *   Consistent across v1/v2, so not a migration hazard, but a transposed
	 *   view texture partway through a rotation test is a thing to expect rather
	 *   than to debug.
	 */
	uint32_t recommended_view_width;
	uint32_t recommended_view_height;
	bool recommended_valid;
};

/*!
 * Create an SR instance, waiting up to @p max_time seconds for the SR server.
 *
 * The server may still be starting when a session is created, so this retries
 * rather than failing on the first refusal — same contract as the v1
 * `SRContext::create()` loop it replaces.
 *
 * @note Does NOT call `srInitialize`. Initialisation starts the trackers, and
 *       the arms must not do that until their weaver is registered — the v1
 *       ordering comment (`initialize()` *after* the weaver exists) applies
 *       identically here. Call @ref leia_sr_v2_initialize once the weaver is up.
 *
 * @return true on success, with `*out_instance` owned by the caller.
 */
bool
leia_sr_v2_create_instance(double max_time, SrInstance *out_instance);

/*!
 * Start the senses. Call once, after the weaver has been created.
 */
bool
leia_sr_v2_initialize(SrInstance instance);

/*!
 * Wait for an SR display to become present and valid, then read its geometry.
 *
 * @param hwnd Window used to pick the display, or NULL for the primary SR
 *             display. Passing the real window matters on a multi-display box:
 *             the weaver is bound to the display the window is on, so the
 *             geometry must come from that same display or the two disagree.
 */
bool
leia_sr_v2_query_display(SrInstance instance,
                         void *hwnd,
                         double max_time,
                         struct leia_sr_v2_display_info *out_info);

/*!
 * Create the lens handle used for 2D/3D switching.
 *
 * A display with no switchable lens is not an error: `*out_lens` is set to NULL
 * and the caller degrades to "cannot switch", exactly as the v1 path does when
 * `SwitchableLensHint::create` throws.
 */
void
leia_sr_v2_create_lens(SrInstance instance, SrLens *out_lens);

/*!
 * Human-readable name for an `SrResult`, for log lines. Never NULL.
 *
 * v1 failures surfaced as exceptions carrying a message; v2 returns a bare
 * integer, and a log line reading "failed: -7" is the kind of thing that turns
 * a five-minute diagnosis into an afternoon.
 */
const char *
leia_sr_v2_result_str(SrResult r);

#ifdef __cplusplus
}
#endif

#endif // DXR_LEIA_HAS_SR_V2
