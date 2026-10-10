// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  macOS arm: the enumeration SR instance, SR display enumeration
 *         (srEnumerateDisplays) joined to the online CGDisplays, the active
 *         panel's geometry, and the topology-change invalidation. See
 *         leia_sr_macos.h.
 * @ingroup drv_leia
 */

#include "leia_sr_macos.h"
#include "leia_edid_table.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <IOKit/graphics/IOGraphicsTypes.h> // kDisplayModeNativeFlag

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

//! The enumeration instance (kept alive) + its topology monitor.
static SrInstance g_inst;
static SrSystemMonitor g_monitor;
static uint64_t g_last_fail_ns = 0;
static struct leia_mac_sr_caps g_caps;

//! Set by SR_EVENT_TYPE_DISPLAY_TOPOLOGY_CHANGED (SR thread); cleared by a re-enumeration.
static atomic_bool g_topology_dirty = true;
static bool g_enumerated = false; //!< the cache below holds a result (possibly 0 displays)

//! The SR displays, [0] = the active panel. Never trusted across a topology change.
static struct leia_mac_display_info g_disp[LEIA_MAC_MAX_SR_DISPLAYS];
static uint32_t g_disp_count = 0;
static int g_last_logged_count = -1;
static uint32_t g_sr_reported = 0; //!< srEnumerateDisplays' raw count (before the guard)

//! How long the probe waits for SRService to report a valid display.
#define LEIA_MAC_PROBE_DISPLAY_WAIT_NS (3ull * 1000ull * 1000ull * 1000ull)


const char *
leia_mac_sr_result_str(SrResult res)
{
	const char *s = srResultToString(res);
	return s != NULL ? s : "?";
}

void
leia_mac_sr_log_cb(SrLogLevel level, const char *message, void *user_data)
{
	(void)user_data;
	if (message == NULL) {
		return;
	}
	if (level == SR_LOG_LEVEL_ERROR || level == SR_LOG_LEVEL_WARNING) {
		U_LOG_W("leia_mac[SR]: %s", message);
	} else if (level == SR_LOG_LEVEL_INFO) {
		U_LOG_I("leia_mac[SR]: %s", message);
	}
}

bool
leia_mac_edid_is_leia_panel(uint16_t vendor, uint16_t product)
{
	for (size_t i = 0; i < sizeof(leia_edid_table) / sizeof(leia_edid_table[0]); i++) {
		if (leia_edid_table[i][0] == vendor && leia_edid_table[i][1] == product) {
			return true;
		}
	}
	return false;
}

static bool
native_mode_px(CGDirectDisplayID did, uint32_t *out_w, uint32_t *out_h, double *out_hz)
{
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(did);
	if (mode == NULL) {
		return false;
	}
	*out_w = (uint32_t)CGDisplayModeGetPixelWidth(mode);
	*out_h = (uint32_t)CGDisplayModeGetPixelHeight(mode);
	if (out_hz != NULL) {
		*out_hz = CGDisplayModeGetRefreshRate(mode);
	}
	CGDisplayModeRelease(mode);
	return true;
}

uint32_t
leia_mac_find_panel_display(uint32_t want_px_w, uint32_t want_px_h)
{
	CGDirectDisplayID ids[16];
	uint32_t count = 0;
	if (CGGetOnlineDisplayList(16, ids, &count) != kCGErrorSuccess) {
		return 0;
	}

	// 1. EDID (vendor, product) in the frozen Leia table — the same identity
	//    the Windows/Linux probes use. CGDisplayVendorNumber/ModelNumber are
	//    the EDID manufacturer/product ids (e.g. 0x4C2D/0x785A = the Odyssey
	//    G90XF, SAMSUNG_4K27_144HZ_2 in leia_edid_table.h).
	for (uint32_t i = 0; i < count; i++) {
		if (leia_mac_edid_is_leia_panel((uint16_t)CGDisplayVendorNumber(ids[i]),
		                                (uint16_t)CGDisplayModelNumber(ids[i]))) {
			return ids[i];
		}
	}

	// 2. NSScreen.localizedName keywords (LeiaSR's mtl_weaver_window pickScreen).
	__block uint32_t by_name = 0;
	void (^scan_names)(void) = ^{
		for (NSScreen *s in [NSScreen screens]) {
			NSString *name = [[s localizedName] lowercaseString];
			if ([name containsString:@"spatiallabs"] || [name containsString:@"acer"] ||
			    [name containsString:@"dimenco"] || [name containsString:@"leia"]) {
				by_name = [[[s deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
				return;
			}
		}
	};
	if ([NSThread isMainThread]) {
		scan_names();
	} else {
		// NSScreen is documented main-thread-only; this runs once at probe.
		@autoreleasepool {
			scan_names();
		}
	}
	if (by_name != 0) {
		return by_name;
	}

	// 3. The only non-builtin display whose native mode matches SR's
	//    physical resolution.
	if (want_px_w != 0 && want_px_h != 0) {
		uint32_t match = 0, matches = 0;
		for (uint32_t i = 0; i < count; i++) {
			uint32_t w = 0, h = 0;
			if (CGDisplayIsBuiltin(ids[i]) || !native_mode_px(ids[i], &w, &h, NULL)) {
				continue;
			}
			if (w == want_px_w && h == want_px_h) {
				match = ids[i];
				matches++;
			}
		}
		if (matches == 1) {
			return match;
		}
	}
	return 0;
}

DEBUG_GET_ONCE_BOOL_OPTION(leia_allow_builtin, "DXR_LEIA_ALLOW_BUILTIN", false)

//! The display's NATIVE mode in pixels (the mode IOKit flags native), else the
//! largest pixel mode; false if CG lists none. The CURRENT mode is not the
//! test: a real panel in a scaled "more space" HiDPI mode has a current pixel
//! size that is not its native one.
static bool
native_display_px(CGDirectDisplayID did, uint32_t *out_w, uint32_t *out_h)
{
	const void *keys[] = {kCGDisplayShowDuplicateLowResolutionModes};
	const void *vals[] = {kCFBooleanTrue};
	CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
	                                          &kCFTypeDictionaryValueCallBacks);
	CFArrayRef modes = CGDisplayCopyAllDisplayModes(did, opts);
	if (opts != NULL) {
		CFRelease(opts);
	}
	if (modes == NULL) {
		return false;
	}
	uint32_t best_w = 0, best_h = 0;
	for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
		CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
		const uint32_t w = (uint32_t)CGDisplayModeGetPixelWidth(m);
		const uint32_t h = (uint32_t)CGDisplayModeGetPixelHeight(m);
		if ((CGDisplayModeGetIOFlags(m) & kDisplayModeNativeFlag) != 0) {
			best_w = w;
			best_h = h;
			break;
		}
		if ((uint64_t)w * h > (uint64_t)best_w * best_h) {
			best_w = w;
			best_h = h;
		}
	}
	CFRelease(modes);
	*out_w = best_w;
	*out_h = best_h;
	return best_w != 0 && best_h != 0;
}

//! Is @p v (either byte order) a manufacturer id anywhere in the Leia EDID table?
static bool
edid_vendor_in_table(uint16_t v)
{
	const uint16_t sw = (uint16_t)((v >> 8) | (v << 8));
	for (size_t i = 0; i < sizeof(leia_edid_table) / sizeof(leia_edid_table[0]); i++) {
		if (leia_edid_table[i][0] == v || leia_edid_table[i][0] == sw) {
			return true;
		}
	}
	return false;
}

/*!
 * Defensive guard on an srEnumerateDisplays entry whose platformHandle is
 * @p did: NULL = plausible, else why it cannot be the SR panel. Exists because
 * an SR runtime has been seen to report an ABSENT, FPC-verified DS1 with the
 * MacBook's built-in display as its platformHandle (EDID APP/0xA04E, native
 * 3024x1964 vs SR's 3840x2160) — the plug-in then claimed the built-in display
 * VERIFIED and went active. Root cause is SR-side; these checks keep a wrong
 * join from binding.
 *   1. a built-in display (CGDisplayIsBuiltin) — DXR_LEIA_ALLOW_BUILTIN=1 for
 *      a real built-in Leia panel;
 *   2. the monitor's NATIVE mode is not the resolution SR reports;
 *   3. the monitor's EDID (vendor, product) is not a Leia panel and its vendor
 *      makes no Leia panel at all (leia_edid_table.h).
 */
static const char *
sr_display_reject_reason(const struct leia_mac_display_info *info, CGDirectDisplayID did, char *buf, size_t cap)
{
	if (did == 0) {
		return NULL; // nothing to cross-check (SR's Quartz source had no monitor)
	}
	if (CGDisplayIsBuiltin(did) && !debug_get_bool_option_leia_allow_builtin()) {
		snprintf(buf, cap, "its monitor (CGDirectDisplayID %u) is the BUILT-IN display "
		         "(set DXR_LEIA_ALLOW_BUILTIN=1 for a built-in Leia panel)", did);
		return buf;
	}
	uint32_t nw = 0, nh = 0;
	if (info->pixel_width != 0 && info->pixel_height != 0 && native_display_px(did, &nw, &nh) &&
	    (nw != info->pixel_width || nh != info->pixel_height)) {
		snprintf(buf, cap, "its monitor (CGDirectDisplayID %u) is natively %ux%u px but SR reports %ux%u", did, nw,
		         nh, info->pixel_width, info->pixel_height);
		return buf;
	}
	const uint16_t v = (uint16_t)CGDisplayVendorNumber(did);
	const uint16_t p = (uint16_t)CGDisplayModelNumber(did);
	if (!leia_mac_edid_is_leia_panel(v, p) && !leia_mac_edid_is_leia_panel((uint16_t)((v >> 8) | (v << 8)), p) &&
	    !edid_vendor_in_table(v)) {
		snprintf(buf, cap, "its monitor (CGDirectDisplayID %u, EDID 0x%04x/0x%04x) is not a Leia panel and that "
		         "vendor makes none (leia_edid_table.h)", did, v, p);
		return buf;
	}
	return NULL;
}

//! Fill the CG side of @p info for display @p did (0 = none). Caller holds g_lock.
static void
fill_cg_info(struct leia_mac_display_info *info, CGDirectDisplayID did)
{
	info->cg_display_id = did;
	if (info->refresh_mhz == 0) {
		info->refresh_mhz = 60000;
	}
	info->backing_scale = 1.0f;
	info->uuid[0] = '\0';
	if (did == 0) {
		return;
	}
	const CGRect b = CGDisplayBounds(did);
	info->screen_left_pt = (int32_t)b.origin.x;
	info->screen_top_pt = (int32_t)b.origin.y;
	info->screen_width_pt = (uint32_t)b.size.width;
	info->screen_height_pt = (uint32_t)b.size.height;
	info->edid_vendor = (uint16_t)CGDisplayVendorNumber(did);
	info->edid_product = (uint16_t)CGDisplayModelNumber(did);
	CFUUIDRef u = CGDisplayCreateUUIDFromDisplayID(did);
	if (u != NULL) {
		CFStringRef us = CFUUIDCreateString(NULL, u);
		if (us != NULL) {
			CFStringGetCString(us, info->uuid, sizeof(info->uuid), kCFStringEncodingUTF8);
			CFRelease(us);
		}
		CFRelease(u);
	}

	uint32_t mw = 0, mh = 0;
	double hz = 0.0;
	if (native_mode_px(did, &mw, &mh, &hz)) {
		// Backing scale = mode pixels / points (sr_display.h: nativeWidth /
		// CGDisplayBounds width; same thing for the current mode).
		if (b.size.width > 0.0) {
			info->backing_scale = (float)((double)mw / b.size.width);
		}
		if (hz >= 1.0 && hz <= 1000.0) {
			info->refresh_mhz = (uint32_t)(hz * 1000.0 + 0.5);
		}
		if (info->pixel_width != 0 && (mw != info->pixel_width || mh != info->pixel_height)) {
			U_LOG_W("leia_mac: panel display %u current mode is %ux%u px but SR reports %ux%u — the "
			        "weave expects the panel at its native resolution",
			        did, mw, mh, info->pixel_width, info->pixel_height);
		}
	}
}

static void SR_CALL
on_system_event(const SrSystemEvent *event, void *user_data)
{
	(void)user_data;
	if (event != NULL && event->eventType == SR_EVENT_TYPE_DISPLAY_TOPOLOGY_CHANGED) {
		atomic_store(&g_topology_dirty, true);
		U_LOG_W("leia_mac: SR display topology changed (%s) — re-enumerating on the next query",
		        event->message != NULL ? event->message : "no detail");
	}
}

//! Bring up the enumeration instance (once; retried at most once a second). Caller holds g_lock.
static bool
ensure_instance_locked(void)
{
	if (g_inst != NULL) {
		return true;
	}
	const uint64_t now = os_monotonic_get_ns();
	if (g_last_fail_ns != 0 && now - g_last_fail_ns < 1000ull * 1000ull * 1000ull) {
		return false;
	}
	SrInstanceCreateInfo ci = SrInstanceCreateInfo(.applicationName = "DisplayXR-LeiaSR",
	                                               .networkMode = SR_NETWORK_MODE_CLIENT);
	SrResult res = srCreateInstance(&ci, &g_inst);
	if (SR_FAILED(res)) {
		U_LOG_W("leia_mac: srCreateInstance failed: %s (loader: %s)", leia_mac_sr_result_str(res),
		        srGetLastLoaderError() ? srGetLastLoaderError() : "-");
		g_inst = NULL;
		g_last_fail_ns = os_monotonic_get_ns();
		return false;
	}
	srSetLogCallback(g_inst, leia_mac_sr_log_cb, NULL);

	// The topology monitor + its callback must exist before srInitialize.
	SrSystemMonitorCreateInfo mci = SrSystemMonitorCreateInfo();
	res = srCreateSystemMonitor(g_inst, &mci, &g_monitor);
	if (SR_SUCCEEDED(res)) {
		srSystemMonitorAddCallback(g_monitor, on_system_event, NULL);
	} else {
		g_monitor = NULL;
		U_LOG_W("leia_mac: srCreateSystemMonitor failed (%s) — display topology changes go unnoticed",
		        leia_mac_sr_result_str(res));
	}

	res = srInitialize(g_inst);
	if (SR_FAILED(res)) {
		U_LOG_W("leia_mac: srInitialize failed: %s", leia_mac_sr_result_str(res));
		if (g_monitor != NULL) {
			srDestroySystemMonitor(g_monitor);
			g_monitor = NULL;
		}
		srDestroyInstance(g_inst);
		g_inst = NULL;
		g_last_fail_ns = os_monotonic_get_ns();
		return false;
	}

	SrEyeTrackerBindingCapabilities etc = SrEyeTrackerBindingCapabilities();
	SrLensBindingCapabilities lc = SrLensBindingCapabilities(.pNext = &etc);
	SrDisplayBindingCapabilities bc = SrDisplayBindingCapabilities(.pNext = &lc);
	SrWeaverRoutingCapabilities rc = SrWeaverRoutingCapabilities(.pNext = &bc);
	SrRuntimeCapabilities caps = SrRuntimeCapabilities(.pNext = &rc);
	memset(&g_caps, 0, sizeof(g_caps));
	if (SR_SUCCEEDED(srGetRuntimeCapabilities(g_inst, &caps))) {
		g_caps.external_routing = rc.externalRouting == SR_TRUE;
		g_caps.display_binding = bc.displayBinding == SR_TRUE;
		g_caps.max_bound_displays = bc.maxBoundDisplays;
		g_caps.lens_per_device = lc.lensPerDevice == SR_TRUE;
		g_caps.eye_tracker_per_device = etc.eyeTrackerPerDevice == SR_TRUE;
	}
	char version[64] = {0};
	srGetRuntimeVersion(g_inst, version, sizeof(version));
	U_LOG_W("leia_mac: SR runtime %s — external routing %s, display binding %s (max %u), lens per device %s, "
	        "eye tracker per device %s",
	        version, g_caps.external_routing ? "yes" : "no", g_caps.display_binding ? "yes" : "no",
	        g_caps.max_bound_displays, g_caps.lens_per_device ? "yes" : "no",
	        g_caps.eye_tracker_per_device ? "yes" : "no");
	return true;
}

//! Active-panel SR geometry (recommended view size, nominal viewer, physical
//! size, refresh) from a display handle, bound to @p display_id when the
//! runtime supports binding. Fills @p info; false if SR has no valid display.
static bool
read_active_geometry_locked(uint64_t display_id, struct leia_mac_display_info *info)
{
	SrDisplayBindingInfo bind = SrDisplayBindingInfo(.displayId = display_id);
	SrDisplayCreateInfo dci = SrDisplayCreateInfo(.window = 0);
	if (display_id != 0 && g_caps.display_binding) {
		dci.pNext = &bind;
	}
	SrDisplay display = NULL;
	SrResult res = srCreateDisplay(g_inst, &dci, &display);
	if (SR_FAILED(res)) {
		U_LOG_W("leia_mac: srCreateDisplay(displayId 0x%016llx) failed: %s", (unsigned long long)display_id,
		        leia_mac_sr_result_str(res));
		return false;
	}
	bool ok = false;
	SrBool32 valid = SR_FALSE;
	const uint64_t start = os_monotonic_get_ns();
	for (;;) {
		if (SR_SUCCEEDED(srDisplayIsValid(display, &valid)) && valid == SR_TRUE) {
			break;
		}
		if (os_monotonic_get_ns() - start > LEIA_MAC_PROBE_DISPLAY_WAIT_NS) {
			break;
		}
		os_nanosleep(100 * 1000 * 1000);
	}
	float w_cm = 0.0f, h_cm = 0.0f, nx = 0.0f, ny = 0.0f, nz = 0.0f, hz = 0.0f;
	int32_t px_w = 0, px_h = 0, rec_w = 0, rec_h = 0;
	if (valid == SR_TRUE && SR_SUCCEEDED(srDisplayGetPhysicalSize(display, &w_cm, &h_cm)) &&
	    SR_SUCCEEDED(srDisplayGetPhysicalResolution(display, &px_w, &px_h)) &&
	    SR_SUCCEEDED(srDisplayGetRecommendedTextureSize(display, &rec_w, &rec_h)) &&
	    SR_SUCCEEDED(srDisplayGetDefaultViewingPosition(display, &nx, &ny, &nz)) && px_w > 0 && px_h > 0) {
		if (w_cm > 0.0f && h_cm > 0.0f) {
			info->width_m = w_cm / 100.0f;
			info->height_m = h_cm / 100.0f;
		}
		info->pixel_width = (uint32_t)px_w;
		info->pixel_height = (uint32_t)px_h;
		info->rec_view_width = rec_w > 0 ? (uint32_t)rec_w : (uint32_t)px_w / 2;
		info->rec_view_height = rec_h > 0 ? (uint32_t)rec_h : (uint32_t)px_h / 2;
		info->nominal_x_m = nx / 1000.0f;
		info->nominal_y_m = ny / 1000.0f;
		info->nominal_z_m = nz / 1000.0f;
		if (SR_SUCCEEDED(srDisplayGetRefreshRate(display, &hz)) && hz >= 1.0f && hz <= 1000.0f) {
			info->refresh_mhz = (uint32_t)(hz * 1000.0f + 0.5f);
		}
		ok = true;
	} else {
		U_LOG_W("leia_mac: SR display query failed (valid=%d)", (int)valid);
	}
	srDestroyDisplay(display);
	return ok;
}

//! Legacy SR runtime (no srEnumerateDisplays): the active display + EDID table. Caller holds g_lock.
static uint32_t
enumerate_legacy_locked(void)
{
	struct leia_mac_display_info info = {0};
	if (!read_active_geometry_locked(0, &info)) {
		return 0;
	}
	// The legacy runtime reports its active display valid even with no panel
	// attached, so the EDID table must ALSO find the panel among the online
	// displays; no match = no panel.
	const uint32_t did = leia_mac_find_panel_display(info.pixel_width, info.pixel_height);
	if (did == 0) {
		U_LOG_W("leia_mac: legacy SR runtime reports a display but no online CGDisplay is a Leia panel "
		        "(EDID table / name / %ux%u native mode) — no SR panel",
		        info.pixel_width, info.pixel_height);
		return 0;
	}
	info.valid = true;
	fill_cg_info(&info, did);
	g_disp[0] = info;
	return 1;
}

//! Re-run the enumeration into g_disp. Caller holds g_lock.
static void
enumerate_locked(void)
{
	atomic_store(&g_topology_dirty, false);
	memset(g_disp, 0, sizeof(g_disp));
	g_disp_count = 0;
	g_sr_reported = 0;
	g_enumerated = false;
	if (!ensure_instance_locked()) {
		atomic_store(&g_topology_dirty, true); // retry on the next query
		return;
	}
	g_enumerated = true;

	uint32_t count = 0;
	SrResult res = srEnumerateDisplays(g_inst, &count, NULL);
	if (res == SR_ERROR_FUNCTION_UNSUPPORTED || res == SR_ERROR_FEATURE_NOT_SUPPORTED) {
		static bool logged;
		if (!logged) {
			logged = true;
			U_LOG_W("leia_mac: srEnumerateDisplays -> %s — SR runtime predates display enumeration; "
			        "identifying the panel through the EDID table",
			        leia_mac_sr_result_str(res));
		}
		g_disp_count = enumerate_legacy_locked();
		goto log;
	}
	if (SR_FAILED(res)) {
		U_LOG_W("leia_mac: srEnumerateDisplays(count) failed: %s", leia_mac_sr_result_str(res));
		goto log;
	}

	SrDisplayDescriptor descs[LEIA_MAC_MAX_SR_DISPLAYS];
	for (uint32_t i = 0; i < LEIA_MAC_MAX_SR_DISPLAYS; i++) {
		descs[i] = SrDisplayDescriptor();
	}
	count = count > LEIA_MAC_MAX_SR_DISPLAYS ? LEIA_MAC_MAX_SR_DISPLAYS : count;
	g_sr_reported = count;
	if (count > 0) {
		res = srEnumerateDisplays(g_inst, &count, descs);
		if (SR_FAILED(res)) {
			U_LOG_W("leia_mac: srEnumerateDisplays failed: %s", leia_mac_sr_result_str(res));
			count = 0;
		}
	}

	// Active panel first: the first FPC-verified display (its displayId is the
	// one srCreateDisplay binds by default), else the first one listed.
	uint32_t active = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (descs[i].confidence == SR_DISPLAY_CONFIDENCE_FPC_VERIFIED) {
			active = i;
			break;
		}
	}
	for (uint32_t k = 0; k < count; k++) {
		const uint32_t i = k == 0 ? active : (k <= active ? k - 1 : k);
		const SrDisplayDescriptor *d = &descs[i];
		struct leia_mac_display_info *info = &g_disp[g_disp_count];
		memset(info, 0, sizeof(*info));
		info->valid = true;
		info->from_enumeration = true;
		info->sr_display_id = d->displayId;
		info->fpc_verified = d->confidence == SR_DISPLAY_CONFIDENCE_FPC_VERIFIED;
		snprintf(info->fpc_serial, sizeof(info->fpc_serial), "%.*s", (int)sizeof(d->serial), d->serial);
		info->pixel_width = d->nativeWidth > 0 ? (uint32_t)d->nativeWidth : 0;
		info->pixel_height = d->nativeHeight > 0 ? (uint32_t)d->nativeHeight : 0;
		info->width_m = d->physicalWidthCm / 100.0f;
		info->height_m = d->physicalHeightCm / 100.0f;
		info->refresh_mhz = d->refreshHz >= 1.0f ? (uint32_t)(d->refreshHz * 1000.0f + 0.5f) : 0;
		// platformHandle = CGDirectDisplayID (0 when SR's Quartz source has
		// none: fall back to the EDID table for the active panel).
		CGDirectDisplayID did = (CGDirectDisplayID)d->platformHandle;
		if (did == 0 && g_disp_count == 0) {
			did = leia_mac_find_panel_display(info->pixel_width, info->pixel_height);
		}
		// The active candidate's SR geometry first: srDisplayGetPhysicalResolution
		// is the resolution SR weaves for, which the guard checks against the
		// monitor's native mode (a descriptor's nativeWidth comes from the very
		// monitor it names, so it cannot disagree with it).
		const bool is_active = g_disp_count == 0;
		const bool have_geometry = is_active && read_active_geometry_locked(d->displayId, info);
		char why_buf[192];
		const char *why = sr_display_reject_reason(info, did, why_buf, sizeof(why_buf));
		if (why != NULL) {
			// Logged once per (display, reason); re-enumeration repeats it silently.
			static char last_logged[256];
			char key[256];
			snprintf(key, sizeof(key), "%016llx:%s", (unsigned long long)d->displayId, why);
			if (strcmp(key, last_logged) != 0) {
				snprintf(last_logged, sizeof(last_logged), "%s", key);
				U_LOG_W("leia_mac: IGNORING SR display id 0x%016llx (%s serial '%s', product '%.*s', %ux%u px): "
				        "%s — treated as not attached",
				        (unsigned long long)d->displayId, info->fpc_verified ? "FPC-verified" : "EDID-only",
				        info->fpc_serial, (int)sizeof(d->productCode), d->productCode, info->pixel_width,
				        info->pixel_height, why);
			}
			memset(info, 0, sizeof(*info));
			continue;
		}
		if (is_active && !have_geometry) {
			// Descriptor geometry only; recommended size = half the panel.
			info->rec_view_width = info->pixel_width / 2;
			info->rec_view_height = info->pixel_height / 2;
			info->nominal_z_m = 0.6f;
		}
		fill_cg_info(info, did);
		U_LOG_W("leia_mac: SR display [%u] id 0x%016llx %s serial '%s' product '%.*s' EDID %.*s/0x%04x -> "
		        "CGDirectDisplayID %u '%s' (%d,%d %ux%u pt, x%.2f), %ux%u px, %.3fx%.3f m%s",
		        g_disp_count, (unsigned long long)d->displayId, info->fpc_verified ? "FPC-VERIFIED" : "EDID-only",
		        info->fpc_serial, (int)sizeof(d->productCode), d->productCode, (int)sizeof(d->edidVendor),
		        d->edidVendor, d->edidProduct, did, info->uuid, info->screen_left_pt, info->screen_top_pt,
		        info->screen_width_pt, info->screen_height_pt, (double)info->backing_scale, info->pixel_width,
		        info->pixel_height, (double)info->width_m, (double)info->height_m, is_active ? " (active)" : "");
		g_disp_count++;
	}

log:
	if ((int)g_disp_count != g_last_logged_count) {
		g_last_logged_count = (int)g_disp_count;
		if (g_disp_count == 0) {
			if (g_sr_reported == 0) {
				U_LOG_W("leia_mac: no SR display attached (srEnumerateDisplays = 0) — the plug-in declines");
			} else {
				U_LOG_W("leia_mac: no usable SR display (%u reported, all rejected above) — the plug-in "
				        "declines",
				        g_sr_reported);
			}
		} else {
			U_LOG_W("leia_mac: %u SR display(s); active panel = CGDirectDisplayID %u", g_disp_count,
			        g_disp[0].cg_display_id);
		}
	}
}

//! Caller holds g_lock.
static void
refresh_if_needed_locked(void)
{
	if (!g_enumerated || atomic_load(&g_topology_dirty)) {
		enumerate_locked();
	}
}

bool
leia_mac_sr_probe(void)
{
	pthread_mutex_lock(&g_lock);
	refresh_if_needed_locked();
	const bool ok = g_disp_count > 0 && g_disp[0].valid;
	pthread_mutex_unlock(&g_lock);
	return ok;
}

bool
leia_mac_get_display_info(struct leia_mac_display_info *out)
{
	pthread_mutex_lock(&g_lock);
	refresh_if_needed_locked();
	const bool ok = g_disp_count > 0 && g_disp[0].valid;
	if (ok && out != NULL) {
		*out = g_disp[0];
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

uint32_t
leia_mac_get_sr_displays(struct leia_mac_display_info *out, uint32_t max)
{
	pthread_mutex_lock(&g_lock);
	refresh_if_needed_locked();
	uint32_t n = g_disp_count < max ? g_disp_count : max;
	if (out != NULL) {
		memcpy(out, g_disp, n * sizeof(out[0]));
	}
	pthread_mutex_unlock(&g_lock);
	return n;
}

bool
leia_mac_get_sr_caps(struct leia_mac_sr_caps *out)
{
	pthread_mutex_lock(&g_lock);
	refresh_if_needed_locked();
	const bool ok = g_inst != NULL;
	if (ok && out != NULL) {
		*out = g_caps;
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}
