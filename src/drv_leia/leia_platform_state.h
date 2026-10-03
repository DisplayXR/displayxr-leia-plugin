// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR platform presence + state machine (install-order epic, P-a/P-b/P-c).
 *
 * The plug-in loads without the SR platform (its SR client DLLs are
 * delay-loaded) and reports WHY it is or is not usable as one generic state +
 * a short hint. The values mirror the runtime's `xrt_plugin_platform_state`
 * (ADR-045) one-to-one, so the runtime slot is a straight copy; they are
 * defined here as well so the plug-in builds against runtime headers that
 * predate the slot.
 *
 *   PLATFORM_ABSENT       SR registry key missing, or the SR client DLLs
 *                         cannot be resolved/loaded.
 *   INCOMPATIBLE          SR client DLLs load but lack an export this build
 *                         imports (platform too old/new for this plug-in).
 *   PLATFORM_NOT_RUNNING  key present, `Global\sharedDeviceSerialMemory`
 *                         absent (SR Service down / starting).
 *   NO_DISPLAY            platform running, no Leia panel attached: no EDID
 *                         table match AND (a panel matched earlier in this
 *                         process, i.e. it was unplugged, OR SR reports no
 *                         identified device).
 *   READY                 platform running and a panel is attached (EDID
 *                         match, or SR has identified a panel our frozen EDID
 *                         table does not know).
 *
 * Every function here is a presence check — registry, named mapping, EDID —
 * and returns well inside the ~100 ms probe budget. Nothing waits for SR.
 * Thread-safe.
 *
 * @ingroup drv_leia
 */

#pragma once

#include "leia_interface.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Values equal `enum xrt_plugin_platform_state` (runtime ADR-045).
enum leia_platform_state
{
	LEIA_PLATFORM_UNKNOWN = 0,
	LEIA_PLATFORM_READY = 1,
	LEIA_PLATFORM_ABSENT = 2,
	LEIA_PLATFORM_NOT_RUNNING = 3,
	LEIA_PLATFORM_NO_DISPLAY = 4,
	LEIA_PLATFORM_INCOMPATIBLE = 5,
};

enum leia_sr_bind_result
{
	LEIA_SR_BIND_OK = 0,
	LEIA_SR_BIND_MISSING = 1,      //!< a client DLL could not be loaded
	LEIA_SR_BIND_INCOMPATIBLE = 2, //!< a client DLL lacks an imported export
};

/*!
 * Bind every delay-loaded SR import (leia_sr_delayload_win.c). MUST return
 * LEIA_SR_BIND_OK before ANY SR SDK call: a missing DLL/export otherwise
 * faults at the call site. Sticky once OK; retried (cheaply) after a failure,
 * so an SR install that lands while this process runs is picked up.
 */
enum leia_sr_bind_result
leia_sr_client_bind(void);

//! true once leia_sr_client_bind() has succeeded. Lock-free.
bool
leia_sr_client_bound(void);

/*!
 * Evaluate the state from a FRESH EDID probe result (the caller just ran
 * leia_edid_probe_display), publish it, log a one-shot WARN on a change and
 * invalidate the cached SR geometry when the matched panel changes (P-c).
 */
enum leia_platform_state
leia_platform_state_evaluate(const struct leia_display_probe_result *edid);

/*!
 * Re-evaluate. The EDID enumeration is re-run when the monitor topology
 * changed (cheap EnumDisplayMonitors signature) or when the last full probe is
 * older than @p max_edid_age_ms (0 = always; UINT32_MAX = only on a topology
 * change — the setting for frame-adjacent callers).
 */
enum leia_platform_state
leia_platform_state_refresh(uint32_t max_edid_age_ms);

//! The last published state. Lock-free; never probes.
enum leia_platform_state
leia_platform_state_get(void);

//! User-facing hint for the last published state ("" for READY). Static
//! storage, ASCII. Lock-free.
const char *
leia_platform_state_get_hint(void);

//! Short name for logs ("READY", "PLATFORM_ABSENT", ...).
const char *
leia_platform_state_name(enum leia_platform_state state);

/*!
 * true while weaving is pointless because the panel is gone (NO_DISPLAY):
 * the DP then passes pixels through unwoven until it returns. Lock-free.
 */
bool
leia_platform_display_absent(void);

#ifdef __cplusplus
}
#endif
