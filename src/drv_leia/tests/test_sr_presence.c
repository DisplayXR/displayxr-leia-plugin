// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for per-display user presence (SR event 20,
 *         leia_sr_presence.h): message parsing, id / serial matching, the
 *         unknown-until-first-present rule, latest-timeUs ordering and the
 *         is_tracking fold. Hardware-free and call-free: no SR loader, no SR
 *         runtime, no panel.
 */

#include "leia_sr_presence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

// Two-screen rig: the laptop panel and a DS1, as SR names them.
#define LAPTOP_ID 81985529216486895ull // 0x0123456789ABCDEF, decimal on the wire
#define DS1_ID 42ull

static bool
parse(const char *s, struct leia_sr_presence_msg *m)
{
	return leia_sr_presence_parse(s, (uint32_t)strlen(s), m);
}

static struct leia_sr_presence_msg
msg(uint64_t id, const char *serial, bool present)
{
	struct leia_sr_presence_msg m;
	memset(&m, 0, sizeof(m));
	m.display_id = id;
	snprintf(m.serial, sizeof(m.serial), "%s", serial);
	m.present = present;
	return m;
}

static void
test_parse(void)
{
	struct leia_sr_presence_msg m;
	CHECK(parse("display=81985529216486895 serial=LP-0042 present=1", &m));
	CHECK(m.display_id == LAPTOP_ID);
	CHECK(strcmp(m.serial, "LP-0042") == 0);
	CHECK(m.present);

	CHECK(parse("display=42 serial=DS1-7 present=0", &m));
	CHECK(m.display_id == DS1_ID && !m.present);

	// id 0 = the runtime cannot name the display; still a valid message.
	CHECK(parse("display=0 serial=DS1-7 present=1", &m));
	CHECK(m.display_id == 0 && m.present);

	// Empty serial is tolerated (the id still matches).
	CHECK(parse("display=42 serial= present=1", &m));
	CHECK(m.display_id == DS1_ID && m.serial[0] == '\0');

	// messageLength excludes the terminator; a NUL / newline after is fine.
	const char with_nul[] = "display=42 serial=X present=1\0garbage";
	CHECK(leia_sr_presence_parse(with_nul, (uint32_t)(sizeof(with_nul) - 1), &m));
	CHECK(parse("display=42 serial=X present=1\n", &m));
	// ...and a message cut by messageLength is read only that far.
	const char *longer = "display=42 serial=X present=1 trailing";
	CHECK(leia_sr_presence_parse(longer, (uint32_t)strlen("display=42 serial=X present=1"), &m));

	// An older runtime passes the service's message through without display=.
	CHECK(!parse("serial=DS1-7 present=1", &m));
	CHECK(!parse("present=1", &m));
	CHECK(!parse("", &m));
	CHECK(!leia_sr_presence_parse(NULL, 10, &m));
	CHECK(!parse("display= serial=X present=1", &m));
	CHECK(!parse("display=4x2 serial=X present=1", &m));
	CHECK(!parse("display=42 serial=X present=2", &m));
	CHECK(!parse("display=42 serial=X present=", &m));
	CHECK(!parse("display=42 serial=X present=1 extra", &m));
	CHECK(!parse("display=99999999999999999999 serial=X present=1", &m)); // > UINT64_MAX
	// A serial longer than the field is refused rather than truncated.
	char big[200];
	snprintf(big, sizeof(big), "display=1 serial=%0100d present=1", 7);
	CHECK(!parse(big, &m));
}

static void
test_unknown_until_first_present(void)
{
	struct leia_sr_presence_state s;
	leia_sr_presence_init(&s, DS1_ID, "DS1-7");
	CHECK(s.state == LEIA_SR_PRESENCE_UNKNOWN);

	// The add-callback replay of a resting tracker: 0 is NOT "viewer lost".
	struct leia_sr_presence_msg m0 = msg(DS1_ID, "DS1-7", false);
	CHECK(!leia_sr_presence_feed(&s, 100, &m0));
	CHECK(s.state == LEIA_SR_PRESENCE_UNKNOWN);

	struct leia_sr_presence_msg m1 = msg(DS1_ID, "DS1-7", true);
	CHECK(leia_sr_presence_feed(&s, 200, &m1));
	CHECK(s.state == LEIA_SR_PRESENCE_PRESENT);

	// Face leaves DS1 -> ABSENT now (the tracker is known to run).
	CHECK(leia_sr_presence_feed(&s, 300, &m0));
	CHECK(s.state == LEIA_SR_PRESENCE_ABSENT);

	// No debounce: a flicker is followed edge for edge.
	CHECK(leia_sr_presence_feed(&s, 301, &m1));
	CHECK(leia_sr_presence_feed(&s, 302, &m0));
	CHECK(s.state == LEIA_SR_PRESENCE_ABSENT);

	// Re-init (the weaver was recreated) forgets the first-present latch.
	leia_sr_presence_init(&s, DS1_ID, "DS1-7");
	CHECK(!leia_sr_presence_feed(&s, 400, &m0));
	CHECK(s.state == LEIA_SR_PRESENCE_UNKNOWN);
}

static void
test_each_screen_follows_its_own_camera(void)
{
	// The bug: blocking the laptop camera must not read "lost" on DS1.
	struct leia_sr_presence_state laptop, ds1;
	leia_sr_presence_init(&laptop, LAPTOP_ID, "LP-0042");
	leia_sr_presence_init(&ds1, DS1_ID, "DS1-7");

	struct leia_sr_presence_msg lp1 = msg(LAPTOP_ID, "LP-0042", true);
	struct leia_sr_presence_msg lp0 = msg(LAPTOP_ID, "LP-0042", false);
	struct leia_sr_presence_msg ds1_1 = msg(DS1_ID, "DS1-7", true);

	// Both see the viewer; every event goes to both monitors (one per DP).
	leia_sr_presence_feed(&laptop, 10, &lp1);
	leia_sr_presence_feed(&ds1, 10, &lp1);
	leia_sr_presence_feed(&laptop, 11, &ds1_1);
	leia_sr_presence_feed(&ds1, 11, &ds1_1);
	CHECK(laptop.state == LEIA_SR_PRESENCE_PRESENT);
	CHECK(ds1.state == LEIA_SR_PRESENCE_PRESENT);

	// Cover the laptop camera.
	leia_sr_presence_feed(&laptop, 20, &lp0);
	leia_sr_presence_feed(&ds1, 20, &lp0);
	CHECK(laptop.state == LEIA_SR_PRESENCE_ABSENT);
	CHECK(ds1.state == LEIA_SR_PRESENCE_PRESENT);

	// The fold: laptop not tracked even though its eye pair is not collapsed;
	// DS1 tracked even if its pair were collapsed by a machine-wide state.
	CHECK(!leia_sr_presence_apply(laptop.state, true));
	CHECK(leia_sr_presence_apply(ds1.state, false));
}

static void
test_latest_time_wins(void)
{
	// The replay (calling thread) can overlap a live event (delivery thread).
	struct leia_sr_presence_state s;
	leia_sr_presence_init(&s, DS1_ID, "");
	struct leia_sr_presence_msg m1 = msg(DS1_ID, "", true);
	struct leia_sr_presence_msg m0 = msg(DS1_ID, "", false);
	CHECK(leia_sr_presence_feed(&s, 500, &m1)); // live
	CHECK(!leia_sr_presence_feed(&s, 400, &m0)); // stale replay arrives after
	CHECK(s.state == LEIA_SR_PRESENCE_PRESENT);
	CHECK(leia_sr_presence_feed(&s, 500, &m0)); // same tick: applied
	CHECK(s.state == LEIA_SR_PRESENCE_ABSENT);
}

static void
test_matching(void)
{
	struct leia_sr_presence_state s;
	leia_sr_presence_init(&s, DS1_ID, "DS1-7");
	struct leia_sr_presence_msg other = msg(LAPTOP_ID, "DS1-7", true);
	// A non-zero id is authoritative: the serial does not override it.
	CHECK(!leia_sr_presence_matches(&s, &other));
	struct leia_sr_presence_msg by_serial = msg(0, "DS1-7", true);
	CHECK(leia_sr_presence_matches(&s, &by_serial));
	struct leia_sr_presence_msg no_key = msg(0, "", true);
	CHECK(!leia_sr_presence_matches(&s, &no_key));

	// A DP that knows neither id nor serial matches nothing.
	leia_sr_presence_init(&s, 0, NULL);
	struct leia_sr_presence_msg zero = msg(0, "", true);
	CHECK(!leia_sr_presence_matches(&s, &zero));
	struct leia_sr_presence_msg any = msg(DS1_ID, "DS1-7", true);
	CHECK(!leia_sr_presence_feed(&s, 1, &any));
	CHECK(s.state == LEIA_SR_PRESENCE_UNKNOWN);

	// An over-long serial is truncated into the field, never overflowed.
	char long_serial[100];
	memset(long_serial, 'S', sizeof(long_serial) - 1);
	long_serial[sizeof(long_serial) - 1] = '\0';
	leia_sr_presence_init(&s, DS1_ID, long_serial);
	CHECK(strlen(s.serial) == LEIA_SR_PRESENCE_SERIAL_MAX - 1);
}

static void
test_apply_unknown_is_todays_heuristic(void)
{
	CHECK(leia_sr_presence_apply(LEIA_SR_PRESENCE_UNKNOWN, true));
	CHECK(!leia_sr_presence_apply(LEIA_SR_PRESENCE_UNKNOWN, false));
	CHECK(leia_sr_presence_apply(LEIA_SR_PRESENCE_PRESENT, false));
	CHECK(!leia_sr_presence_apply(LEIA_SR_PRESENCE_ABSENT, true));
}

int
main(void)
{
	test_parse();
	test_unknown_until_first_present();
	test_each_screen_follows_its_own_camera();
	test_latest_time_wins();
	test_matching();
	test_apply_unknown_is_todays_heuristic();
	if (g_failures != 0) {
		fprintf(stderr, "test_sr_presence: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	printf("test_sr_presence: all passed\n");
	return EXIT_SUCCESS;
}
