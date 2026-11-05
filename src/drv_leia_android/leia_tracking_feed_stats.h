// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Rate accounting for the in-app tracking feed (the stereo camera's
 *         640x480 frames handed to the CNSDK shared-camera sink).
 *
 * The feeder thread hands every frame to CNSDK synchronously, and the CNSDK
 * engine runs detection on the CALLER's thread. Its idle throttle (engine
 * FrameThrottle: active 5 s after start once no face was seen for 2 s) does
 * not drop a frame quietly: it sleeps the caller 500 ms. So the rate the sink
 * actually receives is decided by the engine, not by the camera, and the only
 * honest place to see it is the feed call itself. This module turns the feed
 * calls into one periodic report: frames captured/s, fed/s, the longest call
 * and the longest gap between calls, and how many calls looked like the
 * engine's 500 ms throttle sleep.
 *
 * No Android, CNSDK or runtime dependency: host-tested
 * (tests/test_tracking_feed_stats.c, Linux CI).
 *
 * @ingroup drv_leia_android
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A feed call at least this long is counted as an engine stall (the CNSDK
//! idle throttle sleeps 500 ms; detection itself is tens of ms).
#define LEIA_TFS_STALL_NS (400LL * 1000 * 1000)

//! Report period.
#define LEIA_TFS_PERIOD_NS (10000LL * 1000 * 1000)

struct leia_tfs
{
	int64_t window_ns;  //!< start of the current window (0 = not started)
	uint64_t fed;       //!< feed calls finished in the window
	uint64_t stalls;    //!< of which >= LEIA_TFS_STALL_NS
	int64_t busy_ns;    //!< summed call time in the window
	int64_t max_call_ns;
	int64_t max_gap_ns; //!< longest start-to-start gap between calls
	int64_t last_start_ns;
	//! Totals seen at the last report (the counters passed in are cumulative).
	uint64_t captured_at, faces_at, polls_at;
};

struct leia_tfs_report
{
	double seconds;
	double captured_per_s; //!< frames the capture callback produced
	double fed_per_s;      //!< frames the sink received
	double faces_per_s;    //!< face polls that returned a face
	double polls_per_s;    //!< face polls
	double avg_call_ms, max_call_ms, max_gap_ms;
	uint64_t stalls;
};

//! Start (or restart) the accounting at @p now_ns.
void
leia_tfs_init(struct leia_tfs *s, int64_t now_ns);

//! One feed call, from @p start_ns to @p end_ns (monotonic).
void
leia_tfs_on_feed(struct leia_tfs *s, int64_t start_ns, int64_t end_ns);

/*!
 * True once per LEIA_TFS_PERIOD_NS: fills @p out for the window that just
 * ended and starts the next one. The three totals are cumulative counters
 * owned by the caller (frames captured, face polls with a face, face polls).
 */
bool
leia_tfs_report_due(struct leia_tfs *s,
                    int64_t now_ns,
                    uint64_t captured_total,
                    uint64_t faces_total,
                    uint64_t polls_total,
                    struct leia_tfs_report *out);

#ifdef __cplusplus
}
#endif
