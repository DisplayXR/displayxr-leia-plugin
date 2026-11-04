// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Rate accounting for the in-app tracking feed. See the header.
 * @ingroup drv_leia_android
 */

#include "leia_tracking_feed_stats.h"

#include <string.h>

void
leia_tfs_init(struct leia_tfs *s, int64_t now_ns)
{
	memset(s, 0, sizeof(*s));
	s->window_ns = now_ns;
}

void
leia_tfs_on_feed(struct leia_tfs *s, int64_t start_ns, int64_t end_ns)
{
	const int64_t call = end_ns > start_ns ? end_ns - start_ns : 0;
	s->fed++;
	s->busy_ns += call;
	if (call > s->max_call_ns) {
		s->max_call_ns = call;
	}
	if (call >= LEIA_TFS_STALL_NS) {
		s->stalls++;
	}
	if (s->last_start_ns != 0 && start_ns - s->last_start_ns > s->max_gap_ns) {
		s->max_gap_ns = start_ns - s->last_start_ns;
	}
	s->last_start_ns = start_ns;
}

bool
leia_tfs_report_due(struct leia_tfs *s,
                    int64_t now_ns,
                    uint64_t captured_total,
                    uint64_t faces_total,
                    uint64_t polls_total,
                    struct leia_tfs_report *out)
{
	if (s->window_ns == 0) {
		leia_tfs_init(s, now_ns);
		s->captured_at = captured_total;
		s->faces_at = faces_total;
		s->polls_at = polls_total;
		return false;
	}
	const int64_t span = now_ns - s->window_ns;
	if (span < LEIA_TFS_PERIOD_NS) {
		return false;
	}
	// A window with no call at all still had one: the gap runs to now.
	int64_t gap = s->max_gap_ns;
	const int64_t since = now_ns - (s->last_start_ns != 0 ? s->last_start_ns : s->window_ns);
	if (since > gap) {
		gap = since;
	}
	const double sec = (double)span / 1e9;
	memset(out, 0, sizeof(*out));
	out->seconds = sec;
	out->captured_per_s = (double)(captured_total - s->captured_at) / sec;
	out->fed_per_s = (double)s->fed / sec;
	out->faces_per_s = (double)(faces_total - s->faces_at) / sec;
	out->polls_per_s = (double)(polls_total - s->polls_at) / sec;
	out->avg_call_ms = s->fed > 0 ? (double)s->busy_ns / (double)s->fed / 1e6 : 0.0;
	out->max_call_ms = (double)s->max_call_ns / 1e6;
	out->max_gap_ms = (double)gap / 1e6;
	out->stalls = s->stalls;

	const int64_t last_start = s->last_start_ns;
	leia_tfs_init(s, now_ns);
	s->last_start_ns = last_start; // gaps keep spanning windows
	s->captured_at = captured_total;
	s->faces_at = faces_total;
	s->polls_at = polls_total;
	return true;
}
