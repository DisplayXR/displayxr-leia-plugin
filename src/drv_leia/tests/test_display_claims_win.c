// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for multi-screen M0 on Windows: runtime-descriptor ->
 *         claim matching (leia_display_claims_win.h).
 *
 * Hardware-free and host-runnable (the Linux CI job builds it): descriptors
 * and SR displays are hand-built — no SR SDK, no Win32 monitor enumeration.
 * Fixture = this box on 2026-10-07: an AUO B194 laptop panel (SR device AL,
 * FPC-verified) at (0,0) and an Acer DS1 (ACR 0x0001, EDID-only until the
 * SDK reads shm slot 1) at (3840,0).
 */

#include "leia_display_claims_win.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

#define AUO_ID 0xAF06u
#define B194_PROD 0xB194u
#define ACR_ID 29188u /* "ACR" as the table stores it (0x7204) */
#define DS1_PROD 1u
#define LGD_ID 0xE430u /* not in the table */

static bool
table_contains(uint16_t man, uint16_t prod)
{
	// Mirrors leia_edid_table.h for the ids this test uses: AUO B194 is the
	// laptop panel (the "field report" class the frozen table lacks), ACR
	// 0/1 is the DS1.
	return man == ACR_ID && (prod == 0 || prod == 1);
}

static struct xrt_display_descriptor
desc(uint64_t id, uint16_t man, uint16_t prod, int32_t left, int32_t top, bool primary)
{
	struct xrt_display_descriptor d;
	memset(&d, 0, sizeof(d));
	d.struct_size = sizeof(d);
	d.monitor_id = id;
	d.edid_manufacturer = man;
	d.edid_product = prod;
	d.pixel_width = 3840;
	d.pixel_height = 2160;
	d.refresh_mhz = 60000;
	d.screen_left = left;
	d.screen_top = top;
	d.flags = primary ? 1u : 0u;
	return d;
}

static struct leia_win_sr_display
srd(uint64_t id, const char *pnp, uint16_t prod, bool fpc, const char *serial, int32_t left, int32_t top, bool global)
{
	struct leia_win_sr_display s;
	memset(&s, 0, sizeof(s));
	s.display_id = id;
	s.fpc_verified = fpc;
	if (serial != NULL) {
		snprintf(s.serial, sizeof(s.serial), "%s", serial);
	}
	s.manufacturer_id = leia_win_pnp_to_manufacturer_id(pnp);
	s.product_id = prod;
	s.left = left;
	s.top = top;
	s.location_is_desktop_global = global;
	s.native_w = 3840;
	s.native_h = 2160;
	s.refresh_hz = 60.0f;
	snprintf(s.device_name, sizeof(s.device_name), "%s", global ? "\\\\.\\DISPLAYn" : "");
	s.hmonitor = id ^ 0x1000u;
	return s;
}

static void
test_pnp(void)
{
	CHECK(leia_win_pnp_to_manufacturer_id("ACR") == ACR_ID);
	CHECK(leia_win_pnp_to_manufacturer_id("AUO") == AUO_ID);
	CHECK(leia_win_pnp_to_manufacturer_id("acr") == 0);
	CHECK(leia_win_pnp_to_manufacturer_id("") == 0);
	CHECK(leia_win_pnp_to_manufacturer_id(NULL) == 0);
}

/*! This box: SR lists AL FPC_VERIFIED and D1 EDID_ONLY; both desktop-global. */
static void
test_two_panels_new_api(void)
{
	struct xrt_display_descriptor d[2] = {
	    desc(0x8c41, AUO_ID, B194_PROD, 0, 0, true),
	    desc(0xb72e, ACR_ID, DS1_PROD, 3840, 0, false),
	};
	struct leia_win_sr_display s[2] = {
	    srd(0x1, "AUO", B194_PROD, true, "QALA2137AL0011", 0, 0, true),
	    srd(0xe31a65dd1e032b8eull, "ACR", DS1_PROD, false, NULL, 3840, 0, true),
	};
	struct leia_win_claim_inputs in = {
	    .sr_displays = s,
	    .sr_display_count = 2,
	    .table_contains = table_contains,
	    .legacy_table_verified = true,
	    .supported_apis = 0xF,
	};
	struct xrt_display_claim c[4];
	struct leia_win_claim_binding b[4];
	const uint32_t n = leia_win_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 2);
	// Laptop panel: not in the frozen table, but SR lists it FPC-verified.
	CHECK(c[0].monitor_id == 0x8c41);
	CHECK(c[0].confidence == XRT_DISPLAY_CLAIM_VERIFIED);
	CHECK(strcmp(c[0].serial, "QALA2137AL0011") == 0);
	CHECK(c[0].supported_apis == 0xF);
	CHECK(b[0].sr_display_id == 0x1);
	CHECK(b[0].hmonitor == (0x1u ^ 0x1000u));
	// DS1: EDID-only in SR -> EDID confidence, no serial, displayId kept.
	CHECK(c[1].monitor_id == 0xb72e);
	CHECK(c[1].confidence == XRT_DISPLAY_CLAIM_EDID);
	CHECK(c[1].serial[0] == '\0');
	CHECK(b[1].sr_display_id == 0xe31a65dd1e032b8eull);
}

/*! Identical twins without a desktop-global origin: claim both at EDID, bind neither. */
static void
test_twins_ambiguous(void)
{
	struct xrt_display_descriptor d[2] = {
	    desc(0xA, ACR_ID, DS1_PROD, 0, 0, true),
	    desc(0xB, ACR_ID, DS1_PROD, 3840, 0, false),
	};
	struct leia_win_sr_display s[2] = {
	    srd(0x1, "ACR", DS1_PROD, true, "QI0123A", 0, 0, false),
	    srd(0x2, "ACR", DS1_PROD, true, "QI0123B", 0, 0, false),
	};
	struct leia_win_claim_inputs in = {
	    .sr_displays = s, .sr_display_count = 2, .table_contains = table_contains, .supported_apis = 1};
	struct xrt_display_claim c[4];
	struct leia_win_claim_binding b[4];
	const uint32_t n = leia_win_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 2);
	for (uint32_t i = 0; i < n; i++) {
		CHECK(c[i].confidence == XRT_DISPLAY_CLAIM_EDID);
		CHECK(c[i].serial[0] == '\0');
		CHECK(b[i].sr_display_id == 0);
	}
}

/*! Twins WITH desktop-global origins pair by origin, in either list order. */
static void
test_twins_by_origin(void)
{
	struct xrt_display_descriptor d[2] = {
	    desc(0xA, ACR_ID, DS1_PROD, 0, 0, true),
	    desc(0xB, ACR_ID, DS1_PROD, 3840, 0, false),
	};
	struct leia_win_sr_display s[2] = {
	    srd(0x2, "ACR", DS1_PROD, true, "RIGHT", 3840, 0, true),
	    srd(0x1, "ACR", DS1_PROD, true, "LEFT", 0, 0, true),
	};
	struct leia_win_claim_inputs in = {
	    .sr_displays = s, .sr_display_count = 2, .table_contains = table_contains, .supported_apis = 1};
	struct xrt_display_claim c[4];
	struct leia_win_claim_binding b[4];
	const uint32_t n = leia_win_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 2);
	CHECK(strcmp(c[0].serial, "LEFT") == 0 && b[0].sr_display_id == 0x1);
	CHECK(strcmp(c[1].serial, "RIGHT") == 0 && b[1].sr_display_id == 0x2);
}

/*! A non-Leia monitor is never claimed; a table-known panel SR omits is EDID. */
static void
test_unknown_and_table_only(void)
{
	struct xrt_display_descriptor d[3] = {
	    desc(0xA, LGD_ID, 0x1234, 0, 0, true),
	    desc(0xB, ACR_ID, DS1_PROD, 3840, 0, false),
	    desc(0xC, AUO_ID, B194_PROD, 7680, 0, false),
	};
	struct leia_win_sr_display s[1] = {
	    srd(0x1, "AUO", B194_PROD, true, "QALA", 7680, 0, true),
	};
	struct leia_win_claim_inputs in = {
	    .sr_displays = s, .sr_display_count = 1, .table_contains = table_contains, .supported_apis = 1};
	struct xrt_display_claim c[4];
	const uint32_t n = leia_win_compute_claims(d, 3, &in, c, NULL, 4);
	CHECK(n == 2);
	CHECK(c[0].monitor_id == 0xB && c[0].confidence == XRT_DISPLAY_CLAIM_EDID);
	CHECK(c[1].monitor_id == 0xC && c[1].confidence == XRT_DISPLAY_CLAIM_VERIFIED);
}

/*! Older SR (no enumeration): today's behaviour — table hit = VERIFIED, no serial. */
static void
test_legacy(void)
{
	struct xrt_display_descriptor d[2] = {
	    desc(0xA, AUO_ID, B194_PROD, 0, 0, true),
	    desc(0xB, ACR_ID, DS1_PROD, 3840, 0, false),
	};
	struct leia_win_claim_inputs in = {
	    .sr_display_count = -1, .table_contains = table_contains, .legacy_table_verified = true, .supported_apis = 1};
	struct xrt_display_claim c[4];
	struct leia_win_claim_binding b[4];
	uint32_t n = leia_win_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 1);
	CHECK(c[0].monitor_id == 0xB && c[0].confidence == XRT_DISPLAY_CLAIM_VERIFIED && c[0].serial[0] == '\0');
	CHECK(b[0].sr_display_id == 0);

	in.legacy_table_verified = false;
	n = leia_win_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 1 && c[0].confidence == XRT_DISPLAY_CLAIM_EDID);
}

/*! A runtime with a longer descriptor is walked with ITS stride. */
static void
test_stride(void)
{
	struct padded
	{
		struct xrt_display_descriptor d;
		uint64_t extra[3];
	} p[2];
	memset(p, 0, sizeof(p));
	p[0].d = desc(0xA, ACR_ID, DS1_PROD, 0, 0, true);
	p[1].d = desc(0xB, ACR_ID, DS1_PROD, 3840, 0, false);
	p[0].d.struct_size = p[1].d.struct_size = sizeof(struct padded);
	struct leia_win_claim_inputs in = {
	    .sr_display_count = -1, .table_contains = table_contains, .legacy_table_verified = true, .supported_apis = 1};
	struct xrt_display_claim c[4];
	const uint32_t n = leia_win_compute_claims(&p[0].d, 2, &in, c, NULL, 4);
	CHECK(n == 2);
	CHECK(c[0].monitor_id == 0xA && c[1].monitor_id == 0xB);
}

/*
 * SR's own monitor join (srEnumerateMonitors, LeiaSR 1.38.0+2192).
 */

#define HMON_LEFT 0x10001u
#define HMON_RIGHT 0x10002u

//! This process's HMONITOR for a desktop origin (fake MonitorFromPoint).
static uint64_t
fake_monitor_at(int32_t left, int32_t top)
{
	if (top != 0) {
		return 0;
	}
	return left == 0 ? HMON_LEFT : left == 3840 ? HMON_RIGHT : 0;
}

static struct leia_win_sr_monitor
srm(uint64_t hmon, int32_t x, uint16_t man, uint16_t prod, uint64_t sr_display_id)
{
	struct leia_win_sr_monitor m;
	memset(&m, 0, sizeof(m));
	m.sr_display_id = sr_display_id;
	m.hmonitor = hmon;
	snprintf(m.device_name, sizeof(m.device_name), "\\\\.\\DISPLAY%u", (unsigned)(hmon & 0xF));
	m.x = x;
	m.y = 0;
	m.width = 3840;
	m.height = 2160;
	m.native_w = 3840;
	m.native_h = 2160;
	m.is_primary = x == 0;
	m.edid_manufacturer = man;
	m.edid_product = prod;
	return m;
}

/*! Two identical DS1s, no desktop-global origin: AMBIGUOUS before, now both
 *  bound by SR's join — by HMONITOR, and by origin when there is no resolver. */
static void
test_join_twins(void)
{
	struct xrt_display_descriptor d[2] = {
	    desc(0xA, ACR_ID, DS1_PROD, 0, 0, true),
	    desc(0xB, ACR_ID, DS1_PROD, 3840, 0, false),
	};
	struct leia_win_sr_display s[2] = {
	    srd(0x2, "ACR", DS1_PROD, true, "RIGHT", 0, 0, false),
	    srd(0x1, "ACR", DS1_PROD, true, "LEFT", 0, 0, false),
	};
	s[0].width_mm = 597;
	s[0].height_mm = 336;
	// SR lists the monitors in its own order, with a non-SR monitor too.
	struct leia_win_sr_monitor m[3] = {
	    srm(HMON_RIGHT, 3840, ACR_ID, DS1_PROD, 0x2),
	    srm(0x10009u, 7680, LGD_ID, 0x1234, 0),
	    srm(HMON_LEFT, 0, ACR_ID, DS1_PROD, 0x1),
	};
	struct leia_win_claim_inputs in = {.sr_displays = s,
	                                   .sr_display_count = 2,
	                                   .table_contains = table_contains,
	                                   .supported_apis = 1,
	                                   .sr_monitors = m,
	                                   .sr_monitor_count = 3,
	                                   .monitor_at = fake_monitor_at};
	for (int pass = 0; pass < 2; pass++) {
		in.monitor_at = pass == 0 ? fake_monitor_at : NULL; // HMONITOR, then origin
		struct xrt_display_claim c[4];
		struct leia_win_claim_binding b[4];
		const uint32_t n = leia_win_compute_claims(d, 2, &in, c, b, 4);
		CHECK(n == 2);
		CHECK(c[0].monitor_id == 0xA && c[0].confidence == XRT_DISPLAY_CLAIM_VERIFIED);
		CHECK(strcmp(c[0].serial, "LEFT") == 0 && b[0].sr_display_id == 0x1 && b[0].sr_joined);
		CHECK(c[1].monitor_id == 0xB && c[1].confidence == XRT_DISPLAY_CLAIM_VERIFIED);
		CHECK(strcmp(c[1].serial, "RIGHT") == 0 && b[1].sr_display_id == 0x2 && b[1].sr_joined);
		// SR's size + native mode ride along in the binding.
		CHECK(b[1].width_mm == 597 && b[1].height_mm == 336 && b[1].native_w == 3840 && b[1].native_h == 2160);
		CHECK(b[0].width_mm == 0 && b[0].height_mm == 0);
	}
}

/*! A monitor SR's list does not carry falls back to the heuristic; the
 *  joined twin is already taken, so the heuristic now has a unique answer. */
static void
test_join_partial(void)
{
	struct xrt_display_descriptor d[2] = {
	    desc(0xA, ACR_ID, DS1_PROD, 0, 0, true),
	    desc(0xB, ACR_ID, DS1_PROD, 3840, 0, false),
	};
	struct leia_win_sr_display s[2] = {
	    srd(0x1, "ACR", DS1_PROD, true, "LEFT", 0, 0, false),
	    srd(0x2, "ACR", DS1_PROD, true, "RIGHT", 0, 0, false),
	};
	struct leia_win_sr_monitor m[1] = {srm(HMON_RIGHT, 3840, ACR_ID, DS1_PROD, 0x2)};
	struct leia_win_claim_inputs in = {.sr_displays = s,
	                                   .sr_display_count = 2,
	                                   .table_contains = table_contains,
	                                   .supported_apis = 1,
	                                   .sr_monitors = m,
	                                   .sr_monitor_count = 1,
	                                   .monitor_at = fake_monitor_at};
	struct xrt_display_claim c[4];
	struct leia_win_claim_binding b[4];
	uint32_t n = leia_win_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 2);
	CHECK(b[0].sr_display_id == 0x1 && !b[0].sr_joined && strcmp(c[0].serial, "LEFT") == 0);
	CHECK(b[1].sr_display_id == 0x2 && b[1].sr_joined && strcmp(c[1].serial, "RIGHT") == 0);

	// This box's AL + DS1 fixture with only AL in SR's monitor list: DS1
	// takes the heuristic and lands exactly where it did before.
	struct xrt_display_descriptor d2[2] = {
	    desc(0x8c41, AUO_ID, B194_PROD, 0, 0, true),
	    desc(0xb72e, ACR_ID, DS1_PROD, 3840, 0, false),
	};
	struct leia_win_sr_display s2[2] = {
	    srd(0x1, "AUO", B194_PROD, true, "QALA2137AL0011", 0, 0, true),
	    srd(0xe31a65dd1e032b8eull, "ACR", DS1_PROD, false, NULL, 3840, 0, true),
	};
	struct leia_win_sr_monitor m2[1] = {srm(HMON_LEFT, 0, AUO_ID, B194_PROD, 0x1)};
	in.sr_displays = s2;
	in.sr_monitors = m2;
	n = leia_win_compute_claims(d2, 2, &in, c, b, 4);
	CHECK(n == 2);
	CHECK(c[0].confidence == XRT_DISPLAY_CLAIM_VERIFIED && b[0].sr_display_id == 0x1 && b[0].sr_joined);
	CHECK(c[1].confidence == XRT_DISPLAY_CLAIM_EDID && b[1].sr_display_id == 0xe31a65dd1e032b8eull &&
	      !b[1].sr_joined);

	// A row whose EDID ids contradict the descriptor (topology changed
	// between the two enumerations) is ignored: back to AMBIGUOUS twins.
	struct leia_win_sr_monitor m3[2] = {
	    srm(HMON_LEFT, 0, LGD_ID, 0x1234, 0x1),
	    srm(HMON_RIGHT, 3840, LGD_ID, 0x1234, 0x2),
	};
	in.sr_displays = s;
	in.sr_monitors = m3;
	in.sr_monitor_count = 2;
	n = leia_win_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 2);
	CHECK(b[0].sr_display_id == 0 && b[1].sr_display_id == 0);
	CHECK(c[0].confidence == XRT_DISPLAY_CLAIM_EDID && c[1].confidence == XRT_DISPLAY_CLAIM_EDID);

	// An SR display id the display list does not carry: treated as absent.
	struct leia_win_sr_monitor m4[1] = {srm(HMON_LEFT, 0, ACR_ID, DS1_PROD, 0x77)};
	in.sr_monitors = m4;
	in.sr_monitor_count = 1;
	n = leia_win_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 2 && b[0].sr_display_id == 0 && !b[0].sr_joined && b[1].sr_display_id == 0);
}

/*! SR lists the monitor as NOT an SR display: no binding even though an SR
 *  display with the same EDID ids exists (it is off the desktop); the frozen
 *  table still makes it an EDID claim, a non-table monitor is not claimed. */
static void
test_join_not_sr(void)
{
	struct xrt_display_descriptor d[2] = {
	    desc(0xA, ACR_ID, DS1_PROD, 0, 0, true),
	    desc(0xB, AUO_ID, B194_PROD, 3840, 0, false),
	};
	struct leia_win_sr_display s[2] = {
	    srd(0x1, "ACR", DS1_PROD, true, "DETACHED", 0, 0, false),
	    srd(0x2, "AUO", B194_PROD, true, "ALSO_OFF", 0, 0, false),
	};
	struct leia_win_sr_monitor m[2] = {
	    srm(HMON_LEFT, 0, ACR_ID, DS1_PROD, 0),
	    srm(HMON_RIGHT, 3840, AUO_ID, B194_PROD, 0),
	};
	struct leia_win_claim_inputs in = {.sr_displays = s,
	                                   .sr_display_count = 2,
	                                   .table_contains = table_contains,
	                                   .supported_apis = 1,
	                                   .sr_monitors = m,
	                                   .sr_monitor_count = 2,
	                                   .monitor_at = fake_monitor_at};
	struct xrt_display_claim c[4];
	struct leia_win_claim_binding b[4];
	const uint32_t n = leia_win_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 1);
	CHECK(c[0].monitor_id == 0xA && c[0].confidence == XRT_DISPLAY_CLAIM_EDID && c[0].serial[0] == '\0');
	CHECK(b[0].sr_display_id == 0 && !b[0].sr_joined);
}

/*! Join unavailable (count < 0): every fixture's output is byte-identical to
 *  the run with no monitor inputs at all — even with a list pointer set. */
static void
test_join_unavailable(void)
{
	struct xrt_display_descriptor d[3] = {
	    desc(0xA, ACR_ID, DS1_PROD, 0, 0, true),
	    desc(0xB, ACR_ID, DS1_PROD, 3840, 0, false),
	    desc(0xC, AUO_ID, B194_PROD, 7680, 0, false),
	};
	struct leia_win_sr_display s[3] = {
	    srd(0x1, "ACR", DS1_PROD, true, "LEFT", 0, 0, false),
	    srd(0x2, "ACR", DS1_PROD, true, "RIGHT", 0, 0, false),
	    srd(0x3, "AUO", B194_PROD, true, "QALA", 7680, 0, true),
	};
	struct leia_win_sr_monitor m[2] = {
	    srm(HMON_LEFT, 0, ACR_ID, DS1_PROD, 0x1),
	    srm(HMON_RIGHT, 3840, ACR_ID, DS1_PROD, 0x2),
	};
	for (int legacy = 0; legacy < 2; legacy++) {
		struct leia_win_claim_inputs base = {.sr_displays = s,
		                                     .sr_display_count = legacy ? -1 : 3,
		                                     .table_contains = table_contains,
		                                     .legacy_table_verified = true,
		                                     .supported_apis = 1};
		struct leia_win_claim_inputs off = base;
		off.sr_monitors = m;
		off.sr_monitor_count = -1;
		off.monitor_at = fake_monitor_at;

		struct xrt_display_claim c0[4], c1[4];
		struct leia_win_claim_binding b0[4], b1[4];
		memset(c0, 0x5A, sizeof(c0));
		memset(c1, 0x5A, sizeof(c1));
		memset(b0, 0x5A, sizeof(b0));
		memset(b1, 0x5A, sizeof(b1));
		const uint32_t n0 = leia_win_compute_claims(d, 3, &base, c0, b0, 4);
		const uint32_t n1 = leia_win_compute_claims(d, 3, &off, c1, b1, 4);
		CHECK(n0 == n1);
		CHECK(memcmp(c0, c1, sizeof(c0)) == 0);
		CHECK(memcmp(b0, b1, sizeof(b0)) == 0);
		if (!legacy) {
			// Still today's answer: the twins AMBIGUOUS, the AUO by origin.
			CHECK(n0 == 3 && b0[0].sr_display_id == 0 && b0[1].sr_display_id == 0);
			CHECK(b0[2].sr_display_id == 0x3 && !b0[2].sr_joined);
		} else {
			CHECK(n0 == 2); // table hits only
		}
	}
	// SR's display list unavailable but a monitor list present: ignored too.
	struct leia_win_claim_inputs in = {.sr_display_count = -1,
	                                   .table_contains = table_contains,
	                                   .legacy_table_verified = true,
	                                   .supported_apis = 1,
	                                   .sr_monitors = m,
	                                   .sr_monitor_count = 2,
	                                   .monitor_at = fake_monitor_at};
	struct xrt_display_claim c[4];
	struct leia_win_claim_binding b[4];
	const uint32_t n = leia_win_compute_claims(d, 3, &in, c, b, 4);
	CHECK(n == 2 && b[0].sr_display_id == 0 && !b[0].sr_joined && c[0].confidence == XRT_DISPLAY_CLAIM_VERIFIED);
}

static void
test_store(void)
{
	struct leia_win_claim_binding b[2];
	memset(b, 0, sizeof(b));
	b[0].monitor_id = 0xA;
	b[0].sr_display_id = 0x1;
	b[1].monitor_id = 0xB;
	leia_win_claims_store(b, 2);
	struct leia_win_claim_binding out;
	CHECK(leia_win_claims_lookup(0xA, &out) && out.sr_display_id == 0x1);
	CHECK(leia_win_claims_lookup(0xB, NULL));
	CHECK(!leia_win_claims_lookup(0xC, &out));
	leia_win_claims_store(NULL, 0);
	CHECK(!leia_win_claims_lookup(0xA, &out));
}

int
main(void)
{
	test_pnp();
	test_two_panels_new_api();
	test_twins_ambiguous();
	test_twins_by_origin();
	test_unknown_and_table_only();
	test_legacy();
	test_stride();
	test_join_twins();
	test_join_partial();
	test_join_not_sr();
	test_join_unavailable();
	test_store();
	if (g_failures != 0) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("test_display_claims_win: all checks passed\n");
	return 0;
}
