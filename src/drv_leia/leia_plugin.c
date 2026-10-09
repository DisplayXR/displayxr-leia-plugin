// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
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
 * The probe is presence checks only — SR registry key, SR client DLLs
 * bindable (they are delay-loaded, so this DLL loads without the SR
 * platform), the SR Service's shared-memory mapping, the EDID table — and
 * never creates an SR context or waits for SR (install-order epic,
 * displayxr-runtime#1803, P-a/P-b). Readiness waiting belongs to device
 * creation. The outcome is one generic platform state + hint
 * (leia_platform_state.h), reported through the runtime's
 * `get_platform_state` slot when the runtime headers carry it.
 *
 * Issue #256 — vendor plug-in re-architecture.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"

#include "util/u_logging.h"
#include "os/os_time.h"

#include "leia_interface.h"
#include "leia_platform_state.h"
#include "leia_display_claims_win.h"
#include "leia_display_processor.h"
#ifdef XRT_HAVE_LEIA_SR_VULKAN
/*
 * #1243 fingerprint source. Gated on the SAME condition as the VK sources
 * (src/drv_leia/CMakeLists.txt only adds leia_display_processor.cpp, and only
 * puts the aux Vulkan headers on the include path, inside the Vulkan block), so
 * a Vulkan-less configure still builds.
 */
#include "vk/vk_helpers.h"
#endif
#ifdef XRT_HAVE_LEIA_SR_D3D11
#include "leia_display_processor_d3d11.h"
#include "leia_sr_ready.h" /* the ONE geometry resolver behind get_display_info */
#endif
#ifdef XRT_HAVE_LEIA_SR_D3D12
#include "leia_display_processor_d3d12.h"
#endif
#ifdef XRT_HAVE_LEIA_SR_GL
#include "leia_display_processor_gl.h"
#endif
/* Stereo camera source (runtime ADR-043): only when the runtime headers
 * announce the slots, so a pin that predates them still builds. */
#include "leia_stereo_camera.h"

#include <stddef.h>
#include <string.h>


/*
 *
 * Vtable callbacks.
 *
 */

//! One WARN with the first probe's duration (acceptance: probe() <= ~100 ms).
static bool g_leia_probe_timing_logged = false;

static xrt_result_t
leia_plugin_probe(struct xrt_plugin_instance **out_inst)
{
	/*
	 * Presence checks only (P-b). Never waits for the SR platform: a panel
	 * the SR Service has not identified yet is device creation's problem
	 * (bounded readiness budget there), not the probe's — the probe used to
	 * spend up to the whole 20 s budget here on an EDID-table miss.
	 */
	const uint64_t t0 = os_monotonic_get_ns();
	struct leia_display_probe_result edid = {0};
	(void)leia_edid_probe_display(&edid);
	const enum leia_platform_state st = leia_platform_state_evaluate(&edid);
	const double ms = (double)(os_monotonic_get_ns() - t0) / 1e6;

	if (!g_leia_probe_timing_logged) {
		g_leia_probe_timing_logged = true;
		U_LOG_W("leia_plugin: probe took %.1f ms (platform state %s)", ms, leia_platform_state_name(st));
	}

	/*
	 * No per-instance state: drv_leia stores hardware state in file-scope
	 * statics inside the plug-in DLL (cached probe result, SR context, etc.).
	 * Mirrors the sim_display plug-in shape.
	 */
	*out_inst = NULL;
	if (st != LEIA_PLATFORM_READY) {
		U_LOG_I("leia_plugin: probe declined — %s (hw=%d sdk=%d service=%d, %.1f ms)",
		        leia_platform_state_name(st), edid.hw_found, edid.sdk_installed, edid.service_running, ms);
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	return XRT_SUCCESS;
}

static xrt_result_t
leia_plugin_create_device(struct xrt_plugin_instance *inst, struct xrt_device **out_dev)
{
	(void)inst;
	/* Every path below may reach SR (geometry resolve); the delay-loaded SR
	 * client DLLs must be bound first. probe() already required READY. */
	if (leia_sr_client_bind() != LEIA_SR_BIND_OK) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
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
	/* No instance state — nothing to free. Stop the late-identification
	 * watcher if one is running (leia_sr_ready.h), and the SR
	 * display-enumeration probe instance (multi-screen M0). */
#ifdef XRT_HAVE_LEIA_SR_D3D11
	leiasr_ready_shutdown();
#endif
	leia_win_sr_enumerate_shutdown();
	leia_win_claims_store(NULL, 0);
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
	 * Contract with the runtime (leia_sr_ready.h): this is called at
	 * instance create AND again on every client compositor create, so it
	 * must be cheap and non-blocking once the startup budget is spent —
	 * `false` immediately while the SR platform has not identified the
	 * panel, `true` with the real geometry once it has. The runtime re-fills
	 * its cached info whenever the answer changes. Never report the SDK's
	 * "default display" placeholders here: a false is strictly better than
	 * a wrong true, because a false gets retried and a wrong true gets
	 * cached for the life of the service.
	 *
	 * Native panel resolution + physical size + nominal viewer position all
	 * come from the ONE verified geometry record; SR-recommended view
	 * dimensions seed the per-view scale in the same publish.
	 */
	struct leiasr_geometry g = {0};
	if (leiasr_geometry_resolve(5.0, "get_display_info") && leiasr_geometry_get(&g)) {
		out_info->display_pixel_width = g.pixel_w;
		out_info->display_pixel_height = g.pixel_h;
		out_info->display_width_m = g.width_m;
		out_info->display_height_m = g.height_m;
		out_info->nominal_viewer_x_m = g.nominal_x_m;
		out_info->nominal_viewer_y_m = g.nominal_y_m;
		out_info->nominal_viewer_z_m = g.nominal_z_m;
		any_populated = true;
	}
	/* The scale is NOT computed here. It is derived once (see
	 * leia_view_scale_set_from_dims / leia_view_scale_get) and read back,
	 * so this scalar and rendering_modes[1].view_scale_x/y can never
	 * disagree — a disagreement sizes the app's views from one number and
	 * its tiles/atlas from the other. */
	leia_view_scale_get(&out_info->recommended_view_scale_x, &out_info->recommended_view_scale_y);

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

#ifdef XRT_PLUGIN_IFACE_HAS_DISPLAY_INFO_FOR_MONITOR
/*
 * Multi-screen M1 (and what M3's per-segment views need): describe ONE
 * monitor this plug-in claimed — its physical size and nominal viewer — so
 * the runtime's screen registry carries metres for every Leia panel, not only
 * the active one. The size comes from SR's own calibration (the enumeration
 * descriptor's cm, cached 2 s, non-blocking); the nominal viewer is the
 * active panel's, scaled to this panel's height (the Linux arm's rule).
 * Cheap and callable from any thread, as the slot requires. False for a
 * monitor this plug-in did not claim, or one SR cannot size: the runtime then
 * derives EDID defaults.
 */
static bool
leia_plugin_get_display_info_for_monitor(struct xrt_plugin_instance *inst,
                                         const struct xrt_display_descriptor *display,
                                         const struct xrt_display_physical *physical,
                                         struct xrt_plugin_display_info *out_info)
{
	(void)inst;
	if (display == NULL || out_info == NULL ||
	    display->struct_size < offsetof(struct xrt_display_descriptor, screen_top) + sizeof(display->screen_top)) {
		return false;
	}
	struct leia_win_claim_binding claim;
	memset(&claim, 0, sizeof(claim));
	if (!leia_win_claims_lookup(display->monitor_id, &claim)) {
		return false;
	}

	uint32_t width_mm = 0, height_mm = 0, px_w = 0, px_h = 0;
	if (claim.sr_display_id != 0) {
		struct leia_win_sr_display sd[LEIA_WIN_SR_MAX_DISPLAYS];
		const int32_t n = leia_win_sr_enumerate_displays(sd, LEIA_WIN_SR_MAX_DISPLAYS);
		for (int32_t i = 0; i < n; i++) {
			if (sd[i].display_id == claim.sr_display_id) {
				width_mm = sd[i].width_mm;
				height_mm = sd[i].height_mm;
				px_w = sd[i].native_w;
				px_h = sd[i].native_h;
				break;
			}
		}
	}
	if ((width_mm == 0 || height_mm == 0) && physical != NULL &&
	    physical->struct_size >= offsetof(struct xrt_display_physical, physical_height_mm) + sizeof(uint32_t)) {
		width_mm = physical->physical_width_mm;
		height_mm = physical->physical_height_mm;
	}
	if (width_mm == 0 || height_mm == 0) {
		return false; // nothing better than the runtime's own defaults
	}
	if (px_w == 0 || px_h == 0) {
		px_w = display->pixel_width;
		px_h = display->pixel_height;
	}

	out_info->display_width_m = (float)width_mm / 1000.0f;
	out_info->display_height_m = (float)height_mm / 1000.0f;
	out_info->display_pixel_width = px_w;
	out_info->display_pixel_height = px_h;
	out_info->display_screen_left = display->screen_left;
	out_info->display_screen_top = display->screen_top;
	out_info->refresh_mhz = display->refresh_mhz;

	// Nominal viewer: centred, at the active panel's nominal distance scaled
	// by the height ratio (identical panels => identical viewer). 0.6 m when
	// the active panel's geometry is not resolved yet.
	struct leiasr_geometry g = {0};
	float z = 0.6f;
	if (leiasr_geometry_get(&g) && g.height_m > 0.0f && g.nominal_z_m > 0.0f) {
		z = g.nominal_z_m * (out_info->display_height_m / g.height_m);
	}
	out_info->nominal_viewer_x_m = 0.0f;
	out_info->nominal_viewer_y_m = 0.0f;
	out_info->nominal_viewer_z_m = z;

	// View scale 0 = the runtime derives (a non-default screen renders at
	// native). MANAGED eye tracking, like the active panel: with SR D4 the
	// panel's own camera tracks it; before D4 its DP pins the nominal viewer.
	out_info->recommended_view_scale_x = 0.0f;
	out_info->recommended_view_scale_y = 0.0f;
	out_info->supported_eye_tracking_modes = 1u; /* MANAGED_BIT */
	out_info->default_eye_tracking_mode = 0u;    /* MANAGED */
	return true;
}
#endif

/*
 * The last SR-deferral claim probe_displays() reported, so its WARN fires
 * once per (monitor, EDID identity) rather than once per registry refresh —
 * the runtime re-runs probe_displays at ~1 Hz while the panel is
 * unidentified (displayxr-runtime#1722). Cleared whenever a refresh does not
 * take the deferral path, so a later return to it is logged again. Unguarded
 * like the EDID probe cache: the runtime serialises registry refreshes.
 */
static bool g_leia_sr_claim_logged = false;
static uint64_t g_leia_sr_claim_monitor_id = 0;
static uint16_t g_leia_sr_claim_mfr = 0;
static uint16_t g_leia_sr_claim_prod = 0;

static uint32_t
leia_plugin_probe_displays(struct xrt_plugin_instance *inst,
                           const struct xrt_display_descriptor *displays,
                           uint32_t display_count,
                           struct xrt_display_claim *out_claims,
                           uint32_t max_claims)
{
	(void)inst;

	/*
	 * Per-monitor claims (issue #69 / ADR-015). Mirrors the binary probe():
	 *   - Match EACH runtime-supplied descriptor's (mfr, product) against
	 *     the known-panel EDID table (the runtime already enumerated the
	 *     monitors, so we don't re-enumerate).
	 *   - SR SDK + service presence are system-global; check once and apply
	 *     to every matched monitor. Claims are made only in READY (EDID +
	 *     SDK + running service), always VERIFIED.
	 *   - The EDID table is a frozen copy of SR's product-code map and
	 *     drifts (SR's ProductCodeInstaller registers panels our table can't
	 *     see). So on a CLEAN table miss with the platform state READY (SR
	 *     has identified a device and no table-known panel was ever seen in
	 *     this process) we claim the primary monitor VERIFIED. Without this,
	 *     an SR-confirmed-but-table-unknown panel yields zero registry
	 *     claims, so the registry-driven DP selection (the D3D11 service /
	 *     shell path) loses the monitor to sim_display's FALLBACK claim.
	 *   - P-c: after a table-known panel was seen, a miss is an UNPLUG — the
	 *     state is NO_DISPLAY and nothing is claimed (this path used to claim
	 *     the primary monitor and weave on a normal screen).
	 *   - PLATFORM_ABSENT / INCOMPATIBLE / NO_DISPLAY / PLATFORM_NOT_RUNNING:
	 *     claim nothing — this DLL now loads without SR, and an EDID claim
	 *     would win the monitor over sim_display only for probe() to decline
	 *     (#294: with SR Service stopped the runtime must start on the
	 *     fallback; its slow re-probe timer adopts this plug-in once the
	 *     service is back and the state reads READY).
	 */
	struct leia_display_probe_result probe = {0};
	(void)leia_edid_probe_display(&probe);
	const enum leia_platform_state st = leia_platform_state_evaluate(&probe);
	if (st != LEIA_PLATFORM_READY) {
		g_leia_sr_claim_logged = false;
		return 0;
	}

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

	/*
	 * Multi-screen M0: join the runtime's monitor list with the SR runtime's
	 * own display enumeration (srEnumerateDisplays, Windows slot 106) when the
	 * installed SR runtime has it. Matching + confidence rules live in
	 * leia_display_claims_win.c:
	 *   - SR lists the monitor FPC_VERIFIED -> VERIFIED + FPC serial;
	 *   - SR lists it EDID_ONLY (or the frozen table knows it but SR does not)
	 *     -> EDID confidence, no serial;
	 *   - SR does not enumerate (older runtime, service down, compiled out)
	 *     -> today's rule: table hit + READY = VERIFIED, no serial.
	 * The per-monitor binding (displayId, HMONITOR, device name) is kept in a
	 * plug-in-private table for the M5/M6 per-DP binding.
	 */
	struct leia_win_sr_display sr_displays[LEIA_WIN_SR_MAX_DISPLAYS];
	const int32_t sr_count = leia_win_sr_enumerate_displays(sr_displays, LEIA_WIN_SR_MAX_DISPLAYS);

	const struct leia_win_claim_inputs in = {
	    .sr_displays = sr_displays,
	    .sr_display_count = sr_count,
	    .table_contains = leia_edid_table_contains,
	    .legacy_table_verified = true, /* platform state is READY here */
	    .supported_apis = apis,
	};
	struct leia_win_claim_binding bindings[LEIA_WIN_SR_MAX_DISPLAYS * 2];
	const uint32_t bind_cap = max_claims < (uint32_t)(LEIA_WIN_SR_MAX_DISPLAYS * 2) ? max_claims
	                                                                                : (uint32_t)(LEIA_WIN_SR_MAX_DISPLAYS * 2);
	uint32_t n = leia_win_compute_claims(displays, display_count, &in, out_claims, bindings, bind_cap);
	leia_win_claims_store(bindings, n);

	/* One WARN per change of the claim set, INFO otherwise (the runtime
	 * re-runs this per registry refresh). */
	uint64_t fp = 1469598103934665603ull ^ (uint64_t)n ^ ((uint64_t)(sr_count + 1) << 32);
	for (uint32_t i = 0; i < n; i++) {
		fp = (fp ^ out_claims[i].monitor_id) * 1099511628211ull;
		fp = (fp ^ out_claims[i].confidence) * 1099511628211ull;
		for (const char *c = out_claims[i].serial; *c != '\0'; c++) {
			fp = (fp ^ (uint64_t)(uint8_t)*c) * 1099511628211ull;
		}
	}
	static uint64_t s_logged_claims_fp = 0;
	const bool changed = fp != s_logged_claims_fp;
	s_logged_claims_fp = fp;
	for (uint32_t i = 0; i < n; i++) {
		const struct xrt_display_claim *c = &out_claims[i];
		const char *conf = c->confidence >= (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED ? "VERIFIED" : "EDID";
		if (changed) {
			U_LOG_W("leia_plugin: claim monitor 0x%016llx confidence=%s serial='%s' sr_display=0x%016llx "
			        "device='%s' (%s)",
			        (unsigned long long)c->monitor_id, conf, c->serial,
			        (unsigned long long)bindings[i].sr_display_id, bindings[i].device_name,
			        sr_count >= 0 ? "SR enumeration" : "frozen EDID table");
		} else {
			U_LOG_I("leia_plugin: claim monitor 0x%016llx confidence=%s serial='%s' -- unchanged",
			        (unsigned long long)c->monitor_id, conf, c->serial);
		}
	}

	/*
	 * Clean EDID-table miss while the platform state is READY: by
	 * construction (leia_platform_state.c) that means SR has identified a
	 * device and no table-known panel has been seen in this process — the
	 * stale-table case. Claim the primary monitor VERIFIED so it beats
	 * sim_display's FALLBACK(10). A named-mapping read, not the 2 s SR
	 * context spin it used to be. Single-display assumption: SR confirms *an*
	 * active SR display but not *which* monitor id, so we pin it to the
	 * primary — the monitor the runtime's own back-compat synth-claim picks.
	 * Only when SR could NOT enumerate: with srEnumerateDisplays the table
	 * miss is answered per monitor above.
	 */
	if (sr_count < 0 && n == 0 && !probe.hw_found && display_count > 0 && max_claims > 0) {
		uint32_t pick = 0;
		for (uint32_t i = 0; i < display_count; i++) {
			if (displays[i].flags & 1u) { /* bit 0 = primary monitor */
				pick = i;
				break;
			}
		}
		struct xrt_display_claim *c = &out_claims[n++];
		c->monitor_id = displays[pick].monitor_id;
		c->confidence = (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED;
		c->supported_apis = apis;
		c->serial[0] = '\0';

		const bool same_claim = g_leia_sr_claim_logged &&                              //
		                        g_leia_sr_claim_monitor_id == displays[pick].monitor_id && //
		                        g_leia_sr_claim_mfr == displays[pick].edid_manufacturer && //
		                        g_leia_sr_claim_prod == displays[pick].edid_product;
		if (!same_claim) {
			U_LOG_W("leia_plugin: EDID table miss with SR present — claiming primary monitor "
			        "0x%016llx (mfr=0x%04X prod=0x%04X) VERIFIED via SR runtime probe "
			        "(table stale relative to SR's product-code registry)",
			        (unsigned long long)displays[pick].monitor_id, displays[pick].edid_manufacturer,
			        displays[pick].edid_product);
			g_leia_sr_claim_logged = true;
			g_leia_sr_claim_monitor_id = displays[pick].monitor_id;
			g_leia_sr_claim_mfr = displays[pick].edid_manufacturer;
			g_leia_sr_claim_prod = displays[pick].edid_product;
		} else {
			U_LOG_I("leia_plugin: SR runtime still confirms monitor 0x%016llx — claim unchanged",
			        (unsigned long long)displays[pick].monitor_id);
		}
	} else {
		g_leia_sr_claim_logged = false;
	}

	return n;
}


#ifdef XRT_PLUGIN_HAS_PLATFORM_STATE
/*
 * ADR-045 platform state (runtime slot). Callable before probe(), from any
 * thread, at any time: presence checks only. The EDID enumeration re-runs at
 * most every 2 s (or on a monitor-topology change); the registry key and the
 * SR Service mapping are re-read on every call.
 */
static bool
leia_plugin_get_platform_state(struct xrt_plugin_platform_status *out_status)
{
	if (out_status == NULL || out_status->struct_size < offsetof(struct xrt_plugin_platform_status, hint)) {
		return false;
	}
	const enum leia_platform_state st = leia_platform_state_refresh(2000);
	out_status->state = (uint32_t)st; /* leia_platform_state mirrors xrt_plugin_platform_state */
	out_status->flags = 0;           /* a vendor plug-in: never FALLBACK */
	if (out_status->struct_size >= offsetof(struct xrt_plugin_platform_status, hint) + sizeof(out_status->hint)) {
		const char *hint = leia_platform_state_get_hint();
		size_t len = strlen(hint);
		if (len >= sizeof(out_status->hint)) {
			len = sizeof(out_status->hint) - 1;
		}
		memcpy(out_status->hint, hint, len);
		out_status->hint[len] = '\0';
	}
	return true;
}
#endif


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
    /*
     * #1243/#1244 vk_bundle ABI fingerprint (#233). The loader compares these against
     * its own sizeof/offsetof and refuses the VK DP factory on mismatch, so a
     * build-config skew (NDEBUG changes os_mutex by 16 bytes = two fn-pointer
     * slots) surfaces as an unwoven session plus an actionable error instead of
     * a crash inside the Vulkan driver.
     *
     * Without these the loader takes its "unknown" branch: it REFUSES the VK
     * factory on Android but only WARNS on desktop, so every Windows pairing
     * shipped unverified -- the guard existed and this arm was silently outside
     * it. Android has set them since #1244; this closes the Windows half.
     *
     * Set only where there IS a VK factory to fingerprint; a Vulkan-less build
     * leaves them 0, which is the correct "no VK factory" answer.
     */
    .vk_bundle_abi_size = (uint32_t)sizeof(struct vk_bundle),
    .vk_bundle_fn_table_offset = (uint32_t)offsetof(struct vk_bundle, vkGetInstanceProcAddr),
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
#ifdef XRT_PLUGIN_IFACE_HAS_DISPLAY_INFO_FOR_MONITOR
    /* Multi-screen M1: per-monitor size + nominal viewer for every Leia panel claimed. */
    .get_display_info_for_monitor = leia_plugin_get_display_info_for_monitor,
#endif

    /*
     * ADR-042 lift-only D3D11 DP: no weaver / window / tracker, only the NeurD
     * lift slots (docs/lift-neurd.md). Needs the runtime header's field AND the
     * NeurD headers (DXR_LEIA_HAS_NEURD); otherwise the field is absent or NULL
     * and the runtime has no lift from this plug-in. The iface header carrying
     * this field implies its DP header carries XRT_DP_D3D11_HAS_LIFT.
     */
#if defined(XRT_PLUGIN_IFACE_HAS_D3D11_LIFT_FACTORY)
#if defined(XRT_HAVE_LEIA_SR_D3D11) && defined(DXR_LEIA_HAS_NEURD)
    .create_dp_d3d11_lift = leia_dp_factory_d3d11_lift,
#else
    .create_dp_d3d11_lift = NULL,
#endif
#endif

#ifdef XRT_PLUGIN_HAS_PLATFORM_STATE
    .get_platform_state = leia_plugin_get_platform_state,
#endif

#if defined(XRT_PLUGIN_IFACE_HAS_CREATE_DP_D3D11_FOR_SCREEN)
    /* Multi-screen M6: one D3D11 DP per screen a spanning window covers. */
#if defined(XRT_HAVE_LEIA_SR_D3D11)
    .create_dp_d3d11_for_screen = leia_dp_factory_d3d11_for_screen,
#else
    .create_dp_d3d11_for_screen = NULL,
#endif
#endif

#if defined(XRT_PLUGIN_IFACE_HAS_CREATE_DP_D3D12_FOR_SCREEN)
    /* Multi-screen M6 (D3D12): one D3D12 DP per screen a spanning window covers. */
#if defined(XRT_HAVE_LEIA_SR_D3D12)
    .create_dp_d3d12_for_screen = leia_dp_factory_d3d12_for_screen,
#else
    .create_dp_d3d12_for_screen = NULL,
#endif
#endif

#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA
    /*
     * The SR eye tracker's camera as a runtime stereo camera source
     * (XR_DXR_stereo_camera, runtime ADR-043, phase L1). Read from the SR
     * raw-camera shared memory by POLLING (never the auto-reset event),
     * calibration of the ACTIVE device's serial, tracker keep-alive while
     * open. See leia_stereo_camera.cpp. All six or none (the runtime checks).
     */
    .stereo_camera_enumerate = leia_stereo_camera_enumerate,
    .stereo_camera_get_calibration = leia_stereo_camera_get_calibration,
    .stereo_camera_open = leia_stereo_camera_open,
    .stereo_camera_wait_frame = leia_stereo_camera_wait_frame,
    .stereo_camera_release_frame = leia_stereo_camera_release_frame,
    .stereo_camera_close = leia_stereo_camera_close,
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

	*out_iface = &g_leia_iface;
	return XRT_SUCCESS;
}
