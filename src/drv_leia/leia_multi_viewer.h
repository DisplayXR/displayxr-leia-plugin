// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The MULTI_VIEWER setting (#446): which multi-viewer weave override
 *         the Windows SR v2 weavers get.
 *
 * Multi-viewer weaving used to be rendering mode 2 ("LeiaSR multi-viewer"),
 * identical to mode 1 except for the weaver option. The runtime's two-mode
 * contract (displayxr-runtime#2766 / ADR-065: mode 0 = 2D, mode 1 = 3D, any
 * other choice a per-driver setting) would trim that mode away, so the
 * plug-in declares two modes and the override is this setting instead:
 *
 *     displayxr-cli dp set leia-sr MULTI_VIEWER on
 *
 * read through the host's `get_setting` (`DXR_DP_LEIA_SR_MULTI_VIEWER` in the
 * hosting process's environment wins over the per-user store, which wins over
 * the machine store), ONCE per device creation. Values:
 *
 *   - `system` (default): follow the LeiaSR system setting — no override call;
 *     the old mode-1 behaviour.
 *   - `on`: force multi-viewer weaving on — the old mode-2 behaviour.
 *   - `off`: force it off.
 *
 * When this build's SR SDK or the installed SR runtime cannot weave for more
 * than one viewer, a non-default value is ignored with one WARN at device
 * creation (leia_multi_viewer_effective()).
 *
 * The parse / resolve half is plain C with no SR or runtime types, host-tested
 * by tests/test_multi_viewer.c (Linux CI). The process-wide value the weavers
 * read (leia_multi_viewer_active()) lives in leia_plugin.c, Windows only.
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The setting's key (`DXR_DP_LEIA_SR_MULTI_VIEWER`, `Settings\MULTI_VIEWER`).
#define LEIA_MULTI_VIEWER_KEY "MULTI_VIEWER"
//! The compiled default, as the CLI lists it.
#define LEIA_MULTI_VIEWER_DEFAULT "system"
//! One line of help for `displayxr-cli dp options leia-sr` (enum: the words).
#define LEIA_MULTI_VIEWER_HELP                                                                                         \
	"system|on|off -- multi-viewer weaving: follow the LeiaSR system setting, force it on, or force it off "     \
	"(Windows SR v2 weavers; read at device creation)"

/*!
 * The MULTI_VIEWER values. Numeric values are internal only (never stored).
 */
enum leia_multi_viewer
{
	LEIA_MULTI_VIEWER_SYSTEM = 0, //!< follow the SR system setting (no override call)
	LEIA_MULTI_VIEWER_ON = 1,     //!< force multi-viewer weaving on
	LEIA_MULTI_VIEWER_OFF = 2,    //!< force multi-viewer weaving off
};

/*!
 * Parse a stored value. Case-insensitive, surrounding blanks ignored.
 *
 * @param s    The value as the host returned it; NULL or "" = unset.
 * @param out  The parsed value; LEIA_MULTI_VIEWER_SYSTEM unless the return
 *             is true and the word says otherwise.
 * @return true for `system` / `on` / `off`; false for NULL, "", or any other
 *         word (the caller warns about a non-empty unknown word).
 */
bool
leia_multi_viewer_parse(const char *s, enum leia_multi_viewer *out);

/*!
 * The value the weavers actually apply: @p setting when @p weave_supported,
 * else LEIA_MULTI_VIEWER_SYSTEM (the setting is ignored — the caller logs
 * that once when @p setting was not already SYSTEM).
 */
enum leia_multi_viewer
leia_multi_viewer_effective(enum leia_multi_viewer setting, bool weave_supported);

//! "system" / "on" / "off" (the stored word), "?" for anything else.
const char *
leia_multi_viewer_name(enum leia_multi_viewer v);

/*!
 * The process-wide effective MULTI_VIEWER value the weave paths apply,
 * published at each Leia device creation (leia_plugin.c, Windows arm only —
 * not defined on the Linux / macOS arms, which have no multi-viewer weave).
 * LEIA_MULTI_VIEWER_SYSTEM until a device has been created. Cheap, any thread.
 */
enum leia_multi_viewer
leia_multi_viewer_active(void);

#ifdef __cplusplus
}
#endif
