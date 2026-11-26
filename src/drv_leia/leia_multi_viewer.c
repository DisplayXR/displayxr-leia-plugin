// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The MULTI_VIEWER setting's parse / resolve rules (#446) and the
 *         rules of the "LeiaSR multi-viewer" 3D entry. Plain C, no SR or
 *         runtime types; host-tested by tests/test_multi_viewer.c.
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

uint32_t
leia_multi_viewer_entry_count(bool weave_supported, bool has_entry_contract)
{
	return (weave_supported && has_entry_contract) ? 3u : 2u;
}

uint32_t
leia_multi_viewer_default_entry(enum leia_multi_viewer setting, uint32_t entry_count)
{
	if (setting == LEIA_MULTI_VIEWER_ON && entry_count > LEIA_MV_ENTRY_MULTI_VIEWER) {
		return LEIA_MV_ENTRY_MULTI_VIEWER;
	}
	return LEIA_MV_ENTRY_3D;
}

uint32_t
leia_multi_viewer_latch_entry(uint32_t current, uint32_t incoming, uint32_t entry_count)
{
	if (incoming >= LEIA_MV_ENTRY_3D && incoming < entry_count) {
		return incoming;
	}
	return current;
}

enum leia_multi_viewer
leia_multi_viewer_for_entry(uint32_t entry, uint32_t entry_count, enum leia_multi_viewer effective)
{
	if (effective != LEIA_MULTI_VIEWER_ON && effective != LEIA_MULTI_VIEWER_OFF) {
		effective = LEIA_MULTI_VIEWER_SYSTEM;
	}
	if (entry_count <= LEIA_MV_ENTRY_MULTI_VIEWER) {
		// Two entries: the setting is the only switch.
		return effective;
	}
	if (entry == LEIA_MV_ENTRY_MULTI_VIEWER) {
		return LEIA_MULTI_VIEWER_ON;
	}
	// "LeiaSR" (and anything else): the setting's system or off. `on` only
	// names the default entry here, so a screen put back on "LeiaSR" follows
	// the system setting.
	return effective == LEIA_MULTI_VIEWER_OFF ? LEIA_MULTI_VIEWER_OFF : LEIA_MULTI_VIEWER_SYSTEM;
}

const char *
leia_multi_viewer_entry_name(uint32_t entry)
{
	switch (entry) {
	case LEIA_MV_ENTRY_2D: return LEIA_MV_NAME_2D;
	case LEIA_MV_ENTRY_3D: return LEIA_MV_NAME_3D;
	case LEIA_MV_ENTRY_MULTI_VIEWER: return LEIA_MV_NAME_MULTI_VIEWER;
	default: return "?";
	}
}
