// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The MULTI_VIEWER setting's parse / resolve rules (#446). Plain C,
 *         no SR or runtime types; host-tested by tests/test_multi_viewer.c.
 * @ingroup drv_leia
 */

#include "leia_multi_viewer.h"

#include <stddef.h>

static bool
is_blank(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char
lower_ascii(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Does [s, s+n) equal the lower-case word @p w, ignoring case? */
static bool
word_eq(const char *s, size_t n, const char *w)
{
	size_t i = 0;
	for (; i < n; i++) {
		if (w[i] == '\0' || lower_ascii(s[i]) != w[i]) {
			return false;
		}
	}
	return w[i] == '\0';
}

bool
leia_multi_viewer_parse(const char *s, enum leia_multi_viewer *out)
{
	*out = LEIA_MULTI_VIEWER_SYSTEM;
	if (s == NULL) {
		return false;
	}
	while (is_blank(*s)) {
		s++;
	}
	size_t n = 0;
	while (s[n] != '\0') {
		n++;
	}
	while (n > 0 && is_blank(s[n - 1])) {
		n--;
	}
	if (n == 0) {
		return false;
	}
	if (word_eq(s, n, "system")) {
		*out = LEIA_MULTI_VIEWER_SYSTEM;
		return true;
	}
	if (word_eq(s, n, "on")) {
		*out = LEIA_MULTI_VIEWER_ON;
		return true;
	}
	if (word_eq(s, n, "off")) {
		*out = LEIA_MULTI_VIEWER_OFF;
		return true;
	}
	return false;
}

enum leia_multi_viewer
leia_multi_viewer_effective(enum leia_multi_viewer setting, bool weave_supported)
{
	if (!weave_supported) {
		return LEIA_MULTI_VIEWER_SYSTEM;
	}
	switch (setting) {
	case LEIA_MULTI_VIEWER_ON:
	case LEIA_MULTI_VIEWER_OFF: return setting;
	case LEIA_MULTI_VIEWER_SYSTEM:
	default: return LEIA_MULTI_VIEWER_SYSTEM;
	}
}

const char *
leia_multi_viewer_name(enum leia_multi_viewer v)
{
	switch (v) {
	case LEIA_MULTI_VIEWER_SYSTEM: return "system";
	case LEIA_MULTI_VIEWER_ON: return "on";
	case LEIA_MULTI_VIEWER_OFF: return "off";
	default: return "?";
	}
}
