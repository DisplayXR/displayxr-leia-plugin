// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Host test: SR display readiness (EDID_ONLY = present, not ready) and
 *         the EDID_ONLY -> FPC_VERIFIED edge that asks the runtime for a sweep.
 */

#include "leia_sr_readiness.h"

#include <stdio.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(x)                                                                                                       \
	do {                                                                                                           \
		if (!(x)) {                                                                                            \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                   \
			g_fail++;                                                                                      \
		}                                                                                                      \
	} while (0)

int
main(void)
{
	const struct leia_sr_readiness_entry edid_only = {0x0D55, true, false};
	const struct leia_sr_readiness_entry verified = {0x0D55, true, true};
	const struct leia_sr_readiness_entry legacy = {0, false, false};

	// Readiness.
	CHECK(!leia_sr_display_ready(&edid_only, false));
	CHECK(leia_sr_display_ready(&verified, false));
	CHECK(leia_sr_display_ready(&legacy, false));   // no enumeration = no authentication notion
	CHECK(leia_sr_display_ready(&edid_only, true)); // DXR_LEIA_ACCEPT_EDID_ONLY=1
	CHECK(!leia_sr_display_ready(NULL, true));

	// Pending (what keeps the plug-in's verification poll alive).
	const struct leia_sr_readiness_entry two[] = {{1, true, false}, {2, true, true}};
	CHECK(leia_sr_pending_count(two, 2, false) == 1);
	CHECK(leia_sr_pending_count(two, 2, true) == 0);
	CHECK(leia_sr_pending_count(NULL, 0, false) == 0);

	// The edge: plugged (nothing) -> EDID_ONLY: present, NOT newly ready.
	CHECK(!leia_sr_newly_ready(NULL, 0, &edid_only, 1, false));
	// EDID_ONLY -> FPC_VERIFIED: newly ready (the sweep request).
	CHECK(leia_sr_newly_ready(&edid_only, 1, &verified, 1, false));
	// Verified stays verified: no new edge (no request storm from the poll).
	CHECK(!leia_sr_newly_ready(&verified, 1, &verified, 1, false));
	// Straight to FPC_VERIFIED (fast authentication): an edge too.
	CHECK(leia_sr_newly_ready(NULL, 0, &verified, 1, false));
	// Unplug: nothing ready now, no edge.
	CHECK(!leia_sr_newly_ready(&verified, 1, NULL, 0, false));
	// A second panel authenticating while the first stays verified.
	const struct leia_sr_readiness_entry before2[] = {{1, true, true}, {2, true, false}};
	const struct leia_sr_readiness_entry after2[] = {{1, true, true}, {2, true, true}};
	CHECK(leia_sr_newly_ready(before2, 2, after2, 2, false));
	// Accepting EDID_ONLY: the panel is ready at EDID_ONLY already.
	CHECK(leia_sr_newly_ready(NULL, 0, &edid_only, 1, true));
	CHECK(!leia_sr_newly_ready(&edid_only, 1, &verified, 1, true));

	// The claim: verified -> READY; EDID_ONLY -> PRESENT_NOT_READY (flat 2D)
	// where the runtime knows the level, nothing on an older runtime.
	CHECK(leia_sr_claim_kind(&verified, false, true) == LEIA_SR_CLAIM_READY);
	CHECK(leia_sr_claim_kind(&edid_only, false, true) == LEIA_SR_CLAIM_PRESENT_NOT_READY);
	CHECK(leia_sr_claim_kind(&edid_only, false, false) == LEIA_SR_CLAIM_NONE);
	CHECK(leia_sr_claim_kind(&edid_only, true, true) == LEIA_SR_CLAIM_READY);
	CHECK(leia_sr_claim_kind(&legacy, false, false) == LEIA_SR_CLAIM_READY);

	if (g_fail != 0) {
		fprintf(stderr, "%d failure(s)\n", g_fail);
		return 1;
	}
	printf("test_sr_readiness: all passed\n");
	return 0;
}
