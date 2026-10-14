// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Off-render-thread start/stop for the Linux desktop capture.
 *
 * Starting the window-excluded desktop capture is a handful of D-Bus round
 * trips plus a wait for the PipeWire node — bounded at ~5 s, typically a
 * noticeable fraction of a second (leia_bg_capture_linux_create). Stopping it
 * is a D-Bus Stop plus a PipeWire teardown. With lazy transparency both happen
 * mid-session, on a user's transparency toggle, so neither may run on the
 * compositor's frame thread: a frozen frame on Ctrl+T is exactly the kind of
 * hitch the change exists to remove.
 *
 * One worker thread per display processor runs the jobs strictly in the order
 * they were posted, so a stop always completes before the next start begins —
 * two ScreenCast sessions (and two capture exclusions) never overlap.
 *
 * Ownership: a capture handed out by @ref leia_bg_capture_worker_poll_created
 * belongs to the caller until it is given back with
 * @ref leia_bg_capture_worker_retire. The caller must only retire a capture the
 * GPU has finished sampling.
 *
 * @ingroup drv_leia_linux
 */

#pragma once

#include "leia_bg_capture_linux.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct leia_bg_capture_worker;

//! Create the worker (starts its thread). NULL on failure.
struct leia_bg_capture_worker *
leia_bg_capture_worker_create(struct vk_bundle *vk);

/*!
 * Post a capture start for a panel of @p panel_px_w x @p panel_px_h device
 * pixels. The result arrives through @ref leia_bg_capture_worker_poll_created.
 * The caller keeps at most one start in flight. @p recreate marks a re-create
 * after a lost capture (#364): see leia_bg_capture_linux_create_ex.
 */
void
leia_bg_capture_worker_request_create(struct leia_bg_capture_worker *w,
                                      uint32_t panel_px_w,
                                      uint32_t panel_px_h,
                                      bool recreate);

/*!
 * Non-blocking. True when a posted start has finished; *out_capture is the new
 * capture, or NULL if the start failed, in which case *out_fail_reason (if
 * given) is a short static string saying why.
 */
bool
leia_bg_capture_worker_poll_created(struct leia_bg_capture_worker *w,
                                    struct leia_bg_capture_linux **out_capture,
                                    const char **out_fail_reason);

/*!
 * Watch the session bus for a session UNLOCK while @p on (#364): GNOME closes
 * the ScreenCast session at the lock screen, and unlock is when it can be
 * re-created. The worker thread owns the connection — opened, pumped (every
 * 250 ms, non-blocking) and closed there, never on the frame thread — so it
 * costs nothing while off, which is whenever no capture is being recovered.
 */
void
leia_bg_capture_worker_watch_unlock(struct leia_bg_capture_worker *w, bool on);

//! Non-blocking: true (once) if an unlock was seen since the last call.
bool
leia_bg_capture_worker_take_unlock(struct leia_bg_capture_worker *w);

//! Post a capture stop. NULL is ignored.
void
leia_bg_capture_worker_retire(struct leia_bg_capture_worker *w, struct leia_bg_capture_linux *capture);

/*!
 * Finish every posted job (including any in-flight start, whose result is
 * then stopped too), join the thread and free the worker.
 */
void
leia_bg_capture_worker_destroy(struct leia_bg_capture_worker **w_ptr);

#ifdef __cplusplus
}
#endif
