// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Plug-in entry point for the Leia display driver — macOS arm
 *         (srSDK Metal weaver).
 *
 * Implements @ref xrt_plugin_negotiate_fn_t from `xrt/xrt_plugin.h`. The
 * runtime dlopens this dylib through JSON-manifest discovery
 * (`050-leia-sr.json` in an `XRT_PLUGIN_SEARCH_PATH` dir or a
 * `DisplayProcessors/` root) and resolves exactly one symbol,
 * `xrtPluginNegotiate` (`-exported_symbol`, ADR-019).
 *
 * Probe policy: bind when the SR runtime answers and reports a valid SR
 * display (leia_mac_sr_probe — one short-lived CLIENT instance). No SR runtime,
 * no SRService, or no panel = decline, so a Mac without Leia hardware falls
 * through to sim_display. `DXR_LEIA_FORCE_PROBE=1` binds regardless (bring-up).
 *
 * Panel selection: the SR display is the panel; it is matched to a
 * CGDirectDisplayID by EDID (vendor, product) against the frozen Leia table,
 * then by NSScreen name, then by native resolution (leia_mac_find_panel_display).
 * probe_displays claims the runtime monitor descriptor with the same EDID pair
 * (VERIFIED when SR also answered), else falls back to the descriptor sitting
 * where the panel is.
 *
 * @ingroup drv_leia
 */

#include "xrt/xrt_config_os.h"

#if !defined(XRT_OS_MACOS)
#error "drv_leia_macos is macOS only."
#endif

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"

#include "util/u_debug.h"
#include "util/u_logging.h"

#include "leia_interface.h"
#include "leia_sr_macos.h"
#include "leia_display_processor_macos.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

DEBUG_GET_ONCE_BOOL_OPTION(leia_force_probe, "DXR_LEIA_FORCE_PROBE", false)

//! probe() bound this plug-in. probe_displays then guarantees it a monitor.
static bool g_probe_bound;


/*
 *
 * leia_interface.h probe cache — consumed by the shared leia_device.c.
 *
 */

bool
leiasr_probe_display(double timeout_seconds)
{
	(void)timeout_seconds;
	return leia_mac_sr_probe();
}

bool
leiasr_get_probe_results(struct leiasr_probe_result *out)
{
	struct leia_mac_display_info info;
	if (out == NULL || !leia_mac_get_display_info(&info) || !info.valid) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->hw_found = true;
	out->pixel_w = info.pixel_width;
	out->pixel_h = info.pixel_height;
	out->refresh_hz = (float)info.refresh_mhz / 1000.0f;
	out->display_w_m = info.width_m;
	out->display_h_m = info.height_m;
	out->nominal_z_m = info.nominal_z_m;
	return true;
}


/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
leia_mac_plugin_probe(struct xrt_plugin_instance **out_inst)
{
	*out_inst = NULL;
	const bool forced = debug_get_bool_option_leia_force_probe();
	const bool sr_ok = leia_mac_sr_probe();
	if (!sr_ok && !forced) {
		U_LOG_W("leia_mac_plugin: probe declined — no SR runtime / SRService / SR display "
		        "(set SR_RUNTIME_PATH for a dev SR tree; DXR_LEIA_FORCE_PROBE=1 force-binds)");
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	struct leia_mac_display_info info;
	if (leia_mac_get_display_info(&info) && info.valid) {
		// Seed the ONE per-view-scale derivation before create_device writes
		// the mode table (same reason as the Linux arm).
		leia_view_scale_set_from_dims(info.rec_view_width, info.rec_view_height, info.pixel_width,
		                              info.pixel_height);
	}
	U_LOG_W("leia_mac_plugin: %s", sr_ok ? "SR display found — binding" : "probe FORCED (DXR_LEIA_FORCE_PROBE=1)");
	g_probe_bound = true;
	return XRT_SUCCESS;
}

static xrt_result_t
leia_mac_plugin_create_device(struct xrt_plugin_instance *inst, struct xrt_device **out_dev)
{
	(void)inst;
	struct xrt_device *xdev = leia_hmd_create(); // shared ../drv_leia/leia_device.c
	if (xdev == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	*out_dev = xdev;
	return XRT_SUCCESS;
}

static void
leia_mac_plugin_destroy(struct xrt_plugin_instance *inst)
{
	(void)inst;
}

static void
leia_mac_plugin_set_pose_source(struct xrt_plugin_instance *inst, struct xrt_device *xdev, struct xrt_device *source)
{
	(void)inst;
	leia_hmd_set_pose_source(xdev, source);
}

static void
fill_display_info(const struct leia_mac_display_info *info, struct xrt_plugin_display_info *out_info)
{
	out_info->display_width_m = info->width_m;
	out_info->display_height_m = info->height_m;
	out_info->nominal_viewer_x_m = info->nominal_x_m;
	out_info->nominal_viewer_y_m = info->nominal_y_m;
	out_info->nominal_viewer_z_m = info->nominal_z_m;
	out_info->display_pixel_width = info->pixel_width;
	out_info->display_pixel_height = info->pixel_height;
	leia_view_scale_get(&out_info->recommended_view_scale_x, &out_info->recommended_view_scale_y);
	// macOS desktop space: CGDisplayBounds origin, in POINTS (the global space
	// AppKit/CG place windows in; there is no single device-pixel desktop on a
	// mixed-scale Mac — see srDisplayGetLocation's note).
	out_info->display_screen_left = info->screen_left_pt;
	out_info->display_screen_top = info->screen_top_pt;
	out_info->supported_eye_tracking_modes = 1u; /* MANAGED_BIT */
	out_info->default_eye_tracking_mode = 0u;    /* MANAGED */
	if (out_info->struct_size >= offsetof(struct xrt_plugin_display_info, refresh_mhz) + sizeof(uint32_t)) {
		out_info->refresh_mhz = info->refresh_mhz;
	}
}

static bool
leia_mac_plugin_get_display_info(struct xrt_plugin_instance *inst,
                                 struct xrt_device *xdev,
                                 struct xrt_plugin_display_info *out_info)
{
	(void)inst;
	(void)xdev;
	struct leia_mac_display_info info;
	if (!leia_mac_sr_probe() || !leia_mac_get_display_info(&info) || !info.valid) {
		return false;
	}
	fill_display_info(&info, out_info);
	return true;
}

//! Does descriptor @p d sit where the panel is (origin in points or pixels)?
static bool
descriptor_is_panel_rect(const struct xrt_display_descriptor *d, const struct leia_mac_display_info *info)
{
	if (info->cg_display_id == 0) {
		return false;
	}
	const bool origin_pt = d->screen_left == info->screen_left_pt && d->screen_top == info->screen_top_pt;
	const int32_t s = (int32_t)(info->backing_scale + 0.5f);
	const bool origin_px = s > 1 && d->screen_left == info->screen_left_pt * s && d->screen_top == info->screen_top_pt * s;
	const bool size_pt = d->pixel_width == info->screen_width_pt && d->pixel_height == info->screen_height_pt;
	const bool size_px = d->pixel_width == info->pixel_width && d->pixel_height == info->pixel_height;
	return (origin_pt || origin_px) && (size_pt || size_px);
}

static uint32_t
leia_mac_plugin_probe_displays(struct xrt_plugin_instance *inst,
                               const struct xrt_display_descriptor *displays,
                               uint32_t display_count,
                               struct xrt_display_claim *out_claims,
                               uint32_t max_claims)
{
	(void)inst;
	if (displays == NULL || display_count == 0 || out_claims == NULL || max_claims == 0) {
		return 0;
	}
	struct leia_mac_display_info info = {0};
	const bool sr_ok = leia_mac_get_display_info(&info) && info.valid;

	uint32_t n = 0;
	for (uint32_t i = 0; i < display_count && n < max_claims; i++) {
		const struct xrt_display_descriptor *d = &displays[i];
		if (d->struct_size < offsetof(struct xrt_display_descriptor, edid_product) + sizeof(d->edid_product)) {
			continue;
		}
		if (!leia_mac_edid_is_leia_panel(d->edid_manufacturer, d->edid_product)) {
			continue;
		}
		struct xrt_display_claim *c = &out_claims[n++];
		memset(c, 0, sizeof(*c));
		c->monitor_id = d->monitor_id;
		// VERIFIED only for the monitor SR is actually driving (single-panel
		// SR line on macOS: the one whose EDID pair is the panel we found).
		const bool is_sr_panel = sr_ok && d->edid_manufacturer == info.edid_vendor &&
		                         d->edid_product == info.edid_product;
		c->confidence = is_sr_panel ? XRT_DISPLAY_CLAIM_VERIFIED : XRT_DISPLAY_CLAIM_EDID;
		c->supported_apis = XRT_DP_API_BIT_METAL;
	}

	// probe() bound us but no descriptor carried a known EDID pair (an
	// enumerator that could not read EDID, or DXR_LEIA_FORCE_PROBE): claim the
	// descriptor where the panel sits, so the bound plug-in owns a monitor.
	if (n == 0 && g_probe_bound) {
		for (uint32_t i = 0; i < display_count; i++) {
			if (descriptor_is_panel_rect(&displays[i], &info)) {
				memset(&out_claims[0], 0, sizeof(out_claims[0]));
				out_claims[0].monitor_id = displays[i].monitor_id;
				out_claims[0].confidence = XRT_DISPLAY_CLAIM_EDID;
				out_claims[0].supported_apis = XRT_DP_API_BIT_METAL;
				n = 1;
				break;
			}
		}
	}

	static uint32_t last_n = UINT32_MAX;
	if (n != last_n) {
		last_n = n;
		for (uint32_t i = 0; i < n; i++) {
			U_LOG_I("leia_mac_plugin: claim monitor 0x%016llx confidence=%s",
			        (unsigned long long)out_claims[i].monitor_id,
			        out_claims[i].confidence == XRT_DISPLAY_CLAIM_VERIFIED ? "VERIFIED" : "EDID");
		}
		if (n == 0) {
			U_LOG_I("leia_mac_plugin: probe_displays — no Leia panel among %u monitor(s)", display_count);
		}
	}
	return n;
}

#ifdef XRT_PLUGIN_IFACE_HAS_DISPLAY_INFO_FOR_MONITOR
static bool
leia_mac_plugin_get_display_info_for_monitor(struct xrt_plugin_instance *inst,
                                             const struct xrt_display_descriptor *display,
                                             const struct xrt_display_physical *physical,
                                             struct xrt_plugin_display_info *out_info)
{
	(void)inst;
	(void)physical;
	if (display == NULL || out_info == NULL ||
	    display->struct_size < offsetof(struct xrt_display_descriptor, screen_top) + sizeof(display->screen_top)) {
		return false;
	}
	struct leia_mac_display_info info;
	if (!leia_mac_get_display_info(&info) || !info.valid) {
		return false;
	}
	// Only the monitor SR drives: same EDID pair, or (no EDID) the panel's rect.
	const bool same_edid = info.cg_display_id != 0 && display->edid_manufacturer == info.edid_vendor &&
	                       display->edid_product == info.edid_product;
	if (!same_edid && !descriptor_is_panel_rect(display, &info)) {
		return false;
	}
	fill_display_info(&info, out_info);
	return true;
}
#endif


/*
 *
 * Vtable.
 *
 */

#ifndef DXR_PLUGIN_GIT_DESC
#define DXR_PLUGIN_GIT_DESC "unknown"
#endif

static struct xrt_plugin_iface g_leia_mac_iface = {
    .struct_size = sizeof(struct xrt_plugin_iface),
    .reserved_0 = 0,

    .id = "leia-sr",
    .display_name = "DisplayXR Leia SR (macOS)",
    .vendor = "Leia Inc.",
    .version = DXR_PLUGIN_GIT_DESC,

    .probe = leia_mac_plugin_probe,
    .create_device = leia_mac_plugin_create_device,

    /* Metal only — the macOS compositors that weave call create_dp_metal. */
    .create_dp_vk = NULL,
    .create_dp_d3d11 = NULL,
    .create_dp_d3d12 = NULL,
    .create_dp_gl = NULL,
    .create_dp_metal = leia_mac_dp_factory_metal,

    .destroy = leia_mac_plugin_destroy,
    .get_display_info = leia_mac_plugin_get_display_info,
    .set_pose_source = leia_mac_plugin_set_pose_source,

    .probe_displays = leia_mac_plugin_probe_displays,

#ifdef XRT_PLUGIN_IFACE_HAS_DISPLAY_INFO_FOR_MONITOR
    .get_display_info_for_monitor = leia_mac_plugin_get_display_info_for_monitor,
#endif
};


/*
 *
 * Entry point.
 *
 */

XRT_PLUGIN_EXPORT xrt_result_t
xrtPluginNegotiate(uint32_t runtime_api_version,
                   const struct xrt_plugin_host_iface *host,
                   struct xrt_plugin_iface **out_iface,
                   uint32_t *out_plugin_api_version)
{
	(void)host;
	*out_plugin_api_version = XRT_PLUGIN_API_VERSION_CURRENT;
	if (runtime_api_version != XRT_PLUGIN_API_VERSION_CURRENT) {
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	*out_iface = &g_leia_mac_iface;
	return XRT_SUCCESS;
}
