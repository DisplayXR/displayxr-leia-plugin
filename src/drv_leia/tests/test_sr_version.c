// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Host test: LeiaSR version parsing and the "update recommended" rule.
 */

#include "leia_sr_version.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

static void
check_parse(const char *s, bool ok, int a, int b, int c)
{
	int v[3];
	const bool got = leia_sr_version_parse(s, v);
	CHECK(got == ok);
	if (ok && got) {
		CHECK(v[0] == a && v[1] == b && v[2] == c);
	}
}

int
main(void)
{
	// Shapes the LeiaSR installers have written to DisplayVersion.
	check_parse("1.34.12", true, 1, 34, 12);
	check_parse("1.34.8", true, 1, 34, 8);
	check_parse("1.39.0+3665.92e0c8c02f", true, 1, 39, 0);
	check_parse("1.39.0.3665", true, 1, 39, 0);
	check_parse("1.38.5-RC1", true, 1, 38, 5);
	check_parse("1.39", true, 1, 39, 0);
	check_parse("v1.40.2", true, 1, 40, 2);
	check_parse("", false, 0, 0, 0);
	check_parse("1", false, 0, 0, 0);
	check_parse("abc", false, 0, 0, 0);
	check_parse(NULL, false, 0, 0, 0);

	// The rule: strictly older than 1.39.0 is flagged; unknown is not.
	CHECK(leia_sr_update_recommended("1.34.8"));
	CHECK(leia_sr_update_recommended("1.34.12"));
	CHECK(leia_sr_update_recommended("1.38.5"));
	CHECK(!leia_sr_update_recommended("1.39.0"));
	CHECK(!leia_sr_update_recommended("1.39.0+3665.92e0c8c02f"));
	CHECK(!leia_sr_update_recommended("1.39.1"));
	CHECK(!leia_sr_update_recommended("1.40.0"));
	CHECK(!leia_sr_update_recommended("2.0.0"));
	CHECK(!leia_sr_update_recommended(""));
	CHECK(!leia_sr_update_recommended("garbage"));
	CHECK(!leia_sr_update_recommended(NULL));

	// Non-Windows: never reports a version.
#ifndef _WIN32
	char buf[32] = "x";
	CHECK(!leia_sr_installed_version(buf, sizeof(buf)));
	CHECK(buf[0] == '\0');
#endif

	if (g_failures == 0) {
		printf("test_sr_version: all checks passed\n");
	}
	return g_failures == 0 ? 0 : 1;
}
