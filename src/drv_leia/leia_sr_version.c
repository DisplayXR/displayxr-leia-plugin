// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Installed LeiaSR runtime version and the "update recommended" rule.
 * @ingroup drv_leia
 */

#include "leia_sr_version.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static bool
read_uint(const char **p, int *out)
{
	const char *s = *p;
	if (*s < '0' || *s > '9') {
		return false;
	}
	long v = 0;
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (*s - '0');
		if (v > 100000) {
			return false;
		}
		s++;
	}
	*out = (int)v;
	*p = s;
	return true;
}

bool
leia_sr_version_parse(const char *s, int out[3])
{
	out[0] = out[1] = out[2] = 0;
	if (s == NULL) {
		return false;
	}
	while (*s == ' ' || *s == '\t' || *s == 'v' || *s == 'V') {
		s++;
	}
	if (!read_uint(&s, &out[0]) || *s != '.') {
		return false;
	}
	s++;
	if (!read_uint(&s, &out[1])) {
		return false;
	}
	if (*s == '.') {
		const char *t = s + 1;
		int c = 0;
		if (read_uint(&t, &c)) {
			out[2] = c;
		}
	}
	return true;
}

int
leia_sr_version_cmp(const int a[3], const int b[3])
{
	for (int i = 0; i < 3; i++) {
		if (a[i] != b[i]) {
			return a[i] < b[i] ? -1 : 1;
		}
	}
	return 0;
}

bool
leia_sr_update_recommended(const char *installed)
{
	int have[3];
	int want[3];
	if (installed == NULL || installed[0] == '\0' || !leia_sr_version_parse(installed, have)) {
		return false;
	}
	(void)leia_sr_version_parse(LEIA_SR_RECOMMENDED_VERSION, want);
	return leia_sr_version_cmp(have, want) < 0;
}

#ifdef _WIN32
static bool
read_display_version(const char *subkey, char *out, size_t cap)
{
	char buf[128];
	DWORD size = sizeof(buf);
	LSTATUS st = RegGetValueA(HKEY_LOCAL_MACHINE, subkey, "DisplayVersion", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
	                          NULL, buf, &size);
	if (st != ERROR_SUCCESS || buf[0] == '\0') {
		return false;
	}
	buf[sizeof(buf) - 1] = '\0';
	snprintf(out, cap, "%s", buf);
	return true;
}
#endif

bool
leia_sr_installed_version(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
#ifdef _WIN32
	static const char *const keys[] = {
	    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Simulated Reality Platform",
	    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Simulated Reality",
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (read_display_version(keys[i], out, cap)) {
			return true;
		}
	}
	out[0] = '\0';
#endif
	return false;
}
