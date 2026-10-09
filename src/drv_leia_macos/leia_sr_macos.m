// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  macOS arm: probe SR instance, cached panel geometry, and panel
 *         identification among the online CGDisplays. See leia_sr_macos.h.
 * @ingroup drv_leia
 */

#include "leia_sr_macos.h"
#include "leia_edid_table.h"

#include "util/u_logging.h"
#include "os/os_time.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct leia_mac_display_info g_info;
static bool g_probed_ok = false;
static uint64_t g_last_fail_ns = 0;

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

//! Fill the CG side of @p info (panel id, bounds, scale, refresh). Caller holds g_lock.
static void
fill_cg_info(struct leia_mac_display_info *info)
{
	const CGDirectDisplayID did = leia_mac_find_panel_display(info->pixel_width, info->pixel_height);
	info->cg_display_id = did;
	info->refresh_mhz = 60000;
	info->backing_scale = 1.0f;
	if (did == 0) {
		U_LOG_W("leia_mac: SR reports a display but no online CGDisplay looks like the Leia panel "
		        "(EDID table / name / %ux%u native mode) — window phase and colour tagging fall back",
		        info->pixel_width, info->pixel_height);
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
		if (b.size.width > 0.0) {
			info->backing_scale = (float)((double)mw / b.size.width);
		}
		if (hz >= 1.0 && hz <= 1000.0) {
			info->refresh_mhz = (uint32_t)(hz * 1000.0 + 0.5);
		}
		if (mw != info->pixel_width || mh != info->pixel_height) {
			U_LOG_W("leia_mac: panel display %u current mode is %ux%u px but SR reports %ux%u — the "
			        "weave expects the panel at its native resolution",
			        did, mw, mh, info->pixel_width, info->pixel_height);
		}
	}
	U_LOG_W("leia_mac: Leia panel = CGDirectDisplayID %u (EDID %04x:%04x), bounds (%d,%d %ux%u) pt, "
	        "backing scale %.2f, %.3f Hz",
	        did, info->edid_vendor, info->edid_product, info->screen_left_pt, info->screen_top_pt,
	        info->screen_width_pt, info->screen_height_pt, (double)info->backing_scale,
	        info->refresh_mhz / 1000.0);
}

//! One probe attempt. Caller holds g_lock.
static bool
probe_once_locked(void)
{
	SrInstance inst = NULL;
	SrInstanceCreateInfo ci = SrInstanceCreateInfo(.applicationName = "DisplayXR-LeiaSR probe",
	                                               .networkMode = SR_NETWORK_MODE_CLIENT);
	SrResult res = srCreateInstance(&ci, &inst);
	if (SR_FAILED(res)) {
		U_LOG_W("leia_mac: probe srCreateInstance failed: %s (loader: %s)", leia_mac_sr_result_str(res),
		        srGetLastLoaderError() ? srGetLastLoaderError() : "-");
		return false;
	}
	srSetLogCallback(inst, leia_mac_sr_log_cb, NULL);

	bool ok = false;
	SrDisplay display = NULL;
	res = srInitialize(inst);
	if (SR_FAILED(res)) {
		U_LOG_W("leia_mac: probe srInitialize failed: %s", leia_mac_sr_result_str(res));
		goto out;
	}
	SrDisplayCreateInfo dci = SrDisplayCreateInfo(.window = 0);
	res = srCreateDisplay(inst, &dci, &display);
	if (SR_FAILED(res)) {
		U_LOG_W("leia_mac: probe srCreateDisplay failed: %s", leia_mac_sr_result_str(res));
		display = NULL;
		goto out;
	}

	// SRService may need a moment to report the device to a fresh client.
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
	if (valid != SR_TRUE) {
		U_LOG_W("leia_mac: probe: SR display not valid (no SR panel reported by SRService)");
		goto out;
	}

	float w_cm = 0.0f, h_cm = 0.0f, nx = 0.0f, ny = 0.0f, nz = 0.0f;
	int32_t px_w = 0, px_h = 0, rec_w = 0, rec_h = 0;
	if (SR_FAILED(srDisplayGetPhysicalSize(display, &w_cm, &h_cm)) ||
	    SR_FAILED(srDisplayGetPhysicalResolution(display, &px_w, &px_h)) ||
	    SR_FAILED(srDisplayGetRecommendedTextureSize(display, &rec_w, &rec_h)) ||
	    SR_FAILED(srDisplayGetDefaultViewingPosition(display, &nx, &ny, &nz)) || px_w <= 0 || px_h <= 0) {
		U_LOG_W("leia_mac: probe: SR display query failed");
		goto out;
	}

	struct leia_mac_display_info info = {0};
	info.valid = true;
	info.width_m = w_cm / 100.0f;
	info.height_m = h_cm / 100.0f;
	info.pixel_width = (uint32_t)px_w;
	info.pixel_height = (uint32_t)px_h;
	info.rec_view_width = rec_w > 0 ? (uint32_t)rec_w : (uint32_t)px_w / 2;
	info.rec_view_height = rec_h > 0 ? (uint32_t)rec_h : (uint32_t)px_h / 2;
	info.nominal_x_m = nx / 1000.0f;
	info.nominal_y_m = ny / 1000.0f;
	info.nominal_z_m = nz / 1000.0f;
	fill_cg_info(&info);

	char version[64] = {0};
	srGetRuntimeVersion(inst, version, sizeof(version));
	U_LOG_W("leia_mac: SR runtime %s: display %ux%u px, %.1fx%.1f cm, recommended %ux%u per view, "
	        "nominal viewer (%.0f, %.0f, %.0f) mm",
	        version, info.pixel_width, info.pixel_height, w_cm, h_cm, info.rec_view_width,
	        info.rec_view_height, nx, ny, nz);

	g_info = info;
	ok = true;

out:
	if (display != NULL) {
		srDestroyDisplay(display);
	}
	srDestroyInstance(inst);
	return ok;
}

bool
leia_mac_sr_probe(void)
{
	pthread_mutex_lock(&g_lock);
	if (g_probed_ok) {
		pthread_mutex_unlock(&g_lock);
		return true;
	}
	const uint64_t now = os_monotonic_get_ns();
	if (g_last_fail_ns != 0 && now - g_last_fail_ns < 1000ull * 1000ull * 1000ull) {
		pthread_mutex_unlock(&g_lock);
		return false;
	}
	g_probed_ok = probe_once_locked();
	if (!g_probed_ok) {
		g_last_fail_ns = os_monotonic_get_ns();
	}
	const bool ok = g_probed_ok;
	pthread_mutex_unlock(&g_lock);
	return ok;
}

bool
leia_mac_get_display_info(struct leia_mac_display_info *out)
{
	pthread_mutex_lock(&g_lock);
	const bool ok = g_probed_ok;
	if (ok && out != NULL) {
		*out = g_info;
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}
