// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Per-DP screen state for the Linux arm (multi-screen M4): what ONE
 *         display processor answers for the screen it was created for —
 *         pure logic, no SDK, no Vulkan, no I/O.
 *
 * The runtime's `create_dp_vk_for_screen` slot (multi-screen M2, ADR-047 D2)
 * hands each DP the screen it serves (`struct xrt_screen_binding`): monitor
 * id, desktop rect, native px, EDID mm, serial, vendor display id. Before M4
 * every DP answered `get_display_dimensions` / `get_display_pixel_info` from
 * the process-wide SR display and the "first panel" RandR position, which is
 * one panel by construction. Here each DP resolves its own screen ONCE, at
 * creation, into a @ref leia_lnx_screen it owns, and its getters read only
 * that — so two DPs in one process can never answer for each other.
 *
 * The runtime struct only exists in runtime headers that carry
 * XRT_PLUGIN_IFACE_HAS_CREATE_DP_FOR_SCREEN; @ref leia_lnx_screen_binding is
 * its plain mirror so this module (and its test) build against any pinned
 * runtime. leia_plugin_linux.c converts, honouring the runtime's struct_size.
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#pragma once

#include "leia_display_claims_linux.h" // leia_lnx_claim_binding
#include "leia_edid_probe_linux.h"     // leia_lnx_edid_panel

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct leiasr_lnx_display_info; // leia_sr_linux.h

/*! Plain mirror of the runtime's `struct xrt_screen_binding` (0 / "" = unknown). */
struct leia_lnx_screen_binding
{
	uint64_t monitor_id;
	int32_t desktop_left, desktop_top;
	uint32_t desktop_width, desktop_height;
	uint32_t native_width, native_height; //!< the connector's device mode
	uint32_t width_mm, height_mm;         //!< EDID physical size
	float desktop_scale;
	uint64_t display_id; //!< vendor (SR) display id, 0 = none
	char serial[64];
	char device_name[64]; //!< RandR output (X11) or DRM connector
};

/*! What one DP answers for its screen. Owned by the DP; never shared. */
struct leia_lnx_screen
{
	bool valid;
	uint64_t monitor_id;
	char connector[32]; //!< DRM connector of the matched Leia panel ("" = none matched)

	//! The SR display this screen is (0 = unknown — SR 1.38, or not listed).
	uint64_t sr_display_id;
	//! This screen IS the panel the process-wide SR display describes (the
	//! one panel the SR instance/tracker/lens drive). Only then are the SR
	//! nominal viewer, recommended view size and eye tracking this screen's.
	bool is_sr_panel;

	float width_m, height_m;
	uint32_t pixel_width, pixel_height;
	int32_t screen_left, screen_top; //!< desktop origin (the binding's)

	float nominal_viewer_x_m, nominal_viewer_y_m, nominal_viewer_z_m;
	uint32_t recommended_view_width, recommended_view_height; //!< 0 = unknown
	uint32_t refresh_mhz;                                     //!< 0 = unknown
};

/*!
 * Resolve one screen.
 *
 * @param b            The screen (never NULL).
 * @param claim        The plug-in's probe_displays claim for b->monitor_id, or NULL.
 * @param panels       The EDID panel list (leia_lnx_edid_enumerate_panels, sorted by connector).
 * @param panel_count  Entries in @p panels.
 * @param sr           The process-wide SR display query (valid = false / NULL when none).
 * @param sr_active_display_id  The displayId the process-wide SR display describes
 *                     (the FPC-verified one), 0 when unknown.
 * @param[out] out     Filled; out->valid says whether a usable size + resolution resulted.
 *
 * Rules (each documented at its line in leia_screen_linux.c):
 *   - origin: always the binding's desktop rect (the runtime placed the screen);
 *   - panel: the claim's connector, else the binding's device name (DRM
 *     connector or RandR output), else the panel at the binding's origin;
 *   - pixels: binding native mode, else the panel's EDID native, else the desktop size;
 *   - SR panel: by displayId when both ids are known, else "the first panel"
 *     (connector order — the one SR 1.38 drives on a one-panel box), else
 *     (no panel list at all, a forced probe) assumed;
 *   - size / viewer / recommended view / refresh: the SR query's for the SR
 *     panel (the same numbers get_display_info reports, so the per-screen
 *     and the system answers cannot disagree), else binding mm / EDID mm with
 *     a centred viewer at the SR panel's distance-to-height ratio.
 */
void
leia_lnx_screen_resolve(const struct leia_lnx_screen_binding *b,
                        const struct leia_lnx_claim_binding *claim,
                        const struct leia_lnx_edid_panel *panels,
                        uint32_t panel_count,
                        const struct leiasr_lnx_display_info *sr,
                        uint64_t sr_active_display_id,
                        struct leia_lnx_screen *out);

#ifdef __cplusplus
}
#endif
