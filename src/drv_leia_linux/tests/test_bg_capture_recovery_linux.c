// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the lost-capture re-create schedule (leia_bg_capture_recovery_linux.h, #364).
 */

#include "leia_bg_capture_recovery_linux.h"

#include <stdio.h>

static int g_failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

#define S(x) ((uint64_t)(x) * 1000000000ull)

//! Not recovering: never gates a start, never counts.
static void
test_idle(void)
{
	struct leia_bg_recovery r = {0};
	CHECK(leia_bg_recovery_due(&r, 0));
	CHECK(leia_bg_recovery_attempt_posted(&r) == 0);
	CHECK(leia_bg_recovery_attempt_failed(&r, S(1)) == 0);
	CHECK(!leia_bg_recovery_unlocked(&r, S(1), false));
	CHECK(!r.active);
}

//! Immediately once, then 2, 4, 8, 16, 30, 30, ... s.
static void
test_backoff(void)
{
	struct leia_bg_recovery r = {0};
	uint64_t t = S(100);
	leia_bg_recovery_begin(&r, t);
	CHECK(r.active);
	CHECK(leia_bg_recovery_due(&r, t)); // immediate
	CHECK(leia_bg_recovery_attempt_posted(&r) == 1);

	const uint64_t expect[] = {S(2), S(4), S(8), S(16), S(30), S(30), S(30)};
	for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
		const uint64_t d = leia_bg_recovery_attempt_failed(&r, t);
		CHECK(d == expect[i]);
		CHECK(!leia_bg_recovery_due(&r, t + d - 1));
		CHECK(leia_bg_recovery_due(&r, t + d));
		t += d;
		CHECK(leia_bg_recovery_attempt_posted(&r) == i + 2);
	}
	CHECK(r.active); // never gives up
}

//! An unlock makes the next attempt due now and restarts the backoff.
static void
test_unlock_idle(void)
{
	struct leia_bg_recovery r = {0};
	leia_bg_recovery_begin(&r, S(0));
	leia_bg_recovery_attempt_posted(&r);
	leia_bg_recovery_attempt_failed(&r, S(0)); // 2 s
	leia_bg_recovery_attempt_posted(&r);
	leia_bg_recovery_attempt_failed(&r, S(2)); // 4 s -> due at 6
	CHECK(!leia_bg_recovery_due(&r, S(3)));
	CHECK(leia_bg_recovery_unlocked(&r, S(3), false));
	CHECK(leia_bg_recovery_due(&r, S(3)));
	leia_bg_recovery_attempt_posted(&r);
	CHECK(leia_bg_recovery_attempt_failed(&r, S(3)) == S(2)); // backoff restarted
}

//! An unlock during an in-flight attempt is honoured when that attempt fails.
static void
test_unlock_inflight(void)
{
	struct leia_bg_recovery r = {0};
	leia_bg_recovery_begin(&r, S(0));
	leia_bg_recovery_attempt_posted(&r);
	CHECK(leia_bg_recovery_unlocked(&r, S(1), true));
	CHECK(leia_bg_recovery_attempt_failed(&r, S(2)) == 0);
	CHECK(leia_bg_recovery_due(&r, S(2)));
	leia_bg_recovery_attempt_posted(&r);
	CHECK(leia_bg_recovery_attempt_failed(&r, S(3)) == S(2));
}

//! A second loss while recovering keeps the count and the clock; restore clears.
static void
test_reloss_and_restore(void)
{
	struct leia_bg_recovery r = {0};
	leia_bg_recovery_begin(&r, S(10));
	leia_bg_recovery_attempt_posted(&r);
	leia_bg_recovery_begin(&r, S(20));
	CHECK(r.lost_ns == S(10));
	CHECK(r.attempts == 1);
	leia_bg_recovery_restored(&r);
	CHECK(!r.active);
	CHECK(r.attempts == 0);
	leia_bg_recovery_begin(&r, S(30));
	CHECK(r.lost_ns == S(30));
	CHECK(leia_bg_recovery_due(&r, S(30)));
}

static void
test_log_throttle(void)
{
	for (uint32_t n = 1; n <= 10; n++) {
		CHECK(leia_bg_recovery_should_log(n));
	}
	CHECK(!leia_bg_recovery_should_log(11));
	CHECK(!leia_bg_recovery_should_log(19));
	CHECK(leia_bg_recovery_should_log(20));
	CHECK(leia_bg_recovery_should_log(130));
}

int
main(void)
{
	test_idle();
	test_backoff();
	test_unlock_idle();
	test_unlock_inflight();
	test_reloss_and_restore();
	test_log_throttle();
	if (g_failures != 0) {
		fprintf(stderr, "test_bg_capture_recovery_linux: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_bg_capture_recovery_linux: all checks passed\n");
	return 0;
}
