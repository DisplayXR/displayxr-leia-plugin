// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the Linux camera-rest rules (leia_tracking_policy_linux.h).
 *
 * No SDK, no Vulkan, no panel: DXR_LEIA_TRACKING_POLICY parsing and the
 * is_tracking decision, checked on any box.
 */

#include "leia_tracking_policy_linux.h"

#include <stdio.h>
#include <stdlib.h>

static int g_failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

static void
test_mode_parse(void)
{
	CHECK(leia_lnx_tracking_mode_parse(NULL) == LEIA_LNX_TRACKING_2D_START);
	CHECK(leia_lnx_tracking_mode_parse("") == LEIA_LNX_TRACKING_2D_START);
	CHECK(leia_lnx_tracking_mode_parse("auto") == LEIA_LNX_TRACKING_SR_DEFAULT);
	CHECK(leia_lnx_tracking_mode_parse("AUTO") == LEIA_LNX_TRACKING_SR_DEFAULT);
	CHECK(leia_lnx_tracking_mode_parse("always_on") == LEIA_LNX_TRACKING_ALWAYS_ON);
	CHECK(leia_lnx_tracking_mode_parse("on") == LEIA_LNX_TRACKING_ALWAYS_ON);
	CHECK(leia_lnx_tracking_mode_parse("Off") == LEIA_LNX_TRACKING_OFF);
	CHECK(leia_lnx_tracking_mode_parse("sometimes") == LEIA_LNX_TRACKING_UNRECOGNISED);
}

static void
test_weaver_answer_decides(void)
{
	// The gap this closes: presence or a fresh sample would call a rested
	// weaver's collapsed pair a tracked viewer.
	CHECK(!leia_lnx_is_tracking(LEIA_LNX_WEAVER_TRACKING_RESTED, true, true, true, true));
	CHECK(!leia_lnx_is_tracking(LEIA_LNX_WEAVER_TRACKING_LOST, true, true, true, true));
	// TRACKING needs no tracker of our own (it was destroyed) and no USER_FOUND
	// (a viewer present at connect is never announced).
	CHECK(leia_lnx_is_tracking(LEIA_LNX_WEAVER_TRACKING_TRACKING, false, false, true, false));
	// A device that is not ready tracks nobody, whatever anyone says.
	CHECK(!leia_lnx_is_tracking(LEIA_LNX_WEAVER_TRACKING_TRACKING, true, true, false, true));
}

static void
test_unknown_is_todays_heuristic(void)
{
	// An SR runtime without the query: exactly the pre-#846 rule.
	CHECK(leia_lnx_is_tracking(LEIA_LNX_WEAVER_TRACKING_UNKNOWN, true, false, true, true));
	CHECK(leia_lnx_is_tracking(LEIA_LNX_WEAVER_TRACKING_UNKNOWN, false, true, true, true));
	CHECK(!leia_lnx_is_tracking(LEIA_LNX_WEAVER_TRACKING_UNKNOWN, false, false, true, true));
	CHECK(!leia_lnx_is_tracking(LEIA_LNX_WEAVER_TRACKING_UNKNOWN, true, true, true, false));
	CHECK(!leia_lnx_is_tracking(LEIA_LNX_WEAVER_TRACKING_UNKNOWN, true, true, false, true));
}

int
main(void)
{
	test_mode_parse();
	test_weaver_answer_decides();
	test_unknown_is_todays_heuristic();
	if (g_failures != 0) {
		fprintf(stderr, "test_tracking_policy_linux: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	printf("test_tracking_policy_linux: all checks passed\n");
	return EXIT_SUCCESS;
}
