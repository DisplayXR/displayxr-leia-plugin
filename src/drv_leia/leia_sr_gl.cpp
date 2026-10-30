// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Leia SR OpenGL weaver wrapper implementation.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_sr_gl.h"
#include "leia_sr_api_select.h"
#include "leia_sr_v2_common.h"
#include "leia_interface.h" // leia_hmd_active_mode_get (#446)
#include "leia_sr_liveness.h"
#include "leia_sr_multi_win.h" // multi-screen M6 per-screen weaver plan (plain C)
#include "leia_sr_presence_win.h" // per-display user presence (SR event 20)
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/weaver/glweaver.h>
#include <sr/world/display/display.h>
#include <sr/sense/display/switchablehint.h>
#include <sr/utility/exception.h>

#ifdef DXR_LEIA_HAS_SR_V2
#include <sr/sr_gl.h>
#include <sr/sr_weaver.h>
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
#include <sr/sr_display.h> // SrDisplayBindingInfo (multi-screen M6)
#endif
#endif

#include <windows.h>
#include <sysinfoapi.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

/*!
 * GL SR weaver instance.
 */
struct leiasr_gl
{
	// SR SDK objects
	SR::SRContext *context = nullptr;
	SR::IGLWeaver1 *weaver = nullptr;
	SR::SwitchableLensHint *lens_hint = nullptr;

#ifdef DXR_LEIA_HAS_SR_V2
	// v2 (C99) objects; NULL on the v1 path. `weaver_v2 != nullptr` is the
	// discriminant — see leia_sr_d3d11.cpp.
	SrInstance instance_v2 = nullptr;
	SrWeaver weaver_v2 = nullptr;
	SrLens lens_v2 = nullptr;
	//! Per-display user presence (SR event 20): the monitor on instance_v2,
	//! and its published leia_sr_presence. The state lives HERE, not in the
	//! monitor, so a frame-thread read never races the monitor's destroy.
	struct leia_sr_presence_monitor *presence = nullptr;
	std::atomic<int32_t> presence_state{0};
#endif

	// Current input texture info
	uint32_t input_texture = 0;
	uint32_t view_width = 0;
	uint32_t view_height = 0;
	uint32_t input_format = 0; // GL_RGBA8

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

	// --- #158 SR platform restart detection -------------------------------
	// Reporting only on this arm. Unlike D3D11 — which rebuilds its SDK
	// objects IN PLACE off a detached worker, behind the async_state gate every
	// entry point already honours — there is no async create/publish machinery
	// here, so there is no safe point at which to swap the weaver out from
	// under the render thread. A generation change is therefore reported as
	// STALE, and the remedy is the caller recreating the display processor.
	// TODO(#158 follow-up): port the D3D11 in-place reconnect.
	uint64_t platform_generation = 0;  //!< Incarnation at create (0 = unknown).
	bool warned_platform_down = false; //!< One-shot WARN edges: the poll runs at
	bool warned_client_skew = false;   //!< ~1 Hz forever, so a plain log would be
	bool warned_stale = false;         //!< per-frame-class bloat.

	// --- #215 async lens hint ---------------------------------------------
	// srLensEnable/disable (v2) and SwitchableLensHint::enable/disable (v1)
	// are round trips to the SR service process — ~tens of ms, and the runtime
	// calls request_display_mode from the compositor frame path under its own
	// mutex. The wish goes into a mailbox drained by a short-lived worker
	// instead: latest wish wins, so a burst of flips costs one SR call per
	// value the worker actually observes, and the caller never blocks.
	// (This struct is `new`-allocated in leiasr_gl_create, so the NSDMI
	// defaults below really do apply — no calloc to zero them out.)
	std::atomic<int> lens_mailbox{-1};         //!< latest wish wins: -1 none, 0 → 2D, 1 → 3D
	std::atomic<bool> lens_worker_busy{false}; //!< a worker owns the mailbox right now
	std::thread lens_thread;                   //!< the current/last worker, joined at destroy
	std::mutex lens_thread_mtx;                //!< guards lens_thread (spawn vs join)
	bool async_lens = true;                    //!< false ⟹ synchronous lens call (kill-switch)
	//! runtime#939: set by lens_worker_join on the way to a destroy/reconnect so a
	//! worker sitting in its 2D dwell applies at once instead of finishing the wait.
	std::atomic<bool> lens_quit{false};

	// --- Multi-screen M6: per-screen weaver (create_dp_gl_for_screen) ---
	// The GL twin of the D3D11 arm's block (leia_sr_d3d11.cpp) — same plan,
	// same fields, same per-weave application.
	//! Created for ONE screen: plan EXTERNAL routing + display binding.
	//! The unbound DP (every pre-M6 caller) keeps SDK routing, byte-identical.
	bool screen_bound = false;
	//! The SR display to bind (0 = unknown ⟹ routing only).
	uint64_t bind_display_id = 0;
	//! The binding's desktop origin: what get_display_pixel_info reports
	//! instead of the SR location (runtime #2311). Set before the create.
	struct leia_win_screen_origin bind_origin = {false, 0, 0};
	//! #480: the bound panel's EDID while SR listed its id (0 = not seen), for a re-key.
	uint16_t bind_edid_manufacturer = 0;
	uint16_t bind_edid_product = 0;
	std::atomic<bool> bind_origin_logged{false};
	//! What the create ended up with (for logs and the per-frame paths).
	bool external_routed = false;
	bool display_bound = false;
	//! The weaver reads its phase from the present origin + viewport only
	//! (EXTERNAL without a window). Applied before every weave.
	bool needs_present_origin = false;
	std::atomic<int32_t> present_origin_x{0};
	std::atomic<int32_t> present_origin_y{0};
	//! The runtime's active (tracked) display id, 0 = unknown.
	uint64_t active_display_id = 0;
	//! The bound display is not the tracked one and has no tracker of its own:
	//! a fixed nominal viewer is pinned, re-applied before every weave.
	bool viewer_pinned = false;
#ifdef DXR_LEIA_HAS_SR_MULTI_VIEWER
	//! #446: the rendering mode's multi-viewer override as applied to this
	//! weaver (leia_sr_v2_apply_multiviewer). Re-arms on a new weaver handle.
	struct leia_sr_v2_mv_state mv = {};
#endif
	//! lens_v2 is bound to bind_display_id (SR D3): disabled before destroy.
	bool lens_bound = false;
	float pin_left_mm[3] = {0.0f, 0.0f, 0.0f};
	float pin_right_mm[3] = {0.0f, 0.0f, 0.0f};
	bool pin_eyes_logged = false;
};

namespace {

/*!
 * Create and wait for the SR context to become ready.
 */
bool
create_sr_context(double max_time, leiasr_gl &sr)
{
	const double start_time = (double)GetTickCount64() / 1000.0;

	// Create SR context.
	while (sr.context == nullptr) {
		try {
			sr.context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException &e) {
			(void)e;
		}

		U_LOG_D("Waiting for SR context (GL)...");
		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (sr.context == nullptr) {
		U_LOG_E("Failed to create SR context (GL) within %.1f seconds", max_time);
		return false;
	}

	// Get display manager and wait for display to be ready.
	SR::IDisplayManager *displayManager = nullptr;
	SR::IDisplay *display = nullptr;
	bool display_ready = false;

	try {
		displayManager = SR::GetDisplayManagerInstance(*sr.context);
		if (displayManager == nullptr) {
			U_LOG_E("Failed to get SR DisplayManager instance (GL)");
			return false;
		}
	} catch (...) {
		U_LOG_E("Exception getting SR DisplayManager (GL) - requires runtime 1.34.8-RC1+");
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

				float raw_width_cm = display->getPhysicalSizeWidth();
				float raw_height_cm = display->getPhysicalSizeHeight();
				sr.display_width_m = raw_width_cm / 100.0f;
				sr.display_height_m = raw_height_cm / 100.0f;
				sr.display_dims_valid = true;

				sr.display_pixel_width = static_cast<uint32_t>(width);
				sr.display_pixel_height = static_cast<uint32_t>(height);
				sr.display_screen_left = static_cast<int32_t>(display_location.left);
				sr.display_screen_top = static_cast<int32_t>(display_location.top);
				sr.display_pixel_dims_valid = true;

				U_LOG_W("SR GL display: %ldx%ld px, physical %.2fcm x %.2fcm = %.4fm x %.4fm",
				        (long)width, (long)height,
				        raw_width_cm, raw_height_cm,
				        sr.display_width_m, sr.display_height_m);

				break;
			}
		}

		U_LOG_D("Waiting for SR display (GL)...");
		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (!display_ready) {
		U_LOG_E("SR display not ready (GL) within %.1f seconds", max_time);
		return false;
	}

	// Create SwitchableLensHint for 2D/3D mode switching
	try {
		sr.lens_hint = SR::SwitchableLensHint::create(*sr.context);
		U_LOG_W("SR GL SwitchableLensHint created successfully");
	} catch (...) {
		sr.lens_hint = nullptr;
		U_LOG_W("SR GL SwitchableLensHint not available on this display");
	}

	return true;
}

#ifdef DXR_LEIA_HAS_SR_V2
/*!
 * The v2 creation sequence for the GL arm.
 *
 * Unlike D3D11/D3D12 the create-info carries no device or context: the SDK
 * uses the GL context CURRENT ON THIS THREAD, exactly as v1's
 * `CreateGLWeaver(context, hwnd, &weaver)` does. So the caller's threading
 * contract is unchanged by the migration — whatever had to be current before
 * still has to be current now.
 */
bool
create_v2(double max_time, void *hwnd, leiasr_gl &sr)
{
	if (!leia_sr_v2_create_instance(max_time, &sr.instance_v2)) {
		return false;
	}

	// #480: SR re-keys a panel when its FPC pairs (an EDID_ONLY id is a hash until
	// then), so a bound weaver binds the id SR lists now, not the one it was made for.
	if (sr.screen_bound) {
		sr.bind_display_id = leia_sr_v2_current_display_id(
		    sr.bind_display_id, &sr.bind_origin, &sr.bind_edid_manufacturer, &sr.bind_edid_product, "GL");
	}

	// Multi-screen M6: what the runtime can do, and what this DP will ask for.
	// An unbound DP gets an empty plan (SDK routing, no binding) — today's
	// single-screen behaviour, byte-identical.
	struct leia_win_sr_multi_caps mcaps = {};
	struct leia_win_sr_weaver_plan plan = {};
	if (sr.screen_bound) {
		leia_sr_v2_query_multi_caps(sr.instance_v2, &mcaps);
		plan = leia_win_sr_plan_weaver(&mcaps, sr.screen_bound, hwnd != nullptr, sr.bind_display_id);
		U_LOG_W("SR GL per-screen weaver: display 0x%016llx, window %p, caps{known=%d external=%d snap=%d "
		        "binding=%d maxBound=%u} -> plan{external=%d keep_drag_snap=%d bind=%d}",
		        (unsigned long long)sr.bind_display_id, hwnd, (int)mcaps.known, (int)mcaps.external_routing,
		        (int)mcaps.keep_drag_snap, (int)mcaps.display_binding, mcaps.max_bound_displays,
		        (int)plan.routing_external, (int)plan.keep_drag_snap, (int)plan.bind_display);
	}

	leia_sr_v2_display_info info{};
	if (!leia_sr_v2_query_display(sr.instance_v2, hwnd, max_time, plan.bind_display ? plan.display_id : 0, &info)) {
		srDestroyInstance(sr.instance_v2);
		sr.instance_v2 = nullptr;
		return false;
	}
	// The runtime's ACTIVE display — the one its tracker follows — so a weaver
	// bound to any other display knows it must pin a viewer.
	if (plan.bind_display) {
		leia_sr_v2_display_info active{};
		if (leia_sr_v2_query_display(sr.instance_v2, nullptr, max_time, 0, &active)) {
			sr.active_display_id = active.display_id;
		}
	}

	sr.display_width_m = info.width_m;
	sr.display_height_m = info.height_m;
	sr.display_dims_valid = true;
	if (sr.screen_bound) {
		// A per-screen DP describes ITS display (the runtime's segment views
		// and window-scoped Kooima read it). The unbound DP keeps today's
		// answer, byte-identical.
		sr.display_pixel_width = info.pixel_width;
		sr.display_pixel_height = info.pixel_height;
		sr.display_screen_left = info.screen_left;
		sr.display_screen_top = info.screen_top;
		sr.display_pixel_dims_valid = info.pixel_width != 0 && info.pixel_height != 0;
	}

	// Same DPI dance as the D3D11 arm, for the per-screen weaver only (the
	// unbound create stays as it was): the SDK reads the HWND's geometry
	// during creation and must see physical pixels.
	DPI_AWARENESS_CONTEXT oldDpiCtx =
	    sr.screen_bound ? SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : NULL;

	SrWeaverCreateInfoGL ci{};
	ci.sType = SR_TYPE_WEAVER_CREATE_INFO_GL;
	ci.pNext = nullptr;
	ci.window = (SrNativeWindowHandle)hwnd;

	SrResult wr = SR_ERROR_RUNTIME_FAILURE;
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
	// Multi-screen M6: chain routing + binding per the plan; on a refusal
	// drop the binding first, then routing (leia_win_sr_plan_weaver_fallback),
	// so a runtime that refuses an id still yields a weaver.
	for (;;) {
		SrDisplayBindingInfo binding{};
		binding.sType = SR_TYPE_DISPLAY_BINDING_INFO;
		binding.pNext = nullptr;
		binding.displayId = plan.display_id;
		SrWeaverRoutingInfo routing{};
		routing.sType = SR_TYPE_WEAVER_ROUTING_INFO;
		routing.pNext = nullptr;
		routing.mode = SR_WEAVER_ROUTING_EXTERNAL;
		routing.flags = plan.keep_drag_snap ? SR_WEAVER_ROUTING_KEEP_DRAG_SNAP_BIT : 0;
		const void *head = nullptr;
		if (plan.bind_display) {
			binding.pNext = head;
			head = &binding;
		}
		if (plan.routing_external) {
			routing.pNext = head;
			head = &routing;
		}
		ci.pNext = head;
		wr = srCreateWeaverGL(sr.instance_v2, &ci, &sr.weaver_v2);
		if (SR_SUCCEEDED(wr) && sr.weaver_v2 != nullptr) {
			break;
		}
		if (!plan.bind_display && !plan.routing_external) {
			break; // the plain create failed: a real fault, reported below
		}
		U_LOG_W("srCreateWeaverGL refused plan{external=%d bind=%d id=0x%016llx}: %s (%d) — falling back",
		        (int)plan.routing_external, (int)plan.bind_display, (unsigned long long)plan.display_id,
		        leia_sr_v2_result_str(wr), (int)wr);
		sr.weaver_v2 = nullptr;
		if (!leia_win_sr_plan_weaver_fallback(&plan)) {
			break;
		}
	}
#else
	wr = srCreateWeaverGL(sr.instance_v2, &ci, &sr.weaver_v2);
#endif
	sr.external_routed = plan.routing_external;
	sr.display_bound = plan.bind_display;
	sr.needs_present_origin = leia_win_sr_plan_needs_present_origin(&plan, hwnd != nullptr);

	if (oldDpiCtx != NULL) {
		SetThreadDpiAwarenessContext(oldDpiCtx);
	}

	if (!SR_SUCCEEDED(wr) || sr.weaver_v2 == nullptr) {
		U_LOG_E("srCreateWeaverGL failed: %s (%d)", leia_sr_v2_result_str(wr), (int)wr);
		sr.weaver_v2 = nullptr;
		srDestroyInstance(sr.instance_v2);
		sr.instance_v2 = nullptr;
		return false;
	}

	/*
	 * Multi-screen M6 — the untracked screen (David, 2026-10-07), exactly as
	 * the D3D11 arm: a weaver bound to a display that is NOT the runtime's
	 * active one, with no tracker of its own (before SR D4), pins a fixed
	 * nominal viewer and never initialises its senses (a "no face" feed would
	 * drive it back to 2D); with a tracker per device (D4) it tracks its own
	 * camera like the active one.
	 */
	const bool segment_tracked = leia_win_sr_plan_segment_tracked(
	    &mcaps, sr.display_bound ? sr.bind_display_id : 0, sr.active_display_id);
	const bool pin_viewer = !segment_tracked &&
	                        leia_win_sr_plan_pin_simulated_viewer(sr.display_bound ? sr.bind_display_id : 0,
	                                                              sr.active_display_id);

	/*
	 * Per-display user presence (SR event 20): HERE, between the weaver
	 * create and srInitialize, because senses and their callbacks must exist
	 * before initialize starts them. Only when this instance WILL initialise:
	 * a pinned (untracked) segment has no camera of its own to report.
	 */
	if (!pin_viewer) {
		sr.presence = leia_sr_presence_monitor_create(sr.instance_v2, mcaps.display_user_presence, info.display_id,
		                                               "GL", &sr.presence_state);
	}
	if (!pin_viewer && !leia_sr_v2_initialize(sr.instance_v2)) {
		leia_sr_presence_monitor_destroy(&sr.presence);
		srDestroyWeaver(sr.weaver_v2);
		sr.weaver_v2 = nullptr;
		srDestroyInstance(sr.instance_v2);
		sr.instance_v2 = nullptr;
		return false;
	}

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
	// Multi-screen M6: a pinned (untracked) weaver has no predictor to latch
	// from, so late latching stays off there (the D3D11 arm's rule).
	if (pin_viewer) {
		(void)srWeaverEnableLateLatching(sr.weaver_v2, SR_FALSE);
	} else if (SR_SUCCEEDED(srWeaverEnableLateLatching(sr.weaver_v2, SR_TRUE))) {
		SrBool32 ll = SR_FALSE;
		if (SR_SUCCEEDED(srWeaverIsLateLatchingEnabled(sr.weaver_v2, &ll))) {
			U_LOG_W("SR %s late latching: %s", "GL",
			        ll == SR_TRUE ? "ENABLED (effective)" : "declined by the backend");
		}
	}

	/*
	 * The lens. An unbound DP (every pre-M6 caller) and a screen-bound DP on
	 * the active display take the plain (active-display) lens, as always. A
	 * DP bound to another display must NOT: that handle would switch the
	 * active panel. With SR D3 (lens per device) it binds a lens to ITS
	 * display and drives it from the runtime's 2D/3D requests itself — its
	 * EXTERNAL weaver never votes; before D3 that screen's lens is left alone.
	 */
	const bool non_active = sr.display_bound && sr.active_display_id != 0 && sr.bind_display_id != sr.active_display_id;
	const bool bind_lens = non_active && leia_win_sr_plan_bind_lens(&mcaps, &plan);
	if (!non_active) {
		leia_sr_v2_create_lens(sr.instance_v2, 0, &sr.lens_v2);
	} else if (bind_lens) {
		leia_sr_v2_create_lens(sr.instance_v2, plan.display_id, &sr.lens_v2);
		sr.lens_bound = sr.lens_v2 != nullptr;
	}
	if (non_active && segment_tracked) {
		U_LOG_W("SR GL per-screen weaver: display 0x%016llx is not the active 0x%016llx but the runtime "
		        "reports a tracker per device (D4) — tracked by its own camera, no simulated viewer, lens %s",
		        (unsigned long long)sr.bind_display_id, (unsigned long long)sr.active_display_id,
		        sr.lens_bound ? "BOUND to this display" : "left to the active panel");
	}
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
	if (pin_viewer) {
		// The SDK's own no-face viewer (0, 100, 600) mm, split by a 63 mm IPD
		// — the same pair the runtime's predicted eyes collapse onto.
		sr.pin_left_mm[0] = -31.5f;
		sr.pin_left_mm[1] = 100.0f;
		sr.pin_left_mm[2] = 600.0f;
		sr.pin_right_mm[0] = 31.5f;
		sr.pin_right_mm[1] = 100.0f;
		sr.pin_right_mm[2] = 600.0f;
		SrSimulatedViewer viewer{};
		viewer.sType = SR_TYPE_SIMULATED_VIEWER;
		viewer.pNext = nullptr;
		viewer.leftEye.x = sr.pin_left_mm[0];
		viewer.leftEye.y = sr.pin_left_mm[1];
		viewer.leftEye.z = sr.pin_left_mm[2];
		viewer.rightEye.x = sr.pin_right_mm[0];
		viewer.rightEye.y = sr.pin_right_mm[1];
		viewer.rightEye.z = sr.pin_right_mm[2];
		const SrResult vr = srWeaverSetSimulatedViewer(sr.weaver_v2, &viewer);
		sr.viewer_pinned = SR_SUCCEEDED(vr);
		U_LOG_W("SR GL per-screen weaver: display 0x%016llx is not the active 0x%016llx — simulated "
		        "nominal viewer %s, lens %s",
		        (unsigned long long)sr.bind_display_id, (unsigned long long)sr.active_display_id,
		        sr.viewer_pinned ? "PINNED" : "REFUSED (will fall back to 2D)",
		        sr.lens_bound ? "BOUND to this display (D3)"
		                      : (bind_lens ? "bind REFUSED — left to the active panel"
		                                   : "left to the active panel (runtime predates D3)"));
	}
#else
	(void)bind_lens;
#endif

	U_LOG_W("SR GL weaver created via the v2 C API%s%s%s",
	        sr.external_routed ? " (EXTERNAL routing" : "",
	        sr.external_routed ? (sr.display_bound ? ", display bound)" : ")") : "",
	        sr.needs_present_origin ? " [windowless: phase from the present origin]" : "");
	return true;
}
#endif // DXR_LEIA_HAS_SR_V2

/* ------------------------------------------------------------------ *
 * v1/v2 dispatch — see leia_sr_d3d11.cpp.
 * ------------------------------------------------------------------ */

bool
w_ready(const leiasr_gl *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		return true;
	}
#endif
	return sr->weaver != nullptr;
}

bool
lens_present(const leiasr_gl *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		return sr->lens_v2 != nullptr;
	}
#endif
	return sr->lens_hint != nullptr;
}

void
lens_set(leiasr_gl *sr, bool enable)
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
lens_is_enabled(leiasr_gl *sr)
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
 * runtime#939: how long the lens worker holds a 2D wish before applying it, so
 * a 3D wish that follows within the window replaces it and the pair collapses
 * to no lens movement at all. Every 3D->2D writer funnels through the mailbox
 * (the runtime's mode request, its teardown failsafe, the zone-mask verdict), and
 * a present-owner that has just been uncovered re-asks for 3D ~300 ms after a
 * stale 2D was replayed on its behalf -- so without this the switchable lens
 * went 3D -> 2D -> 3D on every focus trade against a second client (a visible
 * blink), and the ~16 ms 3D -> 2D -> 3D churn on every 3D entry when the mask
 * verdict races the app's own request. The window is deliberately SHORT: it
 * coalesces those bursts and is imperceptible on a real 3D -> 2D transition
 * (leaving a 3D page), which must still go flat promptly -- a long hold there
 * is a regression, not a fix. 2D -> 3D is never held.
 * DXR_LEIA_LENS_2D_DWELL_MS, default 75; 0
 * applies 2D immediately (the pre-#939 behaviour).
 */
static int64_t
lens_2d_dwell_ms()
{
	static int64_t ms = -1;
	if (ms < 0) {
		const char *e = std::getenv("DXR_LEIA_LENS_2D_DWELL_MS");
		ms = (e != nullptr && e[0] != '\0') ? atoll(e) : 75;
		if (ms < 0) {
			ms = 0;
		}
	}
	return ms;
}

/*!
 * #215: drain the lens mailbox on our own thread, then exit. Latest wish wins
 * — a burst of flips collapses to however many values we actually observe.
 *
 * The whole body is wrapped in a catch-all: an exception escaping a
 * std::thread body terminates the process, and the SR SDK throws as routine
 * control flow.
 *
 * NOTE: unlike D3D11 there is no #158 in-place reconnect worker on this arm
 * (a generation change is reported STALE and the DP is recreated), so the only
 * other toucher of the SDK objects is destroy — and that joins this worker
 * before it frees anything. lens_present() plus the try/catch still guards the
 * window between a NULL-ing and our next call.
 */
void
lens_worker_body(leiasr_gl *sr)
{
	try {
		for (;;) {
			int wish = sr->lens_mailbox.exchange(-1, std::memory_order_acq_rel);
			while (wish >= 0) {
				// runtime#939: a 2D wish waits out its dwell here; a 3D wish posted
				// meanwhile replaces it (and is applied below -- a same-state SR
				// call when the lens never left 3D, which the SR service treats as a
				// no-op). A repeated 2D neither extends nor shortens the wait, and
				// lens_quit (destroy / reconnect on the way) ends it at once.
				if (wish == 0) {
					const int64_t dwell_ms = lens_2d_dwell_ms();
					if (dwell_ms > 0 && !sr->lens_quit.load(std::memory_order_acquire)) {
						const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)dwell_ms;
						bool superseded = false;
						while (GetTickCount64() < deadline &&
						       !sr->lens_quit.load(std::memory_order_acquire)) {
							Sleep(5);
							if (sr->lens_mailbox.exchange(-1, std::memory_order_acq_rel) == 1) {
								superseded = true;
								break;
							}
						}
						if (superseded) {
							U_LOG_W("SR GL display mode: 2D wish withdrawn -- 3D re-asked inside the "
							        "%lld ms dwell, the lens is not moved through 2D (runtime #939)",
							        (long long)dwell_ms);
							wish = 1;
						}
					}
				}
				LARGE_INTEGER t0 = {}, t1 = {}, freq = {};
				QueryPerformanceFrequency(&freq);
				QueryPerformanceCounter(&t0);
				if (lens_present(sr)) {
					try {
						lens_set(sr, wish == 1);
						QueryPerformanceCounter(&t1);
						const double ms =
						    freq.QuadPart > 0
						        ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
						              (double)freq.QuadPart
						        : 0.0;
						U_LOG_W("SR GL display mode switched to %s (async lens, SR call %.1f ms)",
						        wish == 1 ? "3D" : "2D", ms);
					} catch (...) {
						U_LOG_E("Failed to switch SR GL display mode to %s (async lens)",
						        wish == 1 ? "3D" : "2D");
					}
				}
				wish = sr->lens_mailbox.exchange(-1, std::memory_order_acq_rel);
			}

			sr->lens_worker_busy.store(false, std::memory_order_release);

			// Close the race: a wish posted after our last exchange and
			// before busy dropped saw busy == true and did not spawn.
			if (sr->lens_mailbox.load(std::memory_order_acquire) < 0) {
				break;
			}
			if (sr->lens_worker_busy.exchange(true, std::memory_order_acq_rel)) {
				break; // a poster took over — it owns the mailbox now
			}
		}
	} catch (...) {
		// Nothing may escape a thread body. Drop the claim so the next wish
		// can spawn a replacement worker.
		sr->lens_worker_busy.store(false, std::memory_order_release);
		U_LOG_E("SR GL async lens worker aborted on an exception");
	}
}

/*!
 * #215: join whatever lens worker is still running and empty the mailbox.
 * Bounded by one in-flight SR call.
 */
void
lens_worker_join(leiasr_gl *sr)
{
	// runtime#939: a worker in its 2D dwell applies at once and exits, so the
	// join stays bounded by one SR call and a departing DP still lands its 2D.
	sr->lens_quit.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> g(sr->lens_thread_mtx);
		if (sr->lens_thread.joinable()) {
			sr->lens_thread.join();
		}
	}
	sr->lens_quit.store(false, std::memory_order_release);
	sr->lens_mailbox.store(-1, std::memory_order_release);
}

void
w_set_input_texture(leiasr_gl *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetInputTextureGL(sr->weaver_v2, (SrGLuint)sr->input_texture, (int32_t)sr->view_width,
		                          (int32_t)sr->view_height, (SrGLenum)sr->input_format);
		return;
	}
#endif
	sr->weaver->setInputViewTexture(static_cast<int>(sr->input_texture), static_cast<int>(sr->view_width),
	                                static_cast<int>(sr->view_height), static_cast<int>(sr->input_format));
}

void
w_weave(leiasr_gl *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
		// Multi-screen M6: a windowless EXTERNAL weaver phases from the
		// present origin + the viewport and nothing else; the runtime feeds
		// the origin every frame it weaves (a drag moves it), so apply it
		// here, right before the weave that uses it. Cheap.
		if (sr->needs_present_origin) {
			srWeaverSetPresentOrigin(sr->weaver_v2, sr->present_origin_x.load(std::memory_order_relaxed),
			                         sr->present_origin_y.load(std::memory_order_relaxed));
		}
		if (sr->viewer_pinned) {
			// Per weave, not once: see the D3D11 arm's field comment.
			SrSimulatedViewer viewer{};
			viewer.sType = SR_TYPE_SIMULATED_VIEWER;
			viewer.pNext = nullptr;
			viewer.leftEye.x = sr->pin_left_mm[0];
			viewer.leftEye.y = sr->pin_left_mm[1];
			viewer.leftEye.z = sr->pin_left_mm[2];
			viewer.rightEye.x = sr->pin_right_mm[0];
			viewer.rightEye.y = sr->pin_right_mm[1];
			viewer.rightEye.z = sr->pin_right_mm[2];
			srWeaverSetSimulatedViewer(sr->weaver_v2, &viewer);
		}
#endif
#ifdef DXR_LEIA_HAS_SR_MULTI_VIEWER
		// #446: the rendering mode's multi-viewer override (mode 2 = ON, mode 1 =
		// FOLLOW_CONFIG), applied on change only, right before the weave that
		// uses it. A weaver pinned to a simulated viewer (an untracked secondary
		// screen) has no tracked viewers to add and is left alone.
		if (!sr->viewer_pinned) {
			uint32_t mode_index = 0;
			if (leia_hmd_active_mode_get(&mode_index)) {
				leia_sr_v2_apply_multiviewer(sr->weaver_v2, mode_index, &sr->mv, "GL");
			}
		}
#endif
		srWeaverWeave(sr->weaver_v2);
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
		if (sr->viewer_pinned && !sr->pin_eyes_logged) {
			// One-shot evidence that the pin took: a collapsed pair (L == R)
			// means the weaver wove 2D, whatever srWeaverWeave returned.
			sr->pin_eyes_logged = true;
			SrPoint3f l{}, r{};
			const SrResult er = srWeaverGetPredictedEyePositions(sr->weaver_v2, &l, &r);
			U_LOG_W("SR GL per-screen weaver 0x%016llx: first weave with the pinned viewer — predicted eyes "
			        "L=(%.1f,%.1f,%.1f) R=(%.1f,%.1f,%.1f) (%s)",
			        (unsigned long long)sr->bind_display_id, l.x, l.y, l.z, r.x, r.y, r.z,
			        SR_SUCCEEDED(er) ? (fabsf(l.x - r.x) > 1.0f ? "PAIR — weaving" : "COLLAPSED — 2D fallback")
			                         : leia_sr_v2_result_str(er));
		}
#endif
		return;
	}
#endif
	/*
	 * v1 throws as routine internal control flow, and the caller above us is
	 * the runtime's C compositor, which cannot catch. An exception escaping a
	 * per-frame display-processor method is std::terminate -- the whole app, or
	 * the whole service, gone with no WER record. Every other SR SDK call in
	 * this file is already wrapped; weave() was the one that was not.
	 *
	 * Dropping the frame is the right degradation: the target keeps whatever was
	 * last presented, and the next frame retries. The log is deliberately NOT
	 * per-frame -- a throwing weaver throws every frame, and a per-frame U_LOG
	 * would bury the log it is supposed to help you read.
	 *
	 * The v2 branch above needs no guard: srWeaverWeave catches internally
	 * (SR_CATCH_ALL in the SR runtime) and reports through SrResult instead.
	 */
	try {
		sr->weaver->weave();
	} catch (...) {
		static uint64_t throws;
		if ((throws++ % 600) == 0) {
			U_LOG_E("SR GL v1 weave() threw -- frame not woven (throw #%llu; "
			        "logged 1-in-600 to stay off the per-frame path)",
			        (unsigned long long)throws);
		}
	}
}

bool
w_get_predicted_eyes(leiasr_gl *sr, float left_mm[3], float right_mm[3])
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
 * The arm's v2 `SrInstance` as an opaque pointer for the leia_sr_liveness_*
 * entry points, or NULL on the v1 path (and on a build without the v2 SDK,
 * where the member does not exist at all).
 *
 * Every liveness call site MUST pass this, stamp sites included: mixing an
 * SDK-id stamp with an SCM-derived poll would read as a restart that never
 * happened.
 */
void *
v2_instance_of(const struct leiasr_gl *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	return sr != nullptr ? static_cast<void *>(sr->instance_v2) : nullptr;
#else
	(void)sr;
	return nullptr;
#endif
}

} // namespace

extern "C" {

xrt_result_t
leiasr_gl_create(double max_time,
                  void *hwnd,
                  uint32_t view_width,
                  uint32_t view_height,
                  struct leiasr_gl **out)
{
	leiasr_gl *sr = new leiasr_gl;
	sr->view_width = view_width;
	sr->view_height = view_height;

	// Create SR context
#ifdef DXR_LEIA_HAS_SR_V2
	if (leia_sr_api_selected() == LEIA_SR_API_V2) {
		// No v1 fallback on failure — see leia_sr_d3d11.cpp.
		if (!create_v2(max_time, hwnd, *sr)) {
			U_LOG_E("SR v2 weaver creation failed - not falling back to v1 "
			        "(set DXR_LEIA_SR_API=v1 to force it)");
			delete sr;
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}
	} else
#endif
	{
		if (!create_sr_context(max_time, *sr)) {
			delete sr;
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}

		// Create GL weaver
		WeaverErrorCode result = SR::CreateGLWeaver(*sr->context,
		                                             static_cast<HWND>(hwnd),
		                                             &sr->weaver);
		if (result != WeaverErrorCode::WeaverSuccess) {
			U_LOG_E("Failed to create SR GL weaver: %d", (int)result);
			SR::SRContext::deleteSRContext(sr->context);
			delete sr;
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}

		// Initialize the context after creating the weaver.
		sr->context->initialize();

		// Set default latency (1 frame)
		sr->weaver->setLatencyInFrames(1);
	}

	// #158: stamp the SR platform incarnation we connected to, so a later poll
	// can tell "the platform restarted" from "the viewer is holding still".
	sr->platform_generation = leia_sr_liveness_platform_generation_ex(v2_instance_of(sr));

	*out = sr;

	U_LOG_I("Created GL SR weaver for HWND %p, view size %ux%u", hwnd, view_width, view_height);

	return XRT_SUCCESS;
}

xrt_result_t
leiasr_gl_create_for_screen(double max_time,
                            void *hwnd,
                            uint64_t display_id,
                            const struct leia_win_screen_origin *origin,
                            struct leiasr_gl **out)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (leia_sr_api_selected() != LEIA_SR_API_V2) {
		// The per-screen plan (EXTERNAL routing, display binding, present
		// origin) exists only on the v2 C API.
		U_LOG_W("SR GL per-screen weaver needs the v2 API (DXR_LEIA_SR_API=v1 is set) — not creating");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	leiasr_gl *sr = new leiasr_gl;
	sr->screen_bound = true;
	sr->bind_display_id = display_id;
	if (origin != nullptr) {
		sr->bind_origin = *origin;
	}
	if (!create_v2(max_time, hwnd, *sr)) {
		delete sr;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	sr->platform_generation = leia_sr_liveness_platform_generation_ex(v2_instance_of(sr));
	*out = sr;
	U_LOG_I("Created GL SR per-screen weaver for display 0x%016llx, HWND %p", (unsigned long long)display_id,
	        hwnd);
	return XRT_SUCCESS;
#else
	(void)max_time;
	(void)hwnd;
	(void)display_id;
	(void)origin;
	(void)out;
	U_LOG_W("SR GL per-screen weaver: plug-in built without the SR v2 SDK — not creating");
	return XRT_ERROR_DEVICE_CREATION_FAILED;
#endif
}

void
leiasr_gl_set_present_origin(struct leiasr_gl *leiasr, int32_t panel_x, int32_t panel_y)
{
	if (leiasr == nullptr) {
		return;
	}
	leiasr->present_origin_x.store(panel_x, std::memory_order_relaxed);
	leiasr->present_origin_y.store(panel_y, std::memory_order_relaxed);
}

bool
leiasr_gl_snap_window_rect(struct leiasr_gl *leiasr,
                           int32_t origin_x,
                           int32_t origin_y,
                           int32_t target_x,
                           int32_t target_y,
                           int32_t *out_x,
                           int32_t *out_y)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return false;
	}
#ifdef DXR_LEIA_HAS_SR_V2
	if (leiasr->weaver_v2 != nullptr) {
		return leia_sr_v2_snap_to_phase(leiasr->weaver_v2, "GL", origin_x, origin_y, target_x, target_y, out_x,
		                                out_y);
	}
#endif
	(void)origin_x;
	(void)origin_y;
	(void)target_x;
	(void)target_y;
	(void)out_x;
	(void)out_y;
	return false;
}

void
leiasr_gl_destroy(struct leiasr_gl **leiasr_ptr)
{
	if (leiasr_ptr == nullptr || *leiasr_ptr == nullptr) {
		return;
	}

	leiasr_gl *sr = *leiasr_ptr;

	// #215: the async lens worker holds a raw sr pointer — it must be gone
	// before the SDK objects it calls into are. Ahead of the v2 early-return
	// below so BOTH teardown paths are covered. On the caller's thread, but
	// bounded by one in-flight SR call (the mailbox is emptied, so the worker
	// cannot pick up more work); this arm has no async destroy to hide it in.
	lens_worker_join(sr);

	// SwitchableLensHint is managed by SRContext — do NOT delete it manually.
	sr->lens_hint = nullptr;

#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		// Destroys the caller-owned handle wrapper only; the hint underneath
		// stays context-owned. See leia_sr_d3d11.cpp.
		if (sr->lens_v2 != nullptr) {
			if (sr->lens_bound) {
				// Multi-screen M6 (SR D3): a bound lens's vote outlives the
				// handle (context-owned) — withdraw it first.
				(void)srLensDisable(sr->lens_v2);
				sr->lens_bound = false;
			}
			srDestroyLens(sr->lens_v2);
			sr->lens_v2 = nullptr;
		}
		// A sense on instance_v2: before the instance.
		leia_sr_presence_monitor_destroy(&sr->presence);
		srDestroyWeaver(sr->weaver_v2);
		sr->weaver_v2 = nullptr;

		if (sr->instance_v2 != nullptr) {
			srDestroyInstance(sr->instance_v2);
			sr->instance_v2 = nullptr;
		}

		delete sr;
		*leiasr_ptr = nullptr;
		U_LOG_I("Destroyed GL SR weaver (v2)");
		return;
	}
#endif

	// Destroy weaver
	if (sr->weaver != nullptr) {
		sr->weaver->destroy();
		sr->weaver = nullptr;
	}

	// Destroy context
	if (sr->context != nullptr) {
		SR::SRContext::deleteSRContext(sr->context);
		sr->context = nullptr;
	}

	delete sr;
	*leiasr_ptr = nullptr;

	U_LOG_I("Destroyed GL SR weaver");
}

void
leiasr_gl_set_input_texture(struct leiasr_gl *leiasr,
                             uint32_t stereo_texture,
                             uint32_t view_width,
                             uint32_t view_height,
                             uint32_t format)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return;
	}

	static uint32_t last_logged_width = 0, last_logged_height = 0;
	if (view_width != last_logged_width || view_height != last_logged_height) {
		U_LOG_I("SR GL weaver setInputViewTexture: view=%ux%u", view_width, view_height);
		last_logged_width = view_width;
		last_logged_height = view_height;
	}

	leiasr->input_texture = stereo_texture;
	leiasr->view_width = view_width;
	leiasr->view_height = view_height;
	leiasr->input_format = format;

	// Configure the weaver with the input texture
	w_set_input_texture(leiasr);
}

void
leiasr_gl_weave(struct leiasr_gl *leiasr)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		U_LOG_W("leiasr_gl_weave called with null instance or weaver");
		return;
	}

	// The weaver writes to the currently bound framebuffer
	w_weave(leiasr);
}

enum leia_sr_presence
leiasr_gl_get_user_presence(struct leiasr_gl *leiasr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (leiasr != nullptr) {
		return leia_sr_presence_load(leiasr->presence_state);
	}
#else
	(void)leiasr;
#endif
	return LEIA_SR_PRESENCE_UNKNOWN;
}

bool
leiasr_gl_get_predicted_eye_positions(struct leiasr_gl *leiasr,
                                       float out_left_eye[3],
                                       float out_right_eye[3])
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return false;
	}

	// On v1 the SR SDK throws std::runtime_error ~per frame from inside this
	// call as routine internal control flow; the catch keeps it from crossing
	// the C ABI. See [[feedback_leia_eye_pos_throws_intrinsic]]. On v2 the same
	// condition arrives as an SrResult. Both handled inside the helper.
	float left_mm[3], right_mm[3];
	if (!w_get_predicted_eyes(leiasr, left_mm, right_mm)) {
		return false;
	}

	// Convert to meters
	out_left_eye[0] = left_mm[0] / 1000.0f;
	out_left_eye[1] = left_mm[1] / 1000.0f;
	out_left_eye[2] = left_mm[2] / 1000.0f;
	out_right_eye[0] = right_mm[0] / 1000.0f;
	out_right_eye[1] = right_mm[1] / 1000.0f;
	out_right_eye[2] = right_mm[2] / 1000.0f;

	return true;
}

bool
leiasr_gl_get_display_dimensions(struct leiasr_gl *leiasr, struct leiasr_display_dimensions *out_dims)
{
	if (leiasr == nullptr || out_dims == nullptr) {
		if (out_dims != nullptr) {
			out_dims->valid = false;
		}
		return false;
	}

	if (!leiasr->display_dims_valid) {
		out_dims->valid = false;
		return false;
	}

	out_dims->width_m = leiasr->display_width_m;
	out_dims->height_m = leiasr->display_height_m;
	out_dims->valid = true;

	return true;
}

bool
leiasr_gl_get_display_pixel_info(struct leiasr_gl *leiasr,
                                  uint32_t *out_display_pixel_width,
                                  uint32_t *out_display_pixel_height,
                                  int32_t *out_display_screen_left,
                                  int32_t *out_display_screen_top,
                                  float *out_display_width_m,
                                  float *out_display_height_m)
{
	if (leiasr == nullptr || out_display_pixel_width == nullptr ||
	    out_display_pixel_height == nullptr || out_display_screen_left == nullptr ||
	    out_display_screen_top == nullptr || out_display_width_m == nullptr ||
	    out_display_height_m == nullptr) {
		return false;
	}

	if (!leiasr->display_pixel_dims_valid || !leiasr->display_dims_valid) {
		return false;
	}

	// runtime #2311: a per-screen weaver reports its binding's origin, not the
	// SR location snapshot from create (see leia_sr_multi_win.h).
	int32_t left = 0;
	int32_t top = 0;
	if (leia_win_screen_origin_pick(&leiasr->bind_origin, leiasr->display_screen_left, leiasr->display_screen_top,
	                                &left, &top) &&
	    !leiasr->bind_origin_logged.exchange(true, std::memory_order_relaxed)) {
		U_LOG_W("SR GL per-screen weaver: SR placed the panel at (%d, %d) when this weaver was created, the "
		        "runtime bound it to the screen at (%d, %d) — reporting the binding's origin (window metrics)",
		        (int)leiasr->display_screen_left, (int)leiasr->display_screen_top, (int)left, (int)top);
	}

	*out_display_pixel_width = leiasr->display_pixel_width;
	*out_display_pixel_height = leiasr->display_pixel_height;
	*out_display_screen_left = left;
	*out_display_screen_top = top;
	*out_display_width_m = leiasr->display_width_m;
	*out_display_height_m = leiasr->display_height_m;

	return true;
}

void
leiasr_gl_set_async_lens(struct leiasr_gl *leiasr, bool enabled)
{
	if (leiasr == nullptr) {
		return;
	}

	// Set once, at DP create, before any frame can call request_display_mode
	// — so a plain store is enough; no reader can be in flight yet.
	leiasr->async_lens = enabled;
}

bool
leiasr_gl_request_display_mode(struct leiasr_gl *leiasr, bool enable_3d)
{
	if (leiasr == nullptr) {
		return false;
	}

	if (!leiasr->async_lens) {
		if (!lens_present(leiasr)) {
			return false;
		}

		try {
			LARGE_INTEGER t0 = {}, t1 = {}, freq = {};
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&t0);
			lens_set(leiasr, enable_3d);
			QueryPerformanceCounter(&t1);
			const double ms = freq.QuadPart > 0
			                      ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart
			                      : 0.0;
			// The caller's thread paid for this call — say how much (#215).
			U_LOG_W("SR GL display mode switched to %s (sync lens, SR call %.1f ms)",
			        enable_3d ? "3D" : "2D", ms);
			return true;
		} catch (...) {
			U_LOG_E("Failed to switch SR GL display mode to %s", enable_3d ? "3D" : "2D");
			return false;
		}
	}

	// #215: the SR call is a round trip to the service process and the runtime
	// gets here from the compositor frame path under its own mutex. Post the
	// wish and let a short-lived worker make the call.
	if (!lens_present(leiasr)) {
		return false;
	}

	leiasr->lens_mailbox.store(enable_3d ? 1 : 0, std::memory_order_release);
	if (!leiasr->lens_worker_busy.exchange(true, std::memory_order_acq_rel)) {
		std::lock_guard<std::mutex> g(leiasr->lens_thread_mtx);
		if (leiasr->lens_thread.joinable()) {
			// The previous worker has already dropped busy → it is on its way
			// out; the join is ~immediate.
			leiasr->lens_thread.join();
		}
		try {
			leiasr->lens_thread = std::thread([leiasr]() { lens_worker_body(leiasr); });
		} catch (...) {
			// Thread creation failed: fall back to synchronous, drop busy.
			leiasr->lens_worker_busy.store(false, std::memory_order_release);
			int wish = leiasr->lens_mailbox.exchange(-1, std::memory_order_acq_rel);
			if (wish >= 0) {
				try {
					lens_set(leiasr, wish == 1);
				} catch (...) {
					return false;
				}
			}
		}
	}

	U_LOG_W("SR GL display mode wish (%s) queued — async lens worker (#215)", enable_3d ? "3D" : "2D");
	return true; // a request API — acceptance, not physical completion
}

bool
leiasr_gl_get_hardware_3d_state(struct leiasr_gl *leiasr, bool *out_is_3d)
{
	if (leiasr == nullptr || !lens_present(leiasr) || out_is_3d == nullptr) {
		return false;
	}
	try {
		*out_is_3d = lens_is_enabled(leiasr);
		return true;
	} catch (...) {
		return false;
	}
}

bool
leiasr_gl_supports_display_mode_switch(struct leiasr_gl *leiasr)
{
	if (leiasr == nullptr) {
		return false;
	}

	return lens_present(leiasr);
}

uint32_t
leiasr_gl_poll_backend_state(struct leiasr_gl *leiasr)
{
	if (leiasr == nullptr) {
		return LEIA_SR_BACKEND_OK;
	}

	// No weaver at all — the DP already degrades on its own NULL checks, and
	// there is no connection whose generation could have changed.
	if (!(w_ready(leiasr))) {
		return LEIA_SR_BACKEND_DEGRADED;
	}

	// One source for the whole poll: the SDK-reported platform id when this arm
	// has a v2 instance and the SDK is new enough, the SCM probe otherwise
	// (leia_sr_liveness.h).
	void *const v2 = v2_instance_of(leiasr);
	const uint64_t gen = leia_sr_liveness_platform_generation_ex(v2);

	// #169: taken BEFORE the "platform down" / "generation changed"
	// early-returns below — an upgrade trips both of those, so after them this
	// check never ran on the only path it describes. TELEMETRY ONLY: it logs
	// the upgrade window and deliberately leaves the reported state alone. The
	// skew is transient (it clears once a weaver is rebuilt on the new images),
	// and the #169 crash it was briefly gated on was a race against the
	// installer's file-replacement window, not a durable skew — see
	// leia_sr_d3d11.cpp's poll for the full reasoning and the hardware evidence.
	if (!leia_sr_liveness_client_matches_platform()) {
		if (!leiasr->warned_client_skew) {
			leiasr->warned_client_skew = true;
			U_LOG_W("SR platform was upgraded while this process was running (GL arm) — the "
			        "mapped SR client DLLs no longer match the installed set. This is "
			        "expected to clear once the weaver is rebuilt; if tracking does not come "
			        "back after that, restart the DisplayXR service.");
		}
	} else {
		leiasr->warned_client_skew = false;
	}

	if (gen == 0) {
		// Platform down. The weaver keeps weaving on the last known eye
		// positions, which beats no picture — do not tear anything down; the
		// generation check below fires once the service is back.
		if (!leiasr->warned_platform_down) {
			leiasr->warned_platform_down = true;
			U_LOG_W("SR platform is DOWN (GL arm) — weaving untracked");
		}
		return LEIA_SR_BACKEND_DEGRADED;
	}
	leiasr->warned_platform_down = false;

	// The SDK's own LIVENESS signal — independent of the identity comparison
	// below, which the vendor expects to keep as the belt-and-braces backup. A
	// no-op unless the SDK is new enough (DXR_LEIA_HAS_SR_CONNECTION_STATE).
	// DISCONNECTED is terminal for the instance, and this arm cannot rebuild in
	// place, so it is exactly the STALE verdict.
	//
	// TODO(#158 follow-up): SR_ERROR_PLATFORM_DISCONNECTED can also come back
	// from the per-frame eye-position path; deliberately not wired there in this
	// change — this 1 Hz poll covers it and the hot path stays untouched.
	if (leia_sr_liveness_connection_is_dead(v2)) {
		if (!leiasr->warned_stale) {
			leiasr->warned_stale = true;
			U_LOG_W("SR platform connection reported DEAD by the SDK (GL arm) — the "
			        "weaver is STALE and cannot rebuild in place; recreate the display "
			        "processor (or restart the DisplayXR service) to recover eye tracking");
		}
		return LEIA_SR_BACKEND_STALE;
	}

	// A zero stamp means the token was never readable at create time, so
	// detection is simply off rather than firing on "unknown -> known".
	if (leiasr->platform_generation != 0 && gen != leiasr->platform_generation) {
		if (!leiasr->warned_stale) {
			leiasr->warned_stale = true;
			U_LOG_W("SR platform restarted (generation %llu -> %llu) — the GL weaver "
			        "is STALE and cannot rebuild in place; recreate the display processor "
			        "(or restart the DisplayXR service) to recover eye tracking",
			        (unsigned long long)leiasr->platform_generation, (unsigned long long)gen);
		}
		return LEIA_SR_BACKEND_STALE;
	}

	// The skew check that used to sit here is now taken right after the
	// generation read — see the #169 block above. It logs; it never changes
	// the state reported from here.

	return LEIA_SR_BACKEND_OK;
}

} // extern "C"
