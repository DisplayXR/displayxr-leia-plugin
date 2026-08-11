// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Implementation of the shared SR v2 instance/display/lens helpers.
 * @ingroup drv_leia
 */

#include "leia_sr_v2_common.h"

#ifdef DXR_LEIA_HAS_SR_V2

#include "util/u_logging.h"

#include <sr/sr_version.h>

#include <windows.h>

const char *
leia_sr_v2_result_str(SrResult r)
{
	switch (r) {
	case SR_SUCCESS: return "SR_SUCCESS";
	case SR_TIMEOUT: return "SR_TIMEOUT";
	case SR_EVENT_UNAVAILABLE: return "SR_EVENT_UNAVAILABLE";
	case SR_ERROR_VALIDATION_FAILURE: return "SR_ERROR_VALIDATION_FAILURE";
	case SR_ERROR_RUNTIME_FAILURE: return "SR_ERROR_RUNTIME_FAILURE";
	case SR_ERROR_OUT_OF_MEMORY: return "SR_ERROR_OUT_OF_MEMORY";
	case SR_ERROR_RUNTIME_UNAVAILABLE: return "SR_ERROR_RUNTIME_UNAVAILABLE";
	case SR_ERROR_DEVICE_NOT_AVAILABLE: return "SR_ERROR_DEVICE_NOT_AVAILABLE";
	case SR_ERROR_HANDLE_INVALID: return "SR_ERROR_HANDLE_INVALID";
	case SR_ERROR_DISPLAY_NOT_FOUND: return "SR_ERROR_DISPLAY_NOT_FOUND";
	case SR_ERROR_FEATURE_NOT_SUPPORTED: return "SR_ERROR_FEATURE_NOT_SUPPORTED";
	case SR_ERROR_GRAPHICS_DEVICE_LOST: return "SR_ERROR_GRAPHICS_DEVICE_LOST";
	case SR_ERROR_FUNCTION_UNSUPPORTED: return "SR_ERROR_FUNCTION_UNSUPPORTED";
	case SR_ERROR_API_VERSION_UNSUPPORTED: return "SR_ERROR_API_VERSION_UNSUPPORTED";
	case SR_ERROR_LENS_NOT_AVAILABLE: return "SR_ERROR_LENS_NOT_AVAILABLE";
	default: return "SR_<unknown>";
	}
}

bool
leia_sr_v2_create_instance(double max_time, SrInstance *out_instance)
{
	*out_instance = nullptr;

	const double start_time = (double)GetTickCount64() / 1000.0;
	SrResult last = SR_ERROR_RUNTIME_UNAVAILABLE;

	for (;;) {
		SrInstanceCreateInfo ci{};
		ci.sType = SR_TYPE_INSTANCE_CREATE_INFO;
		ci.pNext = nullptr;
		ci.apiVersion = SR_CURRENT_API_VERSION;
		ci.networkMode = SR_NETWORK_MODE_STANDALONE;

		SrInstance inst = nullptr;
		last = srCreateInstance(&ci, &inst);
		if (SR_SUCCEEDED(last) && inst != nullptr) {
			*out_instance = inst;
			return true;
		}

		// The server may simply be starting up. Anything else is fatal now and
		// will still be fatal in ten seconds, so do not burn the timeout on it.
		if (last != SR_ERROR_RUNTIME_UNAVAILABLE) {
			U_LOG_E("srCreateInstance failed: %s (%d)", leia_sr_v2_result_str(last), (int)last);
			return false;
		}

		U_LOG_D("Waiting for the SR runtime...");
		Sleep(100);

		const double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			U_LOG_E("SR runtime did not become available within %.1f seconds (last: %s)", max_time,
			        leia_sr_v2_result_str(last));
			return false;
		}
	}
}

bool
leia_sr_v2_initialize(SrInstance instance)
{
	const SrResult r = srInitialize(instance);
	if (!SR_SUCCEEDED(r)) {
		U_LOG_E("srInitialize failed: %s (%d)", leia_sr_v2_result_str(r), (int)r);
		return false;
	}
	return true;
}

bool
leia_sr_v2_query_display(SrInstance instance, void *hwnd, double max_time, struct leia_sr_v2_display_info *out_info)
{
	*out_info = {};

	SrDisplayCreateInfo ci{};
	ci.sType = SR_TYPE_DISPLAY_CREATE_INFO;
	ci.pNext = nullptr;
	// Zero means "primary SR display", which is what the v1 path used
	// unconditionally (getPrimaryActiveSRDisplay). Passing the window when we
	// have one is strictly better on a multi-display box.
	ci.window = (SrNativeWindowHandle)hwnd;

	SrDisplay display = nullptr;
	const SrResult cr = srCreateDisplay(instance, &ci, &display);
	if (!SR_SUCCEEDED(cr) || display == nullptr) {
		U_LOG_E("srCreateDisplay failed: %s (%d)", leia_sr_v2_result_str(cr), (int)cr);
		return false;
	}

	// Wait for the display to report a non-degenerate location. A display can
	// exist but not yet have geometry while the service is still enumerating,
	// which is why v1 spun on exactly this condition rather than on validity.
	const double start_time = (double)GetTickCount64() / 1000.0;
	bool ready = false;
	SrRecti loc{};

	for (;;) {
		SrBool32 valid = SR_FALSE;
		if (SR_SUCCEEDED(srDisplayIsValid(display, &valid)) && valid == SR_TRUE) {
			if (SR_SUCCEEDED(srDisplayGetLocation(display, &loc))) {
				if ((loc.right - loc.left) != 0 && (loc.bottom - loc.top) != 0) {
					ready = true;
					break;
				}
			}
		}

		U_LOG_D("Waiting for the SR display...");
		Sleep(100);

		const double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (!ready) {
		U_LOG_E("SR display not ready within %.1f seconds", max_time);
		srDestroyDisplay(display);
		return false;
	}

	out_info->pixel_width = (uint32_t)(loc.right - loc.left);
	out_info->pixel_height = (uint32_t)(loc.bottom - loc.top);
	out_info->screen_left = (int32_t)loc.left;
	out_info->screen_top = (int32_t)loc.top;

	float width_cm = 0.0f;
	float height_cm = 0.0f;
	const SrResult pr = srDisplayGetPhysicalSize(display, &width_cm, &height_cm);
	if (!SR_SUCCEEDED(pr)) {
		U_LOG_E("srDisplayGetPhysicalSize failed: %s (%d)", leia_sr_v2_result_str(pr), (int)pr);
		srDestroyDisplay(display);
		return false;
	}
	out_info->width_m = width_cm / 100.0f;
	out_info->height_m = height_cm / 100.0f;

	// Per-eye, and the same underlying getter the v1 path uses — see the note on
	// leia_sr_v2_display_info before "simplifying" this by deriving it from the
	// physical resolution.
	int32_t rec_w = 0;
	int32_t rec_h = 0;
	const SrResult rr = srDisplayGetRecommendedTextureSize(display, &rec_w, &rec_h);
	if (SR_SUCCEEDED(rr) && rec_w > 0 && rec_h > 0) {
		out_info->recommended_view_width = (uint32_t)rec_w;
		out_info->recommended_view_height = (uint32_t)rec_h;
		out_info->recommended_valid = true;
		U_LOG_W("SR recommended view texture: %dx%d per eye", rec_w, rec_h);
	} else {
		// Leave invalid rather than substituting a guess; the caller has its own
		// fallback and a wrong view size is worse than a missing one.
		U_LOG_W("SR v2 recommended texture unavailable (%s) - caller will fall back",
		        leia_sr_v2_result_str(rr));
	}

	U_LOG_W("SR v2 display: %ux%u px at (%d,%d), physical %.2fcm x %.2fcm = %.4fm x %.4fm", out_info->pixel_width,
	        out_info->pixel_height, out_info->screen_left, out_info->screen_top, (double)width_cm, (double)height_cm,
	        (double)out_info->width_m, (double)out_info->height_m);

	// The geometry is copied out, so the handle has served its purpose. Holding
	// it would mean tracking staleness across display reconfiguration for no
	// benefit — v1 did not hold one either.
	srDestroyDisplay(display);
	return true;
}

void
leia_sr_v2_create_lens(SrInstance instance, SrLens *out_lens)
{
	*out_lens = nullptr;

	SrLensCreateInfo ci{};
	ci.sType = SR_TYPE_LENS_CREATE_INFO;
	ci.pNext = nullptr;
	ci.admin = SR_FALSE;

	SrLens lens = nullptr;
	const SrResult r = srCreateLens(instance, &ci, &lens);
	if (SR_SUCCEEDED(r) && lens != nullptr) {
		*out_lens = lens;
		U_LOG_W("SR v2 lens created");
		return;
	}

	// Not fatal: a display without a switchable lens is a supported
	// configuration, and the caller degrades to "cannot switch 2D/3D".
	U_LOG_W("SR v2 lens not available (%s) - 2D/3D switching disabled", leia_sr_v2_result_str(r));
}

#endif // DXR_LEIA_HAS_SR_V2
