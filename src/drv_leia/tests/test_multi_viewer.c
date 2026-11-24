// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Host test: the MULTI_VIEWER setting's parse and resolve rules (#446).
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

	if (g_failures == 0) {
		printf("test_multi_viewer: all checks passed\n");
	}
	return g_failures == 0 ? 0 : 1;
}
