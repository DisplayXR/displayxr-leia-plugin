// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Per-monitor display claims for the Linux arm (`probe_displays`,
 *         multi-screen plan M0) — pure matching logic, no I/O.
 *
 * Three inputs are joined here:
 *   1. the runtime's monitor list (`xrt_display_descriptor[]`),
 *   2. the Leia panels this box's EDID scan found
 *      (leia_edid_probe_linux.h — connector, EDID ids + serial, px, RandR
 *      origin),
 *   3. what the SR runtime says, in one of two shapes:
 *        - new SR API (srEnumerateDisplays, LeiaSR line 876620d62+): one
 *          @ref leia_lnx_sr_display per SR display, with FPC confidence,
 *          serial and the opaque displayId the later binding (SR-P2) needs;
 *        - SR 1.38: only "an SR context exists and the lens reports an FPC
 *          serial" — system-global, so it can verify at most ONE panel.
 *
 * Kept free of SDK, Vulkan and sysfs so the unit test drives it with
 * fixtures (tests/test_display_claims_linux.c).
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#pragma once

#include "xrt/xrt_plugin.h"

#include "leia_edid_probe_linux.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Upper bound on SR displays one enumeration is read into.
#define LEIA_LNX_SR_MAX_DISPLAYS 8

/*!
 * One SR display as the new SR API reports it (SrDisplayDescriptor, SR_TYPE
 * 25), reduced to what claiming and binding need. Plain C so the claim logic
 * and its test never see SR headers.
 */
struct leia_lnx_sr_display
{
	uint64_t display_id; //!< opaque SR key for SrDisplayBindingInfo (SR-P2)
	bool fpc_verified;   //!< confidence == SR_DISPLAY_CONFIDENCE_FPC_VERIFIED
	char serial[64];     //!< FPC serial when @ref fpc_verified, else empty
	char product_code[4];

	uint16_t manufacturer_id; //!< EDID PNP id packed like EDID bytes 8-9 (LE word)
	uint16_t product_id;      //!< EDID bytes 10-11
	uint32_t edid_serial;     //!< EDID bytes 12-15; 0 = none
	char connector[32];       //!< DRM connector ("HDMI-A-1"), may be empty
	char output_name[32];     //!< RandR output, when SR proved it is device px

	uint32_t native_w, native_h;
	float refresh_hz;
};

/*!
 * Pack a 3-letter EDID PNP vendor id ("ACR") into the little-endian
 * manufacturer word the frozen table uses (ACR -> 29188).
 * @return 0 when @p pnp is not three letters A-Z.
 */
uint16_t
leia_lnx_pnp_to_manufacturer_id(const char *pnp);

//! Everything probe_displays knows besides the runtime's monitor list.
struct leia_lnx_claim_inputs
{
	const struct leia_lnx_edid_panel *panels;
	uint32_t panel_count;

	//! New-API enumeration; @ref sr_display_count < 0 = the API is not
	//! available (compiled out, runtime predates it, or no SR context).
	const struct leia_lnx_sr_display *sr_displays;
	int32_t sr_display_count;

	//! SR 1.38 evidence, used only when @ref sr_display_count < 0: the FPC
	//! serial of the device the live SR context drives, or NULL/empty.
	const char *legacy_fpc_serial;

	uint32_t supported_apis; //!< XRT_DP_API_BIT_* for every claim
};

/*! What a claim is bound to — the plug-in-private monitor table (M4/M5 bind
 *  each DP's SR display/weaver/lens/tracker by @ref sr_display_id). */
struct leia_lnx_claim_binding
{
	uint64_t monitor_id;
	uint64_t sr_display_id; //!< 0 = none known (SR 1.38, or SR does not list it)
	char connector[32];     //!< DRM connector of the matched panel, may be empty
};

/*!
 * Compute claims for @p display_count runtime descriptors. Descriptors are
 * walked with the runtime's own stride (`displays[0].struct_size`) and never
 * read past it. @p out_bindings (may be NULL) is filled in step with
 * @p out_claims.
 * @return the number of claims written (<= @p max_claims).
 */
uint32_t
leia_lnx_compute_claims(const struct xrt_display_descriptor *displays,
                        uint32_t display_count,
                        const struct leia_lnx_claim_inputs *in,
                        struct xrt_display_claim *out_claims,
                        struct leia_lnx_claim_binding *out_bindings,
                        uint32_t max_claims);

/*!
 * The claim to make when probe() BOUND the plug-in but
 * leia_lnx_compute_claims() matched no monitor (DXR_LEIA_FORCE_PROBE=1, or a
 * descriptor the matcher cannot pair with the panel — never an unplug, see
 * @ref leia_lnx_unmatched). A plug-in that
 * implements probe_displays no longer gets the runtime's fallback claim, so
 * without this the bound plug-in owns no monitor and another plug-in wins
 * the panel. One EDID-confidence claim (no serial, no SR display), on:
 *   1. the first descriptor whose pixel size is a panel's (native mode or
 *      RandR CRTC; panels in connector order) — the bound panel;
 *   2. else the descriptor flagged primary (flags bit 0);
 *   3. else the first descriptor.
 * @return false only when @p display_count is 0.
 */
bool
leia_lnx_fallback_claim(const struct xrt_display_descriptor *displays,
                        uint32_t display_count,
                        const struct leia_lnx_edid_panel *panels,
                        uint32_t panel_count,
                        uint32_t supported_apis,
                        struct xrt_display_claim *out_claim,
                        struct leia_lnx_claim_binding *out_binding);

/*!
 * What earlier passes saw of the panel — sticky for the process, the Linux twin
 * of the Windows arm's `g_hit_seen` (leia_platform_state.c, P-c). The caller
 * owns it and serialises access (probe_displays runs on the runtime's hotplug
 * worker as well as the main thread).
 */
struct leia_lnx_panel_history
{
	//! A Leia panel was found by EDID: probe() bound on one, or a pass's panel
	//! scan listed one. A forced probe (DXR_LEIA_FORCE_PROBE=1) sets nothing.
	bool seen;
	//! A pass paired a Leia panel with a runtime monitor (leia_lnx_compute_claims).
	bool paired;
	//! The @ref LEIA_LNX_UNMATCHED_NOT_LISTED wait is running: since
	//! @ref unlisted_since_ns the panel has been connected, paired before, and
	//! paired with no listed monitor. Stops when it pairs again or goes away.
	bool unlisted;
	int64_t unlisted_since_ns; //!< monotonic; valid only while @ref unlisted
};

/*!
 * How long a panel that paired before may stay connected with no listed monitor
 * pairing with it before probe_displays falls back as for a probe-time miss.
 * A replug the monitor list has not caught up with clears within a sweep or
 * two: the runtime polls every 500 ms (DXR_DISPLAY_HOTPLUG_POLL_MS) and re-runs
 * follow-up sweeps 0.5, 1, 1.5, 2, 3 and 4 s after a monitor change (then 6, 8,
 * 12 s ...); on a DS1 the HDMI monitor was listed one follow-up after the panel
 * came back. Anything still unpaired after 5 s is a monitor the matcher cannot
 * pair — XWayland without EDID ids, an origin that does not line up, another
 * Leia panel swapped in.
 *
 * The bound is only checked when a probe pass runs: nothing here wakes up at
 * 5 s. With no further monitor change, it is the runtime's later follow-up
 * sweeps (6, 8, 12 s ... after the change; offs_ms in
 * displayxr-runtime target_display_hotplug.c) that run the pass which ends the
 * wait and makes the fallback claim. Removing or thinning those sweeps leaves
 * such a panel unclaimed until the next monitor change, so move this bound
 * into a timer first.
 */
#define LEIA_LNX_NOT_LISTED_MAX_NS (5000ll * 1000ll * 1000ll)

//! What a pass with probe() bound does when no monitor matched a Leia panel.
enum leia_lnx_unmatched
{
	//! A monitor matched, or probe() did not bind: nothing to decide.
	LEIA_LNX_UNMATCHED_NONE = 0,
	//! The probe-time cases — a forced probe that never found a panel, or a
	//! connected panel whose monitor the matcher has never paired (or has not
	//! for @ref LEIA_LNX_NOT_LISTED_MAX_NS): one leia_lnx_fallback_claim()
	//! claim, as before.
	LEIA_LNX_UNMATCHED_FALLBACK,
	//! The panel EDID found is gone (unplugged): claim nothing, so the
	//! runtime keeps the last panel rect (displayxr-runtime#2305).
	LEIA_LNX_UNMATCHED_PANEL_GONE,
	//! The panel is connected and was paired before, but no monitor the
	//! runtime lists pairs with it now (a replug the monitor list has not
	//! caught up with yet): claim nothing, as for an unplug — for at most
	//! @ref LEIA_LNX_NOT_LISTED_MAX_NS, then FALLBACK.
	LEIA_LNX_UNMATCHED_NOT_LISTED,
};

/*!
 * The rule behind @ref leia_lnx_unmatched, for a pass whose claims matched
 * nothing with probe() bound. @p panel_count = Leia panels in this pass's
 * panel scan (table panels and panels the SR runtime drives). @p now_ns
 * (monotonic) is judged against @p history's NOT_LISTED wait; with no wait
 * running, this pass is its first.
 */
enum leia_lnx_unmatched
leia_lnx_unmatched_decide(const struct leia_lnx_panel_history *history, uint32_t panel_count, int64_t now_ns);

/*!
 * One probe_displays pass at @p now_ns (monotonic): leia_lnx_compute_claims(),
 * then — only when it matched nothing and @p probe_bound — the
 * @ref leia_lnx_unmatched rule, and the fallback claim when that rule says so.
 * Folds this pass into @p history (and starts or stops its NOT_LISTED wait).
 * @p out_unmatched (may be NULL) receives the rule's answer, NONE when a
 * monitor matched or probe() did not bind.
 * @return the number of claims written (<= @p max_claims).
 */
uint32_t
leia_lnx_claims_pass(const struct xrt_display_descriptor *displays,
                     uint32_t display_count,
                     const struct leia_lnx_claim_inputs *in,
                     bool probe_bound,
                     int64_t now_ns,
                     struct leia_lnx_panel_history *history,
                     struct xrt_display_claim *out_claims,
                     struct leia_lnx_claim_binding *out_bindings,
                     uint32_t max_claims,
                     enum leia_lnx_unmatched *out_unmatched);

/*!
 * Replace the plug-in-private monitor table with the latest probe's bindings
 * (thread-safe). Called by probe_displays; read by the per-DP binding work
 * (M4/M5 — nothing reads it yet).
 */
void
leia_lnx_claims_store(const struct leia_lnx_claim_binding *bindings, uint32_t count);

//! Look up a claimed monitor's binding; false when it was not claimed.
bool
leia_lnx_claims_lookup(uint64_t monitor_id, struct leia_lnx_claim_binding *out);

/*
 * SR service device store (the legacy FPC-serial source).
 *
 * On LeiaSR runtimes without srEnumerateDisplays (the 1.38 line), the FPC
 * serial of the device the SR service drives is NOT available through the API
 * (srLensGetSerialNumber is a stub on Linux: SR_ERROR_FEATURE_NOT_SUPPORTED).
 * The service records it in its device store instead: `<root>/active` is a
 * symlink to `Devices/<serial>`, exactly what the SR runtime's own
 * resolveLinuxPrimaryDeviceSerial reads. <root> = $XDG_CACHE_HOME/leiasr — the
 * service runs with XDG_CACHE_HOME=/var/lib/leiasr under systemd, so on a box
 * with the .deb it is /var/lib/leiasr/leiasr. The serial is system-wide, not
 * tied to a panel: the claim logic VERIFIES a monitor with it only when
 * exactly one Leia panel is connected (leia_lnx_compute_claims).
 */

//! Default store link when $XDG_CACHE_HOME does not lead to one.
#define LEIA_LNX_SR_DEVICE_STORE_ACTIVE "/var/lib/leiasr/leiasr/active"

/*!
 * Read one device-store `active` link: readlink, require the target to be
 * `Devices/<serial>` (relative, or absolute ending so) and to exist as a
 * directory (a dangling link = no active device), and return the basename.
 * The serial must be 1..cap-1 chars of [A-Za-z0-9._-].
 * @return true and the serial in @p out, else false and "" in @p out.
 */
bool
leia_lnx_sr_device_store_serial_at(const char *active_link, char *out, size_t cap);

/*!
 * The active device's FPC serial from the SR service's device store:
 * `$XDG_CACHE_HOME/leiasr/active` when XDG_CACHE_HOME is set and that link
 * resolves, else LEIA_LNX_SR_DEVICE_STORE_ACTIVE. @p out_source (may be NULL)
 * receives the link path that answered. Cheap (one or two readlink + stat),
 * no SR call, no SR context.
 */
bool
leia_lnx_sr_device_store_serial(char *out, size_t cap, char *out_source, size_t source_cap);

#ifdef __cplusplus
}
#endif
