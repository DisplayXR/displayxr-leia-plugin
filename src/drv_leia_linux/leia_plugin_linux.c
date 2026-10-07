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
#include "leia_screen_linux.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
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
	 *   - otherwise the SR service's active device serial (its device store,
	 *     <root>/active -> Devices/<serial>; srLensGetSerialNumber as a last
	 *     resort) — system-wide, so it verifies a single panel only.
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
 * Multi-screen M1/M4: per-monitor display info and per-screen DPs. Both slots
 * are appended to xrt_plugin_iface (ADR-020, struct_size-gated) and compiled
 * only when the runtime headers announce them, so this file still builds —
 * slots compiled out — against a runtime pinned before multi-screen.
 *
 */

#ifdef XRT_PLUGIN_IFACE_HAS_DISPLAY_INFO_FOR_MONITOR
/*!
 * Fill an xrt_plugin_display_info from a resolved screen (the shared tail of
 * get_display_info_for_monitor). The SR-driven panel only: its eye tracking,
 * and the same recommended view scale get_display_info reports (seeded at
 * probe from the same SR numbers — never a second figure).
 */
static void
leia_lnx_fill_info_from_screen(const struct leia_lnx_screen *scr, struct xrt_plugin_display_info *out_info)
{
	out_info->display_width_m = scr->width_m;
	out_info->display_height_m = scr->height_m;
	out_info->nominal_viewer_x_m = scr->nominal_viewer_x_m;
	out_info->nominal_viewer_y_m = scr->nominal_viewer_y_m;
	out_info->nominal_viewer_z_m = scr->nominal_viewer_z_m;
	out_info->display_pixel_width = scr->pixel_width;
	out_info->display_pixel_height = scr->pixel_height;
	leia_view_scale_get(&out_info->recommended_view_scale_x, &out_info->recommended_view_scale_y);
	out_info->display_screen_left = scr->screen_left;
	out_info->display_screen_top = scr->screen_top;
	out_info->supported_eye_tracking_modes = 1u; /* MANAGED_BIT */
	out_info->default_eye_tracking_mode = 0u;    /* MANAGED */
	if (out_info->struct_size >= offsetof(struct xrt_plugin_display_info, refresh_mhz) + sizeof(uint32_t)) {
		out_info->refresh_mhz = scr->refresh_mhz;
	}
}

/*! Resolve @p b against the claim table, the cached EDID list and the SR query. */
static void
leia_lnx_resolve_screen(const struct leia_lnx_screen_binding *b, struct leia_lnx_screen *out)
{
	struct leia_lnx_claim_binding claim = {0};
	const bool have_claim = leia_lnx_claims_lookup(b->monitor_id, &claim);
	struct leia_lnx_edid_panel panels[LEIA_LNX_EDID_MAX_PANELS];
	const uint32_t panel_count = leia_lnx_edid_panels_cached(panels, LEIA_LNX_EDID_MAX_PANELS);
	struct leiasr_lnx_display_info sr = {0};
	if (!leiasr_lnx_query_display_info(&sr)) {
		sr.valid = false;
	}
	leia_lnx_screen_resolve(b, have_claim ? &claim : NULL, panels, panel_count, &sr,
	                        leia_lnx_sr_active_display_id(), out);
}

static bool
leia_lnx_plugin_get_display_info_for_monitor(struct xrt_plugin_instance *inst,
                                             const struct xrt_display_descriptor *display,
                                             const struct xrt_display_physical *physical,
                                             struct xrt_plugin_display_info *out_info)
{
	(void)inst;
	if (display == NULL || out_info == NULL ||
	    display->struct_size < offsetof(struct xrt_display_descriptor, screen_top) + sizeof(display->screen_top)) {
		return false;
	}
	// Only monitors this plug-in claimed in probe_displays.
	if (!leia_lnx_claims_lookup(display->monitor_id, NULL)) {
		return false;
	}

	struct leia_lnx_screen_binding b = {0};
	b.monitor_id = display->monitor_id;
	b.desktop_left = display->screen_left;
	b.desktop_top = display->screen_top;
	b.desktop_width = display->pixel_width;
	b.desktop_height = display->pixel_height;
	if (physical != NULL) {
		const uint32_t psz = physical->struct_size;
		if (psz >= offsetof(struct xrt_display_physical, physical_height_mm) + sizeof(uint32_t)) {
			b.width_mm = physical->physical_width_mm;
			b.height_mm = physical->physical_height_mm;
		}
		if (psz >= offsetof(struct xrt_display_physical, native_pixel_height) + sizeof(uint32_t)) {
			b.native_width = physical->native_pixel_width;
			b.native_height = physical->native_pixel_height;
		}
	}

	struct leia_lnx_screen scr;
	leia_lnx_resolve_screen(&b, &scr);
	// A Leia panel the SR context does not drive (a second panel, until
	// LeiaSR phase D) has no tracking or calibration here: let the runtime
	// derive its EDID defaults (no eye tracking, view scale 1).
	if (!scr.valid || !scr.is_sr_panel) {
		return false;
	}
	leia_lnx_fill_info_from_screen(&scr, out_info);
	return true;
}
#endif

#ifdef XRT_PLUGIN_IFACE_HAS_CREATE_DP_FOR_SCREEN
/*! Mirror the runtime's binding into the plain struct, never reading past its struct_size. */
static void
leia_lnx_binding_from_xrt(const struct xrt_screen_binding *xb, struct leia_lnx_screen_binding *out)
{
	memset(out, 0, sizeof(*out));
	const uint32_t sz = xb->struct_size;
#define LEIA_HAS(field) (sz >= offsetof(struct xrt_screen_binding, field) + sizeof(xb->field))
	if (LEIA_HAS(monitor_id)) {
		out->monitor_id = xb->monitor_id;
	}
	if (LEIA_HAS(desktop_height)) {
		out->desktop_left = xb->desktop_left;
		out->desktop_top = xb->desktop_top;
		out->desktop_width = xb->desktop_width;
		out->desktop_height = xb->desktop_height;
	}
	if (LEIA_HAS(native_pixel_height)) {
		out->native_width = xb->native_pixel_width;
		out->native_height = xb->native_pixel_height;
	}
	if (LEIA_HAS(physical_height_mm)) {
		out->width_mm = xb->physical_width_mm;
		out->height_mm = xb->physical_height_mm;
	}
	if (LEIA_HAS(desktop_scale)) {
		out->desktop_scale = xb->desktop_scale;
	}
	if (LEIA_HAS(display_id)) {
		out->display_id = xb->display_id;
	}
	if (LEIA_HAS(serial)) {
		snprintf(out->serial, sizeof(out->serial), "%.*s", (int)sizeof(xb->serial), xb->serial);
	}
	if (LEIA_HAS(device_name)) {
		snprintf(out->device_name, sizeof(out->device_name), "%.*s", (int)sizeof(xb->device_name),
		         xb->device_name);
	}
#undef LEIA_HAS
}

static xrt_result_t
leia_lnx_plugin_create_dp_vk_for_screen(struct xrt_plugin_instance *inst,
                                        void *vk_bundle,
                                        void *vk_cmd_pool,
                                        void *window_handle,
                                        int32_t target_format,
                                        const struct xrt_screen_binding *binding,
                                        struct xrt_display_processor **out_xdp)
{
	(void)inst;
	if (binding == NULL || out_xdp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	struct leia_lnx_screen_binding b;
	leia_lnx_binding_from_xrt(binding, &b);
	return leia_lnx_dp_factory_vk_for_screen(vk_bundle, vk_cmd_pool, window_handle, target_format, &b, out_xdp);
}
#endif


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

#ifdef XRT_PLUGIN_IFACE_HAS_DISPLAY_INFO_FOR_MONITOR
    /* Per-monitor display info (multi-screen M1). */
    .get_display_info_for_monitor = leia_lnx_plugin_get_display_info_for_monitor,
#endif
#ifdef XRT_PLUGIN_IFACE_HAS_CREATE_DP_FOR_SCREEN
    /* One DP per screen (multi-screen M2 slot; this plug-in's side is M4). */
    .create_dp_vk_for_screen = leia_lnx_plugin_create_dp_vk_for_screen,
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

	*out_iface = &g_leia_lnx_iface;
	return XRT_SUCCESS;
}
