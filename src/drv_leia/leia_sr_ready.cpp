// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR panel readiness predicate, the shared startup budget, the
 *         process-wide geometry cache and the late-identification watcher.
 *         Design + rationale in leia_sr_ready.h and docs/sr-readiness.md.
 * @ingroup drv_leia
 */

#include "leia_sr_ready.h"
#include "leia_interface.h"

#include "util/u_logging.h"

#ifdef XRT_HAVE_LEIA_SR_D3D11

#include "leia_sr_d3d11.h" /* the two fresh-handle SR queries this module gates */

#include <windows.h>
#include <sysinfoapi.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

//! Default startup budget, seconds. SR needs 4–7 s after SRSession start to
//! identify a panel that is ON; this comfortably covers that plus a slow boot,
//! and stays well under what a user would read as "the service hung".
constexpr double kDefaultBudgetS = 20.0;
//! Shared-memory poll period while inside the budget. Far cheaper than the
//! old approach of spinning fresh SR contexts in a loop.
constexpr DWORD kWaitPollMs = 100;
//! Watcher poll period after the budget expired.
constexpr DWORD kWatcherPollMs = 1000;
//! Floor for an SR-side verification spin once identified (see ready_clamp).
constexpr double kClampFloorS = 1.0;

std::mutex g_mu; // guards everything below except the atomics

// --- startup budget ---------------------------------------------------------
bool g_budget_started = false;
double g_budget_s = kDefaultBudgetS;
uint64_t g_budget_start_ms = 0;
uint64_t g_deadline_ms = 0;
bool g_waiting_logged = false;
bool g_identified_logged = false;
bool g_expired_logged = false;

// --- geometry cache + live head device --------------------------------------
leiasr_geometry g_geom = {};
xrt_device *g_hmd = nullptr;

// --- watcher ----------------------------------------------------------------
std::atomic<bool> g_watcher_started{false};
HANDLE g_watcher_stop = nullptr; // manual-reset
HANDLE g_watcher_done = nullptr; // manual-reset

double
budget_from_env()
{
	const char *s = std::getenv("DXR_LEIA_SR_READY_TIMEOUT_S");
	if (s == nullptr || *s == '\0') {
		return kDefaultBudgetS;
	}
	char *end = nullptr;
	const double v = std::strtod(s, &end);
	if (end == s || !std::isfinite(v) || v < 0.0) {
		U_LOG_W("DXR_LEIA_SR_READY_TIMEOUT_S='%s' is not a non-negative number — using the %.0f s default", s,
		        kDefaultBudgetS);
		return kDefaultBudgetS;
	}
	return v;
}

//! Start the ONE process-wide deadline if not started. Caller holds g_mu.
void
budget_start_locked()
{
	if (g_budget_started) {
		return;
	}
	g_budget_started = true;
	g_budget_s = budget_from_env();
	g_budget_start_ms = GetTickCount64();
	g_deadline_ms = g_budget_start_ms + (uint64_t)(g_budget_s * 1000.0);
}

void
watcher_body();

void
watcher_start()
{
	if (g_watcher_started.exchange(true)) {
		return;
	}
	g_watcher_stop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	g_watcher_done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (g_watcher_stop == nullptr || g_watcher_done == nullptr) {
		U_LOG_E("Leia SR readiness watcher: CreateEvent failed (%lu) — late geometry re-derivation disabled",
		        (unsigned long)GetLastError());
		return;
	}
	// Detached by design (there is exactly one, for the life of the process
	// or until identification); shutdown signals g_watcher_stop and waits on
	// g_watcher_done rather than joining, so a static std::thread can never
	// std::terminate at DLL unload.
	std::thread([]() { watcher_body(); }).detach();
}

//! Publish verified geometry: cache it, seed the ONE view-scale derivation and
//! update the live head device in place. Logs the one-off lifecycle line.
void
publish(const leiasr_geometry &g, const char *why)
{
	std::lock_guard<std::mutex> lk(g_mu);
	const bool first = !g_geom.valid;
	g_geom = g;
	leia_view_scale_set_from_dims(g.view_w, g.view_h, g.pixel_w, g.pixel_h);

	bool applied = false;
	if (g_hmd != nullptr) {
		applied = leia_hmd_apply_geometry(g_hmd, &g);
	}
	if (applied) {
		U_LOG_W("Leia display geometry re-derived after late SR identification: %ux%u px, %.4fx%.4f m, "
		        "nominal Z=%.2f m, view %ux%u, %.1f Hz (%s; %.1f s after first use) — live head device updated "
		        "in place",
		        g.pixel_w, g.pixel_h, (double)g.width_m, (double)g.height_m, (double)g.nominal_z_m, g.view_w,
		        g.view_h, (double)g.refresh_hz, why,
		        g_budget_started ? (double)(GetTickCount64() - g_budget_start_ms) / 1000.0 : 0.0);
	} else if (first) {
		U_LOG_W("Leia display geometry resolved: %ux%u px, %.4fx%.4f m, nominal Z=%.2f m, view %ux%u, %.1f Hz (%s)",
		        g.pixel_w, g.pixel_h, (double)g.width_m, (double)g.height_m, (double)g.nominal_z_m, g.view_w,
		        g.view_h, (double)g.refresh_hz, why);
	}
}

//! The fresh-handle query + verification. Never throws (the two callees catch
//! everything). false leaves the cache untouched.
bool
query_fresh(double max_time, leiasr_geometry *out)
{
	uint32_t view_w = 0, view_h = 0, nat_w = 0, nat_h = 0;
	float hz = 0.0f;
	if (!leiasr_query_recommended_view_dimensions(max_time, &view_w, &view_h, &hz, &nat_w, &nat_h) ||
	    nat_w == 0 || nat_h == 0) {
		return false;
	}
	leiasr_display_dimensions dims = {};
	if (!leiasr_static_get_display_dimensions(&dims) || !dims.valid || dims.width_m <= 0.0f ||
	    dims.height_m <= 0.0f) {
		return false;
	}
	*out = {};
	out->valid = true;
	out->pixel_w = nat_w;
	out->pixel_h = nat_h;
	out->view_w = view_w;
	out->view_h = view_h;
	out->refresh_hz = hz;
	out->width_m = dims.width_m;
	out->height_m = dims.height_m;
	out->nominal_x_m = dims.nominal_x_m;
	out->nominal_y_m = dims.nominal_y_m;
	out->nominal_z_m = dims.nominal_z_m;
	return true;
}

void
watcher_body()
{
	const uint64_t t0 = GetTickCount64();
	for (;;) {
		if (WaitForSingleObject(g_watcher_stop, kWatcherPollMs) == WAIT_OBJECT_0) {
			break;
		}
		{
			std::lock_guard<std::mutex> lk(g_mu);
			if (g_geom.valid) {
				break; // someone else (a weaver, a runtime query) got there first
			}
		}
		if (!leiasr_display_identified()) {
			continue;
		}
		// Identified. Query with a FRESH handle; a failure here (SR still
		// switching its active display) is simply retried next tick.
		bool ok = false;
		try {
			ok = leiasr_geometry_resolve(2.0, "late SR identification, watcher");
		} catch (...) {
			ok = false;
		}
		if (ok) {
			break;
		}
	}
	U_LOG_I("Leia SR readiness watcher exiting after %.0f s", (double)(GetTickCount64() - t0) / 1000.0);
	SetEvent(g_watcher_done);
}

} // namespace

extern "C" {

bool
leiasr_display_identified(void)
{
	// Layout (SR's sharedsrdevices.cpp): 1 count byte, then 32-byte serials.
	// The count is a single byte, so the read is atomic and the guarding mutex
	// (Global\sharedDeviceSerialMutex) is not needed for it.
	HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "Global\\sharedDeviceSerialMemory");
	if (mapping == nullptr) {
		return false; // SRService not running (or not yet created it)
	}
	bool identified = false;
	void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 1);
	if (view != nullptr) {
		identified = *static_cast<const volatile uint8_t *>(view) >= 1;
		UnmapViewOfFile(view);
	}
	CloseHandle(mapping);
	return identified;
}

bool
leiasr_ready_wait(void)
{
	if (leiasr_display_identified()) {
		return true;
	}

	uint64_t deadline_ms = 0;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		budget_start_locked();
		deadline_ms = g_deadline_ms;
		if (GetTickCount64() < deadline_ms && !g_waiting_logged) {
			g_waiting_logged = true;
			U_LOG_W("Leia SR has not identified the panel yet (count=0 in Global\\sharedDeviceSerialMemory — "
			        "panel asleep, or SR still enumerating) — waiting up to %.1f s (DXR_LEIA_SR_READY_TIMEOUT_S)",
			        g_budget_s);
		}
	}

	while (GetTickCount64() < deadline_ms) {
		Sleep(kWaitPollMs);
		if (leiasr_display_identified()) {
			std::lock_guard<std::mutex> lk(g_mu);
			if (!g_identified_logged) {
				g_identified_logged = true;
				U_LOG_W("Leia SR identified the panel %.1f s after first use — resuming geometry queries",
				        (double)(GetTickCount64() - g_budget_start_ms) / 1000.0);
			}
			return true;
		}
	}

	// Budget spent, still unidentified. From here every caller returns fast
	// and the watcher owns the late re-derivation.
	{
		std::lock_guard<std::mutex> lk(g_mu);
		if (!g_expired_logged) {
			g_expired_logged = true;
			U_LOG_W("Leia SR readiness budget (%.1f s) expired with the panel unidentified — geometry "
			        "queries now return immediately; a 1 Hz watcher will re-derive the geometry once SR "
			        "identifies the panel (wake the monitor / check the SR eye-tracker link)",
			        g_budget_s);
		}
	}
	watcher_start();
	return false;
}

double
leiasr_ready_clamp(double max_time)
{
	std::lock_guard<std::mutex> lk(g_mu);
	if (!g_budget_started) {
		return max_time;
	}
	const uint64_t now = GetTickCount64();
	const double remaining = now < g_deadline_ms ? (double)(g_deadline_ms - now) / 1000.0 : 0.0;
	const double bound = remaining > kClampFloorS ? remaining : kClampFloorS;
	return max_time < bound ? max_time : bound;
}

bool
leiasr_geometry_get(struct leiasr_geometry *out)
{
	if (out == nullptr) {
		return false;
	}
	std::lock_guard<std::mutex> lk(g_mu);
	*out = g_geom;
	return g_geom.valid;
}

bool
leiasr_geometry_resolve(double max_time, const char *why)
{
	{
		std::lock_guard<std::mutex> lk(g_mu);
		if (g_geom.valid) {
			return true;
		}
	}
	if (!leiasr_ready_wait()) {
		return false; // fast once the budget is spent
	}
	leiasr_geometry g = {};
	if (!query_fresh(max_time, &g)) {
		return false;
	}
	publish(g, why != nullptr ? why : "query");
	return true;
}

void
leiasr_ready_hmd_register(struct xrt_device *xdev)
{
	std::lock_guard<std::mutex> lk(g_mu);
	g_hmd = xdev;
	// Close the window between "published" and "device created": the device
	// was built from whatever was known at create; make it current now.
	if (xdev != nullptr && g_geom.valid && leia_hmd_apply_geometry(xdev, &g_geom)) {
		U_LOG_W("Leia display geometry applied to the head device at registration (published before create)");
	}
}

void
leiasr_ready_hmd_unregister(struct xrt_device *xdev)
{
	std::lock_guard<std::mutex> lk(g_mu);
	if (g_hmd == xdev) {
		g_hmd = nullptr;
	}
}

void
leiasr_ready_note_weaver_ready(void)
{
	{
		std::lock_guard<std::mutex> lk(g_mu);
		if (g_geom.valid) {
			return;
		}
	}
	if (!leiasr_display_identified()) {
		return; // a weaver on the default display — nothing trustworthy to publish
	}
	try {
		(void)leiasr_geometry_resolve(2.0, "weaver came up on an identified panel");
	} catch (...) {
	}
}

void
leiasr_ready_shutdown(void)
{
	if (!g_watcher_started.load() || g_watcher_stop == nullptr) {
		return;
	}
	SetEvent(g_watcher_stop);
	(void)WaitForSingleObject(g_watcher_done, 2500);
}

} // extern "C"

#else // !XRT_HAVE_LEIA_SR_D3D11

// Stubs for configures without the SR D3D11 SDK bits (no static SR queries to
// gate). The Linux arm has its own probe/geometry path and does not compile
// this file at all.

extern "C" {

bool
leiasr_display_identified(void)
{
	return false;
}

bool
leiasr_ready_wait(void)
{
	return false;
}

double
leiasr_ready_clamp(double max_time)
{
	return max_time;
}

bool
leiasr_geometry_get(struct leiasr_geometry *out)
{
	(void)out;
	return false;
}

bool
leiasr_geometry_resolve(double max_time, const char *why)
{
	(void)max_time;
	(void)why;
	return false;
}

void
leiasr_ready_hmd_register(struct xrt_device *xdev)
{
	(void)xdev;
}

void
leiasr_ready_hmd_unregister(struct xrt_device *xdev)
{
	(void)xdev;
}

void
leiasr_ready_note_weaver_ready(void)
{
}

void
leiasr_ready_shutdown(void)
{
}

} // extern "C"

#endif // XRT_HAVE_LEIA_SR_D3D11
