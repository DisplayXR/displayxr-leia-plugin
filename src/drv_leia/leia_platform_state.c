// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR platform state machine — see leia_platform_state.h and
 *         docs/install-order-and-platform-state.md.
 * @ingroup drv_leia
 */

#include "leia_platform_state.h"
#include "leia_sr_ready.h"

#include "util/u_logging.h"

#include <string.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static SRWLOCK g_lock = SRWLOCK_INIT;

// Published state + hint (lock-free reads).
static volatile LONG g_state = LEIA_PLATFORM_UNKNOWN;
static const char *volatile g_hint = "";

// Below: guarded by g_lock.
static bool g_have_probe = false;
static uint64_t g_probe_ms = 0;
static uint64_t g_topology_sig = 0;
static bool g_logged = false;
static enum leia_platform_state g_logged_state = LEIA_PLATFORM_UNKNOWN;
static const char *g_logged_hint = NULL;

/*
 * The panel the EDID table matched, for P-c. `g_hit_seen` is sticky for the
 * process: once a table-known panel has been seen, an EDID miss means it was
 * unplugged, and the "SR knows a panel our table doesn't" fallback (which
 * would claim the primary monitor) must NOT kick in — that is exactly how the
 * plug-in used to weave on a normal monitor after an unplug.
 */
static bool g_hit_seen = false;
static bool g_panel_valid = false;
static uint16_t g_panel_mfr = 0;
static uint16_t g_panel_prod = 0;
static uint32_t g_panel_w = 0;
static uint32_t g_panel_h = 0;

static const char *const k_hint_install = "Install the LeiaSR Runtime";
static const char *const k_hint_reinstall = "LeiaSR Runtime files not found - reinstall the LeiaSR Runtime";
static const char *const k_hint_incompatible =
    "The installed LeiaSR Runtime is not compatible with this plug-in - update the LeiaSR Runtime";
static const char *const k_hint_not_running = "The SR Service is not running";
static const char *const k_hint_no_display = "No Leia 3D display detected";

const char *
leia_platform_state_name(enum leia_platform_state state)
{
	switch (state) {
	case LEIA_PLATFORM_READY: return "READY";
	case LEIA_PLATFORM_ABSENT: return "PLATFORM_ABSENT";
	case LEIA_PLATFORM_NOT_RUNNING: return "PLATFORM_NOT_RUNNING";
	case LEIA_PLATFORM_NO_DISPLAY: return "NO_DISPLAY";
	case LEIA_PLATFORM_INCOMPATIBLE: return "INCOMPATIBLE";
	default: return "UNKNOWN";
	}
}

//! FNV-1a over every monitor rect + the count. Microseconds; no SetupDi.
static BOOL CALLBACK
topology_cb(HMONITOR mon, HDC dc, LPRECT rc, LPARAM lp)
{
	(void)mon;
	(void)dc;
	uint64_t *h = (uint64_t *)lp;
	const unsigned char *p = (const unsigned char *)rc;
	for (size_t i = 0; i < sizeof(*rc); i++) {
		*h = (*h ^ p[i]) * 1099511628211ull;
	}
	*h = (*h ^ 0x5Au) * 1099511628211ull; // monitor separator => count is folded in
	return TRUE;
}

static uint64_t
topology_signature(void)
{
	uint64_t h = 1469598103934665603ull;
	(void)EnumDisplayMonitors(NULL, NULL, topology_cb, (LPARAM)&h);
	return h;
}

//! Caller holds g_lock.
static enum leia_platform_state
evaluate_locked(const struct leia_display_probe_result *edid, const char **out_hint)
{
	// --- P-c: track the matched panel; invalidate geometry on any change ---
	if (edid->hw_found) {
		const bool same = g_panel_valid && g_panel_mfr == edid->manufacturer_id &&
		                  g_panel_prod == edid->product_id && g_panel_w == edid->pixel_w &&
		                  g_panel_h == edid->pixel_h;
		if (g_panel_valid && !same) {
			leiasr_geometry_invalidate("the matched Leia panel changed");
		}
		g_panel_valid = true;
		g_panel_mfr = edid->manufacturer_id;
		g_panel_prod = edid->product_id;
		g_panel_w = edid->pixel_w;
		g_panel_h = edid->pixel_h;
		g_hit_seen = true;
	} else if (g_panel_valid) {
		g_panel_valid = false;
		leiasr_geometry_invalidate("the Leia panel is no longer attached (EDID match lost)");
	}

	// --- presence ladder; nothing here waits for SR ---
	if (!edid->sdk_installed) {
		*out_hint = k_hint_install;
		return LEIA_PLATFORM_ABSENT;
	}
	switch (leia_sr_client_bind()) {
	case LEIA_SR_BIND_OK: break;
	case LEIA_SR_BIND_INCOMPATIBLE: *out_hint = k_hint_incompatible; return LEIA_PLATFORM_INCOMPATIBLE;
	default: *out_hint = k_hint_reinstall; return LEIA_PLATFORM_ABSENT;
	}
	if (!edid->service_running) {
		*out_hint = k_hint_not_running;
		return LEIA_PLATFORM_NOT_RUNNING;
	}
	if (edid->hw_found) {
		*out_hint = "";
		return LEIA_PLATFORM_READY;
	}
	/*
	 * EDID table miss with SR up. The table is a frozen copy of SR's
	 * product-code map and drifts, so SR having identified a device is
	 * accepted as a panel — but ONLY if no table-known panel was seen in this
	 * process. After a hit, a miss is an unplug: NO_DISPLAY, claim nothing.
	 * One named-mapping read; this used to be a 2 s SR context spin.
	 */
	if (!g_hit_seen && leiasr_display_identified()) {
		*out_hint = "";
		return LEIA_PLATFORM_READY;
	}
	*out_hint = k_hint_no_display;
	return LEIA_PLATFORM_NO_DISPLAY;
}

//! Caller holds g_lock.
static enum leia_platform_state
publish_locked(const struct leia_display_probe_result *edid)
{
	const char *hint = "";
	enum leia_platform_state st = evaluate_locked(edid, &hint);

	InterlockedExchangePointer((PVOID volatile *)&g_hint, (PVOID)hint);
	InterlockedExchange(&g_state, (LONG)st);

	if (!g_logged || g_logged_state != st || g_logged_hint != hint) {
		g_logged = true;
		const enum leia_platform_state prev = g_logged_state;
		g_logged_state = st;
		g_logged_hint = hint;
		U_LOG_W("Leia SR platform state: %s%s%s (was %s; key=%d service=%d edid_match=%d dlls_bound=%d)",
		        leia_platform_state_name(st), hint[0] != '\0' ? " - " : "", hint,
		        leia_platform_state_name(prev), edid->sdk_installed, edid->service_running, edid->hw_found,
		        leia_sr_client_bound());
	}
	return st;
}

enum leia_platform_state
leia_platform_state_evaluate(const struct leia_display_probe_result *edid)
{
	if (edid == NULL) {
		return leia_platform_state_get();
	}
	AcquireSRWLockExclusive(&g_lock);
	g_have_probe = true;
	g_probe_ms = GetTickCount64();
	g_topology_sig = topology_signature();
	enum leia_platform_state st = publish_locked(edid);
	ReleaseSRWLockExclusive(&g_lock);
	return st;
}

enum leia_platform_state
leia_platform_state_refresh(uint32_t max_edid_age_ms)
{
	AcquireSRWLockExclusive(&g_lock);
	const uint64_t now = GetTickCount64();
	const uint64_t sig = topology_signature();
	const bool stale = max_edid_age_ms != UINT32_MAX && (now - g_probe_ms) >= (uint64_t)max_edid_age_ms;
	struct leia_display_probe_result edid;
	memset(&edid, 0, sizeof(edid));
	if (!g_have_probe || sig != g_topology_sig || stale) {
		(void)leia_edid_probe_display(&edid); // also re-reads key + mapping
		g_have_probe = true;
		g_probe_ms = now;
		g_topology_sig = sig;
	} else {
		// Same monitors: reuse the EDID match, re-read the two cheap
		// platform signals (registry key, SR Service mapping).
		(void)leia_edid_get_cached_result(&edid);
		leia_sr_presence(&edid.sdk_installed, &edid.service_running);
	}
	enum leia_platform_state st = publish_locked(&edid);
	ReleaseSRWLockExclusive(&g_lock);
	return st;
}

enum leia_platform_state
leia_platform_state_get(void)
{
	return (enum leia_platform_state)InterlockedCompareExchange(&g_state, 0, 0);
}

const char *
leia_platform_state_get_hint(void)
{
	const char *h = (const char *)InterlockedCompareExchangePointer((PVOID volatile *)&g_hint, NULL, NULL);
	return h != NULL ? h : "";
}

bool
leia_platform_display_absent(void)
{
	return leia_platform_state_get() == LEIA_PLATFORM_NO_DISPLAY;
}

#else // !_WIN32

// The Linux and Android arms have their own plug-in entry points and do not
// compile this file; stubs keep a stray non-Windows configure linking.

const char *
leia_platform_state_name(enum leia_platform_state state)
{
	(void)state;
	return "UNKNOWN";
}

enum leia_platform_state
leia_platform_state_evaluate(const struct leia_display_probe_result *edid)
{
	(void)edid;
	return LEIA_PLATFORM_UNKNOWN;
}

enum leia_platform_state
leia_platform_state_refresh(uint32_t max_edid_age_ms)
{
	(void)max_edid_age_ms;
	return LEIA_PLATFORM_UNKNOWN;
}

enum leia_platform_state
leia_platform_state_get(void)
{
	return LEIA_PLATFORM_UNKNOWN;
}

const char *
leia_platform_state_get_hint(void)
{
	return "";
}

bool
leia_platform_display_absent(void)
{
	return false;
}

#endif // _WIN32
