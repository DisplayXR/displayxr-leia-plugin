// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Host test for leia_tracking_feed_stats.c (Linux CI): the 10 s rate
 *         report of the stereo camera's feed into the CNSDK shared-camera sink.
 *
 * The two shapes it must tell apart are the ones measured on a tablet: a
 * steady 30 fps feed, and the CNSDK idle throttle (one fast call, then one
 * call that sleeps 500 ms, repeat: ~4 frames/s).
 */

#include "leia_tracking_feed_stats.h"

#include <math.h>
#include <stdio.h>

static int g_fail = 0;
#define CHECK(c)                                                                                                       \
	do {                                                                                                           \
		if (!(c)) {                                                                                            \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                            \
			g_fail++;                                                                                      \
		}                                                                                                      \
	} while (0)
#define NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

#define MS (1000LL * 1000)

static void
test_steady_30fps(void)
{
	struct leia_tfs s;
	struct leia_tfs_report r;
	const int64_t t0 = 1000 * MS;
	leia_tfs_init(&s, t0);
	uint64_t captured = 0, faces = 0, polls = 0;
	int64_t t = t0;
	bool due = false;
	for (int i = 0; i < 400 && !due; i++) {
		t = t0 + (int64_t)i * 33333333LL;
		captured++;
		leia_tfs_on_feed(&s, t, t + 8 * MS);
		faces += 2;
		polls += 2;
		due = leia_tfs_report_due(&s, t + 8 * MS, captured, faces, polls, &r);
	}
	CHECK(due);
	NEAR(r.fed_per_s, 30.0, 0.5);
	NEAR(r.captured_per_s, 30.0, 0.5);
	NEAR(r.faces_per_s, 60.0, 1.0);
	NEAR(r.avg_call_ms, 8.0, 0.01);
	NEAR(r.max_gap_ms, 33.3, 0.1);
	CHECK(r.stalls == 0);
	// Not due again straight away; the next window starts from the last report.
	CHECK(!leia_tfs_report_due(&s, t + 9 * MS, captured, faces, polls, &r));
}

static void
test_engine_throttle(void)
{
	// Camera at 30 fps, engine: processed call 30 ms, then a skipped call that
	// sleeps 500 ms. The slot is latest-wins, so the feeder picks up a frame
	// as soon as each call returns.
	struct leia_tfs s;
	struct leia_tfs_report r;
	const int64_t t0 = 5000 * MS;
	leia_tfs_init(&s, t0);
	int64_t t = t0;
	bool due = false;
	int calls = 0;
	while (!due) {
		const int64_t len = (calls % 2 == 0) ? 30 * MS : 500 * MS;
		leia_tfs_on_feed(&s, t, t + len);
		t += len;
		calls++;
		const uint64_t captured = (uint64_t)((t - t0) / 33333333LL);
		due = leia_tfs_report_due(&s, t, captured, 0, (uint64_t)calls, &r);
	}
	CHECK(r.fed_per_s > 3.0 && r.fed_per_s < 4.5);
	NEAR(r.captured_per_s, 30.0, 0.5);
	CHECK(r.stalls >= 18 && r.stalls <= 20);
	NEAR(r.max_call_ms, 500.0, 0.01);
	NEAR(r.max_gap_ms, 500.0, 0.01);
	CHECK(r.faces_per_s == 0.0);
}

static void
test_no_frames(void)
{
	// The feeder wakes every second with nothing: the report still comes out,
	// with a gap spanning the whole window.
	struct leia_tfs s;
	struct leia_tfs_report r;
	leia_tfs_init(&s, 100 * MS);
	CHECK(!leia_tfs_report_due(&s, 5100 * MS, 0, 0, 0, &r));
	CHECK(leia_tfs_report_due(&s, 10100 * MS, 0, 0, 0, &r));
	CHECK(r.fed_per_s == 0.0);
	NEAR(r.max_gap_ms, 10000.0, 0.01);
	NEAR(r.seconds, 10.0, 1e-9);
}

static void
test_uninitialised_starts_window(void)
{
	struct leia_tfs s = {0};
	struct leia_tfs_report r;
	CHECK(!leia_tfs_report_due(&s, 7 * MS, 100, 5, 9, &r));
	CHECK(s.window_ns == 7 * MS && s.captured_at == 100 && s.faces_at == 5 && s.polls_at == 9);
}

int
main(void)
{
	test_steady_30fps();
	test_engine_throttle();
	test_no_frames();
	test_uninitialised_starts_window();
	if (g_fail != 0) {
		printf("%d check(s) FAILED\n", g_fail);
		return 1;
	}
	printf("test_tracking_feed_stats: all passed\n");
	return 0;
}
