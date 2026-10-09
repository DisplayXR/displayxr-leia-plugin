// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Per-monitor display claims for the Windows arm (`probe_displays`,
 *         multi-screen plan M0) — pure matching logic, no I/O.
 *
 * Windows twin of leia_display_claims_linux.h. Two inputs are joined:
 *   1. the runtime's monitor list (`xrt_display_descriptor[]`) — on Windows
 *      every descriptor carries EDID ids and a desktop origin, so there is no
 *      "descriptor without ids" path and no separate EDID scan to pair with;
 *   2. what the SR runtime says, in one of two shapes:
 *        - new SR API (srEnumerateDisplays, slot 106 on Windows, LeiaSR
 *          1.38.0+2031 headers): one @ref leia_win_sr_display per SR display,
 *          with FPC confidence, serial and the opaque displayId the later
 *          binding (SR-P2) needs;
 *        - older SR (no enumeration): only "the platform is READY and the
 *          frozen EDID table knows the panel" — today's behaviour, kept
 *          byte-identical so nothing changes on a box whose SR runtime
 *          predates the API.
 *   3. SR's own monitor list (srEnumerateMonitors, slot 108 on Windows,
 *      LeiaSR 1.38.0+2192 headers), when the installed SR runtime has it:
 *      every OS monitor with the SR display it belongs to. SR joins its
 *      displays to monitors by HMONITOR itself, so a monitor it lists is
 *      bound without any EDID/origin guesswork (see leia_win_compute_claims).
 *
 * Kept free of SDK and Win32 headers so a host unit test drives it with
 * fixtures (tests/test_display_claims_win.c, run by the Linux CI job).
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_plugin.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Upper bound on SR displays one enumeration is read into.
#define LEIA_WIN_SR_MAX_DISPLAYS 8

/*!
 * One SR display as the new SR API reports it (SrDisplayDescriptor, SR_TYPE
 * 25), reduced to what claiming and binding need. Plain C so the claim logic
 * and its test never see SR headers.
 */
struct leia_win_sr_display
{
	uint64_t display_id; //!< opaque SR key for SrDisplayBindingInfo (SR-P2)
	bool fpc_verified;   //!< confidence == SR_DISPLAY_CONFIDENCE_FPC_VERIFIED
	char serial[64];     //!< FPC serial when @ref fpc_verified, else empty
	char product_code[4];

	uint16_t manufacturer_id; //!< EDID PNP id packed like EDID bytes 8-9 (LE word)
	uint16_t product_id;      //!< EDID bytes 10-11
	uint32_t edid_serial;     //!< EDID bytes 12-15; 0 = none

	//! Desktop origin in device px — meaningful only when
	//! @ref location_is_desktop_global (SR's own caveat).
	int32_t left, top;
	bool location_is_desktop_global;

	uint32_t native_w, native_h;
	float refresh_hz;
	//! Physical size from SR's own calibration (the descriptor's cm, x10);
	//! 0 = SR did not say. Multi-screen M1/M3: the per-monitor display info.
	uint32_t width_mm, height_mm;

	char device_name[32]; //!< GDI device name (SR's `connector` on Windows), may be empty
	uint64_t hmonitor;    //!< HMONITOR at enumeration time (SR's `platformHandle`), 0 = none
};

//! Upper bound on OS monitors one SR monitor enumeration is read into.
#define LEIA_WIN_SR_MAX_MONITORS 16

/*!
 * One OS monitor as SR's monitor enumeration reports it (SrMonitorDescriptor,
 * SR_TYPE 31), reduced to what claiming needs. Plain C, like
 * @ref leia_win_sr_display.
 */
struct leia_win_sr_monitor
{
	//! The SR display this monitor is (SrDisplayDescriptor displayId), 0 =
	//! SR says it is not an SR display.
	uint64_t sr_display_id;
	uint64_t hmonitor;    //!< HMONITOR (SR's `platformHandle`), the join key
	char device_name[32]; //!< GDI device name (SR's `connector`), may be empty

	//! Desktop rect in device px, desktop coordinates.
	int32_t x, y;
	int32_t width, height;
	uint32_t native_w, native_h; //!< preferred mode, 0 = unknown
	bool is_primary;

	//! Raw little-endian EDID words (bytes 8-9 / 10-11) — the same encoding
	//! as xrt_display_descriptor's edid_manufacturer / edid_product. 0 =
	//! unknown.
	uint16_t edid_manufacturer, edid_product;
	uint32_t edid_serial; //!< EDID bytes 12-15; 0 = none
};

/*!
 * Pack a 3-letter EDID PNP vendor id ("ACR") into the little-endian
 * manufacturer word the frozen table uses (ACR -> 29188).
 * @return 0 when @p pnp is not three letters A-Z.
 */
uint16_t
leia_win_pnp_to_manufacturer_id(const char *pnp);

//! Everything probe_displays knows besides the runtime's monitor list.
struct leia_win_claim_inputs
{
	//! New-API enumeration; @ref sr_display_count < 0 = the API is not
	//! available (compiled out, runtime predates it, or no SR instance).
	const struct leia_win_sr_display *sr_displays;
	int32_t sr_display_count;

	//! Frozen-table membership test (leia_edid_table_contains) — injected so
	//! the test can substitute its own table.
	bool (*table_contains)(uint16_t manufacturer_id, uint16_t product_id);

	//! Older SR only (@ref sr_display_count < 0): the platform state is
	//! READY, so a table-known panel is claimed VERIFIED, as today. False =
	//! claim at EDID confidence.
	bool legacy_table_verified;

	uint32_t supported_apis; //!< XRT_DP_API_BIT_* for every claim

	//! SR's own monitor list (srEnumerateMonitors); @ref sr_monitor_count < 0
	//! = unavailable (compiled out, SR runtime predates it, no instance) ->
	//! every monitor takes the EDID/origin heuristic, exactly as before.
	const struct leia_win_sr_monitor *sr_monitors;
	int32_t sr_monitor_count;

	//! Resolve a runtime descriptor's desktop origin to its HMONITOR in this
	//! process (the runtime derived the origin from that HMONITOR's rect, in
	//! the same process and DPI context). 0 = none. NULL = compare the origin
	//! with SR's desktop rect instead. Injected so the test runs without
	//! Win32 (@ref leia_win_monitor_at is the real one).
	uint64_t (*monitor_at)(int32_t left, int32_t top);
};

/*! What a claim is bound to — the plug-in-private monitor table (M5/M6 bind
 *  each DP's SR display/weaver/lens/tracker by @ref sr_display_id). */
struct leia_win_claim_binding
{
	uint64_t monitor_id;
	uint64_t sr_display_id; //!< 0 = none known (older SR, or SR does not list it)
	uint64_t hmonitor;      //!< SR's HMONITOR for the matched display, 0 = none
	char device_name[32];   //!< GDI device name of the matched SR display, may be empty

	//! True when SR's own monitor join bound this claim (no heuristic).
	bool sr_joined;
	//! The matched SR display's calibrated size and native mode, captured at
	//! claim time so get_display_info_for_monitor needs no re-enumeration.
	//! 0 = SR did not say (or nothing matched).
	uint32_t width_mm, height_mm;
	uint32_t native_w, native_h;
};

/*!
 * Compute claims for @p display_count runtime descriptors. Descriptors are
 * walked with the runtime's own stride (`displays[0].struct_size`) and never
 * read past it. @p out_bindings (may be NULL) is filled in step with
 * @p out_claims.
 *
 * Matching rules:
 *  - SR's own join first (@ref leia_win_claim_inputs::sr_monitor_count >= 0).
 *    A runtime monitor is looked up in SR's monitor list by HMONITOR
 *    (@ref leia_win_claim_inputs::monitor_at on the descriptor's origin), or,
 *    with no resolver / no HMONITOR, by desktop origin == SR's monitor rect
 *    origin. A row whose EDID ids contradict the descriptor's (both known,
 *    different) is treated as absent (topology changed between the two
 *    enumerations). When the row is found:
 *      - its SR display id is in SR's display list -> bound to that SR
 *        display, period: no EDID/origin matching, never AMBIGUOUS (two
 *        identical panels are told apart by SR itself). Confidence as below;
 *      - its SR display id is 0 -> SR says this monitor is not an SR display:
 *        no binding; claimed at EDID confidence only on a frozen-table hit
 *        (the "table knows it, SR does not list it" case below);
 *      - its SR display id is not in SR's display list (the two lists are
 *        cached separately) -> treated as absent.
 *    Joined SR displays are taken before the heuristic runs, so it can never
 *    hand one to a second monitor.
 *  - Monitors absent from SR's list, or every monitor when the list is
 *    unavailable, take the heuristic below — unchanged, so with the list
 *    unavailable the result is byte-identical to before.
 *  - Runtime monitor -> SR display (new API): same (manufacturer, product)
 *    AND, when SR marks its location desktop-global, the same desktop origin;
 *    else (manufacturer, product) when that pair is unique in SR's list. Two
 *    SR displays with these ids and no origin to tell them apart are
 *    AMBIGUOUS: the monitor is claimed (it IS a Leia panel) at EDID
 *    confidence with no serial and no displayId — never someone else's
 *    identity.
 *  - A runtime monitor the frozen table does not know but SR lists is still
 *    claimed: SR's product-code registry is authoritative and the table
 *    drifts (replaces the "claim the primary monitor" deferral with a real
 *    per-monitor answer).
 *  - Confidence (new API): VERIFIED when SR reports the display
 *    FPC_VERIFIED (serial = FPC serial), else EDID.
 *  - Older SR (@ref leia_win_claim_inputs::sr_display_count < 0): table hit
 *    -> VERIFIED when @ref leia_win_claim_inputs::legacy_table_verified, else
 *    EDID; serial empty. Table miss -> no claim (the caller keeps its
 *    primary-monitor deferral for that case).
 *
 * @return the number of claims written (<= @p max_claims).
 */
uint32_t
leia_win_compute_claims(const struct xrt_display_descriptor *displays,
                        uint32_t display_count,
                        const struct leia_win_claim_inputs *in,
                        struct xrt_display_claim *out_claims,
                        struct leia_win_claim_binding *out_bindings,
                        uint32_t max_claims);

/*!
 * Replace the plug-in-private monitor table with the latest probe's bindings
 * (thread-safe). Called by probe_displays; read by the per-DP binding work
 * (M5/M6 — nothing reads it yet).
 */
void
leia_win_claims_store(const struct leia_win_claim_binding *bindings, uint32_t count);

//! Look up a claimed monitor's binding; false when it was not claimed.
bool
leia_win_claims_lookup(uint64_t monitor_id, struct leia_win_claim_binding *out);


/*
 *
 * SR side (implemented in leia_sr_v2_common.cpp when the SDK declares
 * srEnumerateDisplays; stubs in leia_display_claims_win.c otherwise).
 *
 */

/*!
 * New SR API (srEnumerateDisplays): every SR display with its identity, FPC
 * confidence and opaque displayId. Uses a process-wide probe instance that is
 * created lazily (never initialised — no trackers start) and a short-TTL
 * cache, because the runtime re-runs probe_displays per registry refresh.
 *
 * @return the number of SR displays (<= @p cap written to @p out), or -1 when
 *         the API is unavailable: compiled out, the installed SR runtime
 *         predates it (SR_ERROR_FUNCTION_UNSUPPORTED), or no instance could
 *         be created (SR Service down).
 */
int32_t
leia_win_sr_enumerate_displays(struct leia_win_sr_display *out, uint32_t cap);

/*!
 * SR's monitor list (srEnumerateMonitors, Windows slot 108, LeiaSR
 * 1.38.0+2192): every active OS monitor and the SR display it is. Same
 * process-wide probe instance and short-TTL cache as
 * @ref leia_win_sr_enumerate_displays.
 *
 * @return the number of monitors (<= @p cap written to @p out), or -1 when
 *         unavailable: compiled out, the installed SR runtime predates it
 *         (SR_ERROR_FUNCTION_UNSUPPORTED), or no instance could be created.
 */
int32_t
leia_win_sr_enumerate_monitors(struct leia_win_sr_monitor *out, uint32_t cap);

/*!
 * The HMONITOR whose desktop rect contains (@p left, @p top) in this
 * process's coordinate space (MonitorFromPoint, no default), as a uint64_t;
 * 0 when none or not on Windows. The real
 * @ref leia_win_claim_inputs::monitor_at.
 */
uint64_t
leia_win_monitor_at(int32_t left, int32_t top);

//! Destroy the probe instance, if one exists. Plug-in destroy.
void
leia_win_sr_enumerate_shutdown(void);

#ifdef __cplusplus
}
#endif
