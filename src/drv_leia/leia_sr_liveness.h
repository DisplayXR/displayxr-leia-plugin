// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR platform incarnation probe — "did the SR platform restart?".
 *
 * The SR SDK has no restart notification. When SRService/SRSession are
 * restarted underneath a long-lived process (a LeiaSR platform upgrade while
 * displayxr-service.exe keeps running), the process's `SR::SRContext` and its
 * weaver keep answering — with STALE eye positions and a SUCCESS return code.
 * Nothing in the SDK surface distinguishes that from a person holding still,
 * so the plug-in has to learn it out-of-band (leia-plugin#158).
 *
 * The signal used here is the SR service process's IDENTITY, not its
 * reachability: `Global\sharedDeviceSerialMemory` (leia_edid_probe.c) re-opens
 * happily after a restart, so liveness alone proves nothing. A restart always
 * changes the service's (pid, process creation time) pair, and that pair is
 * exactly what a generation token is.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Backend state, vendor-plug-in side. The values match the runtime's
 * XRT_DP_BACKEND_STATE_* contract (the `get_backend_state` DP slot), so a
 * poll result is handed straight to the runtime without translation.
 *
 * @ingroup drv_leia
 */
enum leia_sr_backend_state
{
	//! Weaver is up and talking to the SR platform this process connected to.
	LEIA_SR_BACKEND_OK = 0,
	//! Reconnecting, SR platform down, or mapped-client/installed-platform
	//! version skew. Output is still produced (possibly untracked); the arm
	//! either self-heals or is waiting for the platform to come back.
	LEIA_SR_BACKEND_DEGRADED = 1,
	//! Unrecoverable without a DP recreate — the arm cannot rebuild in place.
	LEIA_SR_BACKEND_STALE = 2,
};

/*!
 * A token identifying the CURRENT SR platform incarnation.
 *
 * - `0` — SR platform down / unreachable (service stopped, or the query was
 *   refused). NOT a generation: never compare two zeros as "same platform".
 * - non-zero — stable for the lifetime of one SR service process. ANY change
 *   in the value means the platform restarted underneath us.
 *
 * Internally cached ~1 s, so this is safe to call per-frame (it is designed to
 * be polled, not subscribed to).
 *
 * Never throws, never blocks on the SR SDK — it only talks to the SCM.
 *
 * @ingroup drv_leia
 */
uint64_t
leia_sr_liveness_platform_generation(void);

/*!
 * One-shot per-generation check: do the SR client DLLs MAPPED in this process
 * match the SR platform installed on disk?
 *
 * After an SR platform upgrade a long-lived process keeps the OLD client DLLs
 * mapped (Windows does not swap a loaded image), so a reconnect gives it a
 * fresh connection driven through the old client libraries. Within a patch
 * train that is usually fine, but it is a real skew and it explains otherwise
 * baffling behaviour, so it is worth one loud line.
 *
 * @return true when matched, or when the answer cannot be determined (nothing
 *         mapped yet, no version resource, a failed query) — this is a
 *         tripwire, not a gate, and must never be the reason something is
 *         refused. false ONLY on a positive mismatch.
 *
 * Result is cached per generation token; the CALLER is responsible for logging
 * on state edges (this function never logs).
 *
 * @ingroup drv_leia
 */
bool
leia_sr_liveness_client_matches_platform(void);

#ifdef __cplusplus
}
#endif
