// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux weaver-backend seam — the plug-in-side mirror of the
 *         LeiaSR Linux SDK interface contract.
 *
 * Every declaration in this header is shaped 1:1 by
 * `docs/leia-linux-sdk-contract.md` (status: RECONCILED vs srSDK 1.0.0,
 * issue #81) and cites the contract requirement (R-W* / R-T* / R-D*) it
 * realizes. Where the real srSDK 1.0.0 prototype diverges from a contract
 * ask, the member/function doc says so ("no srSDK 1.0.0 counterpart") — those
 * fields stay as carried asks (contract §8 verdict table), not dead weight.
 *
 * Two implementations plug in behind this seam (selected by the CMake cache
 * var `DXR_LEIA_LINUX_WEAVER`):
 *   - `leia_sr_stub.c` (Track A, this repo today): no SR SDK — canned display
 *     info + passthrough SBS blit, so the .so builds/discovers/selftests on
 *     machines and CI with no Leia hardware or SDK.
 *   - TODO(Track B): `leia_sr_linux_sdk.c` wrapping the real srSDK
 *     (`sr.h`/`sr_vk.h`, static-loader model), satisfying the same
 *     signatures. Pinned to the prototype: srSDK API 1.0.0,
 *     libLeiaSR_runtime.so BuildID fcf21021eeb277bac06fdb0e484bd4a8f31ad36b.
 *
 * Linux-desktop only — see the platform gate below.
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#pragma once

#include "xrt/xrt_config_os.h"

#if !defined(XRT_OS_LINUX) || defined(XRT_OS_ANDROID)
#error "drv_leia_linux is Linux-desktop only (XRT_OS_LINUX && !XRT_OS_ANDROID); Android uses drv_leia_android."
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Platform defines (VK_USE_PLATFORM_XCB_KHR) MUST precede vulkan.h so that
// TUs which also include vk/vk_helpers.h see the XCB PFN types regardless of
// include order — same pattern as the Windows arm's leia_sr.h.
#include "xrt/xrt_config_vulkan.h"
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Opaque backend instance — SDK context + weaver in Track B, canned state in
 * the Track A stub.
 */
struct leiasr_lnx;

/*!
 * Backend result codes. `SERVICE_UNAVAILABLE` is deliberately distinguishable
 * from generic failure (contract R-W1): the DP retries context creation with a
 * bounded budget only when the SR service is the missing piece.
 */
enum leiasr_lnx_result
{
	LEIASR_LNX_SUCCESS = 0,
	LEIASR_LNX_ERROR_SERVICE_UNAVAILABLE = 1, //!< SR service/daemon not reachable (R-W1)
	LEIASR_LNX_ERROR_FAILED = 2,              //!< any other failure
};

/*!
 * Weaver creation parameters (contract R-W2/R-W3): the backend builds its
 * weaver against the compositor's already-created Vulkan objects — it MUST
 * NOT create its own VkDevice, own a swapchain, or present.
 */
struct leiasr_lnx_create_info
{
	VkPhysicalDevice physical_device;
	VkDevice device;
	VkQueue graphics_queue;         //!< graphics-capable queue (R-W2)
	uint32_t graphics_queue_family; //!< family index of @ref graphics_queue
	VkCommandPool command_pool;

	/*!
	 * Optional X11 window (R-W3). NULL ⟹ display-scoped weaving (full-panel
	 * phase; the monitor comes from the display query). When set, used only
	 * for position/phase and monitor association — never as a render target.
	 * Passed as opaque pointers so this header needs no Xlib/xcb includes:
	 * `x11_window` carries the X11 `Window` XID, `x11_connection` the
	 * `Display*` / `xcb_connection_t*` (backend's preference).
	 *
	 * srSDK 1.0.0: `SrWeaverCreateInfoVulkan.window` takes the XID (0 =
	 * windowless, honored) — but the sdk backend now ALWAYS passes 0
	 * (multi-screen M4, ADR-033): the runtime supplies the phase origin per
	 * frame (set_present_origin -> srWeaverSetPresentOrigin), the Linux SDK
	 * never tracked the window's position anyway (getScreenRect is (0,0)),
	 * and with an X11 id the SDK's resampled-panel refusal forces 2D under a
	 * fractionally scaled XWayland desktop. `x11_window` is kept on the seam
	 * for the stub and for diagnostics only. `x11_connection` has NO srSDK
	 * counterpart (the SDK opens its own X connection) — the sdk backend
	 * ignores it.
	 * `graphics_queue_family` likewise has no counterpart (the command pool
	 * implies the family); kept because the stub and future backends want it.
	 */
	void *x11_window;
	void *x11_connection;

	/*!
	 * Bounded budget for the SR-service connect (R-W1) in seconds. The
	 * Windows plug-in uses ~5 s at DP creation; 0 means a single attempt.
	 */
	double retry_budget_s;

	/*!
	 * Format of the compositor's weave target (R-W5), so a backend that
	 * renders through a render pass can build one compatible with the
	 * caller's framebuffers up front. VK_FORMAT_UNDEFINED ⟹ the contract
	 * baseline VK_FORMAT_B8G8R8A8_UNORM. The stub (blit path) ignores it.
	 */
	VkFormat target_format;

	/*!
	 * The SR display this weaver is for (multi-screen M4): the opaque
	 * `displayId` srEnumerateDisplays reported, chained as
	 * SrDisplayBindingInfo when the installed SR runtime honours binding.
	 * 0 = none (SR 1.38, a monitor SR does not list, the plain factory):
	 * the weaver follows the active SR display, as before. Stub: ignored.
	 */
	uint64_t sr_display_id;
};

/*!
 * Per-frame weave input (contract R-W4): ONE tiled atlas image containing all
 * views side-by-side, with the grid explicit. 2x1 stereo is the baseline; the
 * grid fields keep >2-view panels expressible. Layout at call time is
 * VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL unless the backend documents
 * otherwise (the Track A stub manages its own transitions internally).
 *
 * srSDK 1.0.0: maps to `srWeaverSetInputTextureVulkan(view, w, h, format)` —
 * single SBS view honored, but NO tile-grid parameter (2×1 only; the sdk
 * backend falls back to a passthrough blit for any other grid) and NO y_flip
 * toggle (ignored + logged). Both stay as carried asks (contract §8, R-W4).
 */
struct leiasr_lnx_weave_input
{
	VkImage atlas_image;
	VkImageView atlas_view;
	uint32_t view_width;  //!< width of ONE view in the atlas, pixels
	uint32_t view_height; //!< height of ONE view, pixels
	VkFormat view_format;
	uint32_t tile_columns;
	uint32_t tile_rows;
	bool y_flip; //!< UV-flip toggle (R-W4; Vulkan apps render Y-down)

	/*!
	 * @name 2D under the lens (ADR-027 Amendment; runtime set_overlay_2d)
	 * Optional 2D layer the weaver composites OVER the woven views inside the
	 * weave and band-limits for the lens (srWeaverSetComposeInputsVulkan).
	 * VK_NULL_HANDLE = none, the weave is exactly as before. Only meaningful
	 * after @ref leiasr_lnx_compose_available returned true for this backend.
	 * Contract (the SR one): output-viewport-sized, premultiplied, encoded
	 * sRGB, RGBA8/BGRA8 (`_UNORM` or `_SRGB` view), in
	 * SHADER_READ_ONLY_OPTIMAL when the weave's command buffer executes.
	 * Applies to THIS weave only.
	 * @{
	 */
	VkImageView compose_view;
	VkFormat compose_format;
	bool compose_unchanged;  //!< XR_DXR_weave v14: same pixels as the previous weave's layer
	float compose_strength;  //!< XR_DXR_weave v15: [0,1], negative = the SR runtime's default
	//! @}
};

/*!
 * Per-frame weave output (contract R-W5): a caller-provided target — the
 * compositor owns the swapchain and presents (constraint §3.3).
 * VK_FORMAT_B8G8R8A8_UNORM is the required baseline format.
 */
struct leiasr_lnx_weave_output
{
	VkFramebuffer framebuffer; //!< may be VK_NULL_HANDLE when the backend blits (no render pass)
	VkImage image;
	uint32_t width;
	uint32_t height;
	VkFormat format;
	/*!
	 * Multi-screen M4: begin the backend's render pass with renderArea =
	 * the weave viewport instead of the whole target. Set for a SEGMENT DP
	 * (created for one screen) with a sub-rect canvas, which must not touch
	 * the pixels a sibling DP wove (runtime create_dp_vk_for_screen
	 * contract). The draw is already viewport+scissor-confined either way.
	 */
	bool confine_to_viewport;
};

/*!
 * The app WINDOW's client-area top-left in panel-relative pixels — the term the
 * lenticular interlacing phase anchors against (contract R-W7), decoupled from
 * the draw viewport so a moved/sub-panel window keeps correct lens phase
 * (per-window weaving, display-zones). The DP fills this from the compositor's
 * set_present_origin slot; (0,0) = display-scoped (full-panel window at the
 * panel top-left) = the default.
 *
 * srSDK maps this to `srWeaverSetPresentOrigin` (LeiaSR#85): the SDK combines it
 * with the viewport — phase = present_origin + viewport_offset — so this carries
 * the window term and the `viewport` arg carries the canvas offset. A running SR
 * runtime that predates the #85 slot returns SR_ERROR_FUNCTION_UNSUPPORTED and
 * the sdk backend weaves display-scoped (logged once).
 */
struct leiasr_lnx_phase_origin
{
	int32_t x;
	int32_t y;
};

/*!
 * Millimeter eye pair, SDK shape (contract R-T1: millimeters, display-center
 * origin, X right, Y up, +Z toward the viewer). The DP converts to meters.
 */
struct leiasr_lnx_eye_pair_mm
{
	float left_mm[3];
	float right_mm[3];
};

/*!
 * Static display query result (contract R-D1) — the field set maps 1:1 onto
 * `xrt_plugin_display_info`. Readable BEFORE weaver creation (the runtime asks
 * at instance creation, pre-session) and headless-tolerant (R-D4: fail soft,
 * never crash, when no SR display is attached).
 */
struct leiasr_lnx_display_info
{
	bool valid;
	float width_m;  //!< physical size (R-D1: meters)
	float height_m;
	uint32_t pixel_width; //!< native panel resolution
	uint32_t pixel_height;
	int32_t screen_left; //!< position in the X11 virtual desktop (anchors phase, §3.5)
	int32_t screen_top;
	uint32_t recommended_view_width; //!< recommended per-view render size
	uint32_t recommended_view_height;
	uint32_t refresh_mhz; //!< milli-Hz (60000 = 60 Hz): srDisplayGetRefreshRate when available (#184), else 60000
	float nominal_viewer_x_m; //!< recommended viewing position, display-center meters
	float nominal_viewer_y_m;
	float nominal_viewer_z_m;
};

/*!
 * Eye-tracking control modes (contract R-T4/R-T5; runtime contract:
 * displayxr-runtime docs/specs/vendor/eye-tracking-modes.md).
 */
enum leiasr_lnx_tracking_mode
{
	LEIASR_LNX_TRACKING_MANAGED = 0, //!< SDK owns the loss lifecycle (R-T4, required)
	LEIASR_LNX_TRACKING_MANUAL = 1,  //!< SDK stands down; app drives 2D/3D (R-T5, SHOULD)
};


/*
 *
 * Surface (a) — Vulkan weaver (contract §4).
 *
 */

/*!
 * Create the backend: SR-service connect (R-W1, bounded by
 * `info->retry_budget_s`) + weaver creation on the compositor's Vulkan
 * objects (R-W2/R-W3).
 *
 * srSDK 1.0.0 note: senses/callbacks must be registered before
 * `srInitialize` and `leiasr_lnx_query_display_info` is callable pre-create,
 * so the sdk backend keeps ONE process-lifetime SR context (instance +
 * tracker + monitor + display + lens); this call creates only the weaver
 * against it. `srDestroyInstance` joins SDK threads unboundedly — another
 * reason the context outlives DP create/destroy cycles (contract §8, R-W10).
 *
 * @return LEIASR_LNX_SUCCESS, or LEIASR_LNX_ERROR_SERVICE_UNAVAILABLE when
 *         the SR service is unreachable (distinguishable per R-W1).
 */
enum leiasr_lnx_result
leiasr_lnx_create(const struct leiasr_lnx_create_info *info, struct leiasr_lnx **out_lnx);

/*!
 * Deterministic teardown (R-W10): safe on the render thread with the device
 * idle, no event-loop pumping, bounded time even if the SR service died.
 * NULL-safe.
 */
void
leiasr_lnx_destroy(struct leiasr_lnx *lnx);

/*!
 * Record the weave into the caller's command buffer (R-W6: never submits,
 * never blocks the GPU). Input is one tiled atlas (R-W4), output a
 * caller-provided target (R-W5); @p viewport places the woven pixels on the
 * target while @p phase_origin independently anchors the lens phase (R-W7).
 */
void
leiasr_lnx_weave(struct leiasr_lnx *lnx,
                 VkCommandBuffer cmd_buffer,
                 const struct leiasr_lnx_weave_input *input,
                 const struct leiasr_lnx_weave_output *output,
                 VkRect2D viewport,
                 struct leiasr_lnx_phase_origin phase_origin);

/*!
 * The render pass the weave renders through, so the caller can pre-build
 * compatible framebuffers (R-W5) — VK_NULL_HANDLE when the backend needs no
 * render pass (the Track A stub blits).
 *
 * srSDK 1.0.0 exposes no render pass; the sdk backend returns its OWN
 * single-color-attachment pass and drives the weave through the SDK's
 * "framebuffer = 0, render pass already begun" path, relying on Vulkan
 * render-pass compatibility (contract §8, R-W5 — validated with layers).
 */
VkRenderPass
leiasr_lnx_get_render_pass(struct leiasr_lnx *lnx);

/*!
 * The compositor's output target was (re)created (R-W8) — invalidate any
 * backend cache keyed on VkImage/VkImageView handles (Vulkan recycles them).
 * Called with the device idle.
 */
void
leiasr_lnx_output_invalidated(struct leiasr_lnx *lnx);

/*!
 * Prediction horizon for the internal eye-pose consumer, absolute
 * microseconds (R-W9 — the µs path is the contract; it MUST be functional,
 * unlike the Windows VK setLatencyInFrames no-op that spawned issue #71).
 * srSDK 1.0.0: `srWeaverSetLatency(weaver, latencyUs)` — honored as asked.
 */
void
leiasr_lnx_set_latency_us(struct leiasr_lnx *lnx, uint64_t latency_us);

/*!
 * Declare the transfer function of the atlas the *next* weave will sample
 * (R-W4 nearest — the input atlas's format/encoding semantics; the contract
 * carries no colour-encoding requirement of its own) — (ADR-021
 * runtime-declared atlas encoding, runtime#1484).
 *
 * @p atlas_linear false = display-referred / sRGB-ENCODED (the ADR-021 default
 * a runtime that cannot declare degrades to), true = scene-referred / linear.
 * The backend combines it with what it alone can see — whether the *target*
 * format encodes on store — to pick the weave shader's read/write conversion
 * pair; see the truth table in `sdk_apply_srgb_conversion()`.
 *
 * Sticky, like @ref leiasr_lnx_set_latency_us: set it once and every later
 * weave keeps the value. Safe to call from the render thread immediately
 * before a weave (the sdk backend defers the one SDK call to the weave).
 *
 * srSDK 1.0.0: feeds `srWeaverSetShaderSRGBConversion(weaver, read, write)`.
 */
void
leiasr_lnx_set_atlas_linear(struct leiasr_lnx *lnx, bool atlas_linear);

/*!
 * Phase-snap a proposed window position (runtime#1588 — Windows parity with
 * the D3D11 arm's `snap_window_rect`, drv_leia/leia_sr_d3d11.cpp:2272).
 *
 * The interlacing phase is a function of where the window sits on the lens
 * lattice, so a freely dragged window lands between lens columns and the 3D
 * collapses. The fix is to quantise the drag target onto that lattice: the
 * runtime offers its raw target, the backend returns the nearest
 * lattice-correct position, the runtime moves the window there.
 *
 * Pure coordinate math: no window handle, no hardware touch, no thread
 * affinity. Called from whichever thread owns the window (NOT the render
 * thread) and once per window MOVE, never per weave.
 *
 * @return true only when @p out_x / @p out_y hold a genuinely snapped
 * position. Every other outcome writes the target back unchanged and returns
 * false — including the SDK's SR_DECLINED ("could not snap yet", typically no
 * viewing distance before the first tracked frame), which is a real answer
 * rather than a failure and is reported as such so an unsnapped position is
 * never mistaken for a snap that had nothing to correct. The caller proceeds
 * with its target either way; the boolean only says whether a correction was
 * applied.
 *
 * srSDK: `srWeaverSnapToPhase` (sr_weaver.h), called unconditionally through
 * the loader trampoline — which writes the target back before it can fail, so
 * a runtime predating the call degrades to identity for free (logged once).
 * The loader archive MUST come from the same tree as the runtime; see the sdk
 * backend for why no runtime guard can substitute for that.
 *
 * COORDINATE FRAME — pass both pairs through in whatever frame the runtime
 * gave them, and never translate either one.  The runtime slot is
 * desktop-absolute (caller-frame) and the runtime converts nothing; this
 * backend converts nothing either, and the vendor source is why that is safe
 * rather than lucky: the snap consumes only the DISPLACEMENT, target minus
 * origin, so any constant offset between frames cancels and desktop-absolute
 * and panel-relative give the same answer. What does NOT cancel is mixing the
 * frames between the two pairs — an origin in one and a target in the other
 * corrupts the displacement — and neither does the unit.
 *
 * DEVICE PIXELS, not logical ones. The displacement is measured against a
 * physical lens pitch, so a logical / fractionally-scaled pixel displacement
 * arrives multiplied by the scale factor and snaps to the wrong lattice
 * position. That failure is silent: it still returns a plausible nearby
 * integer.
 *
 * A rotated or flipped panel needs no handling here either — the SR runtime
 * converts into and out of its canonical landscape space using the
 * orientation it is weaving for.
 */
bool
leiasr_lnx_snap_to_phase(struct leiasr_lnx *lnx,
                         int32_t origin_x,
                         int32_t origin_y,
                         int32_t target_x,
                         int32_t target_y,
                         int32_t *out_x,
                         int32_t *out_y);

/*!
 * Whether THIS BUILD carries the SR-side drag phase-snap call (#271).
 *
 * Build-time truth, not run-time: true when the backend was compiled with
 * `srWeaverSnapToPhase` (sdk backend + DXR_LEIA_LNX_HAVE_SR_SNAP). A true
 * here can still meet an SR runtime that predates the call — that case is
 * logged by @ref leiasr_lnx_snap_to_phase on first use. A false here means
 * every snap returns identity no matter which runtime is installed, so the DP
 * logs it once at creation instead of letting drags stutter silently.
 *
 * @param[out] out_reason Optional. On false, a static human-readable reason
 *             (NULL-safe; untouched on true).
 */
bool
leiasr_lnx_has_sr_snap(const char **out_reason);

/*!
 * Can this backend's weaver composite a 2D layer inside the weave (ADR-027
 * Amendment, "2D under the lens"; LeiaSR ST-5801/ST-5792)?
 *
 * Probed ONCE per backend instance (one weaver each) by setting the weaver's
 * sticky compose order to SR_COMPOSE_ORDER_2D_OVER; the answer is cached. False
 * on the stub, on a build without DXR_LEIA_LNX_HAVE_SR_COMPOSE (the SDK
 * headers predate srWeaverSetComposeInputsVulkan), on an installed SR runtime
 * that predates it (SR_ERROR_FUNCTION_UNSUPPORTED) or whose Vulkan weaver
 * cannot compose (SR_ERROR_FEATURE_NOT_SUPPORTED), and when the
 * DXR_LEIA_SR_COMPOSE=0 diagnostic switch is set. Every false of the last three
 * kinds is logged once. A false means the runtime keeps compositing its 2D
 * over-layer post-weave, exactly as before.
 */
bool
leiasr_lnx_compose_available(struct leiasr_lnx *lnx);


/*
 *
 * Surface (b) — eye tracking (contract §5).
 *
 */

/*!
 * Per-frame predicted eye pair pull (R-T1) + explicit tracking state (R-T3).
 * The returned pair is the same pair the most recent weave consumed. MUST
 * always return a plausible pair — never zeros (R-T2): tracked positions
 * while tracking, animated/frozen fallback otherwise.
 *
 * srSDK 1.0.0: pair from `srWeaverGetPredictedEyePositions` (mm, the pair
 * the weave consumes — R-T1 honored). Tracking state has NO pollable flag;
 * the sdk backend latches `srSystemMonitor` events (USER_FOUND/USER_LOST +
 * DEVICE_READY/DISCONNECTED) into atomics — so is_tracking flips at raw
 * face-loss, earlier than R-T4's grace-period preference (contract §8).
 *
 * @param[out] out_pair          Millimeters, display-center (see struct doc).
 * @param[out] out_is_tracking   Explicit tracking state (R-T3) — may be NULL.
 * @param[out] out_timestamp_ns  Sample time, monotonic ns — may be NULL.
 * @return true when @p out_pair was filled (always true per R-T2 once created).
 */
bool
leiasr_lnx_get_predicted_eyes(struct leiasr_lnx *lnx,
                              struct leiasr_lnx_eye_pair_mm *out_pair,
                              bool *out_is_tracking,
                              int64_t *out_timestamp_ns);

/*!
 * Select MANAGED vs MANUAL (R-T4/R-T5). @return false when the backend
 * doesn't support the requested mode (MANAGED-only backends are acceptable
 * for v1 — Windows parity). srSDK 1.0.0 has no stand-down toggle, so the sdk
 * backend is MANAGED-only (contract §8, R-T5 carried ask).
 */
bool
leiasr_lnx_set_eye_tracking_mode(struct leiasr_lnx *lnx, enum leiasr_lnx_tracking_mode mode);


/*
 *
 * Surface (c) — display & calibration (contract §6).
 *
 * Calibration never crosses this seam (R-D3) — the backend exposes geometry
 * and state only.
 *
 */

/*!
 * Identify the (primary) SR display and report its metrics (R-D1). Static —
 * callable before @ref leiasr_lnx_create (the runtime asks at instance
 * creation) and headless-tolerant (R-D4: returns false, never crashes).
 */
bool
leiasr_lnx_query_display_info(struct leiasr_lnx_display_info *out_info);

/*!
 * Lens/backlight 2D⇄3D switch, independent of weaving (R-D2). Backs the
 * runtime's `request_display_mode` DP slot and MANAGED auto-drop (R-T4).
 *
 * srSDK (LeiaSR #266): the first srLensEnable/srLensDisable on an SR context
 * takes that context's lens preference away from the weaver for good. So the
 * SDK backend leaves a 3D request to the weaver until something has asked for
 * 2D, sends every request after that, and re-applies the last one sent to any
 * new SR context. Whoever asks for 2D must ask for the previous state back.
 * Rules + rationale: leia_lens_owner_linux.h, docs/display-mode-switching.md.
 */
bool
leiasr_lnx_request_display_mode(struct leiasr_lnx *lnx, bool enable_3d);

/*!
 * Read the current hardware 2D/3D state (R-D2). Backs the runtime's
 * `get_hardware_3d_state` DP slot.
 */
bool
leiasr_lnx_get_hardware_3d_state(struct leiasr_lnx *lnx, bool *out_is_3d);

/*
 *
 * Multi-screen M0 — per-monitor identity for probe_displays.
 *
 * NOT part of the PROPOSED contract's R-* set: these back the plug-in's
 * per-monitor claims (multi-screen plan M0, SR-P1) and never create an SR
 * context. Creating one just to answer a probe would connect to SRService and
 * spin up the eye tracker for a monitor list query; the plug-in's probe()
 * already brings the context up on a Leia box, so these reuse it when it
 * exists and report "unknown" otherwise (claims then fall back to EDID
 * confidence).
 *
 */

struct leia_lnx_sr_display; // leia_display_claims_linux.h

/*!
 * FPC serial of the device the LIVE SR context drives (srLensGetSerialNumber,
 * slot 48, present on SR 1.38). Never creates a context; never touches the
 * lens preference (a serial read is not an enable/disable, LeiaSR #266).
 * @return false when there is no live context, no lens, or no serial.
 */
bool
leiasr_lnx_peek_fpc_serial(char *out_serial, size_t cap);

/*!
 * New SR API (srEnumerateDisplays, slot 97): every SR display with its
 * identity, FPC confidence and opaque displayId. Queries
 * SrRuntimeCapabilities with SrWeaverRoutingCapabilities +
 * SrDisplayBindingCapabilities chained (logged once per context), then the
 * two-call enumerate; the result is cached until the context changes or SR
 * raises SR_EVENT_TYPE_DISPLAY_TOPOLOGY_CHANGED. Never creates a context.
 * Compiled to "unavailable" when the SDK headers/loader predate the call.
 * @return number of entries written (<= @p cap), or -1 when the API is
 *         unavailable (compiled out, no live context, runtime predates it).
 */
int32_t
leia_lnx_sr_enumerate_displays(struct leia_lnx_sr_display *out, uint32_t cap);

/*!
 * The displayId of the SR display the process-wide context drives (the one
 * the instance, tracker, lens and the display query describe): the first
 * FPC-verified entry of the cached srEnumerateDisplays answer. Never
 * enumerates and never creates a context. 0 = unknown (API compiled out, no
 * enumeration yet, nothing FPC-verified) — callers then fall back to the
 * one-panel rule (leia_screen_linux.h).
 */
uint64_t
leia_lnx_sr_active_display_id(void);

#ifdef __cplusplus
}
#endif
