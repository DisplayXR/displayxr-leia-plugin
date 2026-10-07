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
 * descriptor the matcher cannot pair with the panel). A plug-in that
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
 * Replace the plug-in-private monitor table with the latest probe's bindings
 * (thread-safe). Called by probe_displays; read by the per-DP binding work
 * (M4/M5 — nothing reads it yet).
 */
void
leia_lnx_claims_store(const struct leia_lnx_claim_binding *bindings, uint32_t count);

//! Look up a claimed monitor's binding; false when it was not claimed.
bool
leia_lnx_claims_lookup(uint64_t monitor_id, struct leia_lnx_claim_binding *out);

#ifdef __cplusplus
}
#endif
