// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Lightweight SR hardware probe — detects SR display presence
 *         and caches the panel's real geometry for the Leia builder and device.
 *
 * The probe asks the SR runtime for the active display and reads its pixel
 * dimensions, refresh rate, physical size, and nominal viewing distance.
 * Results are cached in file-scope statics so later calls from
 * leia_hmd_create() do not re-create an SR context.
 *
 * ## This is the device's ONLY source of true panel geometry (#185)
 *
 * `leia_hmd_create()` computes the device FOV, the per-view eye offsets, and
 * the whole split-side-by-side view config from what lands here. When the probe
 * declines, that code falls back to a hardcoded 15.5" panel — which is why two
 * rules matter more here than the terseness of the code suggests:
 *
 * 1. **Never substitute a plausible number for a missing one.** A field this
 *    probe could not measure is left at 0 and the caller is told. The previous
 *    version filled in `0.344 x 0.194 m` when `getPhysicalSize*` returned
 *    nothing, which made "unmeasured" indistinguishable from "measured, and the
 *    panel happens to be 15.5 inches" — and shipped the reference panel's size
 *    for a 27" one.
 * 2. **Both SR lineages must be able to answer.** The probe used to speak only
 *    the v1 C++ API, so on a v2-runtime machine it returned false and the
 *    device silently took the hardcoded geometry even though
 *    `leia_sr_v2_query_display` had the true size in hand. It now dispatches
 *    through @ref leia_sr_api_selected, like every other SR call site.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_interface.h"
#include "leia_sr_api_select.h"
#include "util/u_logging.h"

#ifdef XRT_HAVE_LEIA_SR_D3D11

#include <sr/world/display/display.h>
#include <sr/utility/exception.h>

#ifdef DXR_LEIA_HAS_SR_V2
#include "leia_sr_v2_common.h"
#endif

#include <windows.h>
#include <sysinfoapi.h>

/*
 * File-scope probe cache.
 */
static struct leiasr_probe_result g_probe = {};
static bool g_probe_done = false;

/*!
 * Refresh rate of the monitor the SR display actually sits on.
 *
 * The old code called `EnumDisplaySettingsW(nullptr, ...)`, and `nullptr` means
 * the PRIMARY display — not the SR one. On any box where the SR panel is not
 * primary that reported a completely unrelated monitor's refresh rate. The SR
 * display hands us its desktop rect, so resolve the monitor from that.
 *
 * Returns 0 when it cannot be read. The caller decides what an unknown refresh
 * rate means; this function does not get to invent 60.
 */
static float
refresh_hz_at(int32_t screen_left, int32_t screen_top)
{
	POINT pt = {(LONG)screen_left, (LONG)screen_top};
	HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
	if (mon == nullptr) {
		return 0.0f;
	}

	MONITORINFOEXW mi = {};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(mon, (LPMONITORINFO)&mi)) {
		return 0.0f;
	}

	DEVMODEW dm = {};
	dm.dmSize = sizeof(dm);
	if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1) {
		return 0.0f;
	}
	return (float)dm.dmDisplayFrequency;
}

#ifdef DXR_LEIA_HAS_SR_V2
/*!
 * v2 lineage. Reuses the shared display query the graphics arms use, so the
 * probe and the arm cannot disagree about the panel — they call one function.
 *
 * The instance is created without `srInitialize` (which would start the
 * trackers and touch the lens) and destroyed immediately; this mirrors the
 * capability probe in `leia_sr_api_select.cpp` and is side-effect-free.
 */
static bool
probe_via_v2(double timeout_seconds)
{
	SrInstance instance = nullptr;
	if (!leia_sr_v2_create_instance(timeout_seconds, &instance) || instance == nullptr) {
		U_LOG_I("SR probe (v2): no instance within %.1fs", timeout_seconds);
		return false;
	}

	struct leia_sr_v2_display_info info = {};
	const bool ok = leia_sr_v2_query_display(instance, nullptr, timeout_seconds, &info);
	if (ok) {
		g_probe.pixel_w = info.pixel_width;
		g_probe.pixel_h = info.pixel_height;
		// 0 stays 0: srDisplayGetPhysicalSize failing is a fact the caller
		// needs, not a gap for us to paper over.
		g_probe.display_w_m = info.width_m;
		g_probe.display_h_m = info.height_m;
		g_probe.refresh_hz = refresh_hz_at(info.screen_left, info.screen_top);
		// v2 exposes no default-viewing-position getter yet; leaving this 0
		// lets the caller keep its own nominal distance rather than pretend
		// this probe measured one.
		g_probe.nominal_z_m = 0.0f;
		g_probe.hw_found = true;
	}

	srDestroyInstance(instance);
	return ok;
}
#endif // DXR_LEIA_HAS_SR_V2

static bool
probe_via_v1(double timeout_seconds);

bool
leiasr_probe_display(double timeout_seconds)
{
	if (g_probe_done) {
		return g_probe.hw_found;
	}

	g_probe_done = true;
	g_probe.hw_found = false;

#ifdef DXR_LEIA_HAS_SR_V2
	if (leia_sr_api_selected() == LEIA_SR_API_V2) {
		const bool ok = probe_via_v2(timeout_seconds);
		if (ok) {
			U_LOG_W("SR probe (v2): %ux%u px, %.4fx%.4f m, %.0f Hz%s", g_probe.pixel_w, g_probe.pixel_h,
			        (double)g_probe.display_w_m, (double)g_probe.display_h_m, (double)g_probe.refresh_hz,
			        (g_probe.display_w_m > 0.0f) ? "" : "  [physical size UNMEASURED]");
		} else {
			U_LOG_I("SR probe (v2): no active SR display within %.1fs", timeout_seconds);
		}
		return g_probe.hw_found;
	}
#endif

	return probe_via_v1(timeout_seconds);
}

static bool
probe_via_v1(double timeout_seconds)
{
	const double start_time = (double)GetTickCount64() / 1000.0;

	// Create temporary SR context with retry loop.
	SR::SRContext *context = nullptr;
	while (context == nullptr) {
		try {
			context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException &e) {
			(void)e;
		}

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > timeout_seconds) {
			break;
		}
		Sleep(100);
	}

	if (context == nullptr) {
		U_LOG_I("SR probe: no SR context within %.1fs — no SR hardware", timeout_seconds);
		return false;
	}

	// Query primary active SR display.
	bool success = false;
	try {
		SR::IDisplayManager *dm = SR::GetDisplayManagerInstance(*context);
		if (dm == nullptr) {
			U_LOG_I("SR probe: no display manager");
			goto cleanup;
		}

		// Wait for display to become ready (within remaining time).
		while (true) {
			SR::IDisplay *display = dm->getPrimaryActiveSRDisplay();
			if (display != nullptr && display->isValid()) {
				SR_recti loc = display->getLocation();
				int64_t native_w = loc.right - loc.left;
				int64_t native_h = loc.bottom - loc.top;

				if (native_w > 0 && native_h > 0) {
					// Pixel dimensions.
					g_probe.pixel_w = static_cast<uint32_t>(native_w);
					g_probe.pixel_h = static_cast<uint32_t>(native_h);

					// Refresh rate of the monitor the SR display is ON —
					// not the primary one (see refresh_hz_at).
					g_probe.refresh_hz = refresh_hz_at((int32_t)loc.left, (int32_t)loc.top);

					// Physical dimensions (SR returns centimeters). If the
					// SDK has none, leave 0 — see rule 1 in the file header.
					float width_cm = display->getPhysicalSizeWidth();
					float height_cm = display->getPhysicalSizeHeight();
					if (width_cm > 0.0f && height_cm > 0.0f) {
						g_probe.display_w_m = width_cm / 100.0f;
						g_probe.display_h_m = height_cm / 100.0f;
					}

					// Nominal viewing distance (SR returns mm). 0 means
					// "not measured"; the caller keeps its own default.
					float nom_x_mm = 0, nom_y_mm = 0, nom_z_mm = 0;
					try {
						display->getDefaultViewingPosition(nom_x_mm, nom_y_mm, nom_z_mm);
						g_probe.nominal_z_m = nom_z_mm / 1000.0f;
					} catch (...) {
						g_probe.nominal_z_m = 0.0f;
					}
					if (g_probe.nominal_z_m <= 0.0f) {
						g_probe.nominal_z_m = 0.0f;
					}

					g_probe.hw_found = true;
					success = true;

					U_LOG_W("SR probe (v1): %ux%u px, %.4fx%.4f m, %.0f Hz, Z=%.2f m%s",
					        g_probe.pixel_w, g_probe.pixel_h, (double)g_probe.display_w_m,
					        (double)g_probe.display_h_m, (double)g_probe.refresh_hz,
					        (double)g_probe.nominal_z_m,
					        (g_probe.display_w_m > 0.0f) ? "" : "  [physical size UNMEASURED]");
					break;
				}
			}

			double cur_time = (double)GetTickCount64() / 1000.0;
			if ((cur_time - start_time) > timeout_seconds) {
				break;
			}
			Sleep(100);
		}
	} catch (...) {
		U_LOG_I("SR probe: exception during display query");
	}

cleanup:
	if (context != nullptr) {
		SR::SRContext::deleteSRContext(context);
	}

	if (!success) {
		U_LOG_I("SR probe: no active SR display found within %.1fs", timeout_seconds);
	}

	return g_probe.hw_found;
}

bool
leiasr_get_probe_results(struct leiasr_probe_result *out)
{
	if (out == nullptr || !g_probe_done) {
		return false;
	}
	*out = g_probe;
	return g_probe.hw_found;
}

#else // !XRT_HAVE_LEIA_SR_D3D11

// Stub implementations for non-SR builds.

bool
leiasr_probe_display(double timeout_seconds)
{
	(void)timeout_seconds;
	return false;
}

bool
leiasr_get_probe_results(struct leiasr_probe_result *out)
{
	(void)out;
	return false;
}

#endif // XRT_HAVE_LEIA_SR_D3D11
