// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Host test: the MULTI_VIEWER setting's parse and resolve rules (#446)
 *         and the rules of the "LeiaSR multi-viewer" 3D entry.
 */

#include "leia_multi_viewer.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

static void
check_parse(const char *s, bool ok, enum leia_multi_viewer want)
{
	enum leia_multi_viewer v = LEIA_MULTI_VIEWER_ON; // poisoned: parse must always write
	const bool got = leia_multi_viewer_parse(s, &v);
	CHECK(got == ok);
	CHECK(v == want);
}

int
main(void)
{
	// The three words, any case, surrounding blanks ignored.
	check_parse("system", true, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("on", true, LEIA_MULTI_VIEWER_ON);
	check_parse("off", true, LEIA_MULTI_VIEWER_OFF);
	check_parse("ON", true, LEIA_MULTI_VIEWER_ON);
	check_parse("System", true, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("  off\r\n", true, LEIA_MULTI_VIEWER_OFF);
	check_parse("\ton ", true, LEIA_MULTI_VIEWER_ON);

	// Unset: false, default.
	check_parse(NULL, false, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("", false, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("   ", false, LEIA_MULTI_VIEWER_SYSTEM);

	// Anything else: false, default (the caller warns). No prefixes, no
	// numbers, no near-misses.
	check_parse("1", false, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("0", false, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("o", false, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("onn", false, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("of", false, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("sys", false, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("systems", false, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("o n", false, LEIA_MULTI_VIEWER_SYSTEM);
	check_parse("aware", false, LEIA_MULTI_VIEWER_SYSTEM);

	// Resolve: supported => the setting; unsupported => ignored (system).
	CHECK(leia_multi_viewer_effective(LEIA_MULTI_VIEWER_SYSTEM, true) == LEIA_MULTI_VIEWER_SYSTEM);
	CHECK(leia_multi_viewer_effective(LEIA_MULTI_VIEWER_ON, true) == LEIA_MULTI_VIEWER_ON);
	CHECK(leia_multi_viewer_effective(LEIA_MULTI_VIEWER_OFF, true) == LEIA_MULTI_VIEWER_OFF);
	CHECK(leia_multi_viewer_effective(LEIA_MULTI_VIEWER_SYSTEM, false) == LEIA_MULTI_VIEWER_SYSTEM);
	CHECK(leia_multi_viewer_effective(LEIA_MULTI_VIEWER_ON, false) == LEIA_MULTI_VIEWER_SYSTEM);
	CHECK(leia_multi_viewer_effective(LEIA_MULTI_VIEWER_OFF, false) == LEIA_MULTI_VIEWER_SYSTEM);
	CHECK(leia_multi_viewer_effective((enum leia_multi_viewer)7, true) == LEIA_MULTI_VIEWER_SYSTEM);

	// Names round-trip through the parser; the default word is "system".
	CHECK(strcmp(leia_multi_viewer_name(LEIA_MULTI_VIEWER_SYSTEM), "system") == 0);
	CHECK(strcmp(leia_multi_viewer_name(LEIA_MULTI_VIEWER_ON), "on") == 0);
	CHECK(strcmp(leia_multi_viewer_name(LEIA_MULTI_VIEWER_OFF), "off") == 0);
	CHECK(strcmp(leia_multi_viewer_name((enum leia_multi_viewer)7), "?") == 0);
	for (int i = 0; i <= 2; i++) {
		enum leia_multi_viewer v = LEIA_MULTI_VIEWER_SYSTEM;
		CHECK(leia_multi_viewer_parse(leia_multi_viewer_name((enum leia_multi_viewer)i), &v));
		CHECK((int)v == i);
	}
	{
		enum leia_multi_viewer v = LEIA_MULTI_VIEWER_ON;
		CHECK(leia_multi_viewer_parse(LEIA_MULTI_VIEWER_DEFAULT, &v));
		CHECK(v == LEIA_MULTI_VIEWER_SYSTEM);
	}
	CHECK(strcmp(LEIA_MULTI_VIEWER_KEY, "MULTI_VIEWER") == 0);

	// --- The declared 3D entries (runtime ADR-066 C1) ---

	// Which entries are declared: the third one needs BOTH an SR runtime that
	// weaves for several viewers and runtime headers with the entry contract.
	CHECK(leia_multi_viewer_entry_count(true, true) == 3);
	CHECK(leia_multi_viewer_entry_count(true, false) == 2);
	CHECK(leia_multi_viewer_entry_count(false, true) == 2);
	CHECK(leia_multi_viewer_entry_count(false, false) == 2);
	CHECK(LEIA_MV_ENTRY_2D == 0 && LEIA_MV_ENTRY_3D == 1 && LEIA_MV_ENTRY_MULTI_VIEWER == 2);

	// The names (the runtime's choice names are their slugs).
	CHECK(strcmp(leia_multi_viewer_entry_name(0), "2D") == 0);
	CHECK(strcmp(leia_multi_viewer_entry_name(1), "LeiaSR") == 0);
	CHECK(strcmp(leia_multi_viewer_entry_name(2), "LeiaSR multi-viewer") == 0);
	CHECK(strcmp(leia_multi_viewer_entry_name(3), "?") == 0);

	// The default 3D entry: MULTI_VIEWER=on names the multi-viewer entry where
	// it is declared; everything else names "LeiaSR".
	CHECK(leia_multi_viewer_default_entry(LEIA_MULTI_VIEWER_ON, 3) == LEIA_MV_ENTRY_MULTI_VIEWER);
	CHECK(leia_multi_viewer_default_entry(LEIA_MULTI_VIEWER_SYSTEM, 3) == LEIA_MV_ENTRY_3D);
	CHECK(leia_multi_viewer_default_entry(LEIA_MULTI_VIEWER_OFF, 3) == LEIA_MV_ENTRY_3D);
	CHECK(leia_multi_viewer_default_entry(LEIA_MULTI_VIEWER_ON, 2) == LEIA_MV_ENTRY_3D);
	CHECK(leia_multi_viewer_default_entry(LEIA_MULTI_VIEWER_SYSTEM, 2) == LEIA_MV_ENTRY_3D);
	CHECK(leia_multi_viewer_default_entry(LEIA_MULTI_VIEWER_OFF, 2) == LEIA_MV_ENTRY_3D);
	CHECK(leia_multi_viewer_default_entry(LEIA_MULTI_VIEWER_ON, 0) == LEIA_MV_ENTRY_3D);
	CHECK(leia_multi_viewer_default_entry((enum leia_multi_viewer)7, 3) == LEIA_MV_ENTRY_3D);

	// The entry in effect: a 3D entry of the list replaces it; 2D and an index
	// outside the list keep it.
	CHECK(leia_multi_viewer_latch_entry(1, 2, 3) == 2);
	CHECK(leia_multi_viewer_latch_entry(2, 1, 3) == 1);
	CHECK(leia_multi_viewer_latch_entry(2, 0, 3) == 2);
	CHECK(leia_multi_viewer_latch_entry(1, 0, 3) == 1);
	CHECK(leia_multi_viewer_latch_entry(1, 3, 3) == 1);
	CHECK(leia_multi_viewer_latch_entry(0, 0, 3) == 0);
	CHECK(leia_multi_viewer_latch_entry(0, 2, 3) == 2);
	CHECK(leia_multi_viewer_latch_entry(1, 2, 2) == 1); // not declared: ignored
	CHECK(leia_multi_viewer_latch_entry(0, 1, 2) == 1);
	CHECK(leia_multi_viewer_latch_entry(1, 1, 0) == 1); // no list yet

	// What an entry weaves. Three entries: the multi-viewer entry is ON
	// whatever the setting; "LeiaSR" is the setting's system or off, and
	// follows the system with the setting on (which only names the default).
	for (int i = 0; i < 3; i++) {
		CHECK(leia_multi_viewer_for_entry(2, 3, (enum leia_multi_viewer)i) == LEIA_MULTI_VIEWER_ON);
	}
	CHECK(leia_multi_viewer_for_entry(1, 3, LEIA_MULTI_VIEWER_SYSTEM) == LEIA_MULTI_VIEWER_SYSTEM);
	CHECK(leia_multi_viewer_for_entry(1, 3, LEIA_MULTI_VIEWER_OFF) == LEIA_MULTI_VIEWER_OFF);
	CHECK(leia_multi_viewer_for_entry(1, 3, LEIA_MULTI_VIEWER_ON) == LEIA_MULTI_VIEWER_SYSTEM);
	CHECK(leia_multi_viewer_for_entry(0, 3, LEIA_MULTI_VIEWER_ON) == LEIA_MULTI_VIEWER_SYSTEM);
	CHECK(leia_multi_viewer_for_entry(9, 3, LEIA_MULTI_VIEWER_OFF) == LEIA_MULTI_VIEWER_OFF);
	CHECK(leia_multi_viewer_for_entry(1, 3, (enum leia_multi_viewer)7) == LEIA_MULTI_VIEWER_SYSTEM);
	// Two entries (no contract, or an SR runtime without the feature): the
	// setting is the only switch, exactly as before.
	for (uint32_t e = 0; e < 4; e++) {
		CHECK(leia_multi_viewer_for_entry(e, 2, LEIA_MULTI_VIEWER_SYSTEM) == LEIA_MULTI_VIEWER_SYSTEM);
		CHECK(leia_multi_viewer_for_entry(e, 2, LEIA_MULTI_VIEWER_ON) == LEIA_MULTI_VIEWER_ON);
		CHECK(leia_multi_viewer_for_entry(e, 2, LEIA_MULTI_VIEWER_OFF) == LEIA_MULTI_VIEWER_OFF);
		CHECK(leia_multi_viewer_for_entry(e, 0, LEIA_MULTI_VIEWER_ON) == LEIA_MULTI_VIEWER_ON);
	}
	CHECK(leia_multi_viewer_for_entry(1, 2, (enum leia_multi_viewer)7) == LEIA_MULTI_VIEWER_SYSTEM);

	// A live switch, end to end: default entry -> latched entry -> weaver value.
	{
		const uint32_t n = leia_multi_viewer_entry_count(true, true);
		uint32_t e = leia_multi_viewer_default_entry(LEIA_MULTI_VIEWER_SYSTEM, n);
		CHECK(leia_multi_viewer_for_entry(e, n, LEIA_MULTI_VIEWER_SYSTEM) == LEIA_MULTI_VIEWER_SYSTEM);
		e = leia_multi_viewer_latch_entry(e, LEIA_MV_ENTRY_MULTI_VIEWER, n); // the user picks multi-viewer
		CHECK(leia_multi_viewer_for_entry(e, n, LEIA_MULTI_VIEWER_SYSTEM) == LEIA_MULTI_VIEWER_ON);
		e = leia_multi_viewer_latch_entry(e, LEIA_MV_ENTRY_2D, n); // the app goes to 2D
		CHECK(leia_multi_viewer_for_entry(e, n, LEIA_MULTI_VIEWER_SYSTEM) == LEIA_MULTI_VIEWER_ON);
		e = leia_multi_viewer_latch_entry(e, LEIA_MV_ENTRY_3D, n); // and back to "LeiaSR"
		CHECK(leia_multi_viewer_for_entry(e, n, LEIA_MULTI_VIEWER_SYSTEM) == LEIA_MULTI_VIEWER_SYSTEM);
	}

	if (g_failures == 0) {
		printf("test_multi_viewer: all checks passed\n");
	}
	return g_failures == 0 ? 0 : 1;
}
