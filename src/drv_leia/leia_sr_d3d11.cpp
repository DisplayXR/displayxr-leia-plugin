// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Leia SR D3D11 weaver wrapper implementation.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_sr_d3d11.h"
#include "leia_sr_api_select.h"
#include "leia_sr_liveness.h"
#include "leia_sr_v2_common.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/weaver/dx11weaver.h>
#include <sr/world/display/display.h>
#include <sr/sense/display/switchablehint.h>
#include <sr/utility/exception.h>

#ifdef DXR_LEIA_HAS_SR_V2
#include <sr/sr_dx11.h>
#include <sr/sr_weaver.h>
#endif

#include <d3d11.h>
#include <d3d11_4.h> // ID3D11Multithread — #144 async weaver create/destroy

#include <windows.h>
#include <sysinfoapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

//! #144 async-creation states (see the struct comment block below).
enum
{
	LEIASR_ASYNC_PENDING = 0,   //!< worker still creating; entry points degrade
	LEIASR_ASYNC_READY = 1,     //!< SDK objects published; normal operation
	LEIASR_ASYNC_CANCELLED = 2, //!< destroyed while pending; worker self-cleans
};

/*!
 * D3D11 SR weaver instance.
 */
struct leiasr_d3d11
{
	// SR SDK objects, v1 (legacy C++). NULL on the v2 path.
	SR::SRContext *context = nullptr;
	SR::IDX11Weaver1 *weaver = nullptr;
	SR::SwitchableLensHint *lens_hint = nullptr;

#ifdef DXR_LEIA_HAS_SR_V2
	// SR SDK objects, v2 (C99). NULL on the v1 path.
	//
	// `weaver_v2 != nullptr` IS the discriminant used at every call site below.
	// The selector is consulted exactly once, in create(); after that "which
	// path am I on?" is a property of the object, not a re-query. That matters
	// because a selector that could be re-evaluated mid-session is a selector
	// that could answer differently mid-session, and a half-v1/half-v2 object
	// is the exact silent-divergence failure this migration exists to remove.
	SrInstance instance_v2 = nullptr;
	SrWeaver weaver_v2 = nullptr;
	SrLens lens_v2 = nullptr;
#endif

	// D3D11 resources (references, not owned)
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *d3d11_context = nullptr;

	// Current input texture info
	ID3D11ShaderResourceView *input_srv = nullptr;
	uint32_t view_width = 0;
	uint32_t view_height = 0;
	DXGI_FORMAT input_format = DXGI_FORMAT_R8G8B8A8_UNORM;

	// Display dimensions in meters (for Kooima FOV calculation)
	float display_width_m = 0.0f;
	float display_height_m = 0.0f;
	bool display_dims_valid = false;

	// Display pixel resolution and screen position (for window metrics)
	uint32_t display_pixel_width = 0;
	uint32_t display_pixel_height = 0;
	int32_t display_screen_left = 0;
	int32_t display_screen_top = 0;
	bool display_pixel_dims_valid = false;

	// Recommended view texture dimensions from SR display
	uint32_t recommended_view_width = 0;
	uint32_t recommended_view_height = 0;
	bool recommended_dims_valid = false;

	// Configuration
	bool srgb_read = false;
	bool srgb_write = false;

	// --- #625 window-drag phase-snap probe ---------------------------------
	// A present-owner (CEF host) that drags its OWN window across processes can't
	// get the SR weaver's WndProc phase-snap, because that subclass installs
	// in-process via SetWindowLongPtr and the real weaver runs here in the
	// service against the host's CROSS-process HWND (SetWindowLongPtr fails
	// cross-process). To still snap, we drive the SDK's REAL SnapToPhase (no
	// reimplementation, ADR-019) through a service-owned hidden probe window
	// bound to its own SR weaver: that weaver's WndProc subclass installs
	// in-process, and SnapToPhase reads the DLL-global slant/pitch/user-distance
	// that the MAIN weaver already populates every frame in weave(). Created
	// lazily on the first snap; the probe weaver never weaves (it only carries
	// the WndProc). All driving happens on the snap-calling (IPC) thread —
	// SetWindowPos/SendMessage dispatch WM_WINDOWPOSCHANGING synchronously to the
	// WndProc only on the owning thread.
	HWND snap_probe_hwnd = nullptr;
	SR::IDX11Weaver1 *snap_probe_weaver = nullptr;
	DWORD snap_probe_thread = 0;    //!< Thread that created/owns snap_probe_hwnd.
	bool snap_probe_failed = false; //!< Lazy-init tried and failed — don't retry.

	// --- Adaptive weave-latency estimation (D3D11), microseconds ------------
	// Same additive motion-to-photon model as the VK arm (leia_sr.cpp):
	//     horizon = N_buffered * frame_interval + T_display
	// pushed via setLatency() each frame. The SDK's own frames-based heuristic
	// (setLatencyInFrames) uses the identical shape with N pinned by the app —
	// the point of the per-frame push is the N_BUFFERED KNOB: under the
	// runtime's late-weave scheduling (DXR_LATE_WEAVE=1) the weave runs ~one
	// refresh before scanout, so LEIA_D3D11_LATENCY_FRAMES=0 aligns the
	// predictor with the measured horizon instead of over-predicting ~2x.
	// Defaults reproduce today's numbers (N=1 + one refresh).
	bool     adaptive_latency_enabled = true;
	float    latency_frames_factor = 1.0f;   // N_buffered (frames in flight)
	uint64_t display_term_us = 16667;        // T_display, constant (~1 panel refresh @60Hz)
	uint64_t latency_min_us = 5000;          // clamp floor
	uint64_t latency_max_us = 60000;         // clamp ceiling
	uint64_t latency_fixed_us = 0;           // >0 => bypass adaptive, set once
	double   latency_ema_alpha = 0.15;       // EMA smoothing for frame interval
	uint64_t prev_weave_ns = 0;              // timestamp of previous weave()
	double   ema_interval_ns = 0.0;          // smoothed weave interval (ns)
	uint64_t last_set_latency_us = 0;        // last value pushed to setLatency()

	// Measured weave→scanout residual from the runtime's timing feedback
	// loop (xrt_display_processor set_frame_timing). When fresh, it
	// REPLACES the heuristic: exact per-path, per-panel-Hz horizon.
	uint64_t measured_r_us = 0;
	uint64_t measured_seen_ns = 0;
	double   measured_ema_us = 0.0;

	// --- #144 async weaver creation/destruction --------------------------
	// SR context + weaver creation blocks for seconds (SR-service retry
	// loops, correction-texture PNG loads from disk, senses start) and the
	// runtime's DP factory runs on the service's critical path holding
	// render_mutex. leiasr_d3d11_create_async() returns a PENDING handle
	// immediately; a detached worker runs the real creation and publishes
	// via async_state (release), which every entry point gates on through
	// w_ready() (acquire) — until then callers see the same "no weaver"
	// degradation the existing NULL checks already implement.
	//
	// Lifecycle contract: the worker is DETACHED. Whoever loses the
	// PENDING→{READY,CANCELLED} CAS race owns the struct:
	//   worker CAS PENDING→READY ok  → caller owns; worker never touches
	//                                  sr again (the CAS is its last access).
	//   destroy CAS PENDING→CANCELLED ok → worker observes it, tears down
	//                                  whatever it created, deletes sr.
	std::atomic<int> async_state{LEIASR_ASYNC_READY}; // sync create default
	std::atomic<int> pending_lens_wish{-1}; //!< -1 none, 0 → 2D, 1 → 3D
	//! runtime#1008: HWND handed to leiasr_d3d11_set_window while the async
	//! create was still PENDING. Applied by the worker at publish, exactly
	//! like @ref pending_lens_wish. 0 = none.
	std::atomic<uintptr_t> pending_hwnd{0};

	// --- #158 SR platform hot reconnect -----------------------------------
	// The SR platform can restart underneath a long-lived process (a LeiaSR
	// installer upgrade while displayxr-service.exe keeps running). The SDK
	// gives no notification and keeps answering getPredictedEyePositions with
	// a SUCCESS code and a FROZEN pair — so the only honest signal is the SR
	// service process's identity (leia_sr_liveness.h). On a change we rebuild
	// the SDK objects IN PLACE: this struct and the owning xrt_display_processor
	// keep their addresses, so nothing above the plug-in has to react.
	//
	// The rebuild reuses the #144 async machinery wholesale — CAS READY→PENDING
	// (which is exactly what every entry point's w_ready() gate already
	// degrades on), run the same worker body, CAS PENDING→READY. The lifecycle
	// contract is unchanged: a destroy racing the reconnect flips PENDING→
	// CANCELLED and the worker frees the struct.

	//! Generation token (leia_sr_liveness_platform_generation) captured at the
	//! last successful weaver create. Written before the publishing release
	//! CAS, so any reader that saw READY sees a valid value. 0 = never known.
	uint64_t platform_generation = 0;
	//! HWND the live weaver is bound to — the create argument, kept current by
	//! w_set_window. The reconnect worker has no caller to get it from.
	std::atomic<uintptr_t> bound_hwnd{0};
	//! Last 2D/3D wish the runtime asked for (-1 none). Sticky, unlike
	//! @ref pending_lens_wish which is consumed at publish: a reconnect must
	//! re-assert 3D on the fresh lens or the panel silently comes back 2D.
	std::atomic<int> last_lens_wish{-1};
	//! Completed in-place reconnects (diagnostics; also the "attempt N" in the log).
	std::atomic<uint64_t> reconnect_count{0};
	//! Backend state last returned by leiasr_d3d11_poll_backend_state.
	std::atomic<uint32_t> last_backend_state{LEIA_SR_BACKEND_OK};
	//! @name One-shot WARN edges for the poll (it runs ~1 Hz forever, so a
	//! plain log would be per-frame-class bloat). Cleared when the condition
	//! clears, so a second occurrence logs again.
	//! @{
	std::atomic<bool> warned_platform_down{false};
	std::atomic<bool> warned_client_skew{false};
	//! @}
};

namespace {

/*!
 * Create and wait for the SR context to become ready.
 */
bool
create_sr_context(double max_time, leiasr_d3d11 &sr)
{
	const double start_time = (double)GetTickCount64() / 1000.0;

	// Create SR context.
	while (sr.context == nullptr) {
		try {
			sr.context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException &e) {
			// Ignore errors because SR may be starting-up.
			(void)e;
		}

		U_LOG_D("Waiting for SR context...");

		// Wait a bit.
		Sleep(100);

		// Abort if we exceed the maximum allowed time.
		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (sr.context == nullptr) {
		U_LOG_E("Failed to create SR context within %.1f seconds", max_time);
		return false;
	}

	// Get display manager (modern API) and wait for display to be ready.
	SR::IDisplayManager *displayManager = nullptr;
	SR::IDisplay *display = nullptr;
	bool display_ready = false;

	try {
		displayManager = SR::GetDisplayManagerInstance(*sr.context);
		if (displayManager == nullptr) {
			U_LOG_E("Failed to get SR DisplayManager instance");
			return false;
		}
	} catch (...) {
		U_LOG_E("Exception getting SR DisplayManager - requires runtime version 1.34.8-RC1 or later");
		return false;
	}

	while (!display_ready) {
		display = displayManager->getPrimaryActiveSRDisplay();
		if (display != nullptr && display->isValid()) {
			SR_recti display_location = display->getLocation();
			int64_t width = display_location.right - display_location.left;
			int64_t height = display_location.bottom - display_location.top;
			if ((width != 0) && (height != 0)) {
				display_ready = true;

				// Cache display dimensions in meters for Kooima FOV calculation
				// Use SR SDK's physical size API (returns cm, convert to meters)
				float raw_width_cm = display->getPhysicalSizeWidth();
				float raw_height_cm = display->getPhysicalSizeHeight();
				sr.display_width_m = raw_width_cm / 100.0f;
				sr.display_height_m = raw_height_cm / 100.0f;
				sr.display_dims_valid = true;

				// Cache display pixel resolution and screen position
				sr.display_pixel_width = static_cast<uint32_t>(width);
				sr.display_pixel_height = static_cast<uint32_t>(height);
				sr.display_screen_left = static_cast<int32_t>(display_location.left);
				sr.display_screen_top = static_cast<int32_t>(display_location.top);
				sr.display_pixel_dims_valid = true;

				// Cache recommended view texture dimensions from SR display
				sr.recommended_view_width = display->getRecommendedViewsTextureWidth();
				sr.recommended_view_height = display->getRecommendedViewsTextureHeight();
				sr.recommended_dims_valid = (sr.recommended_view_width > 0 && sr.recommended_view_height > 0);

				U_LOG_W("SR D3D11 display (modern API): %ldx%ld px, physical %.2fcm x %.2fcm = %.4fm x %.4fm",
				        (long)width, (long)height,
				        raw_width_cm, raw_height_cm,
				        sr.display_width_m, sr.display_height_m);
				U_LOG_W("SR recommended view texture: %ux%u per eye",
				        sr.recommended_view_width, sr.recommended_view_height);

				break;
			}
		}

		U_LOG_D("Waiting for SR display...");

		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (!display_ready) {
		U_LOG_E("SR display not ready within %.1f seconds", max_time);
		return false;
	}

	// Create SwitchableLensHint for 2D/3D mode switching
	try {
		sr.lens_hint = SR::SwitchableLensHint::create(*sr.context);
		U_LOG_W("SR D3D11 SwitchableLensHint created successfully");
	} catch (...) {
		sr.lens_hint = nullptr;
		U_LOG_W("SR D3D11 SwitchableLensHint not available on this display");
	}

	return true;
}

#ifdef DXR_LEIA_HAS_SR_V2
/*!
 * The whole v2 creation sequence: instance, display, weaver, senses, lens.
 *
 * Mirrors the v1 ordering exactly, including the one piece of it that is not
 * obvious — `srInitialize` runs *after* the weaver exists, because
 * initialisation starts the senses and the weaver must be registered before
 * they come up. (v1 has the same constraint and the same comment.)
 */
bool
create_v2(double max_time, void *hwnd, leiasr_d3d11 &sr)
{
	if (!leia_sr_v2_create_instance(max_time, &sr.instance_v2)) {
		return false;
	}

	leia_sr_v2_display_info info{};
	if (!leia_sr_v2_query_display(sr.instance_v2, hwnd, max_time, &info)) {
		srDestroyInstance(sr.instance_v2);
		sr.instance_v2 = nullptr;
		return false;
	}

	sr.display_width_m = info.width_m;
	sr.display_height_m = info.height_m;
	sr.display_dims_valid = true;
	sr.display_pixel_width = info.pixel_width;
	sr.display_pixel_height = info.pixel_height;
	sr.display_screen_left = info.screen_left;
	sr.display_screen_top = info.screen_top;
	sr.display_pixel_dims_valid = true;
	sr.recommended_view_width = info.recommended_view_width;
	sr.recommended_view_height = info.recommended_view_height;
	sr.recommended_dims_valid = info.recommended_valid;

	// Same DPI dance as v1: the SDK reads the HWND's geometry during creation
	// and must see physical pixels. See LeiaInc/LeiaSR@a8a9fb9.
	DPI_AWARENESS_CONTEXT oldDpiCtx = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	SrWeaverCreateInfoDX11 ci{};
	ci.sType = SR_TYPE_WEAVER_CREATE_INFO_DX11;
	ci.pNext = nullptr;
	ci.d3d11Context = sr.d3d11_context;
	ci.window = (SrNativeWindowHandle)hwnd;

	const SrResult wr = srCreateWeaverDX11(sr.instance_v2, &ci, &sr.weaver_v2);

	if (oldDpiCtx != NULL) {
		SetThreadDpiAwarenessContext(oldDpiCtx);
	}

	if (!SR_SUCCEEDED(wr) || sr.weaver_v2 == nullptr) {
		U_LOG_E("srCreateWeaverDX11 failed: %s (%d)", leia_sr_v2_result_str(wr), (int)wr);
		sr.weaver_v2 = nullptr;
		srDestroyInstance(sr.instance_v2);
		sr.instance_v2 = nullptr;
		return false;
	}

	if (!leia_sr_v2_initialize(sr.instance_v2)) {
		srDestroyWeaver(sr.weaver_v2);
		sr.weaver_v2 = nullptr;
		srDestroyInstance(sr.instance_v2);
		sr.instance_v2 = nullptr;
		return false;
	}

	// Default latency, overridden per-frame by the adaptive path below exactly
	// as on v1 (setLatency disables the SDK's frames mode).
	srWeaverSetLatencyInFrames(sr.weaver_v2, 1);

	// Late latching: automatic on this backend. It submits implicitly as it
	// records, so the weaver places its own completion marker inside weave()
	// (D3D11_QUERY_EVENT / glFenceSync) and needs nothing from us — unlike
	// Vulkan, where the compositor owns the submit and must call
	// srWeaverWeaveSubmittedVulkan.
	//
	// Read the EFFECTIVE state back rather than trusting the enable: the
	// unimplemented backends return a hardcoded false, and a live one clears
	// the flag itself on failure. Believing the enable is how a latency
	// predictor stands down in favour of a latch that never runs.
	if (SR_SUCCEEDED(srWeaverEnableLateLatching(sr.weaver_v2, SR_TRUE))) {
		SrBool32 ll = SR_FALSE;
		if (SR_SUCCEEDED(srWeaverIsLateLatchingEnabled(sr.weaver_v2, &ll))) {
			U_LOG_W("SR %s late latching: %s", "D3D11",
			        ll == SR_TRUE ? "ENABLED (effective)" : "declined by the backend");
		}
	}

	leia_sr_v2_create_lens(sr.instance_v2, &sr.lens_v2);

	U_LOG_W("SR D3D11 weaver created via the v2 C API");
	return true;
}
#endif // DXR_LEIA_HAS_SR_V2

/* ------------------------------------------------------------------ *
 * v1/v2 dispatch
 *
 * One helper per weaver operation, so the branch lives in exactly one place
 * per operation instead of being sprinkled through the call sites. The
 * discriminant is always `weaver_v2 != nullptr` — see the struct comment.
 *
 * When v1 is retired these bodies collapse to their v2 half and the call
 * sites do not change at all.
 * ------------------------------------------------------------------ */

void
w_set_latency(leiasr_d3d11 *sr, uint64_t latency_us)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetLatency(sr->weaver_v2, latency_us);
		return;
	}
#endif
	sr->weaver->setLatency(latency_us);
}

void
w_set_latency_in_frames(leiasr_d3d11 *sr, uint64_t frames)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetLatencyInFrames(sr->weaver_v2, frames);
		return;
	}
#endif
	sr->weaver->setLatencyInFrames(frames);
}

void
w_set_input_texture(leiasr_d3d11 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetInputTextureDX11(sr->weaver_v2, sr->input_srv, (int32_t)sr->view_width,
		                            (int32_t)sr->view_height, (int32_t)sr->input_format);
		return;
	}
#endif
	sr->weaver->setInputViewTexture(sr->input_srv, static_cast<int>(sr->view_width),
	                                static_cast<int>(sr->view_height), sr->input_format);
}

void
w_set_srgb(leiasr_d3d11 *sr, bool read_srgb, bool write_srgb)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetShaderSRGBConversion(sr->weaver_v2, read_srgb ? SR_TRUE : SR_FALSE,
		                                write_srgb ? SR_TRUE : SR_FALSE);
		return;
	}
#endif
	sr->weaver->setShaderSRGBConversion(read_srgb, write_srgb);
}

void
w_weave(leiasr_d3d11 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverWeave(sr->weaver_v2);
		return;
	}
#endif
	sr->weaver->weave();
}

/*!
 * Predicted eye positions in millimetres, or false if unavailable.
 *
 * The v1 call throws ~11 first-class exceptions per frame as routine internal
 * control flow (see the caller's comment) and the catch is mandatory. The v2
 * call returns an `SrResult` instead — the same condition, reported sanely —
 * but the try/catch stays on the v1 side because the v1 behaviour has not
 * changed. Units are millimetres on both: `rt_WeaverGetPredictedEyePositions`
 * forwards to the same getter.
 */
bool
w_get_predicted_eyes(leiasr_d3d11 *sr, float left_mm[3], float right_mm[3])
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		SrPoint3f l{};
		SrPoint3f r{};
		if (!SR_SUCCEEDED(srWeaverGetPredictedEyePositions(sr->weaver_v2, &l, &r))) {
			return false;
		}
		left_mm[0] = l.x;
		left_mm[1] = l.y;
		left_mm[2] = l.z;
		right_mm[0] = r.x;
		right_mm[1] = r.y;
		right_mm[2] = r.z;
		return true;
	}
#endif
	try {
		sr->weaver->getPredictedEyePositions(left_mm, right_mm);
	} catch (...) {
		return false;
	}
	return true;
}

/*!
 * Is there a usable weaver, on whichever path we are on?
 *
 * Every public entry point guards on this. Before the v2 path existed the
 * guards read `leiasr->weaver == nullptr` directly, which on v2 is ALWAYS true
 * — so left unchanged they would have turned eye tracking, sRGB configuration
 * and mode switching into silent no-ops returning `false`, on a path whose
 * weaver was working perfectly. Nothing would have crashed; the display would
 * simply have had no eye tracking.
 */
bool
w_ready(const leiasr_d3d11 *sr)
{
	// #144: while an async create is pending, the SDK-object fields are
	// being written by the worker — the acquire load here is what makes
	// reading them safe after it flips to READY (release CAS in the worker).
	if (sr->async_state.load(std::memory_order_acquire) != LEIASR_ASYNC_READY) {
		return false;
	}
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		return true;
	}
#endif
	return sr->weaver != nullptr;
}

//! Is a lens present at all? False means 2D/3D switching is unavailable.
bool
lens_present(leiasr_d3d11 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		return sr->lens_v2 != nullptr;
	}
#endif
	return sr->lens_hint != nullptr;
}

void
lens_set(leiasr_d3d11 *sr, bool enable)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		if (sr->lens_v2 != nullptr) {
			enable ? srLensEnable(sr->lens_v2) : srLensDisable(sr->lens_v2);
		}
		return;
	}
#endif
	if (sr->lens_hint != nullptr) {
		enable ? sr->lens_hint->enable() : sr->lens_hint->disable();
	}
}

bool
lens_is_enabled(leiasr_d3d11 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		SrBool32 enabled = SR_FALSE;
		if (sr->lens_v2 == nullptr || !SR_SUCCEEDED(srLensIsEnabled(sr->lens_v2, &enabled))) {
			return false;
		}
		return enabled == SR_TRUE;
	}
#endif
	return sr->lens_hint != nullptr && sr->lens_hint->isEnabled();
}

/*!
 * runtime#1008: re-point a LIVE weaver at another window, on whichever API
 * family this instance is on. Both families expose it (v1
 * IWeaverBase1::setWindowHandle, v2 srWeaverSetWindowHandle) — the SDK
 * re-reads the window's geometry and re-subclasses its WndProc; it does not
 * touch the lens. Caller guarantees w_ready().
 *
 * Returns false on any SDK refusal so the runtime can fall back to
 * destroy+recreate. Never throws (the runtime is C).
 */
bool
w_set_window(leiasr_d3d11 *sr, HWND hwnd)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		const SrResult r = srWeaverSetWindowHandle(sr->weaver_v2, (SrNativeWindowHandle)hwnd);
		if (!SR_SUCCEEDED(r)) {
			U_LOG_W("SR v2 srWeaverSetWindowHandle failed: %s (%d)", leia_sr_v2_result_str(r), (int)r);
			return false;
		}
		// #158: a reconnect recreates the weaver from scratch and has no
		// caller to take the window from — keep the record current here.
		sr->bound_hwnd.store(reinterpret_cast<uintptr_t>(hwnd), std::memory_order_relaxed);
		return true;
	}
#endif
	if (sr->weaver == nullptr) {
		return false;
	}
	try {
		// Same DPI dance as creation: the SDK re-reads the HWND's geometry
		// here and must see physical pixels (see create_weaver_attempt).
		DPI_AWARENESS_CONTEXT oldDpiCtx =
		    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		sr->weaver->setWindowHandle(hwnd);
		if (oldDpiCtx != NULL) {
			SetThreadDpiAwarenessContext(oldDpiCtx);
		}
	} catch (...) {
		U_LOG_E("SR D3D11 setWindowHandle(%p) threw — re-bind refused", (void *)hwnd);
		return false;
	}
	// #158: see the v2 branch — the reconnect worker recreates from this.
	sr->bound_hwnd.store(reinterpret_cast<uintptr_t>(hwnd), std::memory_order_relaxed);
	return true;
}

/*!
 * One full weaver-creation attempt (both API arms + the shared latency
 * config). Factored out of leiasr_d3d11_create so the #144 async worker can
 * run the identical body off the critical path. On failure every partially
 * created SDK object is torn down and nulled, so the struct is reusable for
 * a retry.
 */
bool
create_weaver_attempt(leiasr_d3d11 *sr, double max_time, void *hwnd)
{
	// #169: the single choke point every create runs through — the sync
	// create, the #144 async worker, and the #158 reconnect worker. If the SR
	// platform was upgraded under this process the mapped client DLLs are
	// stale and the vendor runtime access-violates on the create, so refuse
	// before calling into it. Never fires in a fresh process (nothing SR is
	// mapped yet, which the probe reports as "undeterminable" = allowed).
	if (!leia_sr_liveness_weaver_create_allowed()) {
		return false;
	}

#ifdef DXR_LEIA_HAS_SR_V2
	if (leia_sr_api_selected() == LEIA_SR_API_V2) {
		// Deliberately NOT falling back to v1 on failure.
		//
		// The selector already established that the v2 runtime loads and speaks
		// our API version; a failure past that point is a real fault (no SR
		// display, no device, a lost GPU) that v1 would hit for the same reason.
		// Silently retrying on the other path would convert a clear error into a
		// working-but-unexplained session on an API family nobody chose, and the
		// log would say "using v2" while the process ran v1. That is precisely
		// the divergence the selector exists to prevent.
		if (!create_v2(max_time, hwnd, *sr)) {
			U_LOG_E("SR v2 weaver creation failed - not falling back to v1 "
			        "(set DXR_LEIA_SR_API=v1 to force it)");
			return false;
		}
	} else
#endif
	{
		// Create SR context
		if (!create_sr_context(max_time, *sr)) {
			// create_sr_context can fail AFTER the context exists (display
			// never became ready) — clean the partial so a retry starts fresh.
			if (sr->context != nullptr) {
				SR::SRContext::deleteSRContext(sr->context);
				sr->context = nullptr;
			}
			return false;
		}

		// Create D3D11 weaver (SR SDK installs its WndProc via SetWindowLongPtr).
		// Set DPI awareness so the SDK sees physical pixels when it queries the
		// HWND. See LeiaInc/LeiaSR@a8a9fb9 for the pattern.
		DPI_AWARENESS_CONTEXT oldDpiCtx =
		    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		WeaverErrorCode result = SR::CreateDX11Weaver(sr->context,
		                                               sr->d3d11_context,
		                                               static_cast<HWND>(hwnd),
		                                               &sr->weaver);
		if (oldDpiCtx != NULL) {
			SetThreadDpiAwarenessContext(oldDpiCtx);
		}
		if (result != WeaverErrorCode::WeaverSuccess) {
			U_LOG_E("Failed to create SR D3D11 weaver: %d", (int)result);
			SR::SRContext::deleteSRContext(sr->context);
			sr->context = nullptr;
			sr->weaver = nullptr;
			return false;
		}

		// Initialize the context AFTER the weaver has been registered.
		// initialize() starts the senses.
		sr->context->initialize();

		// Set default latency (1 frame). Kept as the fallback for
		// LEIA_D3D11_ADAPTIVE_LATENCY=0; the adaptive per-frame setLatency()
		// below overrides it (setLatency disables the SDK's frames mode).
		w_set_latency_in_frames(sr, 1);
	}

	// EVERYTHING BELOW IS SHARED BY BOTH PATHS and must stay that way. The
	// adaptive-latency configuration is runtime policy, not SDK plumbing: an
	// early return from the v2 branch above would silently skip it, leaving
	// v2 on the SDK default while v1 ran adaptive — which would present as
	// "v2 has worse latency" rather than as the configuration bug it is.

	// Adaptive-latency knobs, mirroring the VK arm's LEIA_VK_* set:
	//   LEIA_D3D11_ADAPTIVE_LATENCY=0   disable (keep SDK frames-based default)
	//   LEIA_D3D11_LATENCY_FIXED_US=N   bypass adaptive, pin setLatency(N) once
	//   LEIA_D3D11_LATENCY_FRAMES=f     N_buffered (default 1.0; 0 under late-weave)
	//   LEIA_D3D11_PANEL_HZ=hz          panel refresh for display term (default 60)
	//   LEIA_D3D11_LATENCY_DISPLAY_US=N override display term outright
	//   LEIA_D3D11_LATENCY_MIN_US/_MAX_US/_EMA_ALPHA  clamps + smoothing
	{
		auto getf = [](const char *n, float def) -> float {
			const char *v = std::getenv(n);
			return (v == nullptr || v[0] == '\0') ? def : (float)atof(v);
		};
		auto getu = [](const char *n, uint64_t def) -> uint64_t {
			const char *v = std::getenv(n);
			if (v == nullptr || v[0] == '\0') return def;
			long long x = atoll(v);
			return x < 0 ? def : (uint64_t)x;
		};

		const char *en = std::getenv("LEIA_D3D11_ADAPTIVE_LATENCY");
		sr->adaptive_latency_enabled = !(en != nullptr && en[0] == '0');
		// Heuristic fallback keeps the classic 1-frame default: paced paths now
		// get their horizon from the runtime's MEASURED feed (set_frame_timing),
		// so the heuristic only serves never-measured paths (e.g. VK DComp
		// transparent) — where pre-late-weave pacing still applies and N=1 is
		// the correct shape. Replaces the interim DXR_LATE_WEAVE env coupling.
		sr->latency_frames_factor = getf("LEIA_D3D11_LATENCY_FRAMES", 1.0f);
		sr->latency_min_us = getu("LEIA_D3D11_LATENCY_MIN_US", 5000);
		sr->latency_max_us = getu("LEIA_D3D11_LATENCY_MAX_US", 60000);
		sr->latency_fixed_us = getu("LEIA_D3D11_LATENCY_FIXED_US", 0);
		float a = getf("LEIA_D3D11_LATENCY_EMA_ALPHA", 0.15f);
		sr->latency_ema_alpha = a < 0.01f ? 0.01f : (a > 1.0f ? 1.0f : a);

		float panel_hz = getf("LEIA_D3D11_PANEL_HZ", 60.0f);
		uint64_t disp_default = (panel_hz > 1.0f) ? (uint64_t)(1.0e6 / panel_hz + 0.5) : 16667;
		sr->display_term_us = getu("LEIA_D3D11_LATENCY_DISPLAY_US", disp_default);

		if (sr->latency_fixed_us > 0) {
			w_set_latency(sr, sr->latency_fixed_us);
			sr->last_set_latency_us = sr->latency_fixed_us;
			sr->adaptive_latency_enabled = false;
			U_LOG_W("Leia D3D11 weave latency: FIXED %llu us (adaptive disabled)",
			        (unsigned long long)sr->latency_fixed_us);
		} else if (sr->adaptive_latency_enabled) {
			U_LOG_W("Leia D3D11 weave latency: ADAPTIVE (horizon = %.2f x frame_interval + %llu us display, clamp %llu..%llu us, alpha %.2f)",
			        (double)sr->latency_frames_factor,
			        (unsigned long long)sr->display_term_us,
			        (unsigned long long)sr->latency_min_us,
			        (unsigned long long)sr->latency_max_us, sr->latency_ema_alpha);
		} else {
			U_LOG_W("Leia D3D11 weave latency: SDK frames-based default (adaptive off)");
		}
	}

	return true;
}

/*!
 * Tear down every SDK object on the struct (both arms + the #625 snap
 * probe), leaving the struct itself alive. Shared by leiasr_d3d11_destroy
 * and the #144 async worker's cancelled path.
 */
void
destroy_sdk_objects(leiasr_d3d11 *sr)
{
	// #625: tear down the phase-snap probe (weaver restores the probe window's
	// WndProc) before the SRContext goes away.
	if (sr->snap_probe_weaver != nullptr) {
		sr->snap_probe_weaver->destroy();
		sr->snap_probe_weaver = nullptr;
	}
	if (sr->snap_probe_hwnd != nullptr) {
		DestroyWindow(sr->snap_probe_hwnd);
		sr->snap_probe_hwnd = nullptr;
	}

#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		// The v2 lens must be destroyed here; the v1 one must NOT. That is not
		// because ownership inverted — it is because v2 adds a second, smaller
		// object that v1 has no equivalent of:
		//
		//   v1 SwitchableLensHint : owned by the SRContext. Deleting it is a
		//                           double-free, which is why the v1 branch
		//                           below only nulls the pointer.
		//   v2 SrLens             : a thin handle WRAPPER we own, over that same
		//                           context-owned hint. srDestroyLens frees only
		//                           the wrapper and deliberately does not touch
		//                           the hint underneath.
		//
		// So there is no double-free hazard in calling it, and skipping it leaks
		// the wrapper (one small struct), not the lens. Confirmed against the SR
		// runtime source — sr_lens.h documents neither, so do not go looking for
		// it there.
		if (sr->lens_v2 != nullptr) {
			srDestroyLens(sr->lens_v2);
			sr->lens_v2 = nullptr;
		}
		srDestroyWeaver(sr->weaver_v2);
		sr->weaver_v2 = nullptr;

		if (sr->instance_v2 != nullptr) {
			srDestroyInstance(sr->instance_v2);
			sr->instance_v2 = nullptr;
		}
		return;
	}
#endif

	// SwitchableLensHint is managed by SRContext — do NOT delete it manually.
	// SRContext::~SRContext() calls deleteAllSenses() which cleans it up.
	// Manually deleting it causes a crash (double-free).
	sr->lens_hint = nullptr;

	// Destroy weaver (SR SDK restores the app's original WndProc)
	if (sr->weaver != nullptr) {
		sr->weaver->destroy();
		sr->weaver = nullptr;
	}

	// Destroy context
	if (sr->context != nullptr) {
		SR::SRContext::deleteSRContext(sr->context);
		sr->context = nullptr;
	}
}

/*!
 * #144: worker/reaper threads issue D3D11 calls concurrently with the
 * render thread on the SAME immediate context — v1 weaver construction does
 * one UpdateSubresource, and both arms' SDK destructors call Flush(). The
 * documented D3D11 mechanism for that is ID3D11Multithread protection
 * (a driver-level critical section per context call, negligible next to a
 * 16 ms frame). Enabled once, left on.
 */
void
enable_context_multithread_protection(ID3D11DeviceContext *ctx)
{
	ID3D11Multithread *mt = nullptr;
	if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11Multithread), (void **)&mt)) && mt != nullptr) {
		BOOL prev = mt->SetMultithreadProtected(TRUE);
		mt->Release();
		U_LOG_W("Leia D3D11 async weaver: immediate-context multithread protection ON (was %d)", (int)prev);
	} else {
		U_LOG_W("Leia D3D11 async weaver: ID3D11Multithread unavailable — "
		        "async create/destroy may race the render thread's context use");
	}
}

/*!
 * The arm's v2 `SrInstance` as an opaque pointer for the leia_sr_liveness_*
 * entry points, or NULL on the v1 path (and on a build without the v2 SDK,
 * where the member does not exist at all).
 *
 * Wrapping it keeps every call site identical on both builds — and every one of
 * them MUST pass this, stamp sites included: mixing an SDK-id stamp with an
 * SCM-derived poll would read as a restart that never happened.
 */
void *
v2_instance_of(const leiasr_d3d11 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	return sr != nullptr ? static_cast<void *>(sr->instance_v2) : nullptr;
#else
	(void)sr;
	return nullptr;
#endif
}

/*!
 * Shared body of the #144 async CREATE worker and the #158 in-place RECONNECT
 * worker. Runs on a detached thread and owns the PENDING→READY publish.
 *
 * The two are one function on purpose: the ownership contract is the delicate
 * part, and a second copy of it is a second place for it to rot. That contract
 * (from the struct's async comment block) is unchanged — the thread is
 * DETACHED, so whoever loses the PENDING→{READY,CANCELLED} CAS owns the
 * struct. Every checkpoint below re-reads async_state; on CANCELLED the
 * destroy caller has ALREADY nulled its own pointer and is expecting us to
 * free, so we tear down and delete.
 *
 * @param sr        The instance, in PENDING state.
 * @param max_time  Per-attempt SDK wait budget, in seconds.
 * @param reconnect Accounting/logging only — true when re-driven by
 *                  leiasr_d3d11_poll_backend_state after an SR platform
 *                  generation change.
 * @param start_ms  GetTickCount64() at the trigger, for the reconnect log.
 */
void
async_create_worker_body(leiasr_d3d11 *sr, double max_time, bool reconnect, uint64_t start_ms)
{
	uint32_t attempt = 0;
	for (;;) {
		if (sr->async_state.load(std::memory_order_acquire) == LEIASR_ASYNC_CANCELLED) {
			// Idempotent, and needed on every path: on the first create
			// iteration there is nothing to tear down, on a retry
			// create_weaver_attempt already cleaned its own partials, and
			// after a reconnect teardown everything is already null.
			destroy_sdk_objects(sr);
			delete sr;
			return;
		}
		attempt++;

		void *hwnd = reinterpret_cast<void *>(sr->bound_hwnd.load(std::memory_order_relaxed));

		// The SDK throws as routine control flow and this is a detached
		// thread — an escaping exception is std::terminate, i.e. the whole
		// service dies. Treat a throw as a failed attempt and retry.
		bool created = false;
		try {
			created = create_weaver_attempt(sr, max_time, hwnd);
		} catch (...) {
			U_LOG_E("Leia D3D11 weaver create attempt %u threw — treating as failure", attempt);
			try {
				destroy_sdk_objects(sr);
			} catch (...) {
			}
			created = false;
		}

		if (created) {
			// runtime#1008: apply a window re-bind recorded while we
			// were creating (the weaver was built against the ORIGINAL
			// hwnd, so without this the DP would silently weave against
			// a window the runtime no longer presents to). Same
			// exactly-once exchange discipline as the lens wish below.
			uintptr_t want = sr->pending_hwnd.exchange(0, std::memory_order_acq_rel);
			if (want != 0) {
				if (w_set_window(sr, reinterpret_cast<HWND>(want))) {
					U_LOG_W("Leia D3D11 async weaver: applied recorded window "
					        "re-bind (hwnd=%p)",
					        (void *)want);
				}
			}
			// Apply any lens wish recorded while we were creating —
			// BEFORE the READY CAS, which must be the last access to
			// sr this thread makes on the success path. A wish that
			// lands in the tiny window between this exchange and the
			// CAS is self-applied by the recorder (it re-checks state
			// after storing), or re-driven by the next mode request.
			// #158 seeds this from last_lens_wish before a reconnect, so
			// a rebuilt weaver comes back in the mode the runtime asked for.
			int wish = sr->pending_lens_wish.exchange(-1, std::memory_order_acq_rel);
			if (wish >= 0 && lens_present(sr)) {
				try {
					lens_set(sr, wish == 1);
					U_LOG_W("Leia D3D11 async weaver: applied recorded lens wish (%s)",
					        wish == 1 ? "3D" : "2D");
				} catch (...) {
				}
			}

			// #158: stamp the incarnation we just connected to BEFORE the
			// release CAS, so any reader that observes READY (acquire) also
			// observes a valid generation. A zero here (SCM unreachable)
			// disables detection rather than faking a restart — see
			// leiasr_d3d11_poll_backend_state.
			const uint64_t prev_gen = sr->platform_generation;
			sr->platform_generation = leia_sr_liveness_platform_generation_ex(v2_instance_of(sr));

			int expected = LEIASR_ASYNC_PENDING;
			if (sr->async_state.compare_exchange_strong(expected, LEIASR_ASYNC_READY,
			                                            std::memory_order_acq_rel)) {
				if (reconnect) {
					sr->reconnect_count.fetch_add(1, std::memory_order_relaxed);
					U_LOG_W("SR weaver reconnected (generation %llu → %llu), "
					        "%llu ms after trigger, attempt %u",
					        (unsigned long long)prev_gen,
					        (unsigned long long)sr->platform_generation,
					        (unsigned long long)(GetTickCount64() - start_ms), attempt);
				} else {
					U_LOG_W("Leia D3D11 weaver READY (async create, attempt %u)", attempt);
				}
				return;
			}
			// Destroyed while we were creating — tear down and free.
			destroy_sdk_objects(sr);
			delete sr;
			return;
		}

		U_LOG_W("Leia D3D11 %s weaver: create attempt %u failed — retrying in 5 s",
		        reconnect ? "reconnect" : "async", attempt);
		for (int i = 0; i < 50; i++) {
			if (sr->async_state.load(std::memory_order_acquire) == LEIASR_ASYNC_CANCELLED) {
				destroy_sdk_objects(sr);
				delete sr;
				return;
			}
			Sleep(100);
		}
	}
}

/*!
 * #158: rebuild the SR SDK objects IN PLACE after the platform restarted.
 *
 * "In place" is the whole point — the @ref leiasr_d3d11 pointer and the
 * owning xrt_display_processor keep their addresses, so the runtime, the
 * compositor and every IPC client are untouched. The alternative (tearing the
 * DP down and asking the runtime to recreate it) is what a service restart
 * already does, and is exactly the outage this issue is about.
 *
 * Entered with async_state already CAS'd READY→PENDING by the poller, so every
 * public entry point is degrading through w_ready() before we touch anything.
 */
void
reconnect_worker_body(leiasr_d3d11 *sr, double max_time)
{
	const uint64_t start_ms = GetTickCount64();

	// A reader that passed w_ready() microseconds before the poller's CAS can
	// still be inside an SDK call. This is the same exposure the #144
	// async-destroy reaper already accepts, and the runtime additionally
	// serialises eye-pos reads against DP lifetime under its render mutex.
	// 100 ms is far longer than any single weaver call.
	Sleep(100);

	// We are about to run SDK teardown + construction off-thread against the
	// shared immediate context, which is what create_async enables this for.
	// Doing it here too is not redundant: a weaver created through the SYNC
	// path (DXR_LEIA_ASYNC_WEAVER=0) never went through create_async, so
	// without this a reconnect on that path would be the one unprotected
	// off-thread context user. Idempotent.
	enable_context_multithread_protection(sr->d3d11_context);

	// Re-assert the mode the runtime last asked for. pending_lens_wish is
	// CONSUMED at publish so it is empty by now; last_lens_wish is the sticky
	// record. Seed it before teardown so a wish arriving mid-rebuild (which
	// writes pending_lens_wish itself) simply wins over this one.
	const int wish = sr->last_lens_wish.load(std::memory_order_acquire);
	if (wish >= 0) {
		sr->pending_lens_wish.store(wish, std::memory_order_release);
	}

	try {
		destroy_sdk_objects(sr);
	} catch (...) {
		U_LOG_E("Leia D3D11 reconnect: SDK teardown threw — continuing into a fresh create");
	}

	// #625: the snap probe is gone with the old SRContext, and a previous
	// lazy-init failure was a property of THAT context — give the fresh one its
	// own chance rather than inheriting a permanent no-snap verdict. Safe to
	// write here: every snap caller is parked on w_ready() while we are PENDING.
	sr->snap_probe_failed = false;

	async_create_worker_body(sr, max_time, /*reconnect=*/true, start_ms);
}

/*!
 * Arm an in-place reconnect: park every entry point on the w_ready() degrade
 * path and spawn exactly one worker.
 *
 * Winning the READY→PENDING CAS both parks the arm and claims the right to
 * spawn, so the WARN is inherently one-shot per armed reconnect. Factored out
 * because there are now TWO detectors that can arm one — the SDK's connection
 * state and the generation token — and this ownership contract must not exist
 * in two copies.
 *
 * @param reason Logged verbatim, so the line names the detector that fired.
 * @return true when THIS call won the claim.
 */
bool
arm_reconnect(leiasr_d3d11 *sr, const char *reason)
{
	// #169 backstop. The poll already refuses to reach here across a version
	// skew, but a reconnect TEARS THE WEAVER DOWN before it rebuilds, so a
	// future detector arming one on a skewed process would trade an untracked
	// picture for no picture at all (create_weaver_attempt would then refuse
	// the rebuild). Cheap to make that structurally impossible.
	if (!leia_sr_liveness_weaver_create_allowed()) {
		return false;
	}

	int expected = LEIASR_ASYNC_READY;
	if (!sr->async_state.compare_exchange_strong(expected, LEIASR_ASYNC_PENDING, std::memory_order_acq_rel)) {
		return false;
	}

	const uint64_t n = sr->reconnect_count.load(std::memory_order_relaxed) + 1;
	U_LOG_W("%s — reconnecting weaver in place, attempt %llu", reason, (unsigned long long)n);
	// Detached, same ownership contract as the create worker.
	std::thread([sr]() { reconnect_worker_body(sr, 5.0); }).detach();
	return true;
}

} // namespace

extern "C" {

xrt_result_t
leiasr_d3d11_create(double max_time,
                    void *d3d11_device,
                    void *d3d11_context,
                    void *hwnd,
                    uint32_t view_width,
                    uint32_t view_height,
                    struct leiasr_d3d11 **out)
{
	if (d3d11_device == nullptr || d3d11_context == nullptr) {
		U_LOG_E("D3D11 device or context is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	leiasr_d3d11 *sr = new leiasr_d3d11;
	sr->device = static_cast<ID3D11Device *>(d3d11_device);
	sr->d3d11_context = static_cast<ID3D11DeviceContext *>(d3d11_context);
	sr->view_width = view_width;
	sr->view_height = view_height;
	// #158: the reconnect worker recreates from this, not from a caller.
	sr->bound_hwnd.store(reinterpret_cast<uintptr_t>(hwnd), std::memory_order_relaxed);

	if (!create_weaver_attempt(sr, max_time, hwnd)) {
		delete sr;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	// #158: stamp the SR platform incarnation we connected to.
	sr->platform_generation = leia_sr_liveness_platform_generation_ex(v2_instance_of(sr));
	// async_state already defaults to LEIASR_ASYNC_READY on the sync path.

	*out = sr;

	U_LOG_I("Created D3D11 SR weaver for HWND %p, view size %ux%u", hwnd, view_width, view_height);

	return XRT_SUCCESS;
}

xrt_result_t
leiasr_d3d11_create_async(double max_time,
                          void *d3d11_device,
                          void *d3d11_context,
                          void *hwnd,
                          uint32_t view_width,
                          uint32_t view_height,
                          struct leiasr_d3d11 **out)
{
	if (d3d11_device == nullptr || d3d11_context == nullptr) {
		U_LOG_E("D3D11 device or context is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// #169: create_weaver_attempt refuses this too, but the worker cannot fail
	// the CALLER — this entry point would already have answered XRT_SUCCESS,
	// and the runtime would have published the new DP and retired the old
	// (still-weaving) one before the worker discovered the skew. Fail
	// synchronously so the runtime keeps the DP it has.
	if (!leia_sr_liveness_weaver_create_allowed()) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	leiasr_d3d11 *sr = new leiasr_d3d11;
	sr->device = static_cast<ID3D11Device *>(d3d11_device);
	sr->d3d11_context = static_cast<ID3D11DeviceContext *>(d3d11_context);
	sr->view_width = view_width;
	sr->view_height = view_height;
	// #158: the reconnect worker recreates from this, not from a caller.
	sr->bound_hwnd.store(reinterpret_cast<uintptr_t>(hwnd), std::memory_order_relaxed);
	sr->async_state.store(LEIASR_ASYNC_PENDING, std::memory_order_relaxed);

	// The worker (and the async destroy reaper) will touch the shared
	// immediate context from off-thread — see the helper's comment.
	enable_context_multithread_protection(sr->d3d11_context);

	// Detached by design: whoever loses the PENDING→{READY,CANCELLED} CAS
	// owns/frees the struct (see the struct's async comment block). The body
	// is shared with the #158 in-place reconnect worker — hwnd comes off the
	// struct (bound_hwnd), which set_window keeps current.
	std::thread([sr, max_time]() {
		async_create_worker_body(sr, max_time, /*reconnect=*/false, /*start_ms=*/0);
	}).detach();

	*out = sr;

	U_LOG_W("Leia D3D11 weaver creation started ASYNC for HWND %p — DP degrades to flat blit until ready", hwnd);

	return XRT_SUCCESS;
}

void
leiasr_d3d11_destroy(struct leiasr_d3d11 **leiasr_ptr)
{
	if (leiasr_ptr == nullptr || *leiasr_ptr == nullptr) {
		return;
	}

	leiasr_d3d11 *sr = *leiasr_ptr;
	*leiasr_ptr = nullptr;

	// #144: still creating? Flag CANCELLED and hand ownership to the worker —
	// it observes the flag, tears down whatever it created, and frees sr.
	int expected = LEIASR_ASYNC_PENDING;
	if (sr->async_state.compare_exchange_strong(expected, LEIASR_ASYNC_CANCELLED,
	                                            std::memory_order_acq_rel)) {
		U_LOG_W("Leia D3D11 weaver destroy while async create pending — worker will clean up");
		return;
	}

	destroy_sdk_objects(sr);
	delete sr;

	U_LOG_I("Destroyed D3D11 SR weaver");
}

void
leiasr_d3d11_destroy_async(struct leiasr_d3d11 **leiasr_ptr)
{
	if (leiasr_ptr == nullptr || *leiasr_ptr == nullptr) {
		return;
	}

	leiasr_d3d11 *sr = *leiasr_ptr;
	*leiasr_ptr = nullptr;

	// Still creating? Same hand-off as the sync destroy.
	int expected = LEIASR_ASYNC_PENDING;
	if (sr->async_state.compare_exchange_strong(expected, LEIASR_ASYNC_CANCELLED,
	                                            std::memory_order_acq_rel)) {
		U_LOG_W("Leia D3D11 weaver destroy while async create pending — worker will clean up");
		return;
	}

	// #144: SDK teardown blocks too (weaver->destroy / SRContext teardown /
	// a context Flush in both arms' destructors) and the runtime calls the
	// DP destroy under render_mutex on close/deactivate. Reap detached; the
	// immediate context already has multithread protection on (create_async).
	std::thread([sr]() {
		leiasr_d3d11 *victim = sr;
		destroy_sdk_objects(victim);
		delete victim;
		U_LOG_W("Leia D3D11 weaver destroyed (async reaper)");
	}).detach();
}

void
leiasr_d3d11_set_input_texture(struct leiasr_d3d11 *leiasr,
                               void *stereo_srv,
                               uint32_t view_width,
                               uint32_t view_height,
                               uint32_t format)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return;
	}

	// Log dimension changes (first time or when dimensions change)
	static uint32_t last_logged_width = 0, last_logged_height = 0;
	if (view_width != last_logged_width || view_height != last_logged_height) {
		U_LOG_I("SR weaver setInputViewTexture: view=%ux%u (expects side-by-side stereo SRV)",
		        view_width, view_height);
		last_logged_width = view_width;
		last_logged_height = view_height;
	}

	leiasr->input_srv = static_cast<ID3D11ShaderResourceView *>(stereo_srv);
	leiasr->view_width = view_width;
	leiasr->view_height = view_height;
	leiasr->input_format = static_cast<DXGI_FORMAT>(format);

	// Configure the weaver with the input texture
	// NOTE: view_width is single-eye width; SR SDK handles the side-by-side stereo layout internally
	w_set_input_texture(leiasr);
}

void
leiasr_d3d11_set_frame_timing(struct leiasr_d3d11 *leiasr,
                              uint64_t weave_to_scanout_ns,
                              uint64_t frame_period_ns)
{
	if (leiasr == nullptr) {
		return;
	}
	// #144: while the async worker is creating, it also writes the latency
	// config fields — skip this push (it re-arrives every frame anyway).
	if (leiasr->async_state.load(std::memory_order_acquire) == LEIASR_ASYNC_PENDING) {
		return;
	}
	leiasr->measured_r_us = weave_to_scanout_ns / 1000;
	if (weave_to_scanout_ns > 0) {
		leiasr->measured_seen_ns = os_monotonic_get_ns();
	}
	// The panel period also upgrades the heuristic fallback's display term
	// (previously a hard-coded 60 Hz constant).
	if (frame_period_ns > 0) {
		leiasr->display_term_us = frame_period_ns / 1000;
	}
}

void
leiasr_d3d11_weave(struct leiasr_d3d11 *leiasr)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		U_LOG_W("leiasr_d3d11_weave called with null instance or weaver");
		return;
	}

	// Adaptive weave latency: estimate the motion-to-photon horizon from the
	// achieved weave() interval and feed it to the predictor via setLatency()
	// BEFORE weave() so this frame's eye prediction uses it. Same model and
	// deadband as the VK arm (leia_sr.cpp).
	if (leiasr->adaptive_latency_enabled && leiasr->latency_fixed_us == 0) {
		const uint64_t now_ns = os_monotonic_get_ns();
		if (leiasr->prev_weave_ns != 0) {
			const uint64_t dt_ns = now_ns - leiasr->prev_weave_ns;
			// Ignore hitches / first-frame gaps (>250 ms).
			if (dt_ns < 250ULL * 1000 * 1000) {
				if (leiasr->ema_interval_ns <= 0.0) {
					leiasr->ema_interval_ns = (double)dt_ns;
				} else {
					const double a = leiasr->latency_ema_alpha;
					leiasr->ema_interval_ns =
					    a * (double)dt_ns + (1.0 - a) * leiasr->ema_interval_ns;
				}
			}
		}
		leiasr->prev_weave_ns = now_ns;

		// Prefer the runtime's MEASURED weave→scanout residual (timing
		// feedback loop) when fresh; heuristic only as fallback.
		const bool measured_fresh = leiasr->measured_r_us > 0 &&
		                            (now_ns - leiasr->measured_seen_ns) < 250ULL * 1000 * 1000;
		if (measured_fresh || leiasr->ema_interval_ns > 0.0) {
			double horizon_us;
			if (measured_fresh) {
				// One-shot lifecycle log: the horizon source flipped from
				// heuristic to runtime-measured (set_frame_timing loop).
				if (leiasr->measured_ema_us <= 0.0) {
					U_LOG_W("Leia D3D11 weave latency: MEASURED horizon engaged (%llu us, display term %llu us)",
					        (unsigned long long)leiasr->measured_r_us,
					        (unsigned long long)leiasr->display_term_us);
				}
				const double a = leiasr->latency_ema_alpha;
				leiasr->measured_ema_us =
				    (leiasr->measured_ema_us <= 0.0)
				        ? (double)leiasr->measured_r_us
				        : a * (double)leiasr->measured_r_us + (1.0 - a) * leiasr->measured_ema_us;
				horizon_us = leiasr->measured_ema_us;
			} else {
				horizon_us = (double)leiasr->latency_frames_factor *
				                 leiasr->ema_interval_ns / 1000.0 +
				             (double)leiasr->display_term_us;
			}
			if (horizon_us < (double)leiasr->latency_min_us)
				horizon_us = (double)leiasr->latency_min_us;
			if (horizon_us > (double)leiasr->latency_max_us)
				horizon_us = (double)leiasr->latency_max_us;
			const uint64_t latency_us = (uint64_t)(horizon_us + 0.5);

			// Deadband: only re-push on a meaningful change (>=250 us).
			const uint64_t prev = leiasr->last_set_latency_us;
			const uint64_t diff = latency_us > prev ? latency_us - prev : prev - latency_us;
			if (prev == 0 || diff >= 250) {
				w_set_latency(leiasr, latency_us);
				if (prev == 0 || diff >= 2000) {
					U_LOG_I("Leia D3D11 adaptive latency: %llu us (%.2f ms/frame ~ %.0f fps; %.2f x iv + %llu us disp)",
					        (unsigned long long)latency_us,
					        leiasr->ema_interval_ns / 1e6,
					        1e9 / leiasr->ema_interval_ns,
					        (double)leiasr->latency_frames_factor,
					        (unsigned long long)leiasr->display_term_us);
				}
				leiasr->last_set_latency_us = latency_us;
			}
		}
	}

	// The weaver writes to the currently bound render target.
	// Make sure OMSetRenderTargets and RSSetViewports have been called.
	// Set DPI awareness so any internal GetClientRect returns physical pixels.
	DPI_AWARENESS_CONTEXT oldDpiCtx =
	    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	w_weave(leiasr);
	if (oldDpiCtx != NULL) {
		SetThreadDpiAwarenessContext(oldDpiCtx);
	}
}

/*!
 * #625: lazily create the hidden probe window + its SR weaver (which installs
 * the SDK's phase-snap WndProc subclass in-process). Reuses the live SRContext +
 * immediate context; the probe weaver never weaves. Returns true when the probe
 * is ready. Must run on the snap-calling thread (WndProc dispatch is
 * thread-affine) and under the render mutex (shared immediate context).
 */
static bool
leiasr_d3d11_snap_probe_ensure(struct leiasr_d3d11 *leiasr)
{
	if (leiasr->snap_probe_weaver != nullptr) {
		return true;
	}
	if (leiasr->snap_probe_failed) {
		return false; // tried once, don't thrash the SR SDK every drag step
	}
	if (leiasr->context == nullptr || leiasr->d3d11_context == nullptr) {
		leiasr->snap_probe_failed = true;
		return false;
	}

	// Register the probe window class once per process.
	static const wchar_t *kProbeClass = L"DXRLeiaSnapProbe";
	static bool class_registered = false;
	HINSTANCE hinst = GetModuleHandleW(nullptr);
	if (!class_registered) {
		WNDCLASSW wc = {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = hinst;
		wc.lpszClassName = kProbeClass;
		RegisterClassW(&wc); // benign if already registered
		class_registered = true;
	}

	// Hidden, non-activating popup on the 3D display. Position is overwritten
	// per-snap (SnapToPhase keys off absolute screen coords) and the window is
	// never shown, so it never paints.
	int px = leiasr->display_screen_left;
	int py = leiasr->display_screen_top;
	HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kProbeClass, L"", WS_POPUP, px, py, 1, 1,
	                            nullptr, nullptr, hinst, nullptr);
	if (hwnd == nullptr) {
		U_LOG_W("#625 snap: probe CreateWindowEx failed (err=%lu)", GetLastError());
		leiasr->snap_probe_failed = true;
		return false;
	}

	// Creating a 2nd weaver on the shared SR context transiently perturbs the
	// physical lens (a brief 2D blip) — which, with the present-owner still
	// feeding full-disparity content, would surface as one mismatched
	// 2D-lens+disparity frame (the drag-time crosstalk). Capture the lens state
	// NOW and re-assert it synchronously right after the create (below), so no
	// presented frame ever sees the lens demoted. (We only run lazily, after 3D
	// is already locked, so re-asserting is safe — it never forces 3D inside the
	// SR init-settle window, unlike pre-creating the probe early.)
	bool lens_was_3d = false;
	if (leiasr->lens_hint != nullptr) {
		try {
			lens_was_3d = leiasr->lens_hint->isEnabled();
		} catch (...) {
		}
	}

	// Bind an SR weaver to the probe window — this installs the SDK's real
	// phase-snap WndProc (SetWindowLongPtr on GA_ROOT of hwnd), in-process.
	DPI_AWARENESS_CONTEXT oldDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	SR::IDX11Weaver1 *probe = nullptr;
	WeaverErrorCode result = SR::CreateDX11Weaver(leiasr->context, leiasr->d3d11_context, hwnd, &probe);
	if (oldDpi != NULL) {
		SetThreadDpiAwarenessContext(oldDpi);
	}
	if (result != WeaverErrorCode::WeaverSuccess || probe == nullptr) {
		U_LOG_W("#625 snap: probe CreateDX11Weaver failed (%d) — drag phase-snap unavailable", (int)result);
		DestroyWindow(hwnd);
		leiasr->snap_probe_failed = true;
		return false;
	}

	// Re-assert the pre-create lens state so the probe's creation blip never
	// reaches a presented frame (see the capture above). Idempotent.
	if (lens_was_3d && leiasr->lens_hint != nullptr) {
		try {
			leiasr->lens_hint->enable();
		} catch (...) {
		}
	}

	leiasr->snap_probe_hwnd = hwnd;
	leiasr->snap_probe_weaver = probe;
	leiasr->snap_probe_thread = GetCurrentThreadId();
	U_LOG_W("#625 snap: probe window %p + weaver ready (thread %lu, lens_was_3d=%d)", (void *)hwnd,
	        leiasr->snap_probe_thread, (int)lens_was_3d);
	return true;
}

bool
leiasr_d3d11_snap_window_rect(struct leiasr_d3d11 *leiasr,
                              int32_t origin_x,
                              int32_t origin_y,
                              int32_t target_x,
                              int32_t target_y,
                              int32_t *out_x,
                              int32_t *out_y)
{
	if (out_x == nullptr || out_y == nullptr) {
		return false;
	}
	*out_x = target_x; // default: no-op snap
	*out_y = target_y;
	if (leiasr == nullptr || !w_ready(leiasr)) {
		// #144: also covers "async create pending" — snap simply reports
		// unavailable and the runtime uses the raw drag position.
		return false;
	}

#ifdef DXR_LEIA_HAS_SR_V2
	if (leiasr->weaver_v2 != nullptr) {
		// The windowless snap — the entry point we asked Leia for, and the
		// reason the v1 probe below exists at all. The probe is a hidden window
		// bound to a SECOND SR weaver, driven with a synthetic drag so the SDK's
		// WndProc subclass snaps it, with the phase grid recovered BY
		// MEASUREMENT. It is unavailable here by construction (no SRContext, no
		// IDX11Weaver1) and it was fragile everywhere: every failure mode
		// returned the target unchanged with no signal, which is what made
		// LeiaInc/LeiaSR#163 take an elimination matrix to isolate.
		//
		// This call replaces all of that with one function.
		//
		// THE HANDLE MUST BE VALID. The loader's weaver trampolines null-check
		// the handle BEFORE the dispatch slot, so calling with a NULL weaver
		// returns SR_ERROR_HANDLE_INVALID whether the slot is present or not —
		// it is NOT a capability probe. (The SDK team hit exactly this and got
		// a false "present" against a runtime that lacked the slot.) We always
		// hold a real weaver here, so the slot check is reached and
		// SR_ERROR_FUNCTION_UNSUPPORTED genuinely means "older runtime".
		int32_t sx = target_x;
		int32_t sy = target_y;
		const SrResult r =
		    srWeaverSnapToPhase(leiasr->weaver_v2, origin_x, origin_y, target_x, target_y, &sx, &sy);

		if (r == SR_ERROR_FUNCTION_UNSUPPORTED) {
			// Runtime predates the windowless snap. Not a fault — and NOT a
			// reason to fall back to the probe, which cannot run on this path.
			static bool warned = false;
			if (!warned) {
				U_LOG_W("#625 snap: this SR runtime has no srWeaverSnapToPhase - window drags "
				        "will not phase-snap on the v2 path (DXR_LEIA_SR_API=v1 still snaps)");
				warned = true;
			}
			return false;
		}

		if (r == SR_DECLINED) {
			// "Nothing to correct" — a real answer, not a failure. Distinct
			// from a silent no-op precisely so we can tell them apart, which
			// is the whole lesson of #163. Caller keeps its target.
			return false;
		}

		if (!SR_SUCCEEDED(r)) {
			U_LOG_E("srWeaverSnapToPhase failed: %s (%d)", leia_sr_v2_result_str(r), (int)r);
			return false;
		}

		// Log the FIRST successful snap, once. Without this there is no way to
		// tell "the v2 snap ran and corrected the target" from "the v2 snap was
		// never called and the drag happened to look fine" — which is the exact
		// ambiguity that made #163 expensive. Includes the delta so a zero
		// correction is visibly a correction of zero rather than a no-op.
		static bool logged_first = false;
		if (!logged_first) {
			U_LOG_W("#625 snap: v2 srWeaverSnapToPhase LIVE — (%d,%d) -> (%d,%d), delta (%d,%d)",
			        target_x, target_y, sx, sy, sx - target_x, sy - target_y);
			logged_first = true;
		}

		*out_x = sx;
		*out_y = sy;
		return true;
	}
#endif

	if (!leiasr_d3d11_snap_probe_ensure(leiasr)) {
		return false;
	}
	// The SDK WndProc only sees SetWindowPos/SendMessage dispatched on the
	// window's OWNING thread — we created the probe on this (IPC) thread.
	if (GetCurrentThreadId() != leiasr->snap_probe_thread) {
		static bool warned = false;
		if (!warned) {
			U_LOG_W("#625 snap: called from thread %lu but probe owned by %lu — skipping",
			        GetCurrentThreadId(), leiasr->snap_probe_thread);
			warned = true;
		}
		return false;
	}

	HWND h = leiasr->snap_probe_hwnd;
	DPI_AWARENESS_CONTEXT oldDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// Drive the SDK's real drag-snap sequence synthetically:
	//   1. place the probe at the drag-start ORIGIN (moving=false ⟹ no snap);
	const UINT kSwp = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;
	SetWindowPos(h, nullptr, origin_x, origin_y, 0, 0, kSwp);
	//   2. begin the move — the WndProc records initialX/Y = GetWindowRect (=origin);
	SendMessageW(h, WM_ENTERSIZEMOVE, 0, 0);
	//   3. propose the TARGET — WM_WINDOWPOSCHANGING runs SnapToPhase, rewrites pos->x/y;
	SetWindowPos(h, nullptr, target_x, target_y, 0, 0, kSwp);
	//   4. read back the phase-snapped position the SDK committed;
	RECT r = {};
	bool got = GetWindowRect(h, &r) != 0;
	//   5. end the move.
	SendMessageW(h, WM_EXITSIZEMOVE, 0, 0);

	if (oldDpi != NULL) {
		SetThreadDpiAwarenessContext(oldDpi);
	}
	if (!got) {
		return false;
	}
	*out_x = r.left;
	*out_y = r.top;
	return true;
}

bool
leiasr_d3d11_set_window(struct leiasr_d3d11 *leiasr, void *hwnd)
{
	if (leiasr == nullptr || hwnd == nullptr) {
		return false;
	}

	// #144: weaver still creating — record the handle; the worker applies it
	// at publish. Re-check afterwards exactly as request_display_mode does: if
	// creation completed between the store and the worker's own consume, apply
	// it here (the exchange makes application happen exactly once).
	if (leiasr->async_state.load(std::memory_order_acquire) == LEIASR_ASYNC_PENDING) {
		leiasr->pending_hwnd.store(reinterpret_cast<uintptr_t>(hwnd), std::memory_order_release);
		if (leiasr->async_state.load(std::memory_order_acquire) == LEIASR_ASYNC_READY) {
			uintptr_t want = leiasr->pending_hwnd.exchange(0, std::memory_order_acq_rel);
			if (want != 0) {
				return w_set_window(leiasr, reinterpret_cast<HWND>(want));
			}
		} else {
			U_LOG_W("SR D3D11 window re-bind (hwnd=%p) recorded — weaver still creating",
			        (void *)hwnd);
		}
		// Accepted: whichever weaver publishes will be bound to this window,
		// so the runtime must NOT fall back to destroy+recreate.
		return true;
	}

	if (!w_ready(leiasr)) {
		return false;
	}

	// The snap probe (#625) deliberately needs NO update: it is a hidden
	// service-owned window with its own weaver, and SnapToPhase keys off
	// absolute screen coordinates plus the DLL-global phase grid — it never
	// referenced the bound presentation window.
	return w_set_window(leiasr, static_cast<HWND>(hwnd));
}

/*!
 * Check if weaver's HWND is still valid (for debugging "window handle is invalid" errors).
 */
bool
leiasr_d3d11_check_window_valid(struct leiasr_d3d11 *leiasr, void *hwnd)
{
	if (leiasr == nullptr) {
		return false;
	}

	HWND h = static_cast<HWND>(hwnd);
	if (h == nullptr) {
		U_LOG_W("leiasr_d3d11: HWND is null");
		return false;
	}

	if (!IsWindow(h)) {
		U_LOG_W("leiasr_d3d11: HWND %p is not a valid window", h);
		return false;
	}

	// Check if window is visible
	if (!IsWindowVisible(h)) {
		static bool warned_invisible = false;
		if (!warned_invisible) {
			U_LOG_W("leiasr_d3d11: Window %p is not visible", h);
			warned_invisible = true;
		}
	}

	// Get window position to check if it's on a valid monitor
	RECT rect;
	if (GetWindowRect(h, &rect)) {
		HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
		if (monitor == nullptr) {
			U_LOG_W("leiasr_d3d11: Window %p is not on any monitor (rect: %ld,%ld-%ld,%ld)",
			        h, rect.left, rect.top, rect.right, rect.bottom);
			return false;
		}
	}

	return true;
}

bool
leiasr_d3d11_get_predicted_eye_positions(struct leiasr_d3d11 *leiasr,
                                         float out_left_eye[3],
                                         float out_right_eye[3])
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return false;
	}

	// On v1 the SDK's getPredictedEyePositions throws ~11 first-chance
	// exceptions per frame (10x leap::api_exception + 1x std::runtime_error)
	// as routine internal control flow when its async cache races the call.
	// Verified empirically in LeiaViewer.exe (Leia's own reference app) —
	// same pattern, same rate. The catch is mandatory: the std::runtime_error
	// must not cross the C ABI boundary back into the runtime. On v2 the same
	// condition arrives as a plain SrResult. Both are handled in w_get_*.
	float left_mm[3], right_mm[3];
	if (!w_get_predicted_eyes(leiasr, left_mm, right_mm)) {
		return false;
	}
	out_left_eye[0]  = left_mm[0]  / 1000.0f;
	out_left_eye[1]  = left_mm[1]  / 1000.0f;
	out_left_eye[2]  = left_mm[2]  / 1000.0f;
	out_right_eye[0] = right_mm[0] / 1000.0f;
	out_right_eye[1] = right_mm[1] / 1000.0f;
	out_right_eye[2] = right_mm[2] / 1000.0f;
	return true;
}

void
leiasr_d3d11_set_srgb_conversion(struct leiasr_d3d11 *leiasr,
                                 bool read_srgb,
                                 bool write_srgb)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return;
	}

	leiasr->srgb_read = read_srgb;
	leiasr->srgb_write = write_srgb;
	w_set_srgb(leiasr, read_srgb, write_srgb);
}

void
leiasr_d3d11_set_latency_in_frames(struct leiasr_d3d11 *leiasr,
                                   uint64_t latency_frames)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return;
	}

	w_set_latency_in_frames(leiasr, latency_frames);
}

bool
leiasr_d3d11_is_ready(struct leiasr_d3d11 *leiasr)
{
	if (leiasr == nullptr) {
		return false;
	}

	return w_ready(leiasr);
}

bool
leiasr_d3d11_get_display_dimensions(struct leiasr_d3d11 *leiasr, struct leiasr_display_dimensions *out_dims)
{
	if (leiasr == nullptr || out_dims == nullptr) {
		if (out_dims != nullptr) {
			out_dims->valid = false;
		}
		return false;
	}

	// #144: w_ready also gates "async create pending" — the dims fields are
	// written by the worker and only safe to read after the READY publish.
	if (!w_ready(leiasr) || !leiasr->display_dims_valid) {
		out_dims->valid = false;
		return false;
	}

	out_dims->width_m = leiasr->display_width_m;
	out_dims->height_m = leiasr->display_height_m;
	out_dims->valid = true;

	return true;
}

bool
leiasr_d3d11_get_display_pixel_info(struct leiasr_d3d11 *leiasr,
                                     uint32_t *out_display_pixel_width,
                                     uint32_t *out_display_pixel_height,
                                     int32_t *out_display_screen_left,
                                     int32_t *out_display_screen_top,
                                     float *out_display_width_m,
                                     float *out_display_height_m)
{
	if (leiasr == nullptr || !w_ready(leiasr) || out_display_pixel_width == nullptr ||
	    out_display_pixel_height == nullptr || out_display_screen_left == nullptr ||
	    out_display_screen_top == nullptr || out_display_width_m == nullptr ||
	    out_display_height_m == nullptr) {
		return false;
	}

	if (!leiasr->display_pixel_dims_valid || !leiasr->display_dims_valid) {
		return false;
	}

	*out_display_pixel_width = leiasr->display_pixel_width;
	*out_display_pixel_height = leiasr->display_pixel_height;
	*out_display_screen_left = leiasr->display_screen_left;
	*out_display_screen_top = leiasr->display_screen_top;
	*out_display_width_m = leiasr->display_width_m;
	*out_display_height_m = leiasr->display_height_m;

	return true;
}

bool
leiasr_d3d11_get_recommended_view_dimensions(struct leiasr_d3d11 *leiasr,
                                              uint32_t *out_width,
                                              uint32_t *out_height)
{
	if (leiasr == nullptr || !w_ready(leiasr) || out_width == nullptr || out_height == nullptr) {
		return false;
	}

	if (!leiasr->recommended_dims_valid) {
		return false;
	}

	*out_width = leiasr->recommended_view_width;
	*out_height = leiasr->recommended_view_height;

	return true;
}

bool
leiasr_query_recommended_view_dimensions(double max_time,
                                          uint32_t *out_width,
                                          uint32_t *out_height,
                                          float *out_refresh_rate_hz,
                                          uint32_t *out_native_width,
                                          uint32_t *out_native_height)
{
	if (out_width == nullptr || out_height == nullptr) {
		return false;
	}

	const double start_time = (double)GetTickCount64() / 1000.0;

	// Create temporary SR context
	SR::SRContext *context = nullptr;
	while (context == nullptr) {
		try {
			context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException &e) {
			(void)e;
		}

		U_LOG_D("Waiting for SR context (dimension query)...");
		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (context == nullptr) {
		U_LOG_E("Failed to create SR context for dimension query within %.1f seconds", max_time);
		return false;
	}

	// Get display manager and query dimensions
	bool success = false;
	try {
		SR::IDisplayManager *displayManager = SR::GetDisplayManagerInstance(*context);
		if (displayManager != nullptr) {
			// Wait for display to be ready
			while (!success) {
				SR::IDisplay *display = displayManager->getPrimaryActiveSRDisplay();
				if (display != nullptr && display->isValid()) {
					SR_recti display_location = display->getLocation();
					int64_t native_width = display_location.right - display_location.left;
					int64_t native_height = display_location.bottom - display_location.top;
					if ((native_width != 0) && (native_height != 0)) {
						*out_width = display->getRecommendedViewsTextureWidth();
						*out_height = display->getRecommendedViewsTextureHeight();
						success = (*out_width > 0 && *out_height > 0);
						if (success) {
							U_LOG_I("SR query: recommended view dimensions %ux%u per eye",
							        *out_width, *out_height);

							// Return native display dimensions if requested
							if (out_native_width != nullptr) {
								*out_native_width = static_cast<uint32_t>(native_width);
							}
							if (out_native_height != nullptr) {
								*out_native_height = static_cast<uint32_t>(native_height);
							}
							U_LOG_I("SR query: native display dimensions %ux%u",
							        (uint32_t)native_width, (uint32_t)native_height);

							// Query monitor refresh rate via Win32
							if (out_refresh_rate_hz != nullptr) {
								DEVMODEW dm = {};
								dm.dmSize = sizeof(dm);
								if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
								    dm.dmDisplayFrequency > 1) {
									*out_refresh_rate_hz = (float)dm.dmDisplayFrequency;
									U_LOG_I("SR query: display refresh rate %.0f Hz",
									        *out_refresh_rate_hz);
								} else {
									*out_refresh_rate_hz = 60.0f;
									U_LOG_W("Could not query display refresh rate, defaulting to 60 Hz");
								}
							}
						}
						break;
					}
				}

				Sleep(100);

				double cur_time = (double)GetTickCount64() / 1000.0;
				if ((cur_time - start_time) > max_time) {
					break;
				}
			}
		}
	} catch (...) {
		U_LOG_E("Exception querying SR display dimensions");
	}

	// Clean up temporary context
	SR::SRContext::deleteSRContext(context);

	if (!success) {
		U_LOG_E("Failed to query SR recommended dimensions within %.1f seconds", max_time);
	}

	return success;
}

// Cached display dimensions for static queries
static float g_cached_display_width_m = 0.0f;
static float g_cached_display_height_m = 0.0f;
static float g_cached_nominal_x_m = 0.0f;
static float g_cached_nominal_y_m = 0.0f;
static float g_cached_nominal_z_m = 0.5f;
static bool g_display_dims_cached = false;

bool
leiasr_static_get_predicted_eye_positions(float out_left_eye[3],
                                          float out_right_eye[3])
{
	if (out_left_eye == nullptr || out_right_eye == nullptr) {
		return false;
	}

	// Eye position prediction requires an active weaver instance because:
	// 1. The weaver owns the LookaroundFilter that provides prediction
	// 2. Prediction is tuned to each application's update rate
	// Without a weaver, we cannot provide accurate predicted eye positions.
	// Callers should use leiasr_d3d11_get_predicted_eye_positions() with
	// their per-session weaver instance instead.

	// Return default center position for graceful fallback
	// This allows apps to start rendering before eye tracking is ready
	out_left_eye[0] = -0.032f;  // -32mm left of center
	out_left_eye[1] = 0.0f;
	out_left_eye[2] = 0.6f;     // 600mm from display

	out_right_eye[0] = 0.032f;  // +32mm right of center
	out_right_eye[1] = 0.0f;
	out_right_eye[2] = 0.6f;    // 600mm from display

	// Return false to indicate these are fallback values, not tracked
	return false;
}

bool
leiasr_static_get_display_dimensions(struct leiasr_display_dimensions *out_dims)
{
	if (out_dims == nullptr) {
		return false;
	}

	// Return cached values if available
	if (g_display_dims_cached) {
		out_dims->width_m = g_cached_display_width_m;
		out_dims->height_m = g_cached_display_height_m;
		out_dims->nominal_x_m = g_cached_nominal_x_m;
		out_dims->nominal_y_m = g_cached_nominal_y_m;
		out_dims->nominal_z_m = g_cached_nominal_z_m;
		out_dims->valid = true;
		return true;
	}

	// Need to query from SR SDK
	// Create temporary context
	SR::SRContext *context = nullptr;

	try {
		context = SR::SRContext::create();
	} catch (...) {
		return false;
	}

	if (context == nullptr) {
		return false;
	}

	bool success = false;
	try {
		SR::IDisplayManager *displayManager = SR::GetDisplayManagerInstance(*context);
		if (displayManager != nullptr) {
			SR::IDisplay *display = displayManager->getPrimaryActiveSRDisplay();
			if (display != nullptr && display->isValid()) {
				// Get physical dimensions using SR SDK's physical size API
				// Returns centimeters, convert to meters
				float raw_width_cm = display->getPhysicalSizeWidth();
				float raw_height_cm = display->getPhysicalSizeHeight();

				if (raw_width_cm > 0.0f && raw_height_cm > 0.0f) {
					g_cached_display_width_m = raw_width_cm / 100.0f;
					g_cached_display_height_m = raw_height_cm / 100.0f;

					// Query nominal viewing position from SR SDK (returns mm)
					float nom_x_mm = 0.0f, nom_y_mm = 0.0f, nom_z_mm = 0.0f;
					try {
						display->getDefaultViewingPosition(nom_x_mm, nom_y_mm, nom_z_mm);
						g_cached_nominal_x_m = nom_x_mm / 1000.0f;
						g_cached_nominal_y_m = nom_y_mm / 1000.0f;
						g_cached_nominal_z_m = nom_z_mm / 1000.0f;
						U_LOG_W("SR nominal viewing position: (%.1f, %.1f, %.1f) mm = (%.4f, %.4f, %.4f) m",
						        nom_x_mm, nom_y_mm, nom_z_mm,
						        g_cached_nominal_x_m, g_cached_nominal_y_m, g_cached_nominal_z_m);
					} catch (...) {
						g_cached_nominal_x_m = 0.0f;
						g_cached_nominal_y_m = 0.0f;
						g_cached_nominal_z_m = 0.5f;
						U_LOG_W("SR getDefaultViewingPosition failed, using fallback (0, 0, 0.5) m");
					}

					g_display_dims_cached = true;

					out_dims->width_m = g_cached_display_width_m;
					out_dims->height_m = g_cached_display_height_m;
					out_dims->nominal_x_m = g_cached_nominal_x_m;
					out_dims->nominal_y_m = g_cached_nominal_y_m;
					out_dims->nominal_z_m = g_cached_nominal_z_m;
					out_dims->valid = true;
					success = true;

					U_LOG_W("Static display dimensions: %.2fcm x %.2fcm = %.4fm x %.4fm",
					        raw_width_cm, raw_height_cm,
					        g_cached_display_width_m, g_cached_display_height_m);
				}
			}
		}
	} catch (...) {
		U_LOG_E("Exception querying static display dimensions");
	}

	// Clean up temporary context
	if (context != nullptr) {
		SR::SRContext::deleteSRContext(context);
	}

	return success;
}

bool
leiasr_d3d11_request_display_mode(struct leiasr_d3d11 *leiasr, bool enable_3d)
{
	if (leiasr == nullptr) {
		return false;
	}

	// #158: sticky record of what the runtime last asked for, so an in-place
	// reconnect can re-assert it on the freshly created lens. Distinct from
	// pending_lens_wish, which is consumed at publish and empty thereafter.
	leiasr->last_lens_wish.store(enable_3d ? 1 : 0, std::memory_order_release);

	// #144: weaver still creating — record the wish; the worker applies it at
	// publish. Re-check afterwards: if creation completed between the store
	// and the worker's own wish-consume, apply it ourselves (the exchange
	// makes application happen exactly once). This is a *request* API —
	// acceptance, not physical completion — so returning true is honest.
	if (leiasr->async_state.load(std::memory_order_acquire) == LEIASR_ASYNC_PENDING) {
		leiasr->pending_lens_wish.store(enable_3d ? 1 : 0, std::memory_order_release);
		if (leiasr->async_state.load(std::memory_order_acquire) == LEIASR_ASYNC_READY) {
			int wish = leiasr->pending_lens_wish.exchange(-1, std::memory_order_acq_rel);
			if (wish >= 0 && lens_present(leiasr)) {
				try {
					lens_set(leiasr, wish == 1);
				} catch (...) {
				}
			}
		} else {
			U_LOG_W("SR D3D11 display mode wish (%s) recorded — weaver still creating",
			        enable_3d ? "3D" : "2D");
		}
		return true;
	}

	if (!lens_present(leiasr)) {
		return false;
	}

	try {
		lens_set(leiasr, enable_3d);
		U_LOG_W("SR D3D11 display mode switched to %s", enable_3d ? "3D" : "2D");
		return true;
	} catch (...) {
		U_LOG_E("Failed to switch SR D3D11 display mode to %s", enable_3d ? "3D" : "2D");
		return false;
	}
}

bool
leiasr_d3d11_supports_display_mode_switch(struct leiasr_d3d11 *leiasr)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return false;
	}

	return lens_present(leiasr);
}

bool
leiasr_d3d11_get_hardware_3d_state(struct leiasr_d3d11 *leiasr, bool *out_is_3d)
{
	if (leiasr == nullptr || !w_ready(leiasr) || !lens_present(leiasr) || out_is_3d == nullptr) {
		return false;
	}

	try {
		*out_is_3d = lens_is_enabled(leiasr);
		return true;
	} catch (...) {
		return false;
	}
}

uint32_t
leiasr_d3d11_poll_backend_state(struct leiasr_d3d11 *leiasr)
{
	if (leiasr == nullptr) {
		return LEIA_SR_BACKEND_OK;
	}

	// A create or a reconnect is already in flight. Every public entry point
	// degrades through w_ready() while that is true, so there is nothing to
	// detect here and nothing to trigger.
	if (leiasr->async_state.load(std::memory_order_acquire) != LEIASR_ASYNC_READY) {
		leiasr->last_backend_state.store(LEIA_SR_BACKEND_DEGRADED, std::memory_order_relaxed);
		return LEIA_SR_BACKEND_DEGRADED;
	}

	// One source for the whole poll: the SDK-reported platform id when this arm
	// has a v2 instance and the SDK is new enough, the SCM probe otherwise
	// (leia_sr_liveness.h).
	void *const v2 = v2_instance_of(leiasr);
	const uint64_t gen = leia_sr_liveness_platform_generation_ex(v2);

	/*
	 * #169: the client/platform version verdict is taken HERE, ahead of the
	 * "platform down" and "generation changed" early-returns below.
	 *
	 * In #162 it sat after them, which made it dead code on the only path that
	 * needs it: an upgrade takes the platform down (gen == 0) and brings it
	 * back under a new identity (generation changed), so one of those two
	 * always returned first. It logged nothing at all while the service
	 * crashed.
	 *
	 * The distinction this encodes:
	 *   - platform RESTART, same version → the mapped client DLLs still match,
	 *     the in-place reconnect below is correct, and stays untouched.
	 *   - platform UPGRADE underneath us → the mapped client DLLs are stale.
	 *     Reconnecting builds a fresh weaver out of old client code against a
	 *     new platform, and the vendor runtime access-violates (#169). No
	 *     in-process action can replace a mapped DLL, so the honest answer is
	 *     STALE ("recreate me"), not DEGRADED (which promises self-healing).
	 *
	 * Nothing is torn down HERE — the weaver keeps weaving on its last eye
	 * positions for as long as it is kept. Losing tracking, or even dropping to
	 * a flat blit, beats taking the service down, which is the whole point.
	 *
	 * What the runtime does with STALE is its business, and it is not gentle:
	 * pipeline_dp_health_poll forces a recreate, and the forced path RETIRES
	 * THE DP FIRST (one live weaver per HWND — displayxr-runtime
	 * comp_d3d11_service.cpp). So under the service the picture does fall back
	 * to flat blit until someone restarts it. The gate still has to hold on the
	 * other side of that: the fresh DP is created in the SAME process, with the
	 * same stale DLLs mapped, so its first create would make exactly the vendor
	 * call that access-violated — create_weaver_attempt refuses it, which turns
	 * a crash loop into a quiet 2 Hz create-refusal the runtime already backs
	 * off from.
	 */
	if (!leia_sr_liveness_client_matches_platform()) {
		if (!leiasr->warned_client_skew.exchange(true, std::memory_order_relaxed)) {
			U_LOG_W("SR platform was UPGRADED while this process was running — refusing an "
			        "in-place weaver reconnect (the OLD SR client DLLs are still mapped and "
			        "reconnecting through them faults inside the vendor runtime, "
			        "leia-plugin#169). Reporting STALE; tracking is gone until the DisplayXR "
			        "service is RESTARTED, which is the only way to map the new libraries.");
		}
		leiasr->last_backend_state.store(LEIA_SR_BACKEND_STALE, std::memory_order_relaxed);
		return LEIA_SR_BACKEND_STALE;
	}
	leiasr->warned_client_skew.store(false, std::memory_order_relaxed);

	if (gen == 0) {
		// SR platform is down. Deliberately do NOT tear the weaver down: it
		// keeps weaving on the last known eye positions, which is a far
		// better picture than the flat blit a teardown would drop us to —
		// and when the service comes back the generation check below fires
		// and rebuilds. Nothing is lost by waiting.
		if (!leiasr->warned_platform_down.exchange(true, std::memory_order_relaxed)) {
			U_LOG_W("SR platform is DOWN — weaving untracked; will reconnect when it returns");
		}
		leiasr->last_backend_state.store(LEIA_SR_BACKEND_DEGRADED, std::memory_order_relaxed);
		return LEIA_SR_BACKEND_DEGRADED;
	}
	leiasr->warned_platform_down.store(false, std::memory_order_relaxed);

	// The SDK's own LIVENESS signal, and a strictly independent detector from
	// the identity comparison below — the vendor expects both to be wired. It
	// catches two cases identity cannot: a platform that crashed while its last
	// published id still lingers, and any machine where no token was readable at
	// create time (which switches the comparison below off entirely). A no-op
	// unless the SDK is new enough (DXR_LEIA_HAS_SR_CONNECTION_STATE).
	//
	// TODO(#158 follow-up): SR_ERROR_PLATFORM_DISCONNECTED can also come back
	// from the per-frame w_get_predicted_eyes() path. Deliberately NOT wired
	// there in this change — this 1 Hz poll already covers the case, and the hot
	// path stays untouched.
	if (leia_sr_liveness_connection_is_dead(v2)) {
		arm_reconnect(leiasr, "SR platform connection reported DEAD by the SDK");
		leiasr->last_backend_state.store(LEIA_SR_BACKEND_DEGRADED, std::memory_order_relaxed);
		return LEIA_SR_BACKEND_DEGRADED;
	}

	// platform_generation == 0 means we never managed to read a token at
	// create time (no SDK id and the SCM unreachable). Detection is simply off
	// in that case — treating "unknown then known" as a restart would rebuild
	// the weaver for no reason on every such machine.
	if (leiasr->platform_generation != 0 && gen != leiasr->platform_generation) {
		char reason[192];
		snprintf(reason, sizeof(reason), "SR platform restarted (generation %llu → %llu)",
		         (unsigned long long)leiasr->platform_generation, (unsigned long long)gen);
		arm_reconnect(leiasr, reason);
		leiasr->last_backend_state.store(LEIA_SR_BACKEND_DEGRADED, std::memory_order_relaxed);
		return LEIA_SR_BACKEND_DEGRADED;
	}

	// The skew check that used to sit here is now taken up front, right after
	// the generation read — see the #169 block above for why its old position
	// was dead code on an upgrade.

	leiasr->last_backend_state.store(LEIA_SR_BACKEND_OK, std::memory_order_relaxed);
	return LEIA_SR_BACKEND_OK;
}

bool
leiasr_d3d11_wait_ready(struct leiasr_d3d11 *leiasr, uint32_t timeout_ms)
{
	if (leiasr == nullptr) {
		return false;
	}
	const uint64_t deadline = GetTickCount64() + timeout_ms;
	while (leiasr->async_state.load(std::memory_order_acquire) == LEIASR_ASYNC_PENDING) {
		if (GetTickCount64() >= deadline) {
			return false;
		}
		Sleep(20);
	}
	return w_ready(leiasr);
}

} // extern "C"
