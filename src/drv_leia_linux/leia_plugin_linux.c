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

#include "vk/vk_helpers.h" // sizeof/offsetof(struct vk_bundle) fingerprint (#233)
#include "leia_interface.h"
#include "leia_sr_linux.h"
#include "leia_display_processor_linux.h"
#include "leia_edid_probe_linux.h"
#include "leia_display_claims_linux.h"

#include <pthread.h>
#include <stddef.h>
#include <string.h>

DEBUG_GET_ONCE_BOOL_OPTION(leia_force_probe, "DXR_LEIA_FORCE_PROBE", false)

//! probe_displays' panel-scan TTL (review C): one sysfs + RandR pass per burst of registry rebuilds.
#define LEIA_LNX_PROBE_PANEL_TTL_NS (1000ull * 1000ull * 1000ull)

//! probe() bound this plug-in (forced or a panel found). probe_displays then
//! guarantees the plug-in at least one monitor (leia_lnx_fallback_claim).
static bool g_probe_bound;


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

	// Seed the ONE per-view-scale derivation from the backend's recommended
	// per-view size. Done HERE, at probe, because the mode table is written in
	// leia_hmd_create() (create_device) — which the runtime calls BEFORE
	// get_display_info — so seeding from get_display_info would be too late and
	// the mode table would keep the 0.5 x 0.5 fallback while the scalar moved.
	// The stub backend answers 1920x1080 of 3840x2160, i.e. exactly the
	// fallback; a Track B SDK backend's real numbers now reach both consumers.
	{
		struct leiasr_lnx_display_info di;
		if (leiasr_lnx_query_display_info(&di) && di.valid) {
			leia_view_scale_set_from_dims(di.recommended_view_width, di.recommended_view_height,
			                              di.pixel_width, di.pixel_height);
		}
	}

	if (forced) {
		U_LOG_W("leia_lnx_plugin: probe FORCED (DXR_LEIA_FORCE_PROBE=1)");
	} else {
		U_LOG_W("leia_lnx_plugin: Leia panel detected via DRM/EDID (manufacturer %u, product %u) — binding",
		        edid_man, edid_prod);
	}

	/* No per-instance state — hardware state lives in file-scope statics
	 * inside the plug-in .so, mirroring the Windows/Android arms. */
	g_probe_bound = true;
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
	/* Read back the pair seeded at probe from these same backend numbers — the
	 * mode table (leia_device.c) reads the identical pair, so the scalar that
	 * sizes XrViewConfigurationView and the scale that sizes the tiles/atlas
	 * cannot disagree. Deliberately NOT re-derived here: the device was created
	 * before this call, so a fresher number would only disagree with it. */
	leia_view_scale_get(&out_info->recommended_view_scale_x, &out_info->recommended_view_scale_y);
	out_info->display_screen_left = info.screen_left;
	out_info->display_screen_top = info.screen_top;

	// Neither backend has a real desktop position on Linux — the stub cans
	// (0, 0) and srDisplayGetLocation's Linux getScreenRect returns (0, 0)
	// — so resolve the panel's actual position from RandR (EDID-matched,
	// #91 / runtime#715) and prefer it. Cached per connector in the EDID
	// module (NULL = the first panel, the one a single-panel session binds;
	// M4/M5 pass the DP's bound connector). Headless (no X / no match) keeps
	// the backend value, so the CI selftest is unaffected.
	{
		int32_t randr_left = 0, randr_top = 0;
		if (leia_lnx_edid_panel_desktop_position_cached(NULL, &randr_left, &randr_top)) {
			static bool logged = false;
			if (!logged && (randr_left != info.screen_left || randr_top != info.screen_top)) {
				U_LOG_W("leia_lnx_plugin: RandR panel position (%d, %d) overrides backend (%d, %d)",
				        randr_left, randr_top, info.screen_left, info.screen_top);
			}
			logged = true;
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

static uint32_t
leia_lnx_plugin_probe_displays(struct xrt_plugin_instance *inst,
                               const struct xrt_display_descriptor *displays,
                               uint32_t display_count,
                               struct xrt_display_claim *out_claims,
                               uint32_t max_claims)
{
	(void)inst;
	if (displays == NULL || display_count == 0 || out_claims == NULL || max_claims == 0) {
		return 0;
	}

	/*
	 * Per-monitor claims (multi-screen plan M0; #69 / ADR-015 shape, Windows
	 * parity with leia_plugin.c). Matching + confidence rules live in
	 * leia_display_claims_linux.c; this gathers the evidence:
	 *   - every Leia panel on this box (one /sys/class/drm pass + RandR join);
	 *   - the new SR API's display list when it is compiled in AND a live SR
	 *     context exists (FPC confidence, serial, displayId);
	 *   - otherwise SR 1.38's only per-device fact: the live context's lens
	 *     serial (verifies a single panel).
	 * Never creates an SR context (seam header explains why) — on a box where
	 * probe() did not bring one up, claims stay at EDID confidence.
	 */
	// Short-TTL snapshot of the shared panel cache: the runtime rebuilds its
	// registry per client connect, and a burst of rebuilds must not cost a
	// sysfs scan + an X connection each; a hot-plugged panel is still picked
	// up by the first rebuild after the TTL (review C).
	struct leia_lnx_edid_panel panels[LEIA_LNX_EDID_MAX_PANELS];
	const uint32_t panel_count =
	    leia_lnx_edid_panels_snapshot(panels, LEIA_LNX_EDID_MAX_PANELS, LEIA_LNX_PROBE_PANEL_TTL_NS);

	struct leia_lnx_sr_display sr_displays[LEIA_LNX_SR_MAX_DISPLAYS];
	const int32_t sr_count = leia_lnx_sr_enumerate_displays(sr_displays, LEIA_LNX_SR_MAX_DISPLAYS);

	char fpc_serial[64] = {0};
	const bool have_legacy_serial = sr_count < 0 && leiasr_lnx_peek_fpc_serial(fpc_serial, sizeof(fpc_serial));

	const struct leia_lnx_claim_inputs in = {
	    .panels = panels,
	    .panel_count = panel_count,
	    .sr_displays = sr_displays,
	    .sr_display_count = sr_count,
	    .legacy_fpc_serial = have_legacy_serial ? fpc_serial : NULL,
	    /* Vulkan only: create_dp_vk is the arm's sole factory (no GL DP). */
	    .supported_apis = XRT_DP_API_BIT_VK,
	};

	struct leia_lnx_claim_binding bindings[LEIA_LNX_EDID_MAX_PANELS + LEIA_LNX_SR_MAX_DISPLAYS];
	const uint32_t cap = max_claims < (uint32_t)(sizeof(bindings) / sizeof(bindings[0]))
	                         ? max_claims
	                         : (uint32_t)(sizeof(bindings) / sizeof(bindings[0]));
	uint32_t n = leia_lnx_compute_claims(displays, display_count, &in, out_claims, bindings, cap);

	// probe() bound us but no monitor matched (DXR_LEIA_FORCE_PROBE=1, or a
	// descriptor the matcher cannot pair). The runtime gives a plug-in that
	// implements probe_displays no fallback claim of its own, so without
	// this the bound plug-in would own no monitor and another plug-in would
	// win the panel. One EDID-confidence claim on the bound panel's monitor
	// (by size), else the primary monitor.
	if (n == 0 && g_probe_bound &&
	    leia_lnx_fallback_claim(displays, display_count, panels, panel_count, XRT_DP_API_BIT_VK, &out_claims[0],
	                            &bindings[0])) {
		n = 1;
		static bool fallback_logged;
		if (!fallback_logged) {
			fallback_logged = true;
			U_LOG_W(
			    "leia_lnx_plugin: probe() bound but no monitor matched a Leia panel — fallback claim on "
			    "monitor 0x%016llx%s%s (EDID confidence)",
			    (unsigned long long)out_claims[0].monitor_id, bindings[0].connector[0] ? " on " : "",
			    bindings[0].connector);
		}
	}

	// Plug-in-private monitor table: displayId per claimed monitor, for the
	// per-DP SR binding (M4/M5). Nothing consumes it yet.
	leia_lnx_claims_store(bindings, n);

	// Logged at INFO and only when the answer changed: probe_displays runs on
	// every registry rebuild (per client connect), so a per-call WARN per
	// claim flooded the service log (review C).
	static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
	static struct xrt_display_claim last_claims[LEIA_LNX_EDID_MAX_PANELS + LEIA_LNX_SR_MAX_DISPLAYS];
	static struct leia_lnx_claim_binding last_bindings[LEIA_LNX_EDID_MAX_PANELS + LEIA_LNX_SR_MAX_DISPLAYS];
	static uint32_t last_n = UINT32_MAX;
	pthread_mutex_lock(&log_lock);
	const bool changed = n != last_n || memcmp(last_claims, out_claims, n * sizeof(out_claims[0])) != 0 ||
	                     memcmp(last_bindings, bindings, n * sizeof(bindings[0])) != 0;
	if (changed) {
		last_n = n;
		memcpy(last_claims, out_claims, n * sizeof(out_claims[0]));
		memcpy(last_bindings, bindings, n * sizeof(bindings[0]));
	}
	pthread_mutex_unlock(&log_lock);
	if (!changed) {
		return n;
	}
	for (uint32_t i = 0; i < n; i++) {
		U_LOG_I("leia_lnx_plugin: claim monitor 0x%016llx on %s confidence=%s serial='%s' sr_display=0x%016llx",
		        (unsigned long long)out_claims[i].monitor_id,
		        bindings[i].connector[0] ? bindings[i].connector : "(unknown connector)",
		        out_claims[i].confidence == XRT_DISPLAY_CLAIM_VERIFIED ? "VERIFIED" : "EDID",
		        out_claims[i].serial, (unsigned long long)bindings[i].sr_display_id);
	}
	if (n == 0) {
		U_LOG_I(
		    "leia_lnx_plugin: probe_displays — no Leia panel among %u monitor(s) (%u panel(s) in EDID scan, "
		    "SR enumeration %s)",
		    display_count, panel_count, sr_count < 0 ? "unavailable" : "available");
	}
	return n;
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
#ifdef DXR_LEIA_WEAVER_SDK
    .display_name = "DisplayXR Leia SR (Linux)",
#else
    .display_name = "DisplayXR Leia SR (Linux, stub weaver)",
#endif
    .vendor = "Leia Inc.",
    .version = DXR_PLUGIN_GIT_DESC,

    .probe = leia_lnx_plugin_probe,
    .create_device = leia_lnx_plugin_create_device,

    /* Vulkan only — the Linux compositor is vk_native (contract §3.1). */
    .create_dp_vk = leia_lnx_dp_factory_vk,
    /*
     * #1243/#1244 vk_bundle ABI fingerprint (#233). Unconditional here,
     * unlike the Windows arm: this arm always has a VK factory, because
     * vk_native IS the Linux compositor.
     *
     * The loader only WARNS on desktop when these are absent (it refuses
     * outright on Android), so the pairing shipped in the .deb has never
     * been layout-checked. The warning text even names the .deb as the
     * reason it stays permissive — which is precisely the pairing this
     * makes verifiable instead of assumed.
     */
    .vk_bundle_abi_size = (uint32_t)sizeof(struct vk_bundle),
    .vk_bundle_fn_table_offset = (uint32_t)offsetof(struct vk_bundle, vkGetInstanceProcAddr),
    .create_dp_d3d11 = NULL,
    .create_dp_d3d12 = NULL,
    .create_dp_gl = NULL,
    .create_dp_metal = NULL,

    .destroy = leia_lnx_plugin_destroy,

    .get_display_info = leia_lnx_plugin_get_display_info,

    .set_pose_source = leia_lnx_plugin_set_pose_source,

    /* Per-monitor claims (multi-screen M0; #69 / ADR-015 shape). */
    .probe_displays = leia_lnx_plugin_probe_displays,
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
