// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the SR-readiness rules (leia_sr_ready_rules.h): the
 *         weaver create retry's wake on identification (#468) and the
 *         geometry a pending weaver's DP reports (#469). Hardware-free and
 *         call-free: no SR, no Win32, no panel.
 */

#include "leia_sr_ready_rules.h"

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

static struct leia_desktop_rect
rect(int32_t left, int32_t top, uint32_t w, uint32_t h)
{
	struct leia_desktop_rect r;
	memset(&r, 0, sizeof(r));
	r.valid = true;
	r.left = left;
	r.top = top;
	r.width = w;
	r.height = h;
	return r;
}

static struct leia_panel_identity
panel(uint16_t mfr, uint16_t prod, struct leia_desktop_rect r)
{
	struct leia_panel_identity p;
	memset(&p, 0, sizeof(p));
	p.valid = true;
	p.manufacturer_id = mfr;
	p.product_id = prod;
	p.rect = r;
	return p;
}

// The 10-28 rig: a DS1 left of the laptop panel.
#define DS1_MFR 0x7204
#define DS1_PROD 0x0001

static void
test_wake(void)
{
	// Unidentified at the mark (the panel is still coming back): the count
	// edge wakes, no change does not.
	struct leiasr_identify_mark m = {false, 7};
	CHECK(leiasr_identify_woke(&m, false, 7) == LEIASR_IDENTIFY_WAKE_NONE);
	CHECK(leiasr_identify_woke(&m, true, 7) == LEIASR_IDENTIFY_WAKE_COUNT);

	// Identified at the mark (a failed attempt on an identified panel, or SR
	// keeping the count across an unplug): no edge to see, the wait runs its
	// period — unless verified geometry is published meanwhile.
	m.identified = true;
	CHECK(leiasr_identify_woke(&m, true, 7) == LEIASR_IDENTIFY_WAKE_NONE);
	CHECK(leiasr_identify_woke(&m, true, 8) == LEIASR_IDENTIFY_WAKE_PUBLISH);

	// A publish wins over the edge when both happen in one poll.
	m.identified = false;
	CHECK(leiasr_identify_woke(&m, true, 8) == LEIASR_IDENTIFY_WAKE_PUBLISH);

	// Wrap-around of the publish counter still reads as a change.
	m.publish_epoch = 0xFFFFFFFFu;
	CHECK(leiasr_identify_woke(&m, false, 0) == LEIASR_IDENTIFY_WAKE_PUBLISH);
}

static void
test_identity(void)
{
	const struct leia_panel_identity a = panel(DS1_MFR, DS1_PROD, rect(-3840, 0, 3840, 2160));
	struct leia_panel_identity b = panel(DS1_MFR, DS1_PROD, rect(0, 1600, 3840, 2160));
	CHECK(leia_panel_identity_same(&a, &b)); // moved: same panel

	b = panel(DS1_MFR, DS1_PROD, rect(-3840, 0, 1920, 1080));
	CHECK(!leia_panel_identity_same(&a, &b)); // another desktop mode: as P-c, another panel

	b = panel(DS1_MFR, 0x0002, rect(-3840, 0, 3840, 2160));
	CHECK(!leia_panel_identity_same(&a, &b)); // another product

	b = a;
	b.valid = false;
	CHECK(!leia_panel_identity_same(&a, &b));
	CHECK(!leia_panel_identity_same(&b, &a));
}

static void
test_pick(void)
{
	const struct leia_desktop_rect at_home = rect(-3840, 0, 3840, 2160);
	const struct leia_panel_identity kept_on = panel(DS1_MFR, DS1_PROD, at_home);
	struct leia_panel_identity none;
	memset(&none, 0, sizeof(none));

	// The 10-28 case: the same DS1 is back at its place, SR has not identified
	// it yet (P-c dropped the verified geometry at the unplug). Kept, for the
	// per-screen DP bound there and for a window-bound DP.
	struct leia_panel_identity attached = panel(DS1_MFR, DS1_PROD, at_home);
	CHECK(leiasr_pending_geometry_pick(false, true, &kept_on, &attached, &at_home) ==
	      LEIASR_PENDING_GEOMETRY_KEPT);
	CHECK(leiasr_pending_geometry_pick(false, true, &kept_on, &attached, NULL) == LEIASR_PENDING_GEOMETRY_KEPT);

	// SR identified it, the weaver is not up yet: the verified geometry.
	CHECK(leiasr_pending_geometry_pick(true, true, &kept_on, &attached, &at_home) ==
	      LEIASR_PENDING_GEOMETRY_VERIFIED);

	// Unplugged (no EDID match): nothing, verified or kept.
	CHECK(leiasr_pending_geometry_pick(false, true, &kept_on, &none, &at_home) == LEIASR_PENDING_GEOMETRY_NONE);
	CHECK(leiasr_pending_geometry_pick(true, true, &kept_on, &none, NULL) == LEIASR_PENDING_GEOMETRY_NONE);
	CHECK(leiasr_pending_geometry_pick(false, true, &kept_on, NULL, NULL) == LEIASR_PENDING_GEOMETRY_NONE);

	// Never verified in this process (boot with the panel asleep): nothing
	// is invented.
	CHECK(leiasr_pending_geometry_pick(false, false, &none, &attached, &at_home) == LEIASR_PENDING_GEOMETRY_NONE);

	// Another panel is attached: nothing.
	struct leia_panel_identity other = panel(DS1_MFR, 0x0002, at_home);
	CHECK(leiasr_pending_geometry_pick(false, true, &kept_on, &other, &at_home) == LEIASR_PENDING_GEOMETRY_NONE);

	// The same panel, moved: a window-bound DP reports it (the rect is where
	// it is now); a per-screen DP bound to the OLD rect does not.
	struct leia_panel_identity moved = panel(DS1_MFR, DS1_PROD, rect(0, 1600, 3840, 2160));
	CHECK(leiasr_pending_geometry_pick(false, true, &kept_on, &moved, NULL) == LEIASR_PENDING_GEOMETRY_KEPT);
	CHECK(leiasr_pending_geometry_pick(false, true, &kept_on, &moved, &at_home) == LEIASR_PENDING_GEOMETRY_NONE);

	// Windows' re-seat transient: the panel listed alone at (0, 0). A DP bound
	// to the panel's real rect reports nothing while it lasts — not even the
	// verified geometry.
	struct leia_panel_identity transient = panel(DS1_MFR, DS1_PROD, rect(0, 0, 3840, 2160));
	CHECK(leiasr_pending_geometry_pick(true, true, &kept_on, &transient, &at_home) ==
	      LEIASR_PENDING_GEOMETRY_NONE);
	// (0, 0) is a real origin: a DP bound there while the panel is there gets it.
	const struct leia_desktop_rect at_origin = rect(0, 0, 3840, 2160);
	CHECK(leiasr_pending_geometry_pick(false, true, &kept_on, &transient, &at_origin) ==
	      LEIASR_PENDING_GEOMETRY_KEPT);

	// A per-screen DP whose binding carried no rect is treated as unbound.
	struct leia_desktop_rect no_rect;
	memset(&no_rect, 0, sizeof(no_rect));
	CHECK(leiasr_pending_geometry_pick(false, true, &kept_on, &moved, &no_rect) == LEIASR_PENDING_GEOMETRY_KEPT);
}

static void
test_pending_snapshot(void)
{
	// The getters trust the refresher's snapshot for 500 ms; none yet, or an
	// older one (the refresher stalled or idled out), reads as no panel.
	CHECK(!leiasr_pending_snapshot_fresh(10000, 0));
	CHECK(leiasr_pending_snapshot_fresh(10000, 10000));
	CHECK(leiasr_pending_snapshot_fresh(10000, 10000 - LEIASR_PENDING_REFRESH_MS));
	CHECK(leiasr_pending_snapshot_fresh(10000, 10000 - LEIASR_PENDING_SNAPSHOT_MAX_AGE_MS));
	CHECK(!leiasr_pending_snapshot_fresh(10000, 10000 - LEIASR_PENDING_SNAPSHOT_MAX_AGE_MS - 1));
	// Taken by the refresher just after the reader read its clock.
	CHECK(leiasr_pending_snapshot_fresh(10000, 10001));
	// A refresh period plus a slow EDID enumeration still fits.
	CHECK(LEIASR_PENDING_REFRESH_MS < LEIASR_PENDING_SNAPSHOT_MAX_AGE_MS);

	// The refresher runs while asked within the idle period, then stops.
	CHECK(!leiasr_pending_refresher_wanted(10000, 0));
	CHECK(leiasr_pending_refresher_wanted(10000, 10000));
	CHECK(leiasr_pending_refresher_wanted(10000, 10000 - LEIASR_PENDING_IDLE_STOP_MS + 1));
	CHECK(!leiasr_pending_refresher_wanted(10000, 10000 - LEIASR_PENDING_IDLE_STOP_MS));
	CHECK(leiasr_pending_refresher_wanted(10000, 10002)); // asked after the refresher read its clock
	// It outlives a snapshot: while asked, the snapshot never ages out.
	CHECK(LEIASR_PENDING_IDLE_STOP_MS > LEIASR_PENDING_SNAPSHOT_MAX_AGE_MS);
}

int
main(void)
{
	test_wake();
	test_identity();
	test_pick();
	test_pending_snapshot();
	if (g_failures != 0) {
		fprintf(stderr, "test_sr_ready_rules: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	printf("test_sr_ready_rules: all passed\n");
	return EXIT_SUCCESS;
}
