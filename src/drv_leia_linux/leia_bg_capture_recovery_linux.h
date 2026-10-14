// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Re-create schedule for a desktop capture the compositor closed (#364).
 *
 * GNOME tears every ScreenCast session down when the session locks — and every
 * laptop suspend goes through the lock screen — so a capture that dies is the
 * normal case, not a user's "Stop Screen Sharing". The display processor
 * therefore treats a lost capture as recoverable: it buries the dead one and
 * re-runs the full start (exclusion + RecordArea + PipeWire) on its capture
 * worker until frames flow again.
 *
 * The schedule, as pure logic so it is host-testable:
 *  - the first attempt runs IMMEDIATELY (a stream that merely hiccupped is back
 *    at once);
 *  - each failed attempt doubles the wait from 2 s, capped at 30 s, forever
 *    (one attempt per 30 s is cheap, and GNOME gives no way to tell a lock from
 *    a user's stop, so there is no "give up" state);
 *  - a session UNLOCK makes the next attempt due at once and restarts the
 *    backoff from 2 s — that is when a lock-closed capture can come back. An
 *    unlock seen while an attempt is in flight is held and honoured as soon as
 *    that attempt reports.
 *
 * Times are CLOCK_MONOTONIC nanoseconds supplied by the caller.
 *
 * @ingroup drv_leia_linux
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEIA_BG_RECOVERY_FIRST_DELAY_NS (2ull * 1000000000ull)
#define LEIA_BG_RECOVERY_MAX_DELAY_NS (30ull * 1000000000ull)

struct leia_bg_recovery
{
	bool active;         //!< a capture was lost and none has delivered a trusted frame since
	bool unlock_pending; //!< an unlock arrived while an attempt was in flight
	uint64_t lost_ns;    //!< when the capture was lost (for the "restored after" line)
	uint64_t next_ns;    //!< earliest time the next attempt may be posted
	uint64_t delay_ns;   //!< the wait applied after the last failure (0 = none yet)
	uint32_t attempts;   //!< attempts posted since the loss
};

//! A live capture was lost. Re-entering while already active keeps the count and backoff.
static inline void
leia_bg_recovery_begin(struct leia_bg_recovery *r, uint64_t now_ns)
{
	if (r->active) {
		return;
	}
	*r = (struct leia_bg_recovery){0};
	r->active = true;
	r->lost_ns = now_ns;
	r->next_ns = now_ns; // retry immediately once
}

//! True when an attempt may be posted now (always true when not recovering).
static inline bool
leia_bg_recovery_due(const struct leia_bg_recovery *r, uint64_t now_ns)
{
	return !r->active || now_ns >= r->next_ns;
}

//! An attempt is being posted; returns its 1-based number (0 when not recovering).
static inline uint32_t
leia_bg_recovery_attempt_posted(struct leia_bg_recovery *r)
{
	if (!r->active) {
		return 0;
	}
	return ++r->attempts;
}

/*!
 * The attempt failed (or the capture it produced died before its first trusted
 * frame). Schedules the next one and returns the wait in ns (0 = immediately,
 * because an unlock arrived meanwhile).
 */
static inline uint64_t
leia_bg_recovery_attempt_failed(struct leia_bg_recovery *r, uint64_t now_ns)
{
	if (!r->active) {
		return 0;
	}
	if (r->unlock_pending) {
		r->unlock_pending = false;
		r->delay_ns = 0;
		r->next_ns = now_ns;
		return 0;
	}
	if (r->delay_ns == 0) {
		r->delay_ns = LEIA_BG_RECOVERY_FIRST_DELAY_NS;
	} else {
		r->delay_ns *= 2u;
		if (r->delay_ns > LEIA_BG_RECOVERY_MAX_DELAY_NS) {
			r->delay_ns = LEIA_BG_RECOVERY_MAX_DELAY_NS;
		}
	}
	r->next_ns = now_ns + r->delay_ns;
	return r->delay_ns;
}

/*!
 * The session was unlocked. Returns true if that changes anything (we are
 * recovering): the next attempt becomes due now — or, with one in flight, as
 * soon as it reports — and the backoff restarts from 2 s.
 */
static inline bool
leia_bg_recovery_unlocked(struct leia_bg_recovery *r, uint64_t now_ns, bool attempt_in_flight)
{
	if (!r->active) {
		return false;
	}
	if (attempt_in_flight) {
		r->unlock_pending = true;
	} else {
		r->delay_ns = 0;
		r->next_ns = now_ns;
	}
	return true;
}

//! A re-created capture delivered a trusted frame: recovery is over.
static inline void
leia_bg_recovery_restored(struct leia_bg_recovery *r)
{
	*r = (struct leia_bg_recovery){0};
}

/*!
 * Whether attempt @p n gets its own WARN line. Every one of the first ten, then
 * every tenth — at the 30 s cap that is one line per five minutes, so a session
 * that stays locked overnight does not fill the log.
 */
static inline bool
leia_bg_recovery_should_log(uint32_t n)
{
	return n <= 10u || (n % 10u) == 0u;
}

#ifdef __cplusplus
}
#endif
