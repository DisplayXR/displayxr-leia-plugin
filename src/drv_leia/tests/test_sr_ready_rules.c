// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the SR-readiness rules (leia_sr_ready_rules.h): the
 *         weaver create retry's wake on identification (#468). Hardware-free
 *         and call-free: no SR, no Win32, no panel.
 */

#include "leia_sr_ready_rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

static void
test_wake(void)
{
	// Unidentified at the mark (the panel is still coming back): the count
	// edge wakes, no change does not.
	struct leiasr_identify_mark m = {false, 7};
	CHECK(leiasr_identify_woke(&m, false, 7) == LEIASR_IDENTIFY_WAKE_NONE);
	CHECK(leiasr_identify_woke(&m, true, 7) == LEIASR_IDENTIFY_WAKE_COUNT);

	// Identified at the mark (a failed attempt on an identified panel, or SR
	// keeping the count across an unplug): no edge to see, the wait runs its
	// period — unless verified geometry is published meanwhile.
	m.identified = true;
	CHECK(leiasr_identify_woke(&m, true, 7) == LEIASR_IDENTIFY_WAKE_NONE);
	CHECK(leiasr_identify_woke(&m, true, 8) == LEIASR_IDENTIFY_WAKE_PUBLISH);

	// A publish wins over the edge when both happen in one poll.
	m.identified = false;
	CHECK(leiasr_identify_woke(&m, true, 8) == LEIASR_IDENTIFY_WAKE_PUBLISH);

	// Wrap-around of the publish counter still reads as a change.
	m.publish_epoch = 0xFFFFFFFFu;
	CHECK(leiasr_identify_woke(&m, false, 0) == LEIASR_IDENTIFY_WAKE_PUBLISH);
}

int
main(void)
{
	test_wake();
	if (g_failures != 0) {
		fprintf(stderr, "test_sr_ready_rules: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	printf("test_sr_ready_rules: all passed\n");
	return EXIT_SUCCESS;
}
