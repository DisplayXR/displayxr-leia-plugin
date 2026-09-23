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
 * The caller keeps at most one start in flight.
 */
void
leia_bg_capture_worker_request_create(struct leia_bg_capture_worker *w, uint32_t panel_px_w, uint32_t panel_px_h);

/*!
 * Non-blocking. True when a posted start has finished; *out_capture is the new
 * capture, or NULL if the start failed (the capture module logged why).
 */
bool
leia_bg_capture_worker_poll_created(struct leia_bg_capture_worker *w, struct leia_bg_capture_linux **out_capture);

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
