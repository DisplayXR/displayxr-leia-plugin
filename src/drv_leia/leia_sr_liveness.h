// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR platform incarnation probe — "did the SR platform restart?".
 *
 * The SR SDK had no restart notification when this was written; it does now
 * (`srInstanceGetConnectionState` + `srGetPlatformInstanceId`, SR >= ST-5673),
 * and the v2-aware entry points below prefer it. The SCM probe described here
 * survives as the FALLBACK — for v1 arms, for builds without the v2 SDK, and
 * for the mixed installs (new plug-in, old SR Platform) that are expected in
 * the field. The original reasoning, which is still what the fallback does:
 *
 * When SRService/SRSession are restarted underneath a long-lived process (a
 * LeiaSR platform upgrade while displayxr-service.exe keeps running), the
 * process's `SR::SRContext` and its weaver keep answering — with STALE eye
 * positions and a SUCCESS return code. Nothing in the v1 SDK surface
 * distinguishes that from a person holding still, so the plug-in has to learn
 * it out-of-band (leia-plugin#158).
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
 * Same token, but preferring the SR SDK's OWN identity signal when the caller
 * has a v2 instance to ask through.
 *
 * `srGetPlatformInstanceId` (SR SDK >= ST-5673) names the platform RUN this
 * instance is bound to; it changes on a service restart, a crash-and-relaunch
 * or an upgrade, and on nothing else. That is strictly better than the SCM
 * probe, which infers the same thing from the service process's (pid, creation
 * time) pair.
 *
 * The SCM probe is therefore now a FALLBACK, not the primary source. It is
 * still needed, and not merely for non-v2 builds: a mixed install (new plug-in,
 * pre-ST-5673 SR Platform) is expected in the field, and there the SDK answers
 * SR_ERROR_FUNCTION_UNSUPPORTED / SR_ERROR_RUNTIME_UNAVAILABLE and this falls
 * through to the SCM token automatically.
 *
 * The SDK half is compiled only when the build's SDK actually declares the two
 * functions (`DXR_LEIA_HAS_SR_CONNECTION_STATE`, a gate narrower than
 * `DXR_LEIA_HAS_SR_V2` for reasons spelled out at the top of the .cpp). With
 * the gate off this whole file behaves exactly as it did before: SCM only.
 *
 * Identity is NOT liveness — the id says which platform is out there, never
 * whether this instance can still reach it. For that, use
 * leia_sr_liveness_connection_is_dead().
 *
 * @param sr_instance_v2 The arm's `SrInstance`, as an opaque pointer, or NULL
 *                       (v1 path, or a build without the v2 SDK) to go
 *                       straight to the SCM probe.
 *
 * @note A caller MUST use the same argument for the create-time stamp and for
 *       every later poll — mixing an id-derived stamp with an SCM-derived poll
 *       would read as a restart that never happened.
 *
 * Cached ~1 s per source, so an instance-bearing call and a bare one do not
 * evict each other. Never throws.
 *
 * @ingroup drv_leia
 */
uint64_t
leia_sr_liveness_platform_generation_ex(void *sr_instance_v2);

/*!
 * Has the SR SDK declared this instance's connection to the platform DEAD?
 *
 * `srInstanceGetConnectionState` is the LIVENESS signal (the id above is only
 * identity). DISCONNECTED is terminal for the instance — the SDK does not
 * reconnect one whose connection died — so a true here means "rebuild", not
 * "wait".
 *
 * @return true ONLY on a successful query reporting DISCONNECTED. CONNECTED,
 *         the reserved-and-never-returned RECONNECTING, any error (an older
 *         platform answers SR_ERROR_FUNCTION_UNSUPPORTED), and a NULL instance
 *         all answer false — like the version tripwire, undeterminable must
 *         never read as a failure.
 *
 * NOT a tracking query: a connected platform that currently sees no viewer
 * still reports CONNECTED. "Eyes not tracked" must never arm a reconnect.
 *
 * Deliberately uncached — the SDK documents this as a cheap read of state it
 * already holds, with no I/O and no blocking.
 *
 * @ingroup drv_leia
 */
bool
leia_sr_liveness_connection_is_dead(void *sr_instance_v2);

/*!
 * Do the SR client DLLs MAPPED in this process match the SR platform installed
 * on disk?
 *
 * After an SR platform upgrade a long-lived process keeps the OLD client DLLs
 * mapped (Windows does not swap a loaded image), so anything it builds
 * afterwards is old client code driven against a new platform. That is not
 * merely untidy: it access-violates inside the vendor runtime
 * (leia-plugin#169), so as of #169 this is a GATE, not just a tripwire — see
 * leia_sr_liveness_weaver_create_allowed().
 *
 * @return true when matched, or when the answer cannot be determined (nothing
 *         mapped yet, no version resource, a failed query). false ONLY on a
 *         positive mismatch. Undeterminable must never read as a mismatch: a
 *         fresh process has nothing SR mapped, and refusing there would break
 *         every start-up.
 *
 * Caching (#169). Keyed on the generation token AND bounded to ~1 s, because
 * generation alone is not a sufficient key for a gate: throughout an upgrade
 * the platform is DOWN, so the token is a constant 0 for the whole window in
 * which the installer swaps the files, and a verdict computed at the START of
 * that window ("matches" — nothing replaced yet) would stay pinned right
 * through the moment the decision is made. A positive mismatch additionally
 * LATCHES for the process lifetime: it is irreversible by construction, and
 * latching stops a transient undeterminable read (a locked file mid-install
 * answers "cannot tell") from talking the gate back open.
 *
 * Never logs — the CALLER owns the state-edge WARN.
 *
 * @ingroup drv_leia
 */
bool
leia_sr_liveness_client_matches_platform(void);

/*!
 * #169 gate: may a weaver be CREATED in this process right now?
 *
 * False when the SR platform was upgraded underneath this process. Nothing
 * in-process can fix that — the old client DLLs stay mapped until the process
 * exits — so the only remedy is a restart of the DisplayXR service, and the
 * only safe action here is to not call into the vendor at all.
 *
 * Every weaver create path must ask, not just the in-place reconnect: the
 * runtime answers a STALE backend by destroying the display processor and
 * creating a fresh one (displayxr-runtime#1064), and a fresh DP in the SAME
 * process still has the old DLLs mapped — so the recreate would make exactly
 * the vendor call that access-violated. Without this gate it becomes a crash
 * loop; with it, a create-refusal the runtime already handles ("create failed —
 * keeping the current one", plus a ~2 Hz backoff) and the panel flat-blits
 * until the service is restarted.
 *
 * Emits ONE process-wide `U_LOG_W` the first time it refuses — process-wide
 * because the condition is, and because the runtime's recreate loop would
 * otherwise turn a per-instance flag into a log storm. Callers just check the
 * bool.
 *
 * @return true when a create may proceed (including every undeterminable case
 *         — see leia_sr_liveness_client_matches_platform()).
 *
 * @ingroup drv_leia
 */
bool
leia_sr_liveness_weaver_create_allowed(void);

#ifdef __cplusplus
}
#endif
