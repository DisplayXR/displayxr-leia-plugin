// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  When an SR display is READY to be claimed (macOS arm).
 *
 * srEnumerateDisplays lists an SR monitor as soon as its EDID matches, at
 * SR_DISPLAY_CONFIDENCE_EDID_ONLY and with no serial — BEFORE the SR service
 * has authenticated the panel's FPC. It turns FPC_VERIFIED (with the serial)
 * only once the service has authenticated it and published
 * devices/<serial>/primary; the service retries a failed authentication with a
 * backoff from 1 s up to 30 s, so that can take a while. An EDID_ONLY entry is
 * "present, not ready": the plug-in does not bind (probe), claim
 * (probe_displays) or make a DP for it (create_dp_metal_for_screen), so no
 * weaver and no lens touch a panel the service does not vouch for. The same
 * race exists at instance create and gets the same answer: the plug-in
 * declines, sim-display stands in, and the EDID_ONLY -> FPC_VERIFIED transition
 * asks the runtime for a claim sweep.
 *
 * A legacy SR runtime (no srEnumerateDisplays) identifies its one panel through
 * the EDID table and has no notion of authentication: always ready.
 * `DXR_LEIA_ACCEPT_EDID_ONLY=1` restores "EDID_ONLY binds" (bring-up).
 *
 * Pure C, host-tested by test_sr_readiness.c.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! One enumerated SR display, as readiness sees it.
struct leia_sr_readiness_entry
{
	uint64_t sr_display_id;
	//! Came from srEnumerateDisplays (not the legacy EDID table).
	bool from_enumeration;
	bool fpc_verified; //!< SR_DISPLAY_CONFIDENCE_FPC_VERIFIED
};

//! May the plug-in claim / bind / weave this display?
static inline bool
leia_sr_display_ready(const struct leia_sr_readiness_entry *e, bool accept_edid_only)
{
	return e != 0 && (!e->from_enumeration || e->fpc_verified || accept_edid_only);
}

//! What the plug-in claims for a listed SR display.
enum leia_sr_claim_kind
{
	LEIA_SR_CLAIM_READY = 0,             //!< VERIFIED (or EDID on a legacy runtime): weave it.
	LEIA_SR_CLAIM_PRESENT_NOT_READY = 1, //!< ours, not authenticated: the runtime shows it flat 2D.
	LEIA_SR_CLAIM_NONE = 2,              //!< nothing (a runtime without the not-ready level).
};

/*!
 * The claim for @p e: READY when it is ready; otherwise PRESENT_NOT_READY
 * when the runtime understands that level (@p runtime_has_not_ready, i.e.
 * XRT_PLUGIN_HAS_CLAIM_PRESENT_NOT_READY), else NONE — an older runtime would
 * read a low claim as "weave it", and the panel would get a fallback weave.
 */
static inline enum leia_sr_claim_kind
leia_sr_claim_kind(const struct leia_sr_readiness_entry *e, bool accept_edid_only, bool runtime_has_not_ready)
{
	if (leia_sr_display_ready(e, accept_edid_only)) {
		return LEIA_SR_CLAIM_READY;
	}
	return runtime_has_not_ready ? LEIA_SR_CLAIM_PRESENT_NOT_READY : LEIA_SR_CLAIM_NONE;
}

//! How many listed displays are present but not ready (EDID_ONLY, awaiting
//! authentication).
uint32_t
leia_sr_pending_count(const struct leia_sr_readiness_entry *cur, uint32_t n, bool accept_edid_only);

/*!
 * Did a display become ready between @p prev and @p cur — one that is ready
 * now and was not ready (or not listed) before? This is the edge on which
 * the plug-in asks the runtime for a claim sweep.
 */
bool
leia_sr_newly_ready(const struct leia_sr_readiness_entry *prev,
                    uint32_t np,
                    const struct leia_sr_readiness_entry *cur,
                    uint32_t nc,
                    bool accept_edid_only);

#ifdef __cplusplus
}
#endif
