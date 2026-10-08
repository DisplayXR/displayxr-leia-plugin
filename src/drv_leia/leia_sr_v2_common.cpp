// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Implementation of the shared SR v2 instance/display/lens helpers.
 * @ingroup drv_leia
 */

#include "leia_sr_v2_common.h"
#include "leia_sr_multi_win.h" // multi-screen M6 caps (plain C)

#ifdef DXR_LEIA_HAS_SR_V2

#include "util/u_logging.h"

#include <sr/sr_instance.h>
#include <sr/sr_version.h>
#include <sr/sr_weaver.h>
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
#include <sr/sr_display.h> // SrDisplayBindingInfo / capabilities, srDisplayGetIdentifier
#endif
#ifdef DXR_LEIA_HAS_SR_LENS_BINDING
#include <sr/sr_lens.h> // SrLensBindingCapabilities (SR D3)
#endif

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
leia_sr_v2_query_display(SrInstance instance,
                         void *hwnd,
                         double max_time,
                         uint64_t bind_display_id,
                         struct leia_sr_v2_display_info *out_info)
{
	*out_info = {};

	SrDisplayCreateInfo ci{};
	ci.sType = SR_TYPE_DISPLAY_CREATE_INFO;
	ci.pNext = nullptr;
	// Zero means "primary SR display", which is what the v1 path used
	// unconditionally (getPrimaryActiveSRDisplay). Passing the window when we
	// have one is strictly better on a multi-display box.
	ci.window = (SrNativeWindowHandle)hwnd;
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
	// Multi-screen M6: a screen-bound DP asks for ITS display by id, so the
	// geometry below describes that panel whatever window (or none) it has.
	SrDisplayBindingInfo binding{};
	binding.sType = SR_TYPE_DISPLAY_BINDING_INFO;
	binding.pNext = nullptr;
	binding.displayId = bind_display_id;
	if (bind_display_id != 0) {
		ci.pNext = &binding;
	}
#else
	(void)bind_display_id;
#endif

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
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
	{
		uint64_t id = 0;
		if (SR_SUCCEEDED(srDisplayGetIdentifier(display, &id))) {
			out_info->display_id = id;
		}
	}
#endif

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
leia_sr_v2_query_multi_caps(SrInstance instance, struct leia_win_sr_multi_caps *out)
{
	memset(out, 0, sizeof(*out));
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
	if (instance == nullptr) {
		return;
	}
	SrDisplayBindingCapabilities bind_caps{};
	bind_caps.sType = SR_TYPE_DISPLAY_BINDING_CAPABILITIES;
#ifdef DXR_LEIA_HAS_SR_LENS_BINDING
	// SR D3: lens per device (tag 27), chained after the binding caps. A
	// runtime that predates it leaves the struct untouched (lensPerDevice 0).
	SrLensBindingCapabilities lens_caps{};
	lens_caps.sType = SR_TYPE_LENS_BINDING_CAPABILITIES;
	bind_caps.pNext = &lens_caps;
#endif
	SrWeaverRoutingCapabilities route_caps{};
	route_caps.sType = SR_TYPE_WEAVER_ROUTING_CAPABILITIES;
	route_caps.pNext = &bind_caps;
	SrRuntimeCapabilities caps{};
	caps.sType = SR_TYPE_RUNTIME_CAPABILITIES;
	caps.pNext = &route_caps;
	const SrResult r = srGetRuntimeCapabilities(instance, &caps);
	if (SR_FAILED(r)) {
		U_LOG_W("SR v2: srGetRuntimeCapabilities failed: %s (%d) — per-screen weaver plan stays SDK-default",
		        leia_sr_v2_result_str(r), (int)r);
		return;
	}
	out->known = true;
	out->external_routing = route_caps.externalRouting != SR_FALSE;
	out->keep_drag_snap = (route_caps.supportedFlags & SR_WEAVER_ROUTING_KEEP_DRAG_SNAP_BIT) != 0;
	out->display_binding = bind_caps.displayBinding != SR_FALSE;
	out->max_bound_displays = bind_caps.maxBoundDisplays;
#ifdef DXR_LEIA_HAS_SR_LENS_BINDING
	out->lens_per_device = lens_caps.lensPerDevice != SR_FALSE;
#endif
#else
	(void)instance;
#endif
}

void
leia_sr_v2_create_lens(SrInstance instance, uint64_t bind_display_id, SrLens *out_lens)
{
	*out_lens = nullptr;

	SrLensCreateInfo ci{};
	ci.sType = SR_TYPE_LENS_CREATE_INFO;
	ci.pNext = nullptr;
	ci.admin = SR_FALSE;
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
	// SR D3: a lens bound to a display drives that display's FPC. The
	// runtime refuses the bind on a non-active display unless it reports
	// lensPerDevice (the caller gates on that), and falls back to the active
	// display's lens for an active-display bind on an older service.
	SrDisplayBindingInfo binding{};
	binding.sType = SR_TYPE_DISPLAY_BINDING_INFO;
	binding.pNext = nullptr;
	binding.displayId = bind_display_id;
	if (bind_display_id != 0) {
		ci.pNext = &binding;
	}
#else
	(void)bind_display_id;
#endif

	SrLens lens = nullptr;
	const SrResult r = srCreateLens(instance, &ci, &lens);
	if (SR_SUCCEEDED(r) && lens != nullptr) {
		*out_lens = lens;
		if (bind_display_id != 0) {
			U_LOG_W("SR v2 lens created, bound to display 0x%016llx", (unsigned long long)bind_display_id);
		} else {
			U_LOG_W("SR v2 lens created");
		}
		return;
	}

	// Not fatal: a display without a switchable lens is a supported
	// configuration, and the caller degrades to "cannot switch 2D/3D".
	U_LOG_W("SR v2 lens not available (%s) - 2D/3D switching disabled", leia_sr_v2_result_str(r));
}


/* ------------------------------------------------------------------ *
 * Multi-screen M0 -- SR display enumeration for probe_displays
 * (leia_display_claims_win.h). Windows twin of the Linux arm's
 * leia_lnx_sr_enumerate_displays().
 *
 * Differences from Linux, and why:
 *  - Linux reads the LIVE weaver context and never creates one just to
 *    probe. On Windows every DP owns its own SrInstance and a headless
 *    caller (displayxr-cli, the service before any session) has none, so
 *    the probe keeps ONE process-wide instance of its own. It is created
 *    lazily, NEVER srInitialize'd (rt_EnumerateDisplays needs only a valid
 *    instance; initialise would start trackers -- confirmed with the SR
 *    session 2026-10-07), in NON_BLOCKING_CLIENT mode so a stopped SR
 *    Service cannot block the registry refresh, and torn down on plug-in
 *    destroy.
 *  - A 2 s cache: the runtime re-runs probe_displays per registry refresh
 *    and at ~1 Hz while a panel is unidentified (runtime#1722).
 *  - HMONITORs and EDID_ONLY displayIds are NOT stable across display-config
 *    events (the EDID_ONLY id is a hash until the FPC pairs it), so nothing
 *    here persists beyond the cache TTL; M5/M6 must re-resolve from the
 *    claim table, not remember a displayId.
 * ------------------------------------------------------------------ */

#ifdef DXR_LEIA_HAS_SR_DISPLAY_ENUM

#include "leia_display_claims_win.h"
#include "leia_platform_state.h"
#include "os/os_time.h"

#include <sr/sr_display.h>

#include <stdio.h>

namespace {

SRWLOCK g_enum_lock = SRWLOCK_INIT;
SrInstance g_probe_instance = nullptr;
bool g_enum_unsupported = false; // installed SR runtime predates slot 106: final for this process
bool g_caps_logged = false;
uint64_t g_last_create_fail_ns = 0;

struct leia_win_sr_display g_cache[LEIA_WIN_SR_MAX_DISPLAYS];
int32_t g_cache_count = -1;
uint64_t g_cache_ns = 0;
uint64_t g_logged_fingerprint = 0;

constexpr uint64_t CACHE_TTL_NS = 2000000000ull;        // 2 s
constexpr uint64_t CREATE_RETRY_NS = 2000000000ull;     // after a failed srCreateInstance
constexpr double CREATE_MAX_WAIT_S = 0.25;              // non-blocking mode: one or two spins at most

void
probe_instance_drop_locked()
{
	if (g_probe_instance != nullptr) {
		srDestroyInstance(g_probe_instance);
		g_probe_instance = nullptr;
	}
	g_caps_logged = false;
}

bool
probe_instance_ensure_locked(uint64_t now_ns)
{
	if (g_probe_instance != nullptr) {
		return true;
	}
	if (g_last_create_fail_ns != 0 && now_ns - g_last_create_fail_ns < CREATE_RETRY_NS) {
		return false;
	}
	// Delay-loaded SR client DLLs must be bound before ANY SR call.
	if (leia_sr_client_bind() != LEIA_SR_BIND_OK) {
		g_last_create_fail_ns = now_ns;
		return false;
	}

	const double start_s = (double)GetTickCount64() / 1000.0;
	for (;;) {
		SrInstanceCreateInfo ci{};
		ci.sType = SR_TYPE_INSTANCE_CREATE_INFO;
		ci.pNext = nullptr;
		ci.apiVersion = SR_CURRENT_API_VERSION;
		ci.networkMode = SR_NETWORK_MODE_NON_BLOCKING_CLIENT;

		SrInstance inst = nullptr;
		const SrResult r = srCreateInstance(&ci, &inst);
		if (SR_SUCCEEDED(r) && inst != nullptr) {
			g_probe_instance = inst;
			g_last_create_fail_ns = 0;
			U_LOG_W("leia_plugin: SR display-enumeration probe instance created (non-blocking client, "
			        "never initialised)");
			return true;
		}
		if (r != SR_ERROR_RUNTIME_UNAVAILABLE ||
		    ((double)GetTickCount64() / 1000.0 - start_s) > CREATE_MAX_WAIT_S) {
			static SrResult logged = SR_SUCCESS;
			if (logged != r) {
				logged = r;
				U_LOG_W("leia_plugin: SR display-enumeration probe instance unavailable: %s (%d) -- "
				        "claims fall back to the frozen EDID table",
				        leia_sr_v2_result_str(r), (int)r);
			}
			g_last_create_fail_ns = now_ns;
			return false;
		}
		Sleep(50);
	}
}

/*! Capability query, once per instance: the phase A/C structs M6 will chain
 *  (routing EXTERNAL, binding by displayId). Logged for bring-up. */
void
log_caps_once_locked()
{
	if (g_caps_logged) {
		return;
	}
	g_caps_logged = true;

	SrDisplayBindingCapabilities bind_caps{};
	bind_caps.sType = SR_TYPE_DISPLAY_BINDING_CAPABILITIES;
	SrWeaverRoutingCapabilities route_caps{};
	route_caps.sType = SR_TYPE_WEAVER_ROUTING_CAPABILITIES;
	route_caps.pNext = &bind_caps;
	SrRuntimeCapabilities caps{};
	caps.sType = SR_TYPE_RUNTIME_CAPABILITIES;
	caps.pNext = &route_caps;
	const SrResult r = srGetRuntimeCapabilities(g_probe_instance, &caps);
	if (SR_FAILED(r)) {
		U_LOG_W("leia_plugin: srGetRuntimeCapabilities failed: %s (%d)", leia_sr_v2_result_str(r), (int)r);
		return;
	}
	U_LOG_W("leia_plugin: SR multi-display caps: externalRouting=%u routingFlags=0x%llx displayBinding=%u "
	        "maxBoundDisplays=%u",
	        (unsigned)route_caps.externalRouting, (unsigned long long)route_caps.supportedFlags,
	        (unsigned)bind_caps.displayBinding, (unsigned)bind_caps.maxBoundDisplays);
}

void
from_descriptor(const SrDisplayDescriptor &d, struct leia_win_sr_display &out)
{
	memset(&out, 0, sizeof(out));
	out.display_id = d.displayId;
	out.fpc_verified = d.confidence == SR_DISPLAY_CONFIDENCE_FPC_VERIFIED;
	if (out.fpc_verified) {
		snprintf(out.serial, sizeof(out.serial), "%.*s", (int)sizeof(d.serial), d.serial);
	}
	snprintf(out.product_code, sizeof(out.product_code), "%.*s", (int)sizeof(d.productCode) - 1, d.productCode);
	out.manufacturer_id = leia_win_pnp_to_manufacturer_id(d.edidVendor);
	out.product_id = d.edidProduct;
	out.edid_serial = d.edidSerial;
	out.left = (int32_t)d.location.left;
	out.top = (int32_t)d.location.top;
	out.location_is_desktop_global = d.locationIsDesktopGlobal == SR_TRUE;
	out.native_w = d.nativeWidth > 0 ? (uint32_t)d.nativeWidth : 0;
	out.native_h = d.nativeHeight > 0 ? (uint32_t)d.nativeHeight : 0;
	out.refresh_hz = d.refreshHz;
	snprintf(out.device_name, sizeof(out.device_name), "%.*s", (int)sizeof(d.connector), d.connector);
	out.hmonitor = d.platformHandle;
}

uint64_t
fingerprint(const struct leia_win_sr_display *v, int32_t n)
{
	// FNV-1a over the fields a claim depends on, so the WARN fires once per
	// change of the answer rather than once per 2 s refresh.
	uint64_t h = 1469598103934665603ull;
	auto mix = [&h](const void *p, size_t len) {
		const uint8_t *b = (const uint8_t *)p;
		for (size_t i = 0; i < len; i++) {
			h ^= b[i];
			h *= 1099511628211ull;
		}
	};
	mix(&n, sizeof(n));
	for (int32_t i = 0; i < n; i++) {
		mix(&v[i].display_id, sizeof(v[i].display_id));
		mix(&v[i].fpc_verified, sizeof(v[i].fpc_verified));
		mix(v[i].serial, strlen(v[i].serial));
		mix(&v[i].manufacturer_id, sizeof(v[i].manufacturer_id));
		mix(&v[i].product_id, sizeof(v[i].product_id));
		mix(&v[i].left, sizeof(v[i].left));
		mix(&v[i].top, sizeof(v[i].top));
		mix(&v[i].location_is_desktop_global, sizeof(v[i].location_is_desktop_global));
	}
	return h;
}

/*! @return true when the cache now holds a fresh answer. */
bool
refresh_locked(uint64_t now_ns)
{
	uint32_t count = 0;
	SrResult r = srEnumerateDisplays(g_probe_instance, &count, nullptr);
	if (SR_FAILED(r)) {
		if (r == SR_ERROR_FUNCTION_UNSUPPORTED || r == SR_ERROR_FEATURE_NOT_SUPPORTED) {
			// The installed SR runtime predates slot 106 (or cannot enumerate
			// here). Final for this process: the frozen-table path takes over.
			U_LOG_W("leia_plugin: srEnumerateDisplays not available on the installed SR runtime (%s) -- "
			        "claims use the frozen EDID table",
			        leia_sr_v2_result_str(r));
			g_enum_unsupported = true;
			probe_instance_drop_locked();
			return false;
		}
		// Anything else (service restarted under us, handle gone): drop the
		// instance so the next refresh recreates it; keep no stale answer.
		U_LOG_W("leia_plugin: srEnumerateDisplays (count) failed: %s (%d) -- probe instance dropped",
		        leia_sr_v2_result_str(r), (int)r);
		probe_instance_drop_locked();
		g_cache_count = -1;
		return false;
	}

	SrDisplayDescriptor descs[LEIA_WIN_SR_MAX_DISPLAYS];
	if (count > LEIA_WIN_SR_MAX_DISPLAYS) {
		count = LEIA_WIN_SR_MAX_DISPLAYS;
	}
	for (uint32_t i = 0; i < count; i++) {
		descs[i] = SrDisplayDescriptor{};
		descs[i].sType = SR_TYPE_DISPLAY_DESCRIPTOR;
	}
	if (count > 0) {
		r = srEnumerateDisplays(g_probe_instance, &count, descs);
		if (SR_FAILED(r)) { // SR_INCOMPLETE is a success code: keep the first `count`
			U_LOG_W("leia_plugin: srEnumerateDisplays failed: %s (%d) -- probe instance dropped",
			        leia_sr_v2_result_str(r), (int)r);
			probe_instance_drop_locked();
			g_cache_count = -1;
			return false;
		}
	}

	struct leia_win_sr_display fresh[LEIA_WIN_SR_MAX_DISPLAYS];
	for (uint32_t i = 0; i < count; i++) {
		from_descriptor(descs[i], fresh[i]);
	}
	const uint64_t fp = fingerprint(fresh, (int32_t)count);
	if (fp != g_logged_fingerprint) {
		g_logged_fingerprint = fp;
		U_LOG_W("leia_plugin: SR enumerates %u display(s):", count);
		for (uint32_t i = 0; i < count; i++) {
			const struct leia_win_sr_display &s = fresh[i];
			U_LOG_W("leia_plugin:   SR display #%u id=0x%016llx %s serial='%s' product=%s edid=0x%04X/0x%04X/%u "
			        "at (%d,%d)%s %ux%u %.2f Hz device='%s' hmonitor=0x%llx",
			        i, (unsigned long long)s.display_id, s.fpc_verified ? "FPC_VERIFIED" : "EDID_ONLY",
			        s.serial, s.product_code, s.manufacturer_id, s.product_id, s.edid_serial, s.left, s.top,
			        s.location_is_desktop_global ? "" : " (not desktop-global)", s.native_w, s.native_h,
			        (double)s.refresh_hz, s.device_name, (unsigned long long)s.hmonitor);
		}
	}
	memcpy(g_cache, fresh, count * sizeof(fresh[0]));
	g_cache_count = (int32_t)count;
	g_cache_ns = now_ns;
	return true;
}

} // namespace

extern "C" int32_t
leia_win_sr_enumerate_displays(struct leia_win_sr_display *out, uint32_t cap)
{
	int32_t ret = -1;
	AcquireSRWLockExclusive(&g_enum_lock);
	if (!g_enum_unsupported) {
		const uint64_t now_ns = os_monotonic_get_ns();
		const bool fresh = g_cache_count >= 0 && now_ns - g_cache_ns < CACHE_TTL_NS;
		if (!fresh && probe_instance_ensure_locked(now_ns)) {
			log_caps_once_locked();
			(void)refresh_locked(now_ns);
		}
		if (g_cache_count >= 0 && (fresh || now_ns - g_cache_ns < CACHE_TTL_NS)) {
			const uint32_t n = (uint32_t)g_cache_count < cap ? (uint32_t)g_cache_count : cap;
			if (out != nullptr && n > 0) {
				memcpy(out, g_cache, n * sizeof(out[0]));
			}
			ret = g_cache_count;
		}
	}
	ReleaseSRWLockExclusive(&g_enum_lock);
	return ret;
}

extern "C" void
leia_win_sr_enumerate_shutdown(void)
{
	AcquireSRWLockExclusive(&g_enum_lock);
	probe_instance_drop_locked();
	g_cache_count = -1;
	ReleaseSRWLockExclusive(&g_enum_lock);
}

#endif // DXR_LEIA_HAS_SR_DISPLAY_ENUM

#endif // DXR_LEIA_HAS_SR_V2
