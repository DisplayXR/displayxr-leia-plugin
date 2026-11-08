// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Camera rest on Linux: when our weaver asks SR for tracking, and what
 *         "a viewer is tracked" means once SR can rest it.
 *
 * Pure decision logic, no SDK calls, so it is unit-testable without hardware
 * (tests/test_tracking_policy_linux.c). leia_sr_linux_sdk.c is the only caller.
 *
 * ## The SR rule this encodes (LeiaSR #844 / #846, the tracker rest gate)
 *
 * A v2 weaver's tracking rests (the camera pauses, its LED goes off) once its
 * context's lens preference has been off for SR's hold (20 s by default).
 * srWeaverSetTrackingPolicy overrides that per weaver (AUTO / ALWAYS_ON / OFF)
 * and srWeaverGetTrackingState reports TRACKING / LOST / RESTED. A live
 * SrEyeTracker keeps the context awake under EVERY policy, by design (that is
 * what keeps a video call's camera on).
 *
 * ## What that means for the Linux arm
 *
 *  1. **Our own eye tracker has to go.** This arm used to create an
 *     SrEyeTracker on the process-wide context only to learn how fresh the
 *     eye samples were. While it lives the camera never rests. Once a weaver
 *     answers srWeaverGetTrackingState, that answer replaces it, and the
 *     tracker is destroyed (once per context: the SDK takes senses only
 *     before srInitialize, so it can't be re-created, and there is no
 *     reason to).
 *     A runtime without the query keeps the tracker and today's heuristic.
 *  2. **2D start.** A new weaver starts at OFF and moves to AUTO on its first
 *     3D request (the runtime asks for 3D at every xrBeginSession of a 3D
 *     session), so a surface that is 2D from launch never lights the camera.
 *     A later 2D stays on AUTO: SR's hold covers quick 2D/3D flips, so a flip
 *     never touches the camera directly. A weaver whose context has no lens
 *     handle is released to AUTO at once: nothing will ever ask it for 3D
 *     through us.
 *  3. **RESTED means no viewer**, and so does LOST. TRACKING is a viewer.
 *     The weaver's answer replaces the freshness heuristic outright (the
 *     tracker that fed it is gone); TRACKING lags a real loss by SR's
 *     face-lost delay (3 s).
 *
 * DXR_LEIA_TRACKING_POLICY = auto (SR's default, no call) | always_on | off
 * overrides (2), exactly as on Windows.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif

//! What DXR_LEIA_TRACKING_POLICY asks for.
enum leia_lnx_tracking_mode
{
	LEIA_LNX_TRACKING_2D_START = 0,  //!< unset: OFF until the first 3D request, then AUTO
	LEIA_LNX_TRACKING_SR_DEFAULT,    //!< "auto": make no call, SR's default
	LEIA_LNX_TRACKING_ALWAYS_ON,     //!< "always_on" / "on"
	LEIA_LNX_TRACKING_OFF,           //!< "off"
	LEIA_LNX_TRACKING_UNRECOGNISED,  //!< anything else: caller warns once, then uses 2D start
};

static inline enum leia_lnx_tracking_mode
leia_lnx_tracking_mode_parse(const char *e)
{
	if (e == NULL || e[0] == '\0') {
		return LEIA_LNX_TRACKING_2D_START;
	}
	if (strcasecmp(e, "auto") == 0) {
		return LEIA_LNX_TRACKING_SR_DEFAULT;
	}
	if (strcasecmp(e, "always_on") == 0 || strcasecmp(e, "on") == 0) {
		return LEIA_LNX_TRACKING_ALWAYS_ON;
	}
	if (strcasecmp(e, "off") == 0) {
		return LEIA_LNX_TRACKING_OFF;
	}
	return LEIA_LNX_TRACKING_UNRECOGNISED;
}

//! The weaver's own answer (srWeaverGetTrackingState); UNKNOWN = no answer.
enum leia_lnx_weaver_tracking
{
	LEIA_LNX_WEAVER_TRACKING_UNKNOWN = 0, //!< no weaver, or a runtime/SDK without the query
	LEIA_LNX_WEAVER_TRACKING_TRACKING,
	LEIA_LNX_WEAVER_TRACKING_LOST,
	LEIA_LNX_WEAVER_TRACKING_RESTED,
};

/*!
 * is_tracking for the seam.
 *
 * @param w             The weaver's answer.
 * @param user_present  USER_FOUND/USER_LOST latch (system monitor).
 * @param sample_fresh  Our own tracker delivered a sample recently.
 * @param device_ready  The SR device is up (system monitor).
 * @param have_tracker  Our own tracker exists (the heuristic needs it).
 */
static inline bool
leia_lnx_is_tracking(
    enum leia_lnx_weaver_tracking w, bool user_present, bool sample_fresh, bool device_ready, bool have_tracker)
{
	if (!device_ready) {
		return false;
	}
	switch (w) {
	case LEIA_LNX_WEAVER_TRACKING_TRACKING: return true;
	case LEIA_LNX_WEAVER_TRACKING_LOST:
	case LEIA_LNX_WEAVER_TRACKING_RESTED: return false;
	case LEIA_LNX_WEAVER_TRACKING_UNKNOWN:
	default:
		/* Today's heuristic: an announced viewer, or a fresh real sample (a
		 * viewer already present at connect is never announced). */
		return (user_present || sample_fresh) && have_tracker;
	}
}

#ifdef __cplusplus
}
#endif
