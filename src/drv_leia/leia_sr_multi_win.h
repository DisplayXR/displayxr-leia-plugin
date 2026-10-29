// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Multi-screen M6 (Windows): the per-screen SR weaver PLAN — which
 *         pNext structs to chain on a weaver create-info, decided from the SR
 *         runtime's capabilities and the DP's screen binding. Plain C, no SDK
 *         types, so the decision is host-testable (tests/test_sr_multi_win.c)
 *         and the SDK-typed chain builder (leia_sr_d3d11.cpp) stays trivial.
 *
 * The Windows twin of drv_leia_linux/leia_sr_routing_linux.h, plus two
 * Windows-only inputs: whether this DP holds the session's real HWND, and
 * whether that HWND is a child window. The DP with a top-level HWND (the
 * screen holding the majority of the window) keeps the SDK's drag phase-snap
 * through KEEP_DRAG_SNAP; every other screen's DP is windowless and gets its
 * phase from set_present_origin. A DP whose HWND is a child window (the Unity
 * editor's docked Game view) never lets the SDK subclass: see
 * leia_win_sr_plan_weaver.
 * @ingroup drv_leia
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! What srGetRuntimeCapabilities answered (routing + binding caps chained).
struct leia_win_sr_multi_caps
{
	//! The query ran and succeeded. False = unknown: the SDK this plug-in was
	//! built against predates the structs, or the query failed.
	bool known;
	//! SrWeaverRoutingCapabilities::externalRouting.
	bool external_routing;
	//! SrWeaverRoutingCapabilities::supportedFlags carries KEEP_DRAG_SNAP.
	bool keep_drag_snap;
	//! SrDisplayBindingCapabilities::displayBinding.
	bool display_binding;
	//! SrDisplayBindingCapabilities::maxBoundDisplays (the live bindable count).
	uint32_t max_bound_displays;
	//! SrLensBindingCapabilities::lensPerDevice (SR D3): a lens bound to a
	//! display drives THAT display's FPC, whichever display is active.
	bool lens_per_device;
	//! SrEyeTrackerBindingCapabilities::eyeTrackerPerDevice (SR D4): a weaver
	//! bound to a calibrated non-active display tracks that display's own
	//! camera (EXTERNAL included); false while the service has no tracker
	//! for it (gate off, old service).
	bool eye_tracker_per_device;
	//! SrDisplayUserPresenceCapabilities::displayUserPresenceEvent (LeiaSR q4
	//! line): the runtime raises SR_EVENT_TYPE_DISPLAY_USER_PRESENCE per
	//! display with its display= id (leia_sr_presence.h).
	bool display_user_presence;
};

//! The chain to build on the weaver create-info.
struct leia_win_sr_weaver_plan
{
	//! Chain SrWeaverRoutingInfo{mode = SR_WEAVER_ROUTING_EXTERNAL}.
	bool routing_external;
	//! ...with SR_WEAVER_ROUTING_KEEP_DRAG_SNAP_BIT (only with a real window).
	bool keep_drag_snap;
	//! Chain SrDisplayBindingInfo{displayId = @ref display_id}.
	bool bind_display;
	uint64_t display_id;
};

/*!
 * Decide the chain.
 *
 * A child window (displayxr-unity #428/#429): with SDK routing, or with
 * KEEP_DRAG_SNAP, the SDK subclasses the window's top-level ancestor
 * (GA_ROOT), i.e. the host application's main window. For the Unity editor's
 * docked Game view that is the editor's main window. That subclass does not
 * chain safely with the host's own subclasses (SR 1.37-1.39): a crash on the
 * second docked Play, and a lost File > Exit. EXTERNAL routing without
 * KEEP_DRAG_SNAP installs nothing, so such a window gets exactly that, on an
 * unbound or a screen-bound DP, whenever the runtime honours EXTERNAL.
 *
 * What the weaver then no longer does for that window (sr_weaver.h, "External
 * display routing"): the drag phase-snap while the host's main window is
 * dragged, the on-panel gate and per-monitor 2D/3D split (the runtime's
 * per-screen segments split a canvas that spans screens), and the lens vote
 * (the DP drives the lens from the runtime's 2D/3D requests). The phase at
 * rest is unchanged: every v2 backend still reads the window term from the
 * window it was given and lets a present origin replace it only when the
 * caller set one (dx11weaver2.cpp, dx12weaver2.cpp, glweaver2.cpp,
 * vkweaver.cpp: getUserPresentOrigin), which this DP does not do with a window
 * (leia_win_sr_plan_needs_present_origin). sr_weaver.h's "phase from the
 * present origin only" describes the windowless case. A top-level window keeps
 * today's plan.
 *
 * @param caps          The runtime's answer (unknown ⟹ an empty plan: SDK
 *                      routing, no binding — exactly today's single-screen DP).
 * @param screen_bound  This DP was created for one screen
 *                      (create_dp_d3d11_for_screen). An unbound DP — every
 *                      pre-M6 caller — keeps SDK routing unless its window is
 *                      a child window, so one-screen behaviour with a top-level
 *                      window is byte-identical.
 * @param has_window    The DP holds the session's real HWND.
 * @param is_child      That HWND is a child window whose top-level window
 *                      (GA_ROOT) this process owns: the one the SDK would
 *                      subclass (leia_sr_v2_hwnd_is_own_child). Ignored
 *                      without a window.
 * @param display_id    The SR display to bind (0 = none / unknown).
 */
static inline struct leia_win_sr_weaver_plan
leia_win_sr_plan_weaver(const struct leia_win_sr_multi_caps *caps,
                        bool screen_bound,
                        bool has_window,
                        bool is_child,
                        uint64_t display_id)
{
	struct leia_win_sr_weaver_plan p = {0};
	if (caps == NULL || !caps->known) {
		return p;
	}
	const bool child_window = has_window && is_child;
	if (!screen_bound) {
		// Only to keep the SDK off the host's top-level window; no binding.
		p.routing_external = child_window && caps->external_routing;
		return p;
	}
	p.routing_external = caps->external_routing;
	// KEEP_DRAG_SNAP only matters with a real window: the SDK installs its
	// WndProc phase-snap only there, and a windowless weaver installs nothing.
	// Never for a child window: the snap is installed on its GA_ROOT.
	p.keep_drag_snap = p.routing_external && has_window && !child_window && caps->keep_drag_snap;
	if (display_id != 0 && caps->display_binding && caps->max_bound_displays >= 1) {
		p.bind_display = true;
		p.display_id = display_id;
	}
	return p;
}

/*!
 * After a refused create, the next thing to try: drop the binding first (the
 * runtime may refuse an EDID-only or unknown id), then routing. Returns false
 * when nothing is left to drop — the caller then fails the create.
 */
static inline bool
leia_win_sr_plan_weaver_fallback(struct leia_win_sr_weaver_plan *p)
{
	if (p->bind_display) {
		p->bind_display = false;
		p->display_id = 0;
		return true;
	}
	if (p->routing_external) {
		p->routing_external = false;
		p->keep_drag_snap = false;
		return true;
	}
	return false;
}

/*!
 * Does this DP's weaver need the runtime's present origin for its phase? An
 * EXTERNAL-routed weaver without a window reads nothing else; a weaver with
 * the real window derives its phase from the window (and the caller must NOT
 * override it, or the SDK's drag snap and the origin disagree).
 */
static inline bool
leia_win_sr_plan_needs_present_origin(const struct leia_win_sr_weaver_plan *p, bool has_window)
{
	return p->routing_external && !has_window;
}

/*!
 * Should a screen-bound DP create a lens bound to ITS display and drive it
 * (SR D3)? Only with the display bound and the runtime reporting lens per
 * device: on an older runtime a bind to a non-active display is refused, and
 * an unbound lens handle would switch the ACTIVE panel — the wrong one. An
 * EXTERNAL weaver never votes, so without this the segment's lens stays 2D.
 */
static inline bool
leia_win_sr_plan_bind_lens(const struct leia_win_sr_multi_caps *caps, const struct leia_win_sr_weaver_plan *p)
{
	return caps != NULL && caps->known && caps->lens_per_device && p->bind_display && p->display_id != 0;
}

/*!
 * Does this segment have a tracker of its own (SR D4)? The active display
 * always does (the primary feed). Any other display does once the runtime
 * reports eyeTrackerPerDevice: its bound weaver then tracks that display's
 * camera automatically, so the DP must NOT pin a simulated viewer (an explicit
 * pin overrides tracking), must initialise its instance (the senses feed the
 * tracker) and may keep late latching.
 *
 * @param caps               The runtime's answer.
 * @param bound_display_id   The display the weaver was bound to (0 = unbound).
 * @param active_display_id  The runtime's active display (0 = unknown).
 */
static inline bool
leia_win_sr_plan_segment_tracked(const struct leia_win_sr_multi_caps *caps,
                                 uint64_t bound_display_id,
                                 uint64_t active_display_id)
{
	if (bound_display_id == 0 || active_display_id == 0 || bound_display_id == active_display_id) {
		return true; // the active display, or unknown: today's primary feed
	}
	return caps != NULL && caps->known && caps->eye_tracker_per_device;
}

/*!
 * Should this DP pin a simulated viewer? David's decision (2026-10-07): an
 * untracked screen renders from its own nominal viewer. The SR tracker follows
 * the ACTIVE display only (until SR D4), so a weaver bound to any other
 * display sees no face and would fall back to 2D every frame.
 *
 * @param bound_display_id   The display the weaver was bound to (0 = unbound).
 * @param active_display_id  What the runtime reports as its active display
 *                           (0 = unknown ⟹ never pin: a wrong pin on the
 *                           tracked panel would weave with no viewer).
 */
static inline bool
leia_win_sr_plan_pin_simulated_viewer(uint64_t bound_display_id, uint64_t active_display_id)
{
	return bound_display_id != 0 && active_display_id != 0 && bound_display_id != active_display_id;
}

/*!
 * The desktop origin a per-screen DP reports (`get_display_pixel_info`, and
 * on the Vulkan arm its own window metrics). The runtime builds the window
 * metrics, and so the window-relative Kooima eye offset, from it.
 *
 * The runtime's per-screen contract (xrt_plugin.h, create_dp_vk_for_screen,
 * which the d3d11/d3d12/gl variants inherit) says this origin is the
 * binding's desktop origin. The SR location read at weaver create is a
 * snapshot of whatever Windows listed at that moment: on a DS1 HDMI re-seat
 * Windows briefly lists the panel alone, as primary, at (0, 0), and a weaver
 * created inside that window kept (0, 0) for its whole life after the panel
 * was back at (-3840, 0) — the eye offset was off by one panel width for 36 s
 * (runtime #2311, -0.458 m instead of -0.114 m).
 *
 * `bound` is out of band on purpose: (0, 0) is a real origin (a panel that is
 * the primary display), not "no value".
 */
struct leia_win_screen_origin
{
	//! A per-screen DP whose binding carried a desktop rect.
	bool bound;
	//! The binding's desktop origin (device px, the space windows are placed in).
	int32_t left;
	int32_t top;
};

/*!
 * Seed from a screen binding (`xrt_screen_binding::desktop_*`). An empty rect
 * leaves the origin unbound, so the DP keeps reporting the SR location.
 */
static inline struct leia_win_screen_origin
leia_win_screen_origin_from_binding(int32_t desktop_left,
                                    int32_t desktop_top,
                                    uint32_t desktop_width,
                                    uint32_t desktop_height)
{
	struct leia_win_screen_origin o = {false, 0, 0};
	if (desktop_width == 0 || desktop_height == 0) {
		return o;
	}
	o.bound = true;
	o.left = desktop_left;
	o.top = desktop_top;
	return o;
}

/*!
 * The origin to report: the binding's for a per-screen DP, else the SR
 * location (a window-bound DP, or a binding without a rect: unchanged).
 *
 * @param o        This DP's origin (NULL = unbound).
 * @param sr_left  The SR location cached at weaver create.
 * @param sr_top   ditto.
 * @return true when the DP is bound and the SR location disagrees with the
 *         binding (the case this fixes); the caller logs it once.
 */
static inline bool
leia_win_screen_origin_pick(const struct leia_win_screen_origin *o,
                            int32_t sr_left,
                            int32_t sr_top,
                            int32_t *out_left,
                            int32_t *out_top)
{
	if (o == NULL || !o->bound) {
		*out_left = sr_left;
		*out_top = sr_top;
		return false;
	}
	*out_left = o->left;
	*out_top = o->top;
	return sr_left != o->left || sr_top != o->top;
}

#ifdef __cplusplus
}
#endif
