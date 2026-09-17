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

#include <sr/sr_instance.h>
#include <sr/sr_version.h>
#include <sr/sr_weaver.h>

#include <atomic>
#include <stdlib.h>
#include <string.h>
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

/* ------------------------------------------------------------------ *
 * Absolute target time -- see the long comment in the header.
 * ------------------------------------------------------------------ */

bool
leia_sr_target_time_opt_in(void)
{
	// Read once, cached. Same shape as the LEIA_*_LATENCY_* knobs, except that
	// this one is process-wide rather than per-arm: the weaver that engages is
	// per-arm, the decision to allow it is not.
	static int cached = -1;
	if (cached < 0) {
		const char *v = getenv("DXR_LEIA_SR_TARGET_TIME");
		const bool on = v != nullptr && (strcmp(v, "1") == 0 || _stricmp(v, "true") == 0 ||
		                                 _stricmp(v, "on") == 0);
		cached = on ? 1 : 0;
		if (on) {
			U_LOG_W("Leia SR: DXR_LEIA_SR_TARGET_TIME set - absolute weave target time "
			        "requested (per-weaver probe + clock gate decide whether it engages)");
		}
	}
	return cached == 1;
}

uint64_t
leia_sr_target_horizon_override_us(void)
{
	// Read once, cached. DIAGNOSTIC knob for the predict-trace experiment; see
	// the header. Parsed with strtoull so a trailing space from `set X=v &&`
	// in cmd.exe (the DXR_LEIA_SR_API trap) cannot silently zero it.
	static long long cached = -1;
	if (cached < 0) {
		const char *v = getenv("DXR_LEIA_SR_TARGET_HORIZON_OVERRIDE_US");
		unsigned long long us = 0;
		if (v != nullptr && v[0] != '\0') {
			us = strtoull(v, nullptr, 10);
		}
		cached = (long long)us;
		if (us != 0) {
			U_LOG_W("Leia SR: DXR_LEIA_SR_TARGET_HORIZON_OVERRIDE_US=%llu - DIAGNOSTIC: the target "
			        "branch will push now + %llu us instead of the computed horizon (clamped to "
			        "%d us). Not a shipping configuration.",
			        us, us, (int)LEIA_SR_TARGET_MAX_HORIZON_US);
		}
	}
	return (uint64_t)cached;
}

bool
leia_sr_v2_target_time_probe_ok(SrResult r, const char *arm, struct leia_sr_v2_warn_latches *w)
{
	if (r == SR_ERROR_FUNCTION_UNSUPPORTED) {
		// Older SR runtime: srWeaverSetTargetTime is appended dispatch slot 90
		// and is NULL there, so the loader trampoline answered without reaching
		// a backend. Not a fault -- and not a reason to do anything other than
		// keep the adaptive setLatency path we have always had.
		if (w != nullptr && !w->no_slot) {
			U_LOG_W("Leia %s target time: this SR runtime has no srWeaverSetTargetTime "
			        "(SR_ERROR_FUNCTION_UNSUPPORTED) - staying on adaptive setLatency",
			        arm);
			w->no_slot = true;
		}
		return false;
	}

	if (r == SR_ERROR_FEATURE_NOT_SUPPORTED) {
		// Current runtime, but this weaver's backend has no target-time
		// interface. Distinct from the above on purpose: the fix is a backend,
		// not a runtime.
		if (w != nullptr && !w->no_interface) {
			U_LOG_W("Leia %s target time: this weaver backend has no target-time interface "
			        "(SR_ERROR_FEATURE_NOT_SUPPORTED) - staying on adaptive setLatency",
			        arm);
			w->no_interface = true;
		}
		return false;
	}

	if (!SR_SUCCEEDED(r)) {
		if (w != nullptr && !w->probe_failed) {
			U_LOG_W("Leia %s target time: probe failed: %s (%d) - staying on adaptive "
			        "setLatency",
			        arm, leia_sr_v2_result_str(r), (int)r);
			w->probe_failed = true;
		}
		return false;
	}

	return true;
}

namespace {

/*!
 * This process's own QPC-since-boot in microseconds.
 *
 * Deliberately NOT routed through any existing monotonic helper: os_monotonic
 * on Windows is its own normalisation and there is no guarantee it is the same
 * domain, which is the entire thing this gate exists to check. Split-divide so
 * a multi-hour uptime cannot overflow the intermediate.
 */
bool
qpc_since_boot_us(uint64_t *out_us)
{
	LARGE_INTEGER freq{};
	LARGE_INTEGER ctr{};
	if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
		return false;
	}
	if (!QueryPerformanceCounter(&ctr) || ctr.QuadPart < 0) {
		return false;
	}
	const uint64_t f = (uint64_t)freq.QuadPart;
	const uint64_t c = (uint64_t)ctr.QuadPart;
	*out_us = (c / f) * 1000000ULL + ((c % f) * 1000000ULL) / f;
	return true;
}

/*!
 * The last delta the gate computed, for @ref leia_sr_v2_clock_gate_last_delta.
 *
 * Two atomics rather than a sentinel value: 0 us is a perfectly possible --
 * indeed the ideal -- delta, so "never ran" has to be its own bit. Written on
 * the weave thread (the gate runs inside the first weave's
 * w_target_time_available), read on a diagnostic thread; relaxed is enough for
 * a value that is only ever printed.
 */
std::atomic<int64_t> g_clock_gate_delta_us{0};
std::atomic<bool> g_clock_gate_ran{false};

} // namespace

bool
leia_sr_v2_clock_gate_last_delta(int64_t *out_delta_us)
{
	if (out_delta_us == nullptr || !g_clock_gate_ran.load(std::memory_order_acquire)) {
		return false;
	}
	*out_delta_us = g_clock_gate_delta_us.load(std::memory_order_relaxed);
	return true;
}

bool
leia_sr_v2_clock_gate(SrInstance instance, const char *arm)
{
	uint64_t sr_us = 0;
	const SrResult r = srGetTimeUs(instance, &sr_us);
	if (!SR_SUCCEEDED(r)) {
		U_LOG_W("Leia %s target time: srGetTimeUs failed: %s (%d) - not using absolute "
		        "target time for this weaver",
		        arm, leia_sr_v2_result_str(r), (int)r);
		return false;
	}

	uint64_t our_us = 0;
	if (!qpc_since_boot_us(&our_us)) {
		U_LOG_W("Leia %s target time: QueryPerformanceCounter unavailable - not using "
		        "absolute target time for this weaver",
		        arm);
		return false;
	}

	// Signed, and always logged: "they were both large numbers" is not a
	// verification. The delta IS the measurement.
	const int64_t delta_us = (int64_t)sr_us - (int64_t)our_us;
	const int64_t mag_us = delta_us < 0 ? -delta_us : delta_us;

	// Publish it for leia_sr_v2_clock_gate_last_delta. Recorded here, at the
	// one place it is measured, and deliberately NOT on the two early-return
	// paths above: if either clock could not be read there is no delta, and a
	// diagnostic must be able to tell that apart from a delta of zero.
	g_clock_gate_delta_us.store(delta_us, std::memory_order_relaxed);
	g_clock_gate_ran.store(true, std::memory_order_release);

	if (mag_us > 250000) {
		U_LOG_W("Leia %s target time: srGetTimeUs is NOT our clock - sr %llu us vs our "
		        "QPC-since-boot %llu us, delta %+lld us (> 250 ms). Not using absolute "
		        "target time for this weaver; adaptive setLatency stays in charge",
		        arm, (unsigned long long)sr_us, (unsigned long long)our_us,
		        (long long)delta_us);
		return false;
	}

	if (mag_us > 1000) {
		// Passes the fail-closed gate, but this is not the agreement the two
		// clocks should have: both are QueryPerformanceCounter divided by
		// QueryPerformanceFrequency, so anything past call latency means a
		// different frequency read or a wall-clock leak somewhere.
		U_LOG_W("Leia %s target time: clock-domain agreement WEAKER THAN EXPECTED - "
		        "delta %+lld us (expected tens of us; both sides are QPC/QPF). Engaging "
		        "anyway (under the 250 ms gate), but treat the prediction with suspicion",
		        arm, (long long)delta_us);
		return true;
	}

	U_LOG_W("Leia %s target time: clock gate OK - srGetTimeUs %llu us vs our "
	        "QPC-since-boot %llu us, delta %+lld us",
	        arm, (unsigned long long)sr_us, (unsigned long long)our_us, (long long)delta_us);
	return true;
}

bool
leia_sr_v2_now_us(SrInstance instance, uint64_t *out_now_us, const char *arm, struct leia_sr_v2_warn_latches *w)
{
	uint64_t now_us = 0;
	const SrResult r = srGetTimeUs(instance, &now_us);
	if (!SR_SUCCEEDED(r)) {
		// Once, not per frame -- this sits on the weave path.
		if (w != nullptr && !w->now_failed) {
			U_LOG_W("Leia %s target time: srGetTimeUs failed mid-run: %s (%d) - this weave "
			        "keeps the previous target",
			        arm, leia_sr_v2_result_str(r), (int)r);
			w->now_failed = true;
		}
		return false;
	}
	*out_now_us = now_us;
	return true;
}

void
leia_sr_v2_log_target_accepted(const char *arm, uint64_t target_us, uint64_t horizon_us, uint64_t resolved_us)
{
	// ACCEPTANCE SIGNAL ONLY -- never a horizon for our own maths.
	//
	// srWeaverGetLatency reports the horizon the target RESOLVED to, computed
	// inside srWeaverWeave (updateLatencyState + both predict calls), against
	// the SDK's own clock reading at that point -- not the reading our
	// predictors used. It is read here immediately AFTER the weave returns, so
	// it describes THIS weave; a read taken before the weave would describe the
	// previous one. Either way it is evidence that the target was consumed, and
	// nothing else: feeding it back into the horizon estimate would close a loop
	// between our prediction and the SDK's rounding of it.
	const int64_t delta_us = (int64_t)resolved_us - (int64_t)horizon_us;
	U_LOG_W("Leia %s target time: LIVE - target %llu us, horizon asked %llu us, weaver "
	        "resolved %llu us, delta %+lld us (acceptance readback only; never fed back)",
	        arm, (unsigned long long)target_us, (unsigned long long)horizon_us,
	        (unsigned long long)resolved_us, (long long)delta_us);
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
