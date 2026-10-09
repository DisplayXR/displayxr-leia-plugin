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
 * Probe policy: bind when the SR runtime's srEnumerateDisplays reports at
 * least one SR display (leia_sr_macos.m: one long-lived CLIENT instance that
 * re-enumerates on SR_EVENT_TYPE_DISPLAY_TOPOLOGY_CHANGED). No SR runtime, or
 * srEnumerateDisplays = 0 (panel unplugged) = decline, so sim-display wins.
 * An SR runtime without the call falls back to the active display + the
 * frozen EDID table. `DXR_LEIA_FORCE_PROBE=1` binds regardless (bring-up).
 *
 * Panel selection: each SR display's platformHandle (CGDirectDisplayID) is
 * resolved to CGDisplayBounds + its CoreGraphics UUID at enumeration time
 * (never cached across a topology change). probe_displays claims the runtime
 * monitor with that CGDisplay rect: VERIFIED when FPC verified, EDID when
 * EDID only. create_dp_metal_for_screen binds a screen whose UUID (or rect)
 * is an SR display, and declines every other.
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
#include <strings.h>

DEBUG_GET_ONCE_BOOL_OPTION(leia_force_probe, "DXR_LEIA_FORCE_PROBE", false)



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
		// leia_sr_macos.m logged the reason (no SR runtime, or
		// srEnumerateDisplays = 0 / no panel located on a legacy SR runtime).
		U_LOG_W("leia_mac_plugin: probe declined — no SR display (see the leia_mac line above); "
		        "DXR_LEIA_FORCE_PROBE=1 force-binds");
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

//! EDID id equality across byte orders: CoreGraphics reports the ids as
//! big-endian integers (0x4C2D for "SAM"), the runtime's descriptor carries the
//! raw EDID bytes, which on a little-endian read is 0x2D4C — leia_edid_table.h
//! lists both spellings for that reason.
static bool
edid_id_eq(uint16_t a, uint16_t b)
{
	return a == b || a == (uint16_t)((b >> 8) | (b << 8));
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

//! Index of the SR display (from leia_mac_get_sr_displays) that monitor
//! descriptor @p d is, joined through SR's platformHandle (CGDirectDisplayID)
//! -> CGDisplayBounds, the rect the runtime's macOS enumerator reports, with
//! the EDID product as a cross-check. -1 = not an SR display.
static int
sr_display_for_descriptor(const struct xrt_display_descriptor *d, const struct leia_mac_display_info *sr, uint32_t n)
{
	if (d->struct_size < offsetof(struct xrt_display_descriptor, screen_top) + sizeof(d->screen_top)) {
		return -1;
	}
	for (uint32_t j = 0; j < n; j++) {
		if (!descriptor_is_panel_rect(d, &sr[j])) {
			continue;
		}
		if (d->edid_product != 0 && sr[j].edid_product != 0 && !edid_id_eq(d->edid_product, sr[j].edid_product)) {
			continue;
		}
		return (int)j;
	}
	return -1;
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
	// The SR displays as SR enumerates them now (re-enumerated after a
	// topology change), joined to the runtime's monitors by CGDisplay.
	// No SR display = no claim: sim-display (or another vendor) keeps them.
	struct leia_mac_display_info sr[LEIA_MAC_MAX_SR_DISPLAYS];
	const uint32_t n_sr = leia_mac_get_sr_displays(sr, LEIA_MAC_MAX_SR_DISPLAYS);

	uint32_t n = 0;
	for (uint32_t i = 0; i < display_count && n < max_claims; i++) {
		const int j = sr_display_for_descriptor(&displays[i], sr, n_sr);
		if (j < 0) {
			continue;
		}
		struct xrt_display_claim *c = &out_claims[n++];
		memset(c, 0, sizeof(*c));
		c->monitor_id = displays[i].monitor_id;
		// Linux-arm meaning: VERIFIED = the SR service vouches for THIS panel
		// (FPC verified; on a legacy SR runtime, its one active display that
		// the EDID table located), EDID = recognised by EDID only.
		const bool verified = sr[j].from_enumeration ? sr[j].fpc_verified : true;
		c->confidence = verified ? XRT_DISPLAY_CLAIM_VERIFIED : XRT_DISPLAY_CLAIM_EDID;
		c->supported_apis = XRT_DP_API_BIT_METAL;
		snprintf(c->serial, sizeof(c->serial), "%s", sr[j].fpc_serial);
	}

	static uint32_t last_n = UINT32_MAX;
	static uint32_t last_sr = UINT32_MAX;
	if (n != last_n || n_sr != last_sr) {
		last_n = n;
		last_sr = n_sr;
		for (uint32_t i = 0; i < n; i++) {
			U_LOG_W("leia_mac_plugin: claim monitor 0x%016llx confidence=%s serial='%s'",
			        (unsigned long long)out_claims[i].monitor_id,
			        out_claims[i].confidence == XRT_DISPLAY_CLAIM_VERIFIED ? "VERIFIED" : "EDID",
			        out_claims[i].serial);
		}
		if (n == 0) {
			U_LOG_W("leia_mac_plugin: probe_displays — no claim among %u monitor(s) (%u SR display(s))",
			        display_count, n_sr);
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
	// Only the ACTIVE SR panel (the one the SR geometry describes), joined by
	// its CGDisplay rect. Another SR display (macOS binds one today) or a
	// non-SR monitor gets the runtime's EDID defaults.
	if (sr_display_for_descriptor(display, &info, 1) != 0) {
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

#ifdef XRT_PLUGIN_IFACE_HAS_CREATE_DP_METAL_FOR_SCREEN
/*!
 * One windowless DP per screen (multi-screen on macOS). Binds ONLY when the
 * binding's display is an SR display srEnumerateDisplays reports — joined by
 * the CoreGraphics UUID the runtime puts in device_name (SR's platformHandle
 * -> CGDisplayCreateUUIDFromDisplayID), else by its CGDisplayBounds (points)
 * — and declines otherwise, so the screen's other claimant (sim-display)
 * serves it. The DP's weaver is bound to that display (SrDisplayBindingInfo)
 * with EXTERNAL routing; see leia_display_processor_macos.m.
 */
static xrt_result_t
leia_mac_plugin_create_dp_metal_for_screen(struct xrt_plugin_instance *inst,
                                           void *metal_device,
                                           void *command_queue,
                                           void *window_handle,
                                           const struct xrt_screen_binding *binding,
                                           struct xrt_display_processor_metal **out_xdp)
{
	(void)inst;
	(void)window_handle; // NULL today; the segment DP is windowless by contract
	if (binding == NULL || out_xdp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	struct leia_mac_display_info sr[LEIA_MAC_MAX_SR_DISPLAYS];
	const uint32_t n_sr = leia_mac_get_sr_displays(sr, LEIA_MAC_MAX_SR_DISPLAYS);

	const uint32_t sz = binding->struct_size;
#define LEIA_HAS(field) (sz >= offsetof(struct xrt_screen_binding, field) + sizeof(binding->field))
	char name[sizeof(binding->device_name) + 1] = {0};
	if (LEIA_HAS(device_name)) {
		memcpy(name, binding->device_name, sizeof(binding->device_name));
	}
	int match = -1;
	const char *how = "";
	for (uint32_t j = 0; j < n_sr && match < 0; j++) {
		if (sr[j].cg_display_id == 0) {
			continue;
		}
		if (name[0] != '\0' && sr[j].uuid[0] != '\0' && strcasecmp(name, sr[j].uuid) == 0) {
			match = (int)j;
			how = "UUID";
		} else if (LEIA_HAS(desktop_height) && binding->desktop_left == sr[j].screen_left_pt &&
		           binding->desktop_top == sr[j].screen_top_pt && binding->desktop_width == sr[j].screen_width_pt &&
		           binding->desktop_height == sr[j].screen_height_pt) {
			match = (int)j;
			how = "desktop rect";
		}
	}
#undef LEIA_HAS
	if (match < 0) {
		U_LOG_W("leia_mac_plugin: create_dp_metal_for_screen declined — screen '%s' (%d,%d %ux%u pt) is not an "
		        "SR display (%u enumerated)",
		        name, binding->desktop_left, binding->desktop_top, binding->desktop_width, binding->desktop_height,
		        n_sr);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	U_LOG_W("leia_mac_plugin: create_dp_metal_for_screen: screen '%s' is SR display 0x%016llx (%s, matched by %s)",
	        name, (unsigned long long)sr[match].sr_display_id, sr[match].fpc_verified ? "FPC verified" : "EDID only",
	        how);
	return leia_mac_dp_factory_metal_for_screen(metal_device, command_queue, &sr[match], out_xdp);
}
#endif

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
#ifdef XRT_PLUGIN_IFACE_HAS_CREATE_DP_METAL_FOR_SCREEN
    /* One windowless DP per screen (multi-screen on macOS), Leia panel only. */
    .create_dp_metal_for_screen = leia_mac_plugin_create_dp_metal_for_screen,
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
