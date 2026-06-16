// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Plug-in entry point for the Leia SR display driver.
 *
 * Implements the @ref xrt_plugin_negotiate_fn_t signature defined in
 * `xrt/xrt_plugin.h`. The runtime DLL loads this plug-in via
 * `LoadLibraryExW` + `GetProcAddress("xrtPluginNegotiate")` once the
 * registry sort lands on this entry (intended `ProbeOrder=50` per
 * ADR-019; lower than sim_display's 200, so this wins on machines
 * with SR hardware).
 *
 * The probe is fast on the common path — EDID + SR-SDK/service presence
 * check only, no SR context creation. The slower
 * `leiasr_probe_display(timeout)` path is invoked only as a fallback when
 * the static EDID table misses but SR is installed and running (see
 * `leia_plugin_probe`), so a panel that SR accepts but our frozen table
 * has not yet learned still binds.
 *
 * Issue #256 — vendor plug-in re-architecture.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"

#include "util/u_logging.h"

#include "leia_interface.h"
#include "leia_display_processor.h"
#ifdef XRT_HAVE_LEIA_SR_D3D11
#include "leia_display_processor_d3d11.h"
#include "leia_sr_d3d11.h" /* leiasr_query_recommended_view_dimensions + leiasr_static_get_display_dimensions */
#endif
#ifdef XRT_HAVE_LEIA_SR_D3D12
#include "leia_display_processor_d3d12.h"
#endif
#ifdef XRT_HAVE_LEIA_SR_GL
#include "leia_display_processor_gl.h"
#endif

#include <stddef.h>


/*
 *
 * Vtable callbacks.
 *
 */

/*
 * Fallback SR-context probe timeout, used only when the fast EDID table
 * misses. SR SDK + SRService are already confirmed present at that point, so
 * the context should come up quickly; keep the wait short to stay friendly on
 * the xrCreateInstance path.
 */
#define LEIA_PLUGIN_SR_PROBE_TIMEOUT_S 2.0

static xrt_result_t
leia_plugin_probe(struct xrt_plugin_instance **out_inst)
{
	/*
	 * SR presence is required regardless of how the panel is identified:
	 *   - SR SDK installed on this machine (sdk_installed),
	 *   - SRService running (service_running).
	 * If either is missing there is no SR display — decline cleanly so the
	 * next plug-in (or the sim_display fallback) gets a turn.
	 */
	struct leia_display_probe_result edid = {0};
	leia_edid_probe_display(&edid);

	if (!edid.sdk_installed || !edid.service_running) {
		U_LOG_I("leia_plugin: probe declined — sdk=%d service=%d", edid.sdk_installed,
		        edid.service_running);
		*out_inst = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	/*
	 * Panel identification. The fast path is the EDID table match
	 * (hw_found). That table is a frozen copy of SR's product-code map and
	 * drifts: SR's ProductCodeInstaller registers new panels in SR's own
	 * registry, which our table cannot see. So on a table miss we defer to
	 * the authoritative source — the SR runtime itself. If SR reports an
	 * active SR display, trust it. This costs one SR-context creation, but
	 * only on the rare miss and only on a machine that already has the SDK +
	 * a running service, so it stays off the common hot path.
	 */
	bool panel_ok = edid.hw_found;
	if (!panel_ok) {
		U_LOG_W("leia_plugin: EDID table miss with SR present — deferring to SR runtime probe "
		        "(table is stale relative to SR's product-code registry)");
		panel_ok = leiasr_probe_display(LEIA_PLUGIN_SR_PROBE_TIMEOUT_S);
		if (panel_ok) {
			U_LOG_W("leia_plugin: SR runtime confirms an active SR display — binding");
		}
	}

	if (!panel_ok) {
		U_LOG_I("leia_plugin: probe declined — no SR display (hw=%d sdk=%d service=%d)",
		        edid.hw_found, edid.sdk_installed, edid.service_running);
		*out_inst = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	/*
	 * No per-instance state: drv_leia stores hardware state in
	 * file-scope statics inside the plug-in DLL (cached probe result,
	 * SR context, etc.). Mirrors the sim_display plug-in shape.
	 */
	*out_inst = NULL;
	return XRT_SUCCESS;
}

static xrt_result_t
leia_plugin_create_device(struct xrt_plugin_instance *inst, struct xrt_device **out_dev)
{
	(void)inst;
	struct xrt_device *xdev = leia_hmd_create();
	if (xdev == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	*out_dev = xdev;
	return XRT_SUCCESS;
}

static void
leia_plugin_destroy(struct xrt_plugin_instance *inst)
{
	(void)inst;
	/* No instance state — nothing to free. */
}

static void
leia_plugin_set_pose_source(struct xrt_plugin_instance *inst,
                            struct xrt_device *xdev,
                            struct xrt_device *source)
{
	(void)inst;
	leia_hmd_set_pose_source(xdev, source);
}

static bool
leia_plugin_get_display_info(struct xrt_plugin_instance *inst,
                             struct xrt_device *xdev,
                             struct xrt_plugin_display_info *out_info)
{
	(void)inst;
	(void)xdev;

	(void)out_info->struct_size; /* v1: see sim_display plug-in's note. */

	bool any_populated = false;

	/*
	 * SR-recommended view dimensions + native panel resolution. Both
	 * are needed by the compositor for atlas sizing + the per-view
	 * scale factor stored in xrt_system_compositor_info.
	 */
	uint32_t sr_w = 0, sr_h = 0, nat_w = 0, nat_h = 0;
	float refresh = 0.0f;
	if (leiasr_query_recommended_view_dimensions(5.0, &sr_w, &sr_h, &refresh, &nat_w, &nat_h) && nat_w > 0 &&
	    nat_h > 0) {
		out_info->display_pixel_width = nat_w;
		out_info->display_pixel_height = nat_h;
		out_info->recommended_view_scale_x = (float)sr_w / (float)nat_w;
		out_info->recommended_view_scale_y = (float)sr_h / (float)nat_h;
		any_populated = true;
	}

	/* Physical dimensions + nominal viewer position from SR SDK. */
	struct leiasr_display_dimensions dims = {0};
	if (leiasr_static_get_display_dimensions(&dims) && dims.valid) {
		out_info->display_width_m = dims.width_m;
		out_info->display_height_m = dims.height_m;
		out_info->nominal_viewer_x_m = dims.nominal_x_m;
		out_info->nominal_viewer_y_m = dims.nominal_y_m;
		out_info->nominal_viewer_z_m = dims.nominal_z_m;
		any_populated = true;
	}

	/* EDID screen position — cached by probe(), zero if not available. */
	struct leia_display_probe_result edid;
	if (leia_edid_get_cached_result(&edid) && edid.hw_found) {
		out_info->display_screen_left = edid.screen_left;
		out_info->display_screen_top = edid.screen_top;
	}

	/* Leia: MANAGED eye tracking only — the SR SDK owns the grace
	 * period + transition handling. */
	out_info->supported_eye_tracking_modes = 1u; /* MANAGED_BIT */
	out_info->default_eye_tracking_mode = 0u;    /* MANAGED */

	return any_populated;
}

static uint32_t
leia_plugin_probe_displays(struct xrt_plugin_instance *inst,
                           const struct xrt_display_descriptor *displays,
                           uint32_t display_count,
                           struct xrt_display_claim *out_claims,
                           uint32_t max_claims)
{
	(void)inst;

	/*
	 * Per-monitor claims (issue #69 / ADR-015). Thin wrapper over the same
	 * detection the binary probe() uses — no new logic, just re-shaped:
	 *   - Match EACH runtime-supplied descriptor's (mfr, product) against
	 *     the known-panel EDID table (the runtime already enumerated the
	 *     monitors, so we don't re-enumerate).
	 *   - SR SDK + service presence are system-global; check once and apply
	 *     to every matched monitor. EDID match alone → EDID confidence;
	 *     EDID + SDK + running service → VERIFIED.
	 */
	struct leia_display_probe_result probe = {0};
	(void)leia_edid_probe_display(&probe);
	const bool verified = probe.sdk_installed && probe.service_running;

	/* Which create_dp_<api> factories this build actually ships — mirror
	 * the #ifdef gating of the vtable factory fields. The runtime masks
	 * these against the non-NULL factory pointers as well. */
	uint32_t apis = 0;
#ifdef XRT_HAVE_LEIA_SR_VULKAN
	apis |= XRT_DP_API_BIT_VK;
#endif
#ifdef XRT_HAVE_LEIA_SR_D3D11
	apis |= XRT_DP_API_BIT_D3D11;
#endif
#ifdef XRT_HAVE_LEIA_SR_D3D12
	apis |= XRT_DP_API_BIT_D3D12;
#endif
#ifdef XRT_HAVE_LEIA_SR_GL
	apis |= XRT_DP_API_BIT_GL;
#endif
	/* No Metal weaver in drv_leia — that's the macOS sim_display path. */

	uint32_t n = 0;
	for (uint32_t i = 0; i < display_count && n < max_claims; i++) {
		if (!leia_edid_table_contains(displays[i].edid_manufacturer, displays[i].edid_product)) {
			continue;
		}
		struct xrt_display_claim *c = &out_claims[n++];
		c->monitor_id = displays[i].monitor_id;
		c->confidence = verified ? (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED : (uint32_t)XRT_DISPLAY_CLAIM_EDID;
		c->supported_apis = apis;
		/*
		 * TODO(#69 Phase 2 follow-up): read the FPC device serial from
		 * `Global\sharedDeviceSerialMemory` so multi-Leia setups can pair
		 * each monitor with its own camera/calibration unit. Empty for now
		 * — single-display setups don't need it.
		 */
		c->serial[0] = '\0';

		U_LOG_I("leia_plugin: claim monitor 0x%016llx (mfr=0x%04X prod=0x%04X) confidence=%s",
		        (unsigned long long)displays[i].monitor_id, displays[i].edid_manufacturer,
		        displays[i].edid_product, verified ? "VERIFIED" : "EDID");
	}
	return n;
}


/*
 *
 * Vtable.
 *
 */

// Baked in by CMake from `git describe` (#47) so consumers (loader log,
// displayxr-cli, the Android diagnostics dashboard) can spot stale builds.
#ifndef DXR_PLUGIN_GIT_DESC
#define DXR_PLUGIN_GIT_DESC "unknown"
#endif

static struct xrt_plugin_iface g_leia_iface = {
    .struct_size = sizeof(struct xrt_plugin_iface),
    .reserved_0 = 0,

    .id = "leia-sr",
    .display_name = "DisplayXR Leia SR",
    .vendor = "Leia Inc.",
    .version = DXR_PLUGIN_GIT_DESC,

    .probe = leia_plugin_probe,
    .create_device = leia_plugin_create_device,

    /*
     * Per-graphics-API DP factories. Each compile-time-gated to the
     * weaver libraries available in the SR SDK at build time, so a
     * plug-in built without the D3D12 weaver (etc.) cleanly surfaces
     * NULL — the runtime then falls back to the sim_display DP for
     * that API path on the same probe-winning Leia device. The
     * factory signatures already match the xrt_dp_factory_*_fn_t
     * typedefs.
     */
#ifdef XRT_HAVE_LEIA_SR_VULKAN
    .create_dp_vk = leia_dp_factory_vk,
#else
    .create_dp_vk = NULL,
#endif

#ifdef XRT_HAVE_LEIA_SR_D3D11
    .create_dp_d3d11 = leia_dp_factory_d3d11,
#else
    .create_dp_d3d11 = NULL,
#endif

#ifdef XRT_HAVE_LEIA_SR_D3D12
    .create_dp_d3d12 = leia_dp_factory_d3d12,
#else
    .create_dp_d3d12 = NULL,
#endif

#ifdef XRT_HAVE_LEIA_SR_GL
    .create_dp_gl = leia_dp_factory_gl,
#else
    .create_dp_gl = NULL,
#endif

    /* drv_leia has no Metal weaver — that's the macOS sim_display path. */
    .create_dp_metal = NULL,

    .destroy = leia_plugin_destroy,

    .get_display_info = leia_plugin_get_display_info,

    .set_pose_source = leia_plugin_set_pose_source,

    .probe_displays = leia_plugin_probe_displays,
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

	*out_iface = &g_leia_iface;
	return XRT_SUCCESS;
}
