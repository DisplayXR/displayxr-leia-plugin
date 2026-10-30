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

	//! SR lists the matched display FPC_VERIFIED (the claim is VERIFIED with
	//! @ref serial) — read back by get_screen_status, never re-queried.
	bool fpc_verified;
	char serial[64]; //!< FPC serial when @ref fpc_verified, else empty

        /*!
         * @name The claimed monitor as the RUNTIME enumerated it (#430)
         *
         * The descriptor's desktop rect, in this process's (the host's) DPI
         * space — the space get_display_info's `display_screen_left/top` is
         * read in by the runtime, which is why it is the descriptor's and not
         * SR's own rect. Lets get_display_info report the panel's position when
         * the frozen-EDID-table probe missed it but the claim found it.
         * @{
         */
        bool has_pos; //!< The descriptor carried a desktop position.
        int32_t screen_left, screen_top;
        uint32_t pixel_w,
            pixel_h; //!< The monitor's current mode (descriptor), 0 = unknown.
        //! The claim was made at XRT_DISPLAY_CLAIM_VERIFIED (FPC-verified, or
        //! the legacy table + READY rule) — not EDID-only, not AMBIGUOUS.
        bool verified;
        /*! @} */
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

/*!
 * Which claimed monitor is THE panel get_display_info describes, when the
 * frozen-EDID-table probe could not say (displayxr-leia-plugin#430: a
 * Lenovo 3D monitor the table lacks, on a non-main monitor, SR-verified by
 * the claims — the runtime then put the panel rect on the laptop screen).
 * Pure: no lock, no I/O.
 *
 * Only VERIFIED bindings with a desktop position count; an EDID-only or
 * AMBIGUOUS claim is not identity enough to move an app's window. One such
 * binding: it. Several: the one whose SR native mode (else the descriptor's
 * mode) is @p panel_w x @p panel_h, when exactly one is. Otherwise false —
 * no guess; the runtime then decides from its own claim registry.
 */
bool leia_win_claims_pick_panel(const struct leia_win_claim_binding *bindings,
                                uint32_t count, uint32_t panel_w,
                                uint32_t panel_h,
                                struct leia_win_claim_binding *out);

/*!
 * @ref leia_win_claims_pick_panel over the stored table (thread-safe).
 */
bool leia_win_claims_panel(uint32_t panel_w, uint32_t panel_h,
                           struct leia_win_claim_binding *out);

/*!
 * Look up the claimed monitor whose SR display carries FPC serial @p serial
 * (exact string match; only FPC-verified bindings carry one). The stereo
 * camera provider names a device's camera after its screen and binds its
 * keep-alive tracker to the display id this returns. False when no stored
 * claim has that serial, or @p serial is empty.
 */
bool
leia_win_claims_find_serial(const char *serial, struct leia_win_claim_binding *out);

/*!
 * Moves whenever leia_win_claims_store() stored a claim set that differs from
 * the previous one (monitor, SR display, FPC verification or serial) — a
 * topology / identity event, never a poll. Feeds get_screen_status's
 * change_counter.
 */
uint64_t
leia_win_claims_generation(void);

/*!
 * #480: the SR display a screen-bound weaver should bind on this create attempt.
 *
 * SR re-keys a panel when its FPC pairs: an EDID_ONLY displayId is a hash until
 * then. A per-screen DP made while SR knew its panel by EDID only keeps an id SR
 * drops once the FPC authenticates, and every srCreateDisplay with it fails
 * (SR_ERROR_DISPLAY_NOT_FOUND) until the app restarts. So the weaver re-resolves
 * its id before each attempt instead of remembering it:
 * - @p bound while SR still lists it, or when SR's list is unavailable;
 * - else the SR display of the monitor at the DP's desktop origin (SR's own
 *   monitor join, srEnumerateMonitors);
 * - else the SR display whose desktop-global location is that origin;
 * - else @p bound: nothing better is known, and the caller retries as before.
 *
 * The origin alone could pick another Leia panel moved there before the runtime
 * rebuilt its screens. So when the panel's EDID is known (@p edid_manufacturer
 * non-zero, noted by @ref leia_win_sr_note_display_edid while SR listed the
 * bound id), the monitor's or display's EDID manufacturer and product must match
 * too; a monitor or display with no EDID then never matches.
 *
 * Pure: the caller passes SR's enumerations. A count < 0 means that
 * enumeration is unavailable.
 */
uint64_t
leia_win_sr_rebind_display_id(uint64_t bound,
                              bool has_origin,
                              int32_t left,
                              int32_t top,
                              uint16_t edid_manufacturer,
                              uint16_t edid_product,
                              const struct leia_win_sr_display *displays,
                              int32_t display_count,
                              const struct leia_win_sr_monitor *monitors,
                              int32_t monitor_count);

//! #480: does SR's display list (@p count < 0 = unavailable) hold @p id?
bool
leia_win_sr_display_listed(uint64_t id, const struct leia_win_sr_display *displays, int32_t count);

/*!
 * #480: while SR lists @p id, note its EDID manufacturer and product into
 * @p io_manufacturer / @p io_product, for @ref leia_win_sr_rebind_display_id
 * once SR has re-keyed the panel. Leaves them as they were when SR does not
 * list @p id or gives no EDID.
 */
void
leia_win_sr_note_display_edid(uint64_t id,
                              const struct leia_win_sr_display *displays,
                              int32_t count,
                              uint16_t *io_manufacturer,
                              uint16_t *io_product);


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

/*!
 * #480: the next @ref leia_win_sr_enumerate_displays and
 * @ref leia_win_sr_enumerate_monitors re-read SR even inside the cache TTL.
 * A failed re-read keeps the cached list, as an expired one does. Any thread.
 */
void
leia_win_sr_enumerate_refresh_next(void);

/*!
 * runtime ADR-060 R2 (L4): does the installed SR runtime support EXTERNAL
 * weaver routing, as the process-wide probe instance's capability query
 * reported it (SrWeaverRoutingCapabilities::externalRouting)? Answered from the
 * probe made by @ref leia_win_sr_enumerate_displays — false until that has run,
 * when it is compiled out, or when SR is older than routing. Cheap; any thread.
 */
bool
leia_win_sr_probe_external_routing(void);

/*!
 * #446: can the installed SR runtime weave for more than one viewer
 * (SrMultiViewerCapabilities::multiViewerWeave, chained on the runtime
 * capabilities in a query of its OWN, so an older runtime cannot cost any
 * other capability answer)? Runtime-wide: the D3D11 and Vulkan weavers honour
 * it, the D3D12 and GL weavers refuse the ON override and weave one viewer.
 *
 * Asked on the process-wide probe instance (created on demand if
 * @ref leia_win_sr_enumerate_displays has not run yet); the answer is latched
 * for the plug-in instance's lifetime once SR has given one. False when
 * compiled out (SDK predates the feature), when the SR runtime predates it
 * (it leaves the field untouched), or when no probe instance can be created
 * right now (not latched, so a later device creation asks again).
 * Any thread; may block for the probe instance's short create spin.
 */
bool
leia_win_sr_multi_viewer_weave_supported(void);

#ifdef __cplusplus
}
#endif
