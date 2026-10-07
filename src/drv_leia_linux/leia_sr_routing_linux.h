// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  What to chain on an SR weaver's create-info (multi-screen M4):
 *         weaver routing (SrWeaverRoutingInfo) and display binding
 *         (SrDisplayBindingInfo) — the decision, kept free of SR types.
 *
 * The new SR API (LeiaSR Linux line 876620d62, phases A-C) lets a display
 * controller (a) turn off the weaver's own window/monitor detection —
 * SR_WEAVER_ROUTING_EXTERNAL: weave whatever it is given, phase = present
 * origin + viewport only, no lens vote, no polling — and (b) bind a weaver to
 * one SR display by the `displayId` srEnumerateDisplays reported. Both are
 * gated on what the INSTALLED SR runtime says it honours
 * (SrWeaverRoutingCapabilities / SrDisplayBindingCapabilities chained on
 * SrRuntimeCapabilities); a runtime that predates them (the 1.38 .deb) leaves
 * the caps at "not supported" and gets the plain create-info, exactly as
 * before.
 *
 * Pure, header-only: tests/test_sr_routing_linux.c checks the plan on any box;
 * leia_sr_linux_sdk.c turns it into the actual pNext chain
 * (leia_sr_chain_sdk_linux.h, tested where the SDK headers exist).
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! What the installed SR runtime honours (SR types reduced to plain C). */
struct leia_lnx_sr_multi_caps
{
	//! The capability query ran and succeeded. False = unknown: the SDK this
	//! plug-in was built against predates the structs, or the query failed.
	bool known;
	//! SrWeaverRoutingCapabilities::externalRouting.
	bool external_routing;
	//! SrDisplayBindingCapabilities::displayBinding.
	bool display_binding;
	//! SrDisplayBindingCapabilities::maxBoundDisplays (1 today).
	uint32_t max_bound_displays;
};

/*! The chain one weaver create gets. */
struct leia_lnx_sr_weaver_plan
{
	//! Chain SrWeaverRoutingInfo{mode = SR_WEAVER_ROUTING_EXTERNAL, flags = 0}.
	//! No KEEP_DRAG_SNAP on Linux: the bit only matters with a real window,
	//! and the Linux weaver is windowless (drag phase-snap is the runtime's
	//! snap_window_rect slot -> srWeaverSnapToPhase, independent of routing).
	bool routing_external;
	//! Chain SrDisplayBindingInfo{displayId = @ref display_id}.
	bool bind_display;
	uint64_t display_id;
};

/*!
 * Decide the weaver chain.
 *
 * @param caps          What the SR runtime honours (NULL = unknown).
 * @param want_external The plug-in's wish (DXR_LEIA_SR_EXTERNAL_ROUTING, default on).
 * @param display_id    The SR displayId this weaver is for (0 = none known:
 *                      SR 1.38, a monitor SR does not list, or a DP created
 *                      through the plain factory).
 */
static inline struct leia_lnx_sr_weaver_plan
leia_lnx_sr_plan_weaver(const struct leia_lnx_sr_multi_caps *caps, bool want_external, uint64_t display_id)
{
	struct leia_lnx_sr_weaver_plan p = {0};
	if (caps == NULL || !caps->known) {
		return p;
	}
	p.routing_external = want_external && caps->external_routing;
	// Binding needs the runtime to honour it AND to bind at least one display
	// (maxBoundDisplays is 1 today: only the FPC-verified display binds).
	if (display_id != 0 && caps->display_binding && caps->max_bound_displays >= 1) {
		p.bind_display = true;
		p.display_id = display_id;
	}
	return p;
}

/*!
 * The plan for the retry after a create with @p tried failed: drop the
 * display binding first (SR refuses an EDID-only id with
 * SR_ERROR_DEVICE_NOT_AVAILABLE and an unknown one with
 * SR_ERROR_DISPLAY_NOT_FOUND — the active display is then the right
 * fallback), then the routing. @return false when there is nothing left to
 * drop (the plain create already failed: give up as before).
 */
static inline bool
leia_lnx_sr_plan_weaver_fallback(const struct leia_lnx_sr_weaver_plan *tried, struct leia_lnx_sr_weaver_plan *out)
{
	*out = *tried;
	if (out->bind_display) {
		out->bind_display = false;
		out->display_id = 0;
		return true;
	}
	if (out->routing_external) {
		out->routing_external = false;
		return true;
	}
	return false;
}

#ifdef __cplusplus
}
#endif
