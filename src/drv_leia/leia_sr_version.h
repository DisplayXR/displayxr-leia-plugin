// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Installed LeiaSR runtime version and the "update recommended" rule.
 *
 * The plug-in weaves on every LeiaSR runtime from 1.34.8 on, but DisplayXR
 * recommends 1.39.0 or later (the first release built for it: per-display
 * presence, the windowless v2 weaver, the keep-newer installer). Below that,
 * the plug-in reports XRT_PLUGIN_PLATFORM_FLAG_UPDATE_RECOMMENDED through the
 * runtime's ADR-045 platform-state slot, with the installed and recommended
 * versions and a link to the public LeiaSR-Runtime releases. The runtime shows
 * it verbatim; it is advisory only and never changes what the plug-in does.
 *
 * The version comes from the registry, not from the SR SDK, so the check stays
 * inside the platform-state budget (presence checks only, no SR connection)
 * and also works on runtimes that predate the C99 API, where the SDK cannot
 * answer at all. The parsing and the rule are plain C and host-tested
 * (tests/test_sr_version.c, run by the Linux CI job).
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The LeiaSR runtime version DisplayXR recommends (and below which it says so).
#define LEIA_SR_RECOMMENDED_VERSION "1.39.0"

//! Where an update comes from: the public LeiaSR runtime releases.
#define LEIA_SR_UPDATE_URL "https://github.com/LeiaInc/LeiaSR-Runtime/releases/latest"

//! The action label shown next to the link.
#define LEIA_SR_UPDATE_LABEL "Update the LeiaSR Runtime"

/*!
 * Parse the leading "A.B.C" of a LeiaSR version string, such as "1.34.12",
 * "1.39.0+3665.92e0c8c02f" or "1.39.0.3665". A missing C reads as 0 ("1.39").
 * Returns false when the string does not start with "A.B".
 */
bool
leia_sr_version_parse(const char *s, int out[3]);

//! <0, 0 or >0 as a is older than, equal to or newer than b (A.B.C only).
int
leia_sr_version_cmp(const int a[3], const int b[3]);

/*!
 * The rule: true when @p installed parses and is older than
 * LEIA_SR_RECOMMENDED_VERSION. An unparsable or empty version is NOT
 * flagged: no notice is better than a wrong one.
 */
bool
leia_sr_update_recommended(const char *installed);

/*!
 * The installed LeiaSR runtime version, from the Windows uninstall entry the
 * LeiaSR installer has written since before 1.34 ("Simulated Reality
 * Platform", falling back to the platform component's "Simulated Reality"),
 * 64-bit registry view. Registry reads only: cheap, non-blocking,
 * thread-safe. Returns false (and an empty string) when not found, and
 * always on non-Windows builds.
 */
bool
leia_sr_installed_version(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
