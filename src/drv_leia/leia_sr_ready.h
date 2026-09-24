// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR panel readiness — "has the SR platform identified the panel yet?"
 *         — plus the ONE process-wide display-geometry cache it gates.
 * @ingroup drv_leia
 *
 * ## The problem this solves
 *
 * The SR platform has no readiness API. `SRService` starts at boot, but the
 * panel is only *identified* once the FPC serial link (the eye-tracker's COM
 * port) has answered — 4–7 s after `SRSession` starts with the panel on, and
 * **never** while the panel sleeps (an unattended Windows-Update reboot puts it
 * back to sleep 3 s after boot). Until then every SR client sees a "default
 * display" with an empty product code: `getPrimaryActiveSRDisplay()` is not
 * valid / has no location on the v1 API, and the v2 `rt_DisplayGet*` return
 * `SR_SUCCESS` with 15.6"-4K-ish placeholder values. Worse, the SDK caches the
 * `IDisplay*` per handle, so a handle created before identification stays on
 * the default display forever.
 *
 * `displayxr-service` auto-starts at logon, right into that window. Before
 * this module the plug-in queried once, fell back to hardcoded 15.6" 4K
 * geometry, LATCHED it (view scale, probe cache, `leia_hmd` views/FOV) and
 * never asked again — so a box that rebooted overnight weaved with the wrong
 * geometry until someone restarted the service by hand.
 *
 * ## The two readiness signals that actually work
 *
 * 1. **`Global\sharedDeviceSerialMemory`** — the SR service's shared-memory
 *    block: one count byte followed by 32-byte device serials. Count >= 1 means
 *    the platform has a serial, i.e. has identified a panel. Reading it costs a
 *    file-mapping open, no SR context, no SDK — cheap enough to poll.
 * 2. **A FRESH display handle** reporting `isValid()` with a non-empty
 *    `getLocation()` — the spin `leiasr_query_recommended_view_dimensions` and
 *    `leia_sr_v2_query_display` already do. Only a fresh handle counts (see the
 *    caching note above).
 *
 * Signal 1 gates everything (no SR context is created while it is false);
 * signal 2 is what the geometry query then verifies before anything is cached.
 * Geometry read while unidentified is never cached.
 *
 * ## The contract with the runtime (plugin_api 5, no new ABI fields)
 *
 * - `get_display_info` is **cheap and non-blocking once the startup budget is
 *   spent**: `false` immediately while the panel is unidentified, `true` with
 *   the real geometry once it is. The runtime re-calls it on every client
 *   compositor create and re-fills its cached info when the answer changes.
 * - The plug-in keeps its own `leia_hmd` device correct **in place** when the
 *   geometry arrives late (the runtime never rebuilds the head device).
 *
 * ## The startup budget
 *
 * ONE process-wide deadline, started on first use (probe / create_device /
 * get_display_info), default 20 s, overridable with
 * `DXR_LEIA_SR_READY_TIMEOUT_S`. Until it expires the startup queries may
 * block-poll signal 1 at 100 ms. After it, everything returns fast. This
 * replaces the old 2 + 2 + 5 + 5 s of *serial* per-call blocking, each of
 * which spun a fresh SR context in a loop.
 *
 * ## Late re-derivation
 *
 * If the budget expires unidentified, ONE detached watcher thread polls signal
 * 1 at 1 Hz. When it flips, the watcher queries the geometry with a fresh
 * handle, publishes it into the shared statics (probe cache, static display
 * dimensions, the view scale) and updates the live `leia_hmd` in place — so
 * the next `get_display_info` from the runtime returns `true` with the real
 * values. The D3D11 weaver's async create worker (which retries forever and
 * therefore also learns the real geometry) publishes through the same path.
 *
 * Full write-up: `docs/sr-readiness.md`.
 */

#pragma once

#include "leia_interface.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Has the SR platform identified a panel? Reads the count byte of
 * `Global\sharedDeviceSerialMemory`. No SR context, no SDK call; safe to poll.
 *
 * false when the mapping does not exist (SRService not running) or the count
 * is 0 (service up, panel not identified — asleep, or still enumerating).
 *
 * @ingroup drv_leia
 */
bool
leiasr_display_identified(void);

/*!
 * Block-poll @ref leiasr_display_identified within the ONE process-wide
 * startup budget. Starts the budget on first call. Returns true as soon as the
 * panel is identified; false once the budget is spent (and, from then on,
 * immediately — the caller must not block on SR after a false).
 *
 * Expiry starts the late-identification watcher (once).
 *
 * @ingroup drv_leia
 */
bool
leiasr_ready_wait(void);

/*!
 * Clamp an SR-side spin timeout to what is left of the startup budget, with a
 * 1 s floor so an identified display still gets a bounded verification spin.
 *
 * @ingroup drv_leia
 */
double
leiasr_ready_clamp(double max_time);

/*!
 * The cached, verified display geometry. Cheap. false until a query on an
 * identified panel has succeeded.
 *
 * @ingroup drv_leia
 */
bool
leiasr_geometry_get(struct leiasr_geometry *out);

/*!
 * Resolve the geometry: cached → true at once; else wait for identification
 * within the startup budget, query SR with a FRESH handle (bounded by
 * @p max_time clamped to the budget), verify, publish everywhere (probe cache,
 * view scale, live head device) and return true. false when the panel is not
 * identified (fast once the budget is spent) or the query failed.
 *
 * @param max_time SR-side spin bound in seconds (clamped, see leiasr_ready_clamp).
 * @param why      Logged with the one-off publish line.
 *
 * @ingroup drv_leia
 */
bool
leiasr_geometry_resolve(double max_time, const char *why);

/*!
 * Register / unregister the live head device so a late geometry publish can
 * update it in place. Registering applies already-published geometry at once,
 * closing the window between "watcher published" and "device created".
 *
 * @ingroup drv_leia
 */
void
leiasr_ready_hmd_register(struct xrt_device *xdev);

void
leiasr_ready_hmd_unregister(struct xrt_device *xdev);

/*!
 * A weaver just came up, so the panel is identified for sure: if the process
 * geometry is still unresolved, resolve + publish it now. Cheap when already
 * resolved. Called off the frame path (weaver create workers).
 *
 * @ingroup drv_leia
 */
void
leiasr_ready_note_weaver_ready(void);

/*!
 * Stop the watcher (if running) and wait briefly for it to exit. Idempotent.
 *
 * @ingroup drv_leia
 */
void
leiasr_ready_shutdown(void);

#ifdef __cplusplus
}
#endif
