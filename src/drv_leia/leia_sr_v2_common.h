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

/* ------------------------------------------------------------------ *
 * Absolute target time (SR SDK 1584: srGetTimeUs + srWeaverSetTargetTime)
 *
 * The runtime hands the plug-in a per-weave FORWARD HORIZON (a duration)
 * through the `set_predicted_scanout` DP slot. `srWeaverSetLatency` takes a
 * duration too, so the horizon has to be re-anchored to the SDK's idea of
 * "now", which it samples further down inside the weave -- the difference
 * lands straight on the predicted eye position. The absolute API removes that
 * conversion: we hand over the instant itself.
 *
 * OPT-IN, default OFF -- `DXR_LEIA_SR_TARGET_TIME=1|true|on`. The adaptive
 * `setLatency` path stays as the fallback and is NOT going away.
 *
 * ## Why the shape is "engage once, then never stop"
 *
 * SDK 1584 ships three state-machine defects (fixed upstream, not in 1584):
 *
 *  1. `srWeaverSetTargetTime(w, 0)` -- the documented "clear" -- does NOT
 *     restore the previously configured latency mode. The weaver keeps the
 *     last target's horizon forever.
 *  2. Target mode starves the SDK's cadence sampler, so a clear yields ~10
 *     frames pinned at the 150 ms ceiling, and toggling per frame silently
 *     inflates the fallback horizon.
 *  3. The target is a non-atomic field read twice; a concurrent clear can make
 *     it predict from minus the machine uptime.
 *
 * So the design never clears and never toggles. Target mode is a property of
 * the WEAVER'S LIFE, not of the frame:
 *
 *  - probe ONCE per weaver, before any real target is ever set;
 *  - once engaged, EVERY weave sets a fresh target;
 *  - if the runtime's forward horizon goes stale we do NOT clear and do NOT
 *    drop back to `setLatency` (a sticky target would override it anyway) --
 *    whatever horizon the fallback logic computed is expressed as
 *    `now + horizon` instead;
 *  - the setter is only ever called from the weave thread.
 *
 * The helpers below are the parts that do not depend on how an arm reaches its
 * weaver: the opt-in read, the probe-result classification, the clock gate,
 * the clock read and the acceptance log. Each arm keeps its own three-line
 * dispatch helper (see the `w_*` blocks) because the three structs reach the
 * weaver differently -- a typed `SrWeaver` member on D3D11/D3D12, an opaque
 * `void *` behind the `leia_vk_weaver_ops` vtable on Vulkan.
 * ------------------------------------------------------------------ */

//! Per-weaver latch for "can this weaver take an absolute target?".
/*!
 * One-shot WARN latches, owned by the CALLER (one set per weaver).
 *
 * These used to be function-local `static bool` inside the shared helpers,
 * which made them once per PROCESS per branch rather than once per arm: if
 * D3D11 hit a failure and VK later hit the same one, only D3D11 logged and the
 * record read as though VK had been fine. Same class of lying diagnostic as the
 * getLatency readback that disagreed with the weave. Keyed per weaver now, so
 * every arm that refuses target mode says so in its own voice.
 */
struct leia_sr_v2_warn_latches
{
	bool no_slot;      //!< SR_ERROR_FUNCTION_UNSUPPORTED reported
	bool no_interface; //!< SR_ERROR_FEATURE_NOT_SUPPORTED reported
	bool probe_failed; //!< any other probe failure reported
	bool now_failed;   //!< srGetTimeUs failed mid-run
};

enum leia_sr_target_state
{
	LEIA_SR_TARGET_UNKNOWN = 0,   //!< Not probed yet.
	LEIA_SR_TARGET_AVAILABLE = 1, //!< Probed and engaged; every weave sets one.
	LEIA_SR_TARGET_UNAVAILABLE = 2, //!< Opt-in off, no interface, or clock mismatch.
};

/*!
 * Ceiling we clamp the horizon to ourselves before building a target.
 *
 * The SDK documents the same 150 ms clamp, but its bounds handling is the
 * subject of defect 2 above, so we do not rely on it.
 */
#define LEIA_SR_TARGET_MAX_HORIZON_US 150000

/*!
 * Is the absolute-target opt-in set? Read once per process, cached.
 *
 * `DXR_LEIA_SR_TARGET_TIME` = `1` / `true` / `on` enables. Anything else,
 * including unset, leaves the plug-in on the adaptive `setLatency` path.
 */
bool
leia_sr_target_time_opt_in(void);

/*!
 * Classify the one-shot probe's result, warning once per arm on each way of
 * saying "no".
 *
 * Two distinct codes mean no, and they mean different things:
 *   - `SR_ERROR_FUNCTION_UNSUPPORTED` -- older SR runtime; slots 89/90 are
 *     NULL and the loader trampoline returned without reaching a backend.
 *   - `SR_ERROR_FEATURE_NOT_SUPPORTED` -- current runtime, but this weaver's
 *     backend has no target-time interface.
 * Both are "no", neither is a fault, and they are logged separately because
 * the fix is different.
 *
 * @param r   Result of `srWeaverSetTargetTime(weaver, 0)` on a REAL weaver.
 * @param arm "D3D11" / "D3D12" / "VK", for the log line.
 * @return true when the weaver accepted the call and target mode may engage.
 */
bool
leia_sr_v2_target_time_probe_ok(SrResult r, const char *arm, struct leia_sr_v2_warn_latches *w);

/*!
 * Verify `srGetTimeUs` really is the clock it documents before we build any
 * target on top of it.
 *
 * `srGetTimeUs` is documented as QPC-since-boot microseconds. Rather than
 * trust the doc, this compares it against this process's own QPC-since-boot,
 * computed from `QueryPerformanceCounter`/`QueryPerformanceFrequency`. The two
 * are derived from the same counter divided by the same frequency, so the
 * honest expected agreement is TENS OF MICROSECONDS -- call latency, nothing
 * more. The gate fails closed at 250 ms (target mode is simply not used for
 * this weaver); a delta above 1 ms still engages but earns its own WARN,
 * because at that size something real is wrong (a different frequency read, a
 * wall-clock leak) even though it passes.
 *
 * Runs BEFORE the probe deliberately: if the clock is not the clock, we never
 * call `srWeaverSetTargetTime` at all, not even with the probe's 0.
 *
 * @return true when the two clocks agree well enough to build targets.
 */
bool
leia_sr_v2_clock_gate(SrInstance instance, const char *arm);

/*!
 * The signed delta the last @ref leia_sr_v2_clock_gate computed, in
 * microseconds (`srGetTimeUs - our QPC-since-boot`).
 *
 * Exposed so a diagnostic can RECORD the number the gate already measured
 * rather than measure its own. A second measurement would be a second
 * measurement: taken at a different instant, through a different call, and
 * therefore able to disagree with the one that actually decided whether target
 * mode engaged. This returns that decision's evidence or nothing at all.
 *
 * @return false when the gate has never run in this process -- which is the
 *         NORMAL state on the adaptive-setLatency arm, where it is never
 *         reached. `*out_delta_us` is left untouched.
 */
bool
leia_sr_v2_clock_gate_last_delta(int64_t *out_delta_us);

/*!
 * `srGetTimeUs` with the failure logged once per arm. False leaves `*out_now_us`
 * untouched and the caller must not push a target this weave.
 */
bool
leia_sr_v2_now_us(SrInstance instance, uint64_t *out_now_us, const char *arm, struct leia_sr_v2_warn_latches *w);

/*!
 * One-shot acceptance log for the first target a weaver accepts.
 *
 * Without this there is no way to tell "target mode ran" from "target mode was
 * never called and the image happened to look fine" -- a weaver that ignored
 * the target just falls back to the configured latency and still looks
 * plausible. Same reasoning as the `#625 snap ... LIVE` line.
 *
 * @param resolved_us `srWeaverGetLatency` read back AFTER the weave returned.
 */
void
leia_sr_v2_log_target_accepted(const char *arm,
                               uint64_t target_us,
                               uint64_t horizon_us,
                               uint64_t resolved_us);

#ifdef __cplusplus
}
#endif

#endif // DXR_LEIA_HAS_SR_V2
