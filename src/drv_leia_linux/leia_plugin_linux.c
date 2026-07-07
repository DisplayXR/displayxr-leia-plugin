// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Plug-in entry point for the Leia display driver — Linux desktop arm
 *         (Track A: stub weaver, no LeiaSR Linux SDK).
 *
 * Implements the @ref xrt_plugin_negotiate_fn_t signature from
 * `xrt/xrt_plugin.h`. The runtime dlopens this .so via JSON-manifest
 * discovery (`<probe-order>-leia-sr.json` in an XDG/system
 * `DisplayProcessors/` root or a `XRT_PLUGIN_SEARCH_PATH` dir) and resolves
 * exactly one symbol: `xrtPluginNegotiate` (the version script hides
 * everything else — runtime issue #496 / ADR-019).
 *
 * Probe policy (Track A): there is no hardware probe yet — the stub backend
 * has no SR service or panel to find. The probe therefore DECLINES by default
 * (contract R-D4: decline cleanly on machines without SR hardware) and
 * accepts only when the `DXR_LEIA_FORCE_PROBE=1` env var is set (CI, dev
 * bring-up). TODO(Track B): real probe — SR-service reachability + DRM/EDID
 * panel identification (/sys/class/drm/<connector>/edid).
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#include "xrt/xrt_config_os.h"

#if !defined(XRT_OS_LINUX) || defined(XRT_OS_ANDROID)
#error "drv_leia_linux is Linux-desktop only (XRT_OS_LINUX && !XRT_OS_ANDROID)."
#endif

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"

#include "util/u_debug.h"
#include "util/u_logging.h"

#include "leia_interface.h"
#include "leia_sr_linux.h"
#include "leia_display_processor_linux.h"
#include "leia_edid_probe_linux.h"

#include <stddef.h>

DEBUG_GET_ONCE_BOOL_OPTION(leia_force_probe, "DXR_LEIA_FORCE_PROBE", false)


/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
leia_lnx_plugin_probe(struct xrt_plugin_instance **out_inst)
{
	/*
	 * Bind order (mirrors the Windows arm's EDID fast path):
	 *   1. DXR_LEIA_FORCE_PROBE=1 — unconditional override (bring-up/CI).
	 *   2. DRM/EDID match against the frozen Leia panel table — a real
	 *      panel auto-binds, no env var needed.
	 *   3. Decline — the plug-in must not hijack a Linux box with no Leia
	 *      panel just because it is registered.
	 */
	const bool forced = debug_get_bool_option_leia_force_probe();
	uint16_t edid_man = 0, edid_prod = 0;
	const bool panel = forced ? false : leia_lnx_edid_panel_present(&edid_man, &edid_prod);
	if (!forced && !panel) {
		U_LOG_I("leia_lnx_plugin: probe declined — no Leia panel in /sys/class/drm EDID scan; "
		        "set DXR_LEIA_FORCE_PROBE=1 to force-bind for bring-up/CI");
		*out_inst = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	// Seed the backend probe cache so leia_device.c (leia_hmd_create) and
	// get_display_info report consistent values (canned in the stub, real
	// SR display queries in the sdk backend).
	(void)leiasr_probe_display(0.0);

	if (forced) {
		U_LOG_W("leia_lnx_plugin: probe FORCED (DXR_LEIA_FORCE_PROBE=1)");
	} else {
		U_LOG_W("leia_lnx_plugin: Leia panel detected via DRM/EDID (manufacturer %u, product %u) — binding",
		        edid_man, edid_prod);
	}

	/* No per-instance state — hardware state lives in file-scope statics
	 * inside the plug-in .so, mirroring the Windows/Android arms. */
	*out_inst = NULL;
	return XRT_SUCCESS;
}

static xrt_result_t
leia_lnx_plugin_create_device(struct xrt_plugin_instance *inst, struct xrt_device **out_dev)
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
leia_lnx_plugin_destroy(struct xrt_plugin_instance *inst)
{
	(void)inst;
	/* No instance state — nothing to free. */
}

static void
leia_lnx_plugin_set_pose_source(struct xrt_plugin_instance *inst, struct xrt_device *xdev, struct xrt_device *source)
{
	(void)inst;
	leia_hmd_set_pose_source(xdev, source);
}

static bool
leia_lnx_plugin_get_display_info(struct xrt_plugin_instance *inst,
                                 struct xrt_device *xdev,
                                 struct xrt_plugin_display_info *out_info)
{
	(void)inst;
	(void)xdev;

	// Contract R-D1 field map: backend static display query →
	// xrt_plugin_display_info. Headless-tolerant (R-D4) — the stub always
	// answers; a Track B SDK backend may return false with no panel.
	struct leiasr_lnx_display_info info;
	if (!leiasr_lnx_query_display_info(&info) || !info.valid) {
		return false;
	}

	out_info->display_width_m = info.width_m;
	out_info->display_height_m = info.height_m;
	out_info->nominal_viewer_x_m = info.nominal_viewer_x_m;
	out_info->nominal_viewer_y_m = info.nominal_viewer_y_m;
	out_info->nominal_viewer_z_m = info.nominal_viewer_z_m;
	out_info->display_pixel_width = info.pixel_width;
	out_info->display_pixel_height = info.pixel_height;
	out_info->recommended_view_scale_x =
	    info.pixel_width != 0 ? (float)info.recommended_view_width / (float)info.pixel_width : 1.0f;
	out_info->recommended_view_scale_y =
	    info.pixel_height != 0 ? (float)info.recommended_view_height / (float)info.pixel_height : 1.0f;
	out_info->display_screen_left = info.screen_left;
	out_info->display_screen_top = info.screen_top;

	// Neither backend has a real desktop position on Linux — the stub cans
	// (0, 0) and srDisplayGetLocation's Linux getScreenRect returns (0, 0)
	// — so resolve the panel's actual position from RandR (EDID-matched,
	// #91 / runtime#715) and prefer it. Resolved once; headless (no X /
	// no match) keeps the backend value, so the CI selftest is unaffected.
	{
		static bool resolved = false;
		static bool randr_found = false;
		static int32_t randr_left = 0;
		static int32_t randr_top = 0;
		if (!resolved) {
			randr_found = leia_lnx_edid_panel_desktop_position(&randr_left, &randr_top);
			if (randr_found && (randr_left != info.screen_left || randr_top != info.screen_top)) {
				U_LOG_W("leia_lnx_plugin: RandR panel position (%d, %d) overrides backend (%d, %d)",
				        randr_left, randr_top, info.screen_left, info.screen_top);
			}
			resolved = true;
		}
		if (randr_found) {
			out_info->display_screen_left = randr_left;
			out_info->display_screen_top = randr_top;
		}
	}

	/* MANAGED-only for v1, Windows parity (contract R-T4; R-T5 MANUAL is a
	 * SHOULD the stub also honors as a no-op). */
	out_info->supported_eye_tracking_modes = 1u; /* MANAGED_BIT */
	out_info->default_eye_tracking_mode = 0u;    /* MANAGED */

	// refresh_mhz was appended to the info struct (append-only rule) —
	// clamp the write to the runtime's advertised struct_size.
	if (out_info->struct_size >= offsetof(struct xrt_plugin_display_info, refresh_mhz) + sizeof(uint32_t)) {
		out_info->refresh_mhz = info.refresh_mhz;
	}

	return true;
}


/*
 *
 * Vtable.
 *
 */

// Baked in by CMake from `git describe` (#47) so consumers (loader log,
// displayxr-cli) can spot stale builds.
#ifndef DXR_PLUGIN_GIT_DESC
#define DXR_PLUGIN_GIT_DESC "unknown"
#endif

static struct xrt_plugin_iface g_leia_lnx_iface = {
    .struct_size = sizeof(struct xrt_plugin_iface),
    .reserved_0 = 0,

    .id = "leia-sr",
    .display_name = "DisplayXR Leia SR (Linux, stub weaver)",
    .vendor = "Leia Inc.",
    .version = DXR_PLUGIN_GIT_DESC,

    .probe = leia_lnx_plugin_probe,
    .create_device = leia_lnx_plugin_create_device,

    /* Vulkan only — the Linux compositor is vk_native (contract §3.1). */
    .create_dp_vk = leia_lnx_dp_factory_vk,
    .create_dp_d3d11 = NULL,
    .create_dp_d3d12 = NULL,
    .create_dp_gl = NULL,
    .create_dp_metal = NULL,

    .destroy = leia_lnx_plugin_destroy,

    .get_display_info = leia_lnx_plugin_get_display_info,

    .set_pose_source = leia_lnx_plugin_set_pose_source,

    /* TODO(Track B): per-monitor claims once the DRM/EDID probe exists
     * (issue #69 / ADR-015 shape — see the Windows arm's probe_displays). */
    .probe_displays = NULL,
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

	*out_iface = &g_leia_lnx_iface;
	return XRT_SUCCESS;
}
