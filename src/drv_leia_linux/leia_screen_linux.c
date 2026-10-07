// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Per-DP screen state (multi-screen M4) — see leia_screen_linux.h.
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#include "leia_screen_linux.h"
#include "leia_sr_linux.h" // leiasr_lnx_display_info

#include <stdio.h>
#include <string.h>

//! Fallback nominal viewing distance when nothing better is known (m).
#define LEIA_LNX_SCREEN_DEFAULT_VIEWER_Z_M 0.65f

static const struct leia_lnx_edid_panel *
find_panel(const struct leia_lnx_screen_binding *b,
           const struct leia_lnx_claim_binding *claim,
           const struct leia_lnx_edid_panel *panels,
           uint32_t panel_count)
{
	if (panels == NULL || panel_count == 0) {
		return NULL;
	}
	// 1. The binding's OS output name: a DRM connector, or (X11) a RandR
	//    output — matched against both spellings the panel list carries.
	//    The runtime resolved it for THIS monitor, so it outranks the claim
	//    table (review B: twins paired by list order could swap connectors).
	if (b->device_name[0] != '\0') {
		for (uint32_t i = 0; i < panel_count; i++) {
			if (strcmp(panels[i].connector, b->device_name) == 0 ||
			    (panels[i].randr_output[0] != '\0' &&
			     strcmp(panels[i].randr_output, b->device_name) == 0)) {
				return &panels[i];
			}
		}
	}
	// 2. The claim's DRM connector from probe_displays.
	if (claim != NULL && claim->connector[0] != '\0') {
		for (uint32_t i = 0; i < panel_count; i++) {
			if (strcmp(panels[i].connector, claim->connector) == 0) {
				return &panels[i];
			}
		}
	}
	// 3. The panel sitting at the binding's desktop origin.
	for (uint32_t i = 0; i < panel_count; i++) {
		if (panels[i].has_position && panels[i].left == b->desktop_left && panels[i].top == b->desktop_top) {
			return &panels[i];
		}
	}
	return NULL;
}

void
leia_lnx_screen_resolve(const struct leia_lnx_screen_binding *b,
                        const struct leia_lnx_claim_binding *claim,
                        const struct leia_lnx_edid_panel *panels,
                        uint32_t panel_count,
                        const struct leiasr_lnx_display_info *sr,
                        uint64_t sr_active_display_id,
                        struct leia_lnx_screen *out)
{
	memset(out, 0, sizeof(*out));
	if (b == NULL) {
		return;
	}
	const bool have_sr = sr != NULL && sr->valid;
	const struct leia_lnx_edid_panel *p = find_panel(b, claim, panels, panel_count);

	out->monitor_id = b->monitor_id;
	if (p != NULL) {
		snprintf(out->connector, sizeof(out->connector), "%s", p->connector);
	}

	// The SR display id: the runtime's binding (opaque, plug-in supplied
	// upstream), else the plug-in's own probe_displays table.
	out->sr_display_id = b->display_id != 0 ? b->display_id : (claim != NULL ? claim->sr_display_id : 0);

	// Origin: always the binding's — the runtime placed the screen on the
	// desktop and every window rect it hands this DP is in that frame.
	out->screen_left = b->desktop_left;
	out->screen_top = b->desktop_top;

	// Pixels: the device mode (weave pixels are device pixels), else the
	// panel's EDID native mode, else the desktop size.
	if (b->native_width != 0 && b->native_height != 0) {
		out->pixel_width = b->native_width;
		out->pixel_height = b->native_height;
	} else if (p != NULL && p->native_w != 0 && p->native_h != 0) {
		out->pixel_width = p->native_w;
		out->pixel_height = p->native_h;
	} else {
		out->pixel_width = b->desktop_width;
		out->pixel_height = b->desktop_height;
	}

	// Is this the panel the process-wide SR display describes? By id when
	// both sides know one; otherwise SR 1.38's one-panel rule (the first
	// panel in connector order, the same panel the pre-M4 "first panel"
	// caches resolved); with no panel list at all (forced probe, no EDID
	// match) the single-panel assumption stands.
	if (have_sr) {
		if (out->sr_display_id != 0 && sr_active_display_id != 0) {
			out->is_sr_panel = out->sr_display_id == sr_active_display_id;
		} else if (p != NULL) {
			out->is_sr_panel = p == &panels[0];
		} else {
			out->is_sr_panel = panel_count == 0;
		}
	}

	if (out->is_sr_panel) {
		// The same numbers get_display_info reports (SR's calibrated size,
		// already EDID-corrected by the backend when SR answers its built-in
		// default), so the system answer and this screen's agree.
		out->width_m = sr->width_m;
		out->height_m = sr->height_m;
		out->nominal_viewer_x_m = sr->nominal_viewer_x_m;
		out->nominal_viewer_y_m = sr->nominal_viewer_y_m;
		out->nominal_viewer_z_m = sr->nominal_viewer_z_m;
		out->recommended_view_width = sr->recommended_view_width;
		out->recommended_view_height = sr->recommended_view_height;
		out->refresh_mhz = sr->refresh_mhz;
		if (out->pixel_width == 0 || out->pixel_height == 0) {
			out->pixel_width = sr->pixel_width;
			out->pixel_height = sr->pixel_height;
		}
	} else {
		// A Leia panel the SR context does not drive (a second panel: LeiaSR
		// phase D), or no SR at all: physical facts only.
		if (b->width_mm != 0 && b->height_mm != 0) {
			out->width_m = (float)b->width_mm / 1000.0f;
			out->height_m = (float)b->height_mm / 1000.0f;
		} else if (p != NULL && p->width_mm != 0 && p->height_mm != 0) {
			out->width_m = (float)p->width_mm / 1000.0f;
			out->height_m = (float)p->height_mm / 1000.0f;
		}
		// Centred viewer at the SR panel's distance-to-height ratio (SR's
		// own design ratio), else a desktop default.
		float z = LEIA_LNX_SCREEN_DEFAULT_VIEWER_Z_M;
		if (have_sr && sr->height_m > 0.0f && sr->nominal_viewer_z_m > 0.0f && out->height_m > 0.0f) {
			z = sr->nominal_viewer_z_m * (out->height_m / sr->height_m);
		}
		out->nominal_viewer_z_m = z;
	}

	out->valid = out->width_m > 0.0f && out->height_m > 0.0f && out->pixel_width != 0 && out->pixel_height != 0;
}
