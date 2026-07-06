// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux weaver-backend seam — the plug-in-side mirror of the
 *         LeiaSR Linux SDK interface contract.
 *
 * Every declaration in this header is shaped 1:1 by
 * `docs/leia-linux-sdk-contract.md` (status: PROPOSED, issue #81) and cites
 * the contract requirement (R-W* / R-T* / R-D*) it realizes. The contract may
 * still shift on ratification — expect a small realignment pass here when it
 * does.
 *
 * Two implementations plug in behind this seam (selected by the CMake cache
 * var `DXR_LEIA_LINUX_WEAVER`):
 *   - `leia_sr_stub.c` (Track A, this repo today): no SR SDK — canned display
 *     info + passthrough SBS blit, so the .so builds/discovers/selftests on
 *     machines and CI with no Leia hardware or SDK.
 *   - TODO(Track B): `leia_sr_linux_sdk.c` wrapping the real LeiaSR Linux SDK
 *     the day it ships, satisfying the same signatures.
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
#include <stdint.h>

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
	 */
	void *x11_window;
	void *x11_connection;

	/*!
	 * Bounded budget for the SR-service connect (R-W1) in seconds. The
	 * Windows plug-in uses ~5 s at DP creation; 0 means a single attempt.
	 */
	double retry_budget_s;
};

/*!
 * Per-frame weave input (contract R-W4): ONE tiled atlas image containing all
 * views side-by-side, with the grid explicit. 2x1 stereo is the baseline; the
 * grid fields keep >2-view panels expressible. Layout at call time is
 * VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL unless the backend documents
 * otherwise (the Track A stub manages its own transitions internally).
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
};

/*!
 * Absolute panel coordinate the lenticular interlacing pattern is computed
 * against (contract R-W7) — decoupled from the viewport so weaving into a
 * sub-rectangle keeps correct lens phase (display-zones, per-window weaving).
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
	uint32_t refresh_mhz; //!< refresh rate in milli-Hz (60000 = 60 Hz)
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
 */
void
leiasr_lnx_set_latency_us(struct leiasr_lnx *lnx, uint64_t latency_us);


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
 * for v1 — Windows parity).
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
 */
bool
leiasr_lnx_request_display_mode(struct leiasr_lnx *lnx, bool enable_3d);

/*!
 * Read the current hardware 2D/3D state (R-D2). Backs the runtime's
 * `get_hardware_3d_state` DP slot.
 */
bool
leiasr_lnx_get_hardware_3d_state(struct leiasr_lnx *lnx, bool *out_is_3d);

#ifdef __cplusplus
}
#endif
