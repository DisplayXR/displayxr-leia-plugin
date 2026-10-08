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
 * The Windows twin of drv_leia_linux/leia_sr_routing_linux.h, plus the one
 * Windows-only input: whether this DP holds the session's real HWND. The DP
 * with the HWND (the screen holding the majority of the window) keeps the
 * SDK's drag phase-snap through KEEP_DRAG_SNAP; every other screen's DP is
 * windowless and gets its phase from set_present_origin.
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
struct leia_win_sr_multi_caps {
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
};

//! The chain to build on the weaver create-info.
struct leia_win_sr_weaver_plan {
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
 * @param caps          The runtime's answer (unknown ⟹ an empty plan: SDK
 *                      routing, no binding — exactly today's single-screen DP).
 * @param screen_bound  This DP was created for one screen
 *                      (create_dp_d3d11_for_screen). An unbound DP — every
 *                      pre-M6 caller — never changes routing: its weaver stays
 *                      in SDK mode so one-screen behaviour is byte-identical.
 * @param has_window    The DP holds the session's real HWND.
 * @param display_id    The SR display to bind (0 = none / unknown).
 */
static inline struct leia_win_sr_weaver_plan
leia_win_sr_plan_weaver(const struct leia_win_sr_multi_caps *caps,
                        bool screen_bound, bool has_window,
                        uint64_t display_id) {
  struct leia_win_sr_weaver_plan p = {0};
  if (caps == NULL || !caps->known || !screen_bound) {
    return p;
  }
  p.routing_external = caps->external_routing;
  // KEEP_DRAG_SNAP only matters with a real window: the SDK installs its
  // WndProc phase-snap only there, and a windowless weaver installs nothing.
  p.keep_drag_snap = p.routing_external && has_window && caps->keep_drag_snap;
  if (display_id != 0 && caps->display_binding &&
      caps->max_bound_displays >= 1) {
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
leia_win_sr_plan_weaver_fallback(struct leia_win_sr_weaver_plan *p) {
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
leia_win_sr_plan_needs_present_origin(const struct leia_win_sr_weaver_plan *p,
                                      bool has_window) {
  return p->routing_external && !has_window;
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
leia_win_sr_plan_pin_simulated_viewer(uint64_t bound_display_id,
                                      uint64_t active_display_id) {
  return bound_display_id != 0 && active_display_id != 0 &&
         bound_display_id != active_display_id;
}

#ifdef __cplusplus
}
#endif
