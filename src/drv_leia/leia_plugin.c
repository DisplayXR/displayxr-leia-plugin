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
#include "leia_multi_viewer.h"
#include "leia_platform_state.h"
#include "leia_sr_version.h"
#include "leia_sr_api_select.h"
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
#include <stdio.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> /* InterlockedExchange (the MULTI_VIEWER value) */
#endif

#ifndef XRT_DP_API_BIT_HINT_WINDOWLESS_PHASE
/* runtime ADR-060 R2: claim-mask capability hint, from xrt_plugin.h once the
 * runtime pin includes it (XRT_PLUGIN_HAS_WINDOWLESS_PHASE_HINT). Same value. */
#define XRT_DP_API_BIT_HINT_WINDOWLESS_PHASE (1u << 24)
#endif


/*
 *
 * Vtable callbacks.
 *
 */

//! One WARN with the first probe's duration (acceptance: probe() <= ~100 ms).
static bool g_leia_probe_timing_logged = false;

//! `xrt_plugin_iface::id`, and the `id` this plug-in's settings are read under.
#define LEIA_PLUGIN_ID "leia-sr"


/*
 *
 * Settings (ADR-054 decision 7: read ONLY through host->get_setting).
 *
 */

#ifdef XRT_PLUGIN_HOST_HAS_GET_SETTING
//! The host's setting reader, copied at negotiate (process-lifetime pointer in
//! the runtime). NULL on a runtime that predates the slot: every setting then
//! keeps its compiled default.
static xrt_component_get_setting_fn_t g_leia_get_setting = NULL;
#endif

/*!
 * The effective MULTI_VIEWER value (enum leia_multi_viewer) the weave paths
 * apply, published by leia_plugin_resolve_multi_viewer() at each device
 * creation. A LONG for the Interlocked functions; SYSTEM until then.
 */
static volatile LONG g_leia_multi_viewer_active = (LONG)LEIA_MULTI_VIEWER_SYSTEM;

enum leia_multi_viewer
leia_multi_viewer_active(void)
{
	return (enum leia_multi_viewer)InterlockedCompareExchange(&g_leia_multi_viewer_active, 0, 0);
}

/*!
 * Read MULTI_VIEWER once (#446: multi-viewer weaving is a setting, not a third
 * rendering mode, under the runtime's two-mode contract), decide whether this
 * build and the installed SR runtime can honour it, and publish the effective
 * value for the weavers. Called at device creation, after the SR client DLLs
 * are bound (the capability query reaches SR). Start-up only by contract: a
 * change takes effect at the next device creation (the next app start, or the
 * next service start for IPC apps).
 */
static void
leia_plugin_resolve_multi_viewer(void)
{
	char buf[64];
	buf[0] = '\0';
	bool set = false;
#ifdef XRT_PLUGIN_HOST_HAS_GET_SETTING
	if (g_leia_get_setting != NULL) {
		set = g_leia_get_setting(LEIA_PLUGIN_ID, LEIA_MULTI_VIEWER_KEY, buf, (uint32_t)sizeof(buf));
		buf[sizeof(buf) - 1] = '\0';
	}
#endif

	enum leia_multi_viewer setting = LEIA_MULTI_VIEWER_SYSTEM;
	if (set && !leia_multi_viewer_parse(buf, &setting) && buf[0] != '\0') {
		U_LOG_W("leia_plugin: MULTI_VIEWER='%s' is not one of system|on|off -- using system", buf);
	}

	enum leia_multi_viewer effective = LEIA_MULTI_VIEWER_SYSTEM;
	if (setting != LEIA_MULTI_VIEWER_SYSTEM) {
		/* Asked only when it matters: the query may spin up the SR probe
		 * instance. False when this build's SR SDK predates the feature
		 * (compiled out), or the SR runtime does not report it. */
		const bool supported = leia_win_sr_multi_viewer_weave_supported();
		effective = leia_multi_viewer_effective(setting, supported);
		if (effective != setting) {
#ifdef DXR_LEIA_HAS_SR_MULTI_VIEWER
			const char *why = "the installed LeiaSR runtime does not report multi-viewer weaving";
#else
			const char *why = "this plug-in was built against an SR SDK without multi-viewer weaving";
#endif
			U_LOG_W("leia_plugin: MULTI_VIEWER=%s ignored -- %s; following the LeiaSR system setting",
			        leia_multi_viewer_name(setting), why);
		}
	}

	U_LOG_W("leia_plugin: multi-viewer weave: %s (MULTI_VIEWER %s%s)", leia_multi_viewer_name(effective),
	        leia_multi_viewer_name(setting), set ? "" : ", default");
	InterlockedExchange(&g_leia_multi_viewer_active, (LONG)effective);
}

/*
 * Setting status (displayxr-runtime #2773): tells `displayxr-cli dp options`
 * and the DisplayXR Dashboard whether "Several viewers" (MULTI_VIEWER) can
 * take effect on this machine. It cannot when the installed LeiaSR runtime
 * has no multi-viewer weaving (plain LeiaSR 1.39 rc1), or when this plug-in
 * was built against an SR SDK without it. The Dashboard then shows "Not
 * supported here" with the note below, instead of a choice that silently
 * does nothing.
 *
 * Mirrored like the platform remedy, so the plug-in reports it WITHOUT
 * re-pinning DXR_RUNTIME_GIT_TAG (a re-pin raises the installer's derived
 * MIN_RUNTIME_VERSION floor). The status struct's layout and the iface
 * slot's offset are the runtime's, checked at compile time against headers
 * that carry them (XRT_PLUGIN_IFACE_HAS_SETTING_STATUS).
 */
#define LEIA_SETTING_STATE_APPLIES 1u
#define LEIA_SETTING_STATE_NOT_SUPPORTED 2u

struct leia_setting_status
{
	uint32_t struct_size;
	uint32_t state;
	char effective_value[64];
	char note[160];
	uint64_t reserved[4];
};

#ifdef XRT_PLUGIN_IFACE_HAS_SETTING_STATUS
/* Compile-time checks (portable to MSVC C): a negative array size fails the build. */
typedef char leia_setting_status_mirror_size[(sizeof(struct leia_setting_status) ==
                                              sizeof(struct xrt_component_setting_status))
                                                 ? 1
                                                 : -1];
typedef char leia_setting_status_mirror_note[(offsetof(struct leia_setting_status, note) ==
                                              offsetof(struct xrt_component_setting_status, note))
                                                 ? 1
                                                 : -1];
typedef char leia_setting_state_mirror[(LEIA_SETTING_STATE_NOT_SUPPORTED ==
                                            XRT_COMPONENT_SETTING_STATE_NOT_SUPPORTED &&
                                        LEIA_SETTING_STATE_APPLIES == XRT_COMPONENT_SETTING_STATE_APPLIES)
                                           ? 1
                                           : -1];
#endif

static void
leia_status_copy(char *dst, size_t cap, const char *src)
{
	size_t len = strlen(src);
	if (len >= cap) {
		len = cap - 1;
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

#define LEIA_STATUS_HAS(st, field)                                                                                     \
	((size_t)(st)->struct_size >= offsetof(struct leia_setting_status, field) + sizeof((st)->field))

static bool
leia_plugin_get_setting_status_impl(const char *key, struct leia_setting_status *out)
{
	if (key == NULL || out == NULL || strcmp(key, LEIA_MULTI_VIEWER_KEY) != 0 || !LEIA_STATUS_HAS(out, state)) {
		return false;
	}
	/* Answer only when SR is installed and its service is up: otherwise the
	 * platform state (and its hint) already says what is wrong, and a
	 * capability query would only fail. */
	const enum leia_platform_state st = leia_platform_state_refresh(2000);
	if (st != LEIA_PLATFORM_READY && st != LEIA_PLATFORM_NO_DISPLAY) {
		return false;
	}
#ifdef DXR_LEIA_HAS_SR_MULTI_VIEWER
	if (leia_win_sr_multi_viewer_weave_supported()) {
		out->state = LEIA_SETTING_STATE_APPLIES;
		return true;
	}
	const char *note = "This LeiaSR runtime shows 3D to one viewer at a time. "
	                   "Install a LeiaSR build with multi-viewer weaving to use this option.";
#else
	const char *note = "This Leia plug-in was built without multi-viewer support. "
	                   "Update the DisplayXR Leia plug-in to use this option.";
#endif
	out->state = LEIA_SETTING_STATE_NOT_SUPPORTED;
	if (LEIA_STATUS_HAS(out, effective_value)) {
		leia_status_copy(out->effective_value, sizeof(out->effective_value), LEIA_MULTI_VIEWER_DEFAULT);
	}
	if (LEIA_STATUS_HAS(out, note)) {
		leia_status_copy(out->note, sizeof(out->note), note);
	}
	return true;
}

#ifdef XRT_PLUGIN_IFACE_HAS_SETTING_STATUS
static bool
leia_plugin_get_setting_status(const char *key, struct xrt_component_setting_status *out)
{
	return leia_plugin_get_setting_status_impl(key, (struct leia_setting_status *)out);
}
#else
static bool
leia_plugin_get_setting_status(const char *key, struct leia_setting_status *out)
{
	return leia_plugin_get_setting_status_impl(key, out);
}
#endif

#ifdef XRT_PLUGIN_IFACE_HAS_DESCRIBE_SETTINGS
//! What `displayxr-cli dp options leia-sr` lists (key, type, default, help).
static const struct xrt_component_setting_desc g_leia_settings[] = {
    {LEIA_MULTI_VIEWER_KEY, XRT_COMPONENT_SETTING_TYPE_ENUM, LEIA_MULTI_VIEWER_DEFAULT, LEIA_MULTI_VIEWER_HELP},
};

static uint32_t
leia_plugin_describe_settings(const struct xrt_component_setting_desc **out_descs)
{
	if (out_descs != NULL) {
		*out_descs = g_leia_settings;
	}
	return (uint32_t)(sizeof(g_leia_settings) / sizeof(g_leia_settings[0]));
}
#endif

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
	/* Before any display processor (and so any weaver) exists for it. */
	leia_plugin_resolve_multi_viewer();
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

/*
 * The origin just written came from an identified monitor (an EDID record,
 * SR's enumeration, a verified claim, the runtime's own descriptor): say so,
 * so a panel that IS the monitor at the desktop origin is not read as "no
 * preference" (displayxr-runtime#2577). Written only when the runtime's
 * struct reaches the field (an older runtime's struct ends before it).
 */
static void
leia_plugin_mark_origin_valid(struct xrt_plugin_display_info *out_info)
{
#ifdef XRT_PLUGIN_HAS_DISPLAY_ORIGIN_VALID
	if (out_info->struct_size >=
	    offsetof(struct xrt_plugin_display_info, display_flags) + sizeof(out_info->display_flags)) {
		out_info->display_flags |= XRT_PLUGIN_DISPLAY_FLAG_SCREEN_ORIGIN_VALID;
	}
#else
	(void)out_info;
#endif
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
		leia_plugin_mark_origin_valid(out_info);
        } else {
                /*
                 * #430: the frozen EDID table does not know this panel (a
                 * Lenovo 3D monitor on a laptop's non-main output), so the
                 * probe has no position — and (0, 0) means "no preference",
                 * which put the panel rect, Unity's TargetDisplay and the weave
                 * adapter on the laptop screen. probe_displays() already found
                 * the panel from SR's own enumeration and VERIFIED it: report
                 * THAT monitor's origin. Same space as the EDID probe's (the
                 * runtime's descriptor, read in this process). No SR call — a
                 * lock-guarded table read.
                 */
                struct leia_win_claim_binding claim;
                memset(&claim, 0, sizeof(claim));
                if (leia_win_claims_panel(out_info->display_pixel_width,
                                          out_info->display_pixel_height,
                                          &claim)) {
                  out_info->display_screen_left = claim.screen_left;
                  out_info->display_screen_top = claim.screen_top;
                  leia_plugin_mark_origin_valid(out_info);
                  static uint64_t s_logged_monitor = 0;
                  static int32_t s_logged_left = 0, s_logged_top = 0;
                  if (s_logged_monitor != claim.monitor_id ||
                      s_logged_left != claim.screen_left ||
                      s_logged_top != claim.screen_top) {
                    s_logged_monitor = claim.monitor_id;
                    s_logged_left = claim.screen_left;
                    s_logged_top = claim.screen_top;
                    U_LOG_W("leia_plugin: EDID table has no position for the "
                            "panel; reporting the "
                            "VERIFIED claim's monitor 0x%016llx '%s' at (%d, "
                            "%d) %ux%u (#430)",
                            (unsigned long long)claim.monitor_id,
                            claim.device_name, (int)claim.screen_left,
                            (int)claim.screen_top, claim.pixel_w,
                            claim.pixel_h);
                  }
                }
        }

	/*
	 * MANAGED always (the SR runtime owns the grace period + collapse). MANUAL
	 * too when the SR runtime has the per-weaver tracking-loss mode: the DPs
	 * then put their weavers in MANUAL on the runtime's set_eye_tracking_mode
	 * and report isTracking from the weaver's state. Default stays MANAGED.
	 * The probe is an SR call, so only once the SR client DLLs are bound.
	 */
	out_info->supported_eye_tracking_modes =
	    (leia_sr_client_bound() && leia_sr_api_tracking_loss_mode_available()) ? 3u /* MANAGED|MANUAL */
	                                                                          : 1u /* MANAGED_BIT */;
	out_info->default_eye_tracking_mode = 0u; /* MANAGED */

	return any_populated;
}

#ifdef XRT_PLUGIN_IFACE_HAS_DISPLAY_INFO_FOR_MONITOR
/*
 * Multi-screen M1 (and what M3's per-segment views need): describe ONE
 * monitor this plug-in claimed — its physical size and nominal viewer — so
 * the runtime's screen registry carries metres for every Leia panel, not only
 * the active one. The size comes from SR's own calibration (the enumeration
 * descriptor's cm), captured in the claim table when probe_displays bound the
 * monitor — a lock-guarded lookup, no SR call here; the nominal viewer is the
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

	// The bound SR display's calibrated size + native mode, captured at claim
	// time (zero when the claim has no SR display).
	uint32_t width_mm = claim.width_mm, height_mm = claim.height_mm, px_w = claim.native_w, px_h = claim.native_h;
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
	leia_plugin_mark_origin_valid(out_info);
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
	 * Which SR display a monitor IS comes from SR's own monitor list
	 * (srEnumerateMonitors, slot 108, SR joins by HMONITOR) when the installed
	 * SR runtime has it; monitors it does not list, or every monitor on an
	 * older SR runtime, fall back to EDID ids + desktop origin.
	 * The per-monitor binding (displayId, HMONITOR, device name, SR size) is
	 * kept in a plug-in-private table for the M5/M6 per-DP binding and
	 * get_display_info_for_monitor.
	 */
	struct leia_win_sr_display sr_displays[LEIA_WIN_SR_MAX_DISPLAYS];
	const int32_t sr_count = leia_win_sr_enumerate_displays(sr_displays, LEIA_WIN_SR_MAX_DISPLAYS);
	struct leia_win_sr_monitor sr_monitors[LEIA_WIN_SR_MAX_MONITORS];
	const int32_t sr_mon_count =
	    sr_count >= 0 ? leia_win_sr_enumerate_monitors(sr_monitors, LEIA_WIN_SR_MAX_MONITORS) : -1;

	/*
	 * runtime ADR-060 R2 (L4): hint that this box's SR can route a windowless
	 * weaver EXTERNALLY, so the D3D11 service may make a windowless weave DP
	 * for a present-owner (phase from set_present_origin) instead of binding a
	 * weaver to another process's window. Read from the probe instance the
	 * enumeration above just used; the DP instance's scanout bit stays the
	 * authority. Per-screen D3D11 weavers (the service's path) and per-screen
	 * D3D12 weavers (the in-process texture target) — the two arms whose DP
	 * declares the scanout bit.
	 */
#if defined(DXR_LEIA_HAS_SR_MULTI_WIN) && (defined(XRT_HAVE_LEIA_SR_D3D11) || defined(XRT_HAVE_LEIA_SR_D3D12))
	if (leia_win_sr_probe_external_routing()) {
		apis |= XRT_DP_API_BIT_HINT_WINDOWLESS_PHASE;
	}
#endif

	const struct leia_win_claim_inputs in = {
	    .sr_displays = sr_displays,
	    .sr_display_count = sr_count,
	    .table_contains = leia_edid_table_contains,
	    .legacy_table_verified = true, /* platform state is READY here */
	    .supported_apis = apis,
	    .sr_monitors = sr_monitors,
	    .sr_monitor_count = sr_mon_count,
	    .monitor_at = leia_win_monitor_at,
	};
	struct leia_win_claim_binding bindings[LEIA_WIN_SR_MAX_DISPLAYS * 2];
	const uint32_t bind_cap = max_claims < (uint32_t)(LEIA_WIN_SR_MAX_DISPLAYS * 2) ? max_claims
	                                                                                : (uint32_t)(LEIA_WIN_SR_MAX_DISPLAYS * 2);
	uint32_t n = leia_win_compute_claims(displays, display_count, &in, out_claims, bindings, bind_cap);
	leia_win_claims_store(bindings, n);

	/* One WARN per change of the claim set, INFO otherwise (the runtime
	 * re-runs this per registry refresh). */
	uint64_t fp = 1469598103934665603ull ^ (uint64_t)n ^ ((uint64_t)(sr_count + 1) << 32) ^
	              ((uint64_t)(sr_mon_count + 1) << 48);
	for (uint32_t i = 0; i < n; i++) {
		fp = (fp ^ out_claims[i].monitor_id) * 1099511628211ull;
		fp = (fp ^ out_claims[i].confidence) * 1099511628211ull;
		fp = (fp ^ bindings[i].sr_display_id ^ (uint64_t)bindings[i].sr_joined) * 1099511628211ull;
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
			U_LOG_W(
			    "leia_plugin: claim monitor 0x%016llx confidence=%s serial='%s' sr_display=0x%016llx "
			    "device='%s' (%s)",
			    (unsigned long long)c->monitor_id, conf, c->serial,
			    (unsigned long long)bindings[i].sr_display_id, bindings[i].device_name,
			    bindings[i].sr_joined ? "SR monitor join"
			    : sr_count >= 0       ? "SR enumeration, EDID/origin match"
			                          : "frozen EDID table");
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
 * ADR-045 remedy fields (displayxr-runtime#2765, ADR-045 amendment
 * 2026-11-22), mirrored here so the plug-in fills them WITHOUT re-pinning
 * DXR_RUNTIME_GIT_TAG: a re-pin would raise the installer's derived
 * MIN_RUNTIME_VERSION floor for an advisory notice. The layout is the
 * runtime's: four char arrays appended directly after `reserved[8]`. Each is
 * written only when the runtime's struct_size covers it, so an older runtime
 * (smaller struct_size) gets exactly what it got before. The flag bit is
 * likewise the runtime's value; an older runtime ignores unknown flag bits.
 * With headers that carry the fields, the static asserts below pin the mirror
 * to them.
 */
#define LEIA_REMEDY_FLAG_UPDATE_RECOMMENDED (1u << 1)

struct leia_platform_status_remedy
{
	/* The ADR-045 prefix, field for field as every runtime header has it. */
	uint32_t struct_size;
	uint32_t state;
	uint32_t flags;
	uint32_t reserved_0;
	char hint[XRT_PLUGIN_PLATFORM_HINT_MAX];
	uint64_t reserved[8];
	/* The remedy fields appended after it. */
	char installed_version[32];
	char required_version[32];
	char action_label[64];
	char action_url[256];
};

#ifdef XRT_PLUGIN_HAS_PLATFORM_REMEDY
/* Compile-time checks (portable to MSVC C): a negative array size fails the build. */
typedef char leia_remedy_hint_mirror[(offsetof(struct leia_platform_status_remedy, reserved) ==
                                      offsetof(struct xrt_plugin_platform_status, reserved))
                                         ? 1
                                         : -1];
typedef char leia_remedy_flag_mirror[(LEIA_REMEDY_FLAG_UPDATE_RECOMMENDED == XRT_PLUGIN_PLATFORM_FLAG_UPDATE_RECOMMENDED) ? 1 : -1];
typedef char leia_remedy_installed_mirror[(offsetof(struct leia_platform_status_remedy, installed_version) ==
                                           offsetof(struct xrt_plugin_platform_status, installed_version))
                                              ? 1
                                              : -1];
typedef char leia_remedy_url_mirror[(offsetof(struct leia_platform_status_remedy, action_url) ==
                                     offsetof(struct xrt_plugin_platform_status, action_url))
                                        ? 1
                                        : -1];
typedef char leia_remedy_size_mirror[(sizeof(struct leia_platform_status_remedy) ==
                                      sizeof(struct xrt_plugin_platform_status))
                                         ? 1
                                         : -1];
#endif

#define LEIA_REMEDY_HAS(st, field)                                                                                     \
	((size_t)(st)->struct_size >=                                                                                  \
	 offsetof(struct leia_platform_status_remedy, field) + sizeof(((struct leia_platform_status_remedy *)0)->field))

static void
copy_field(char *dst, size_t cap, const char *src)
{
	size_t len = strlen(src);
	if (len >= cap) {
		len = cap - 1;
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

/*
 * "Update recommended" when the installed LeiaSR runtime is older than
 * LEIA_SR_RECOMMENDED_VERSION AND a Leia panel is attached (EDID match).
 * Advisory only: the plug-in keeps weaving on every runtime from 1.34.8 on,
 * and the runtime never gates on the flag.
 */
static void
leia_plugin_fill_remedy(struct xrt_plugin_platform_status *out_status, enum leia_platform_state st)
{
	if (st == LEIA_PLATFORM_ABSENT) {
		return; // nothing installed: the hint already says "Install the LeiaSR Runtime"
	}
	struct leia_display_probe_result edid;
	memset(&edid, 0, sizeof(edid));
	if (!leia_edid_get_cached_result(&edid) || !edid.hw_found) {
		return; // no Leia panel: nothing to recommend on this machine
	}
	char installed[64];
	if (!leia_sr_installed_version(installed, sizeof(installed)) || !leia_sr_update_recommended(installed)) {
		return;
	}
	out_status->flags |= LEIA_REMEDY_FLAG_UPDATE_RECOMMENDED;
	struct leia_platform_status_remedy *r = (struct leia_platform_status_remedy *)out_status;
	if (LEIA_REMEDY_HAS(out_status, installed_version)) {
		copy_field(r->installed_version, sizeof(r->installed_version), installed);
	}
	if (LEIA_REMEDY_HAS(out_status, required_version)) {
		copy_field(r->required_version, sizeof(r->required_version), LEIA_SR_RECOMMENDED_VERSION);
	}
	if (LEIA_REMEDY_HAS(out_status, action_label)) {
		copy_field(r->action_label, sizeof(r->action_label), LEIA_SR_UPDATE_LABEL);
	}
	if (LEIA_REMEDY_HAS(out_status, action_url)) {
		copy_field(r->action_url, sizeof(r->action_url), LEIA_SR_UPDATE_URL);
	}
}
#endif

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
	leia_plugin_fill_remedy(out_status, st);
	return true;
}
#endif


#ifdef XRT_PLUGIN_IFACE_HAS_GET_SCREEN_STATUS
/*
 * ADR-051 D2 dashboard cell for one claimed screen. PASSIVE by construction:
 * it only reads what probe_displays() and the platform-state machine already
 * published (claim table + lock-free cached state) — no SR handle, no SR SDK
 * call, no probe. Tracker / lens / calibration are not known without a
 * handle, so they stay UNKNOWN / UNSUPPORTED / false.
 */
static xrt_result_t
leia_plugin_get_screen_status(struct xrt_plugin_instance *inst,
                              uint64_t monitor_id,
                              struct xrt_plugin_screen_status *out)
{
	(void)inst;
	if (out == NULL || out->struct_size < offsetof(struct xrt_plugin_screen_status, warning_count)) {
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	struct leia_win_claim_binding claim;
	memset(&claim, 0, sizeof(claim));
	if (!leia_win_claims_lookup(monitor_id, &claim)) {
		return XRT_ERROR_PROBER_NOT_SUPPORTED; /* not a screen this plug-in claimed */
	}
	const enum leia_platform_state st = leia_platform_state_get();
	out->version = XRT_PLUGIN_SCREEN_STATUS_VERSION;
	/* Moves on a claim-set change (re-probe that saw a different topology /
	 * identity) or a platform-state transition — never by polling hardware. */
	out->change_counter = (leia_win_claims_generation() << 4) | ((uint64_t)st & 0xF);
	out->ready = st == LEIA_PLATFORM_READY;
	out->verified = claim.fpc_verified;
	out->calibrated = false;
	out->tracker = XRT_PLUGIN_TRACKER_STATE_UNSUPPORTED;
	out->lens = XRT_PLUGIN_LENS_STATE_UNKNOWN;
	memset(out->model, 0, sizeof(out->model)); /* SR exposes no model name passively */
	snprintf(out->serial, sizeof(out->serial), "%s", claim.fpc_verified ? claim.serial : "");

	/* #552: say WHY a screen SR knows cannot weave, and the remedy, rather than
	 * leaving the session to fall back silently. Both come from state already
	 * published (claim table, last display create) -- still no SR call. */
	const size_t warnings_end = offsetof(struct xrt_plugin_screen_status, warnings);
	uint32_t nwarn = 0;
	if (leia_win_sr_device_unavailable() &&
	    out->struct_size >= warnings_end + (nwarn + 1) * sizeof(out->warnings[0])) {
		struct xrt_plugin_screen_warning *w = &out->warnings[nwarn++];
		snprintf(w->code, sizeof(w->code), "SR_DEVICE_UNAVAILABLE");
		w->level = XRT_PLUGIN_SCREEN_WARNING_LEVEL_CRITICAL;
		snprintf(w->text, sizeof(w->text), "SR Service reports no available SR device. Restart SR Service.");
	}
	if (claim.sr_display_id != 0 && !claim.fpc_verified &&
	    out->struct_size >= warnings_end + (nwarn + 1) * sizeof(out->warnings[0])) {
		/* SR lists the panel EDID_ONLY: briefly normal after a connect (#483). */
		struct xrt_plugin_screen_warning *w = &out->warnings[nwarn++];
		snprintf(w->code, sizeof(w->code), "SR_NOT_AUTHENTICATED");
		w->level = XRT_PLUGIN_SCREEN_WARNING_LEVEL_WARN;
		snprintf(w->text, sizeof(w->text), "SR has not authenticated this display. If it persists, restart SR Service.");
	}
	out->warning_count = nwarn;
	return XRT_SUCCESS;
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

    .id = LEIA_PLUGIN_ID,
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
     * ADR-042 lift-only D3D11 factory: deliberately NULL. This plug-in carries
     * no 2D->3D conversion; Leia's is the separate conversion provider
     * (docs/conversion-moved.md). NULL here plus NULL lift slots on the D3D11
     * DP is what makes the runtime's legacy `dp-lift` adapter report ABSENT.
     */
#if defined(XRT_PLUGIN_IFACE_HAS_D3D11_LIFT_FACTORY)
    .create_dp_d3d11_lift = NULL,
#endif

#ifdef XRT_PLUGIN_HAS_PLATFORM_STATE
    .get_platform_state = leia_plugin_get_platform_state,
#endif

#if defined(XRT_PLUGIN_IFACE_HAS_CREATE_DP_FOR_SCREEN)
    /*
     * Multi-screen M6 (Vulkan): one VK DP per screen a spanning window covers,
     * for the in-process Vulkan compositor off the #918 split (under the split
     * the runtime weaves with D3D11 DPs on the scanout adapter and uses the
     * D3D11 slot below). SR v2 only — a v1 weaver fails the create and the
     * runtime shows that segment flat 2D.
     */
#if defined(XRT_HAVE_LEIA_SR_VULKAN)
    .create_dp_vk_for_screen = leia_dp_factory_vk_for_screen,
#else
    .create_dp_vk_for_screen = NULL,
#endif
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

#ifdef XRT_PLUGIN_IFACE_HAS_GET_SCREEN_STATUS
    /* ADR-051 D2: passive per-screen dashboard cell (claim table + cached state). */
    .get_screen_status = leia_plugin_get_screen_status,
#endif

#if defined(XRT_PLUGIN_IFACE_HAS_CREATE_DP_GL_FOR_SCREEN)
    /* Multi-screen M6 (GL): one GL DP per screen a spanning window covers. */
#if defined(XRT_HAVE_LEIA_SR_GL)
    .create_dp_gl_for_screen = leia_dp_factory_gl_for_screen,
#else
    .create_dp_gl_for_screen = NULL,
#endif
#endif

#ifdef XRT_PLUGIN_IFACE_HAS_DESCRIBE_SETTINGS
    /* ADR-054 decision 7: the settings read through host->get_setting (MULTI_VIEWER, #446). */
    .describe_settings = leia_plugin_describe_settings,
#endif
#ifdef XRT_PLUGIN_IFACE_HAS_SETTING_STATUS
    .get_setting_status = leia_plugin_get_setting_status,
#endif
};

#if defined(XRT_PLUGIN_HAS_PLATFORM_STATE) && !defined(XRT_PLUGIN_IFACE_HAS_SETTING_STATUS)
/*
 * Older runtime headers (the DXR_RUNTIME_GIT_TAG pin) end the iface before
 * the setting-status slot, so the plug-in hands the runtime an extended copy.
 * The runtime's layout puts get_license_state and activate right after the
 * pinned header's last slot (both left NULL: no licence here), then
 * get_setting_status. struct_size tells a runtime that knows the slot that it
 * is filled; an older runtime reads only what its own struct_size covers.
 * Checked for the v2.66.8 pin (x64: iface 280 bytes, slot at 296). Re-check
 * when DXR_RUNTIME_GIT_TAG moves to a tag that still lacks the slot.
 */
struct leia_plugin_iface_ext
{
	struct xrt_plugin_iface base;
	void *get_license_state; /* NULL: no licence */
	void *activate;          /* NULL */
	bool (*get_setting_status)(const char *key, struct leia_setting_status *out);
};
typedef char leia_iface_ext_pin_check[(sizeof(void *) == 8 && sizeof(struct xrt_plugin_iface) == 280 &&
                                       offsetof(struct leia_plugin_iface_ext, get_setting_status) == 296)
                                          ? 1
                                          : -1];
static struct leia_plugin_iface_ext g_leia_iface_ext;
#endif



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
	*out_plugin_api_version = XRT_PLUGIN_API_VERSION_CURRENT;

	if (runtime_api_version != XRT_PLUGIN_API_VERSION_CURRENT) {
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

#ifdef XRT_PLUGIN_HOST_HAS_DISPLAY_RESCAN
	/* #483 (DisplayXR ADR-047 Amendment 8): SR lists a replugged panel
	 * EDID_ONLY until its FPC pairs, then re-keys it FPC_VERIFIED with no OS
	 * display event; asking the runtime for a claim sweep then moves the
	 * claim to VERIFIED with the serial and the new id. As on macOS. A runtime
	 * predating the slot leaves it zero (carved from reserved[]). */
	if (host != NULL &&
	    host->struct_size >= offsetof(struct xrt_plugin_host_iface, request_display_rescan) +
	                             sizeof(host->request_display_rescan) &&
	    host->request_display_rescan != NULL) {
		leia_win_sr_set_rescan_listener(host->request_display_rescan);
	}
#else
	(void)host;
#endif

#ifdef XRT_PLUGIN_HOST_HAS_GET_SETTING
	/* ADR-054 decision 7: settings come through the host (MULTI_VIEWER, #446).
	 * Carved from reserved[]: cover-check struct_size; a runtime predating
	 * the slot leaves it NULL and every setting keeps its default. */
	if (host != NULL &&
	    host->struct_size >= offsetof(struct xrt_plugin_host_iface, get_setting) + sizeof(host->get_setting)) {
		g_leia_get_setting = host->get_setting;
	}
#endif

#if defined(XRT_PLUGIN_HAS_PLATFORM_STATE) && !defined(XRT_PLUGIN_IFACE_HAS_SETTING_STATUS)
	g_leia_iface_ext.base = g_leia_iface;
	g_leia_iface_ext.base.struct_size = (uint32_t)sizeof(g_leia_iface_ext);
	g_leia_iface_ext.get_setting_status = leia_plugin_get_setting_status;
	*out_iface = &g_leia_iface_ext.base;
#else
	*out_iface = &g_leia_iface;
#endif
	return XRT_SUCCESS;
}
