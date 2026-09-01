// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Opaque wrapper around CNSDK (Android) interlacing API.
 *
 * Encapsulates leia_core and leia_interlacer so the compositor
 * does not include CNSDK headers directly.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdint.h>
#include "xrt/xrt_vulkan_includes.h"

#ifdef __cplusplus
extern "C" {
#endif

struct leia_cnsdk;

/*!
 * Create and asynchronously initialise a CNSDK core + backlight.
 *
 * @param[out] out_cnsdk  Receives the opaque handle (NULL on failure).
 * @return XRT_SUCCESS on success.
 */
xrt_result_t
leia_cnsdk_create(struct leia_cnsdk **out_cnsdk);

/*!
 * Provide the host-iface Android accessors (JavaVM + Activity getters)
 * captured at `xrtPluginNegotiate` (`xrt_plugin_host_iface`).
 *
 * The plug-in statically links its own copy of the runtime's
 * `android_globals`, which the runtime never populates (hidden-visibility
 * symbols bind to the plug-in's private copy), so CNSDK init must obtain
 * the `JavaVM`/Activity through these host callbacks rather than calling
 * `android_globals_get_vm()` itself. NULL accessors (older runtime, or a
 * non-Android host) leave the legacy `android_globals` fallback in place.
 *
 * @param get_vm        Host getter for the `JavaVM *` (as `void *`), or NULL.
 * @param get_activity  Host getter for the Activity `jobject` (as `void *`), or NULL.
 */
void
leia_cnsdk_set_host_android_accessors(void *(*get_vm)(void), void *(*get_activity)(void));

/*!
 * Register the runtime's optional "class-host Context" accessor
 * (`xrt_plugin_host_iface::get_android_class_host_context`, runtime #1037 /
 * ADR-036 D2). When the plug-in runs IN THE APP's process the app ships no
 * vendor Java glue, so CNSDK's core loader must build its DexClassLoader with
 * the RUNTIME APK's classloader as the parent — which it takes from the
 * Context handed to `leia_core_library_load`. Class loading only: everything
 * Activity-typed keeps using the accessor above.
 *
 * NULL (an older runtime without the slot, or a runtime that could not create
 * the Context) keeps the previous behaviour — the Activity / Service Context
 * is used for everything.
 */
void
leia_cnsdk_set_host_class_context_accessor(void *(*get_class_host_context)(void));

/*!
 * Panel size in pixels from Android's `Display.getRealMetrics()`, in the
 * device's NATURAL orientation. Needs no CNSDK — usable before the CNSDK
 * worker has cached the device config.
 *
 * @return true and fills the outputs, false when they cannot be determined.
 */
bool
leia_cnsdk_get_android_panel_px(uint32_t *out_w, uint32_t *out_h, float *out_w_m, float *out_h_m);

/*!
 * Read all `debug.dxr.leia.*` calibration setprops and log them.
 *
 * Idempotent (cached after first call). Safe to call from the plug-in
 * `probe()` so the values land in logcat at `xrCreateInstance` time
 * even when the emulator never reaches CNSDK init due to a missing
 * Vulkan extension. See
 * `docs/cnsdk-android-calibration.md` for the knob table.
 */
void
leia_cnsdk_log_calibration_knobs(void);

/*!
 * Destroy a CNSDK handle and release all resources.
 *
 * @param cnsdk_ptr  Pointer to handle; set to NULL on return.
 */
void
leia_cnsdk_destroy(struct leia_cnsdk **cnsdk_ptr);

/*!
 * Check whether the asynchronous core init has completed.
 *
 * @return true once the core is fully initialised.
 */
bool
leia_cnsdk_is_initialized(struct leia_cnsdk *cnsdk);

/*!
 * Notify CNSDK that the host Activity has paused (backgrounded).
 *
 * Wraps leia_core_on_pause. Safe to call any time after
 * @ref leia_cnsdk_create — CNSDK no-ops if the core isn't initialized
 * yet. Stops face-tracking cameras + dims the backlight; idempotent
 * across repeated calls.
 *
 * Intended caller: OpenXR session state machine in oxr_session.c on
 * entering XR_SESSION_STATE_STOPPING.
 */
void
leia_cnsdk_on_pause(struct leia_cnsdk *cnsdk);

/*!
 * Notify CNSDK that the host Activity has resumed (foregrounded).
 *
 * Wraps leia_core_on_resume. Counterpart of @ref leia_cnsdk_on_pause.
 *
 * Intended caller: OpenXR session state machine on entering
 * XR_SESSION_STATE_READY after a pause.
 */
void
leia_cnsdk_on_resume(struct leia_cnsdk *cnsdk);

/*!
 * Select the eye-tracking control mode (#522): 0 = MANAGED (CNSDK owns the
 * tracking-loss lifecycle via NoFaceMode — grace + auto-2D), 1 = MANUAL (CNSDK
 * stands down; the app drives 2D⇄3D). Stored and applied to the core; the
 * face-tracking worker reconciles it once licensing/availability is known
 * (MANAGED auto-2D only engages when face tracking is actually available, else
 * the force-3D fallback persists). Safe to call any time after
 * @ref leia_cnsdk_create.
 *
 * @param cnsdk  Opaque CNSDK handle.
 * @param mode   0 = MANAGED, 1 = MANUAL.
 */
void
leia_cnsdk_set_eye_tracking_mode(struct leia_cnsdk *cnsdk, uint32_t mode);

/*!
 * Fetch native display metrics from CNSDK's device config.
 *
 * The four values are snapshotted once by the face-tracking worker
 * thread right after @ref leia_core_is_initialized first returns true
 * and stored on the wrapper struct. Subsequent calls return the
 * cached values without re-acquiring CNSDK's device config every
 * frame. Returns false until that snapshot has happened; caller is
 * expected to poll across frames.
 *
 * @param[out] out_width_m   Display physical width in meters.
 * @param[out] out_height_m  Display physical height in meters.
 * @param[out] out_pixel_w   Panel pixel width.
 * @param[out] out_pixel_h   Panel pixel height.
 * @return true if all outputs were populated.
 */
bool
leia_cnsdk_get_display_metrics(struct leia_cnsdk *cnsdk,
                               float *out_width_m,
                               float *out_height_m,
                               uint32_t *out_pixel_w,
                               uint32_t *out_pixel_h);

/*!
 * Per-view (tile) resolution in pixels, in the device NATURAL orientation
 * (CNSDK VIEW_RESOLUTION_PX). The 3D view_scale is this ÷ panel resolution
 * (#518). Cached by the worker; poll across frames.
 *
 * @param[out] out_view_w              Per-view (tile) pixel width, natural frame.
 * @param[out] out_view_h             Per-view (tile) pixel height, natural frame.
 * @param[out] out_natural_orientation Device natural orientation (-1 if unknown).
 * @return true once the worker has cached a non-zero view resolution.
 */
bool
leia_cnsdk_get_view_resolution(struct leia_cnsdk *cnsdk,
                               uint32_t *out_view_w,
                               uint32_t *out_view_h,
                               int32_t *out_natural_orientation);

/*!
 * Non-blocking check for whether CNSDK face tracking is running.
 *
 * Enable + start happens asynchronously on a worker thread spawned by
 * @ref leia_cnsdk_create, so callers can poll this every frame from the
 * render thread without stalling. Returns true once the worker has
 * finished enabling + starting; false until then (or permanently if
 * the enable call failed).
 *
 * @return true once face tracking is started.
 */
bool
leia_cnsdk_ensure_face_tracking_started(struct leia_cnsdk *cnsdk);

/*!
 * Idempotent: lazily create the CNSDK Vulkan interlacer in atlas mode
 * once @ref leia_core_is_initialized returns true. Safe to call every
 * frame. Atlas mode means CNSDK accepts the SBS atlas VkImage+View
 * directly via @ref leia_cnsdk_weave and does the L/R split internally;
 * the DP doesn't have to manage per-view images or per-tile blits.
 *
 * @return true if the interlacer exists and is ready to weave.
 */
bool
leia_cnsdk_ensure_interlacer(struct leia_cnsdk *cnsdk,
                              VkDevice device,
                              VkPhysicalDevice physDev,
                              VkFormat targetFmt);

/*!
 * Fetch the latest predicted primary face position from CNSDK.
 *
 * Returns false until face tracking is running and CNSDK has a face
 * lock. Position is returned in meters relative to the **display
 * center** (matching `xrt_eye_position`'s convention).
 *
 * Three sources, in preference order: the core's non-predicted face,
 * the core's predicted face, then the head-tracking service's raw
 * frame-listener detection. The first two already carry the camera
 * extrinsics and are used as-is; the third is CAMERA-space
 * `posePosition` (origin at the camera, image Y down) and is lifted
 * into display-center space with the `leia_camera::translation_mm`
 * snapshot taken from `leia_device_config` once the core initialized.
 *
 * The listener source also carries a wall-clock liveness bound: if the
 * frame-listener callback stops firing, the cached face expires and is
 * no longer served (only the callback used to be able to clear it, so
 * a wedged service could latch a stale eye forever — #152 L-c). When
 * no source has a face this returns false and the caller falls back to
 * the nominal viewer with `is_tracking` false.
 *
 * @param[out] out_x  Face position X (meters, display-relative).
 * @param[out] out_y  Face position Y (meters, display-relative).
 * @param[out] out_z  Face position Z (meters, +toward viewer).
 * @return true if a valid face was returned.
 */
bool
leia_cnsdk_get_primary_face(struct leia_cnsdk *cnsdk,
                            float *out_x,
                            float *out_y,
                            float *out_z);

/*!
 * Fetch the viewer's two EYE positions, not just the face centre.
 *
 * The reason this exists alongside @ref leia_cnsdk_get_primary_face: a face
 * point alone forces the DP to synthesize the pair as centre ± (IPD/2, 0, 0),
 * a constant HORIZONTAL vector. That silently discards head ROLL — roll your
 * head and the true eyes rotate about the face centre, so the synthesized pair
 * keeps the full horizontal disparity and has zero vertical disparity, and the
 * runtime's per-eye Kooima frustums are built from the wrong geometry. The
 * Windows arm never had this problem because the SR SDK hands back two real
 * eye points; this restores parity on Android.
 *
 * Positions are in meters relative to the **display center**, expressed in the
 * CURRENT held orientation — the same frame and units as
 * @ref leia_cnsdk_get_primary_face, so the two are directly comparable.
 * `out_left` is the VIEWER's left eye (more negative display X).
 *
 * Four sources are tried in preference order: CNSDK's experimental
 * `leia_core_get_lookaround_eyes` (the pair the Unity/LeiaViewer path uses),
 * `leia_core_get_non_predicted_eyes`, the frame listener's deprojected
 * `eyePoints`, and finally the listener's face point combined with its reported
 * head-roll angle. Returns false when none is available, and the caller should
 * then fall back to its own synthesis.
 *
 * @param[out] out_left   Left-eye xyz (meters, display-relative). 3 floats.
 * @param[out] out_right  Right-eye xyz (meters, display-relative). 3 floats.
 * @return true if a real eye PAIR was produced.
 */
bool
leia_cnsdk_get_primary_eyes(struct leia_cnsdk *cnsdk, float out_left[3], float out_right[3]);

/*!
 * Kill switch for @ref leia_cnsdk_get_primary_eyes, read from
 * `debug.dxr.leia.lookaround_eyes` (default ON) and cached.
 *
 * Setting it to 0 restores the legacy fixed-horizontal eye pair, so the roll
 * behaviour can be A/B'd live on a device without a rebuild. Lives here rather
 * than in the DP because the sysprop helpers are private to leia_cnsdk.cpp.
 */
bool
leia_cnsdk_use_lookaround_eyes(void);

/*!
 * Perform CNSDK Vulkan interlacing on an SBS atlas.
 *
 * Atlas mode: pass the runtime's pre-composited SBS atlas image+view
 * directly. CNSDK does the L/R split internally via
 * @ref leia_interlacer_vulkan_set_interlace_view_texture_atlas. The DP
 * does no per-view image management and no per-frame blits, so there's
 * no GPU stall between us and CNSDK — its `do_post_process` records
 * and submits its own cmd buffer when it's ready.
 *
 * Caller must have first invoked @ref leia_cnsdk_ensure_interlacer; if
 * the interlacer isn't ready yet this function is a no-op (no submit,
 * no GPU side effects), making it safe to call every frame during the
 * async core-init window.
 *
 * @param cnsdk         Opaque CNSDK handle.
 * @param device        Vulkan logical device.
 * @param physDev       Vulkan physical device.
 * @param atlas_image   SBS atlas VkImage.
 * @param atlas_view    Matching VkImageView covering the full atlas.
 * @param atlas_width   Atlas width in pixels (= view_w * tile_columns).
 * @param atlas_height  Atlas height in pixels (= view_h * tile_rows).
 * @param targetFmt     Format of the target / swapchain image.
 * @param w             Target width in pixels.
 * @param h             Target height in pixels.
 * @param fb            Target framebuffer.
 * @param targetImage   Target VkImage (for CNSDK-side layout transitions).
 * @param vp_x          Weave viewport origin x in the target (0 → full target).
 * @param vp_y          Weave viewport origin y in the target.
 * @param vp_w          Weave viewport width  (0 → full target = w).
 * @param vp_h          Weave viewport height (0 → full target = h).
 *
 * The viewport params (XR_DXR_display_zones, #568) confine the interlace to a
 * sub-rect of the target — the CNSDK weaver folds vpX/vpY into its phase math
 * so the lenticular alignment stays correct for an offset band. Pass 0,0,0,0
 * to fill the whole target (the pre-zone behavior).
 *
 * @param wait_sem   Optional binary semaphore the interlacer waits before it
 *                   samples the atlas (runtime#1073 L11). Pass VK_NULL_HANDLE
 *                   when the caller has already synchronised on the CPU.
 * @param signal_sem Optional binary semaphore the interlacer signals when the
 *                   weave has finished writing the target. VK_NULL_HANDLE if
 *                   the caller does not chain anything after it.
 *
 * @return true if the weave was submitted — i.e. `wait_sem` was consumed and
 *         `signal_sem` will be signalled. false means neither happened, and a
 *         caller that passed binary semaphores MUST drain them itself before
 *         the next frame re-signals them.
 */
bool
leia_cnsdk_weave(struct leia_cnsdk *cnsdk,
                 VkDevice device,
                 VkPhysicalDevice physDev,
                 VkImage atlas_image,
                 VkImageView atlas_view,
                 uint32_t atlas_width,
                 uint32_t atlas_height,
                 VkFormat targetFmt,
                 uint32_t w,
                 uint32_t h,
                 VkFramebuffer fb,
                 VkImage targetImage,
                 int32_t vp_x,
                 int32_t vp_y,
                 uint32_t vp_w,
                 uint32_t vp_h,
                 VkSemaphore wait_sem,
                 VkSemaphore signal_sem);

/*!
 * Report this window's rectangle on the panel (runtime#1033 / #150, ADR-036 D6).
 *
 * The runtime's per-window compositor instance calls this before every weave. It
 * becomes the BASE screen position the interlace phase is referenced to; the
 * per-frame zone/canvas offset is added on top
 * (`set_viewport_screen_position = window origin + zone offset`).
 *
 * Coordinates are CURRENT-orientation screen pixels, exactly as Android's
 * `View.getLocationOnScreen()` reports them — CNSDK rotates them into the panel's
 * natural orientation itself (`interlacer.cpp`: "the user provides data in the
 * current orientation space, we convert it to the natural one"), which is also
 * what CNSDK's own `InterlacedSurfaceView._updatePosition` passes through. Do NOT
 * pre-rotate and do NOT grid-snap here: snapping is the weaver's business
 * (ADR-033), and the runtime deliberately reports the raw geometry.
 *
 * Cached; a repeat of the same rect makes no vendor call. Never calling it leaves
 * the base at (0,0) = display-scoped weaving, the pre-#150 behaviour.
 *
 * @param cnsdk       Handle (NULL-safe).
 * @param x           Window left edge, current-orientation screen pixels.
 * @param y           Window top edge, current-orientation screen pixels.
 * @param w           Window width in screen pixels.
 * @param h           Window height in screen pixels.
 * @param display_id  Android `Display.getDisplayId()`; -1 = unknown. Recorded
 *                    only — CNSDK has no multi-display concept yet (L6).
 */
/*!
 * True while the #201 tracking watchdog is cycling core pause/resume to
 * recover a lost frame subscription. The VK DP skips the weave for these few
 * frames — CNSDK throws if the interlacer runs mid-teardown.
 */
bool
leia_cnsdk_is_tracking_cycling(struct leia_cnsdk *cnsdk);

void
leia_cnsdk_set_window_screen_rect(
    struct leia_cnsdk *cnsdk, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t display_id);

/*!
 * Panel size in the display's CURRENT orientation, reported by the runtime.
 *
 * CNSDK's own metrics are orientation-blind (native portrait on NP02J), so this
 * is the only reliable answer to "which panel dimension is height right now",
 * which the bottom-origin phase conversion needs.
 */
void
leia_cnsdk_set_panel_size(struct leia_cnsdk *cnsdk, uint32_t panel_w, uint32_t panel_h, int32_t display_id);

/*!
 * #206: the runtime's MEASURED weave->scanout residual, in ns (0 = unknown).
 *
 * A DURATION, not a timestamp. It is converted to CNSDK's absolute
 * CLOCK_MONOTONIC target inside `leia_cnsdk_weave()` — AT the weave, never
 * here — because any latency between the runtime publishing it and the weave
 * happening would silently shorten the horizon, and a shortened horizon looks
 * like an improved measurement while making prediction worse.
 */
void
leia_cnsdk_set_predicted_scanout(struct leia_cnsdk *cnsdk, uint64_t weave_to_scanout_ns);

#ifdef __cplusplus
}
#endif
