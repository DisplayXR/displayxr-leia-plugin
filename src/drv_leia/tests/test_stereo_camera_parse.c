// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Host unit test for leia_stereo_camera_parse (stereo camera source,
 *         runtime ADR-043 L1). No SDK, no Win32: runs on Linux CI and macOS.
 *
 * The calibration text below is SYNTHETIC (made-up numbers in the OpenCV
 * FileStorage layout the SR platform writes), never a real device's file.
 */

#include "leia_stereo_camera_parse.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c)                                                                                                       \
	do {                                                                                                           \
		if (!(c)) {                                                                                            \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                   \
			g_fail++;                                                                                      \
		}                                                                                                      \
	} while (0)
#define NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

static const char k_intr5[] =
    "%YAML:1.0\n"
    "---\n"
    "M1: !!opencv-matrix\n"
    "   rows: 3\n"
    "   cols: 3\n"
    "   dt: d\n"
    "   data: [ 4.3150000000000000e+02, 0., 3.2110000000000000e+02, 0.,\n"
    "       4.2980000000000001e+02, 2.3420000000000002e+02, 0., 0., 1. ]\n"
    "D1: !!opencv-matrix\n"
    "   rows: 1\n"
    "   cols: 5\n"
    "   dt: d\n"
    "   data: [ -8.1000000000000000e-02, 1.2e-01,\n"
    "       -9.0e-04, 4.0e-04,\n"
    "       -5.5e-02 ]\n"
    "M2: !!opencv-matrix\n"
    "   rows: 3\n"
    "   cols: 3\n"
    "   dt: d\n"
    "   data: [ 4.3310000000000002e+02, 0., 3.1790000000000001e+02, 0.,\n"
    "       4.3190000000000001e+02, 2.4060000000000002e+02, 0., 0., 1. ]\n"
    "D2: !!opencv-matrix\n"
    "   rows: 1\n"
    "   cols: 5\n"
    "   dt: d\n"
    "   data: [ -7.4e-02, 9.6e-02, 6.0e-04, -8.0e-04, -4.1e-02 ]\n";

// T.x > 0: camera 2 sits at -x of camera 1 (the case that needs a swap).
static const char k_extr_pos[] =
    "%YAML:1.0\n"
    "---\n"
    "R: !!opencv-matrix\n"
    "   rows: 3\n"
    "   cols: 3\n"
    "   dt: d\n"
    "   data: [ 9.9998000000000000e-01, 1.9999e-03, -5.9990e-03,\n"
    "       -1.9406e-03, 9.9994800000000000e-01, 9.9989e-03,\n"
    "       6.0187e-03, -9.9871e-03, 9.9993200000000000e-01 ]\n"
    "T: !!opencv-matrix\n"
    "   rows: 3\n"
    "   cols: 1\n"
    "   dt: d\n"
    "   data: [ 6.2150000000000006e+01, -3.1e-01, 1.24e+00 ]\n"
    "R1: !!opencv-matrix\n"
    "   rows: 3\n"
    "   cols: 3\n"
    "   dt: d\n"
    "   data: [ 1., 0., 0., 0., 1., 0., 0., 0., 1. ]\n";

static void
test_header_and_jpeg(void)
{
	uint8_t blk[256];
	memset(blk, 0, sizeof(blk));
	struct leia_scam_header h;
	CHECK(leia_scam_header_check(blk, sizeof(blk), &h) == LEIA_SCAM_HEADER_EMPTY);

	uint64_t f[4] = {0, 150, 480, 1280}; // little-endian host assumed below
	memcpy(blk, f, sizeof(f));
	CHECK(leia_scam_header_check(blk, sizeof(blk), &h) == LEIA_SCAM_HEADER_OK);
	CHECK(h.width == 1280 && h.height == 480 && h.compressed_size == 150);
	f[1] = 1000; // past the region
	memcpy(blk, f, sizeof(f));
	CHECK(leia_scam_header_check(blk, sizeof(blk), &h) == LEIA_SCAM_HEADER_BAD);
	f[1] = 150;
	f[0] = 1; // unknown format
	memcpy(blk, f, sizeof(f));
	CHECK(leia_scam_header_check(blk, sizeof(blk), &h) == LEIA_SCAM_HEADER_BAD);
	f[0] = 0;
	f[3] = 1282; // not two even-width eyes
	memcpy(blk, f, sizeof(f));
	CHECK(leia_scam_header_check(blk, sizeof(blk), &h) == LEIA_SCAM_HEADER_BAD);

	// SEC_RESERVE (SR #548): the header COPY is judged against the readable
	// extent (committed prefix, capped at the view capacity), never the mapping.
	uint8_t head[LEIA_SCAM_HEADER_SIZE];
	// SR #548: the header page is committed at creation, all ZERO until the
	// first frame -> "no frame yet" (EMPTY: no copy, no decode, no warning).
	memset(head, 0, sizeof(head));
	CHECK(leia_scam_header_check_extent(head, 4096, &h) == LEIA_SCAM_HEADER_EMPTY);
	CHECK(h.compressed_size == 0 && h.width == 0 && h.height == 0);
	CHECK(leia_scam_header_check_extent(head, LEIA_SCAM_HEADER_SIZE, &h) == LEIA_SCAM_HEADER_EMPTY);
	f[3] = 1280;
	f[1] = 4064; // header + payload = exactly one 4 KiB page
	memcpy(head, f, sizeof(f));
	CHECK(leia_scam_header_check_extent(head, 4096, &h) == LEIA_SCAM_HEADER_OK);
	CHECK(h.compressed_size == 4064);
	CHECK(leia_scam_header_check_extent(head, 4095, &h) == LEIA_SCAM_HEADER_BAD); // one byte uncommitted
	f[1] = 4065;
	memcpy(head, f, sizeof(f));
	CHECK(leia_scam_header_check_extent(head, 4096, &h) == LEIA_SCAM_HEADER_BAD);
	CHECK(leia_scam_header_check_extent(head, 8192, &h) == LEIA_SCAM_HEADER_OK);
	f[1] = (uint64_t)-1; // absurd size never wraps the bound
	memcpy(head, f, sizeof(f));
	CHECK(leia_scam_header_check_extent(head, 8192, &h) == LEIA_SCAM_HEADER_BAD);
	CHECK(leia_scam_header_check_extent(head, 16, &h) == LEIA_SCAM_HEADER_BAD); // header page not readable
	CHECK(leia_scam_header_check_extent(NULL, 8192, &h) == LEIA_SCAM_HEADER_BAD);

	// SOI, APP0 (len 16), SOF0 1280x480 x1 component, SOS.
	const uint8_t jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J',  'F',  'I',  'F',  0x00, 0x01, 0x01,
	                       0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x01,
	                       0xE0, 0x05, 0x00, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xDA, 0x00, 0x08};
	uint32_t w = 0, hh = 0, comps = 0;
	CHECK(leia_scam_jpeg_dims(jpg, sizeof(jpg), &w, &hh, &comps));
	CHECK(w == 1280 && hh == 480 && comps == 1);
	CHECK(!leia_scam_jpeg_dims(jpg + 2, sizeof(jpg) - 2, &w, &hh, &comps));

	uint8_t a[2000], b[2000];
	for (int i = 0; i < 2000; i++) {
		a[i] = b[i] = (uint8_t)(i * 31);
	}
	CHECK(leia_scam_fingerprint(a, sizeof(a)) == leia_scam_fingerprint(b, sizeof(b)));
	b[1000] ^= 1;
	CHECK(leia_scam_fingerprint(a, sizeof(a)) != leia_scam_fingerprint(b, sizeof(b)));
	CHECK(leia_scam_fingerprint(a, sizeof(a)) != leia_scam_fingerprint(a, sizeof(a) - 1));
}

static void
test_serials(void)
{
	uint8_t blk[LEIA_SCAM_SERIAL_MEMORY_SIZE];
	memset(blk, 0, sizeof(blk));
	blk[0] = 2;
	memcpy(blk + 1, "SRX12345678", 11);
	memcpy(blk + 1 + 32, "SECOND-0001 ", 12);
	char s[4][LEIA_SCAM_SERIAL_LEN + 1];
	CHECK(leia_scam_parse_serials(blk, sizeof(blk), s, 4) == 2);
	CHECK(strcmp(s[0], "SRX12345678") == 0);
	CHECK(strcmp(s[1], "SECOND-0001") == 0);
	blk[0] = 0;
	CHECK(leia_scam_parse_serials(blk, sizeof(blk), s, 4) == 0);
	blk[0] = 200; // more than the block holds -> clamped
	CHECK(leia_scam_parse_serials(blk, 1 + 3 * 32, s, 4) == 3);
}

static void
test_section_names(void)
{
	char n[128];
	// Legacy (primary-only) names: no serial.
	CHECK(leia_scam_section_name("Global\\SREyetrackerRawCamera", NULL, n, sizeof(n)));
	CHECK(strcmp(n, "Global\\SREyetrackerRawCamera") == 0);
	CHECK(leia_scam_section_name("Global\\SREyetrackerRawCamera", "", n, sizeof(n)));
	CHECK(strcmp(n, "Global\\SREyetrackerRawCamera") == 0);
	// Per-device: base + "_" + serial, for every object of the family.
	CHECK(leia_scam_section_name("Global\\SREyetrackerRawCamera", "SRX12345678", n, sizeof(n)));
	CHECK(strcmp(n, "Global\\SREyetrackerRawCamera_SRX12345678") == 0);
	CHECK(leia_scam_section_name("Global\\SREyetrackerRawCameraMutex", "SRX12345678", n, sizeof(n)));
	CHECK(strcmp(n, "Global\\SREyetrackerRawCameraMutex_SRX12345678") == 0);
	// A serial that could name another object is refused, never truncated.
	CHECK(!leia_scam_section_name("Global\\X", "a\\b", n, sizeof(n)) && n[0] == '\0');
	CHECK(!leia_scam_section_name("Global\\X", "a b", n, sizeof(n)) && n[0] == '\0');
	CHECK(!leia_scam_section_name("Global\\X", "abc\x01", n, sizeof(n)));
	CHECK(!leia_scam_section_name("Global\\SREyetrackerRawCamera", "SRX12345678", n, 20) && n[0] == '\0');
	// SR #548 isValidCameraImagesSerial: 1-31 of [A-Za-z0-9._-], nothing else.
	CHECK(leia_scam_section_name("B", "Qa.Z_0-9", n, sizeof(n)) && strcmp(n, "B_Qa.Z_0-9") == 0);
	CHECK(!leia_scam_section_name("B", "abc/def", n, sizeof(n)));
	CHECK(!leia_scam_section_name("B", "abc:def", n, sizeof(n)));
	CHECK(!leia_scam_section_name("B", "caf\xc3\xa9", n, sizeof(n)));
	CHECK(leia_scam_section_name("B", "0123456789012345678901234567890", n, sizeof(n))); // 31
	CHECK(!leia_scam_section_name("B", "01234567890123456789012345678901", n, sizeof(n))); // 32
	CHECK(!leia_scam_section_name(NULL, "S", n, sizeof(n)));
	CHECK(!leia_scam_section_name("", "S", n, sizeof(n)));
}

static void
put_row(uint8_t *blk, uint32_t row, const char *serial, uint32_t pid, uint32_t state)
{
	uint8_t *r = blk + (size_t)row * LEIA_SCAM_STATS_ROW_SIZE;
	memset(r, 0, LEIA_SCAM_STATS_ROW_SIZE);
	memcpy(r, serial, strlen(serial));
	for (int i = 0; i < 4; i++) {
		r[LEIA_SCAM_STATS_PID_OFFSET + i] = (uint8_t)(pid >> (8 * i));
		r[LEIA_SCAM_STATS_STATE_OFFSET + i] = (uint8_t)(state >> (8 * i));
	}
}

static bool
pid_alive_not_666(uint32_t pid)
{
	return pid != 666;
}

static void
test_tracker_stats(void)
{
	// SR_TrackerStatsRow: 232 bytes, serial at 0, pid at 32, state at 48.
	_Static_assert(LEIA_SCAM_STATS_MEMORY_SIZE == 8 * 232, "SR_TrackerStatsRow table size");
	uint8_t blk[LEIA_SCAM_STATS_MEMORY_SIZE];
	memset(blk, 0, sizeof(blk));
	put_row(blk, 0, "PRIMARY-0001", 100, 1);
	put_row(blk, 2, "CHILD-0002", 200, 0); // paused: still a live tracker
	put_row(blk, 3, "STALE-0003", 666, 1); // its pid is gone
	put_row(blk, 4, "ZERO-PID", 0, 1);
	put_row(blk, 5, "BADSTATE", 300, 7);
	put_row(blk, 6, "CHILD-0002", 201, 1); // repeat of a serial: first row wins
	put_row(blk, 7, "has space", 400, 1);
	struct leia_scam_tracker t[8];
	uint32_t n = leia_scam_parse_tracker_stats(blk, sizeof(blk), pid_alive_not_666, t, 8);
	CHECK(n == 2);
	CHECK(strcmp(t[0].serial, "PRIMARY-0001") == 0 && t[0].pid == 100 && t[0].streaming);
	CHECK(strcmp(t[1].serial, "CHILD-0002") == 0 && t[1].pid == 200 && !t[1].streaming);
	// No liveness probe: the stale row counts.
	CHECK(leia_scam_parse_tracker_stats(blk, sizeof(blk), NULL, t, 8) == 3);
	// cap, and a short mapping (rows past it are not read).
	CHECK(leia_scam_parse_tracker_stats(blk, sizeof(blk), NULL, t, 1) == 1);
	CHECK(leia_scam_parse_tracker_stats(blk, LEIA_SCAM_STATS_ROW_SIZE * 2, NULL, t, 8) == 1);
	// A 32-byte serial with no NUL is out of contract.
	memset(blk, 0, sizeof(blk));
	memset(blk, 'A', 32);
	blk[LEIA_SCAM_STATS_PID_OFFSET] = 1;
	CHECK(leia_scam_parse_tracker_stats(blk, sizeof(blk), NULL, t, 8) == 0);
}

static bool
has_section_only_ccc(const char *serial)
{
	return strcmp(serial, "CCC-4") == 0;
}

static bool
has_no_section(const char *serial)
{
	(void)serial;
	return false;
}

static void
test_camera_list(void)
{
	struct leia_scam_camera_slot s[LEIA_SCAM_MAX_CAMERAS];
	struct leia_scam_tracker t[6];
	memset(t, 0, sizeof(t));

	// Today's SR (no Stats2 table): exactly the primary.
	CHECK(leia_scam_build_camera_list("SRX1", NULL, -1, NULL, s, 4) == 1);
	CHECK(strcmp(s[0].serial, "SRX1") == 0 && s[0].primary);
	CHECK(leia_scam_build_camera_list("", NULL, -1, NULL, s, 4) == 0);
	CHECK(leia_scam_build_camera_list(NULL, t, -1, NULL, s, 4) == 0);

	// Stats2: primary first, then the others by serial, the primary not repeated.
	snprintf(t[0].serial, sizeof(t[0].serial), "ZZZ-3");
	snprintf(t[1].serial, sizeof(t[1].serial), "SRX1");
	snprintf(t[2].serial, sizeof(t[2].serial), "BBB-2");
	snprintf(t[3].serial, sizeof(t[3].serial), "BBB-2");
	CHECK(leia_scam_build_camera_list("SRX1", t, 4, NULL, s, 4) == 3);
	CHECK(strcmp(s[0].serial, "SRX1") == 0 && s[0].primary);
	CHECK(strcmp(s[1].serial, "BBB-2") == 0 && !s[1].primary);
	CHECK(strcmp(s[2].serial, "ZZZ-3") == 0 && !s[2].primary);

	// The primary has the legacy sections, so it is listed without a Stats2 row.
	CHECK(leia_scam_build_camera_list("SRX1", t, 1, NULL, s, 4) == 2);
	CHECK(strcmp(s[0].serial, "SRX1") == 0 && strcmp(s[1].serial, "ZZZ-3") == 0);
	// An empty table: the primary alone.
	CHECK(leia_scam_build_camera_list("SRX1", t, 0, NULL, s, 4) == 1);
	// No primary known: the live trackers, by serial.
	CHECK(leia_scam_build_camera_list("", t, 3, NULL, s, 4) == 3);
	CHECK(strcmp(s[0].serial, "BBB-2") == 0 && !s[0].primary);

	// Cap: the primary survives, the lowest serials fill the rest.
	snprintf(t[4].serial, sizeof(t[4].serial), "AAA-0");
	snprintf(t[5].serial, sizeof(t[5].serial), "CCC-4");
	CHECK(leia_scam_build_camera_list("SRX1", t, 6, NULL, s, 3) == 3);
	CHECK(strcmp(s[0].serial, "SRX1") == 0 && strcmp(s[1].serial, "AAA-0") == 0 &&
	      strcmp(s[2].serial, "BBB-2") == 0);
	CHECK(leia_scam_build_camera_list("SRX1", t, 6, NULL, s, 99) == LEIA_SCAM_MAX_CAMERAS);

	// A live tracker whose own section does not exist (an SR whose children do
	// not publish yet) is not advertised; the primary never needs one.
	CHECK(leia_scam_build_camera_list("SRX1", t, 6, has_section_only_ccc, s, 4) == 2);
	CHECK(strcmp(s[0].serial, "SRX1") == 0 && s[0].primary && strcmp(s[1].serial, "CCC-4") == 0);
	CHECK(leia_scam_build_camera_list("SRX1", t, 6, has_no_section, s, 4) == 1);
}

static void
test_merge_stable(void)
{
	struct leia_scam_camera_slot reg[LEIA_SCAM_MAX_CAMERAS];
	memset(reg, 0, sizeof(reg));
	struct leia_scam_camera_slot cur[LEIA_SCAM_MAX_CAMERAS];
	bool here[LEIA_SCAM_MAX_CAMERAS];
	memset(cur, 0, sizeof(cur));

	// First enumerate: the registry takes the list as is.
	snprintf(cur[0].serial, sizeof(cur[0].serial), "SRX1");
	cur[0].primary = true;
	snprintf(cur[1].serial, sizeof(cur[1].serial), "BBB-2");
	uint32_t n = leia_scam_merge_stable(reg, 0, 4, cur, 2, here);
	CHECK(n == 2 && strcmp(reg[0].serial, "SRX1") == 0 && reg[0].primary && here[0] && here[1]);

	// The primary moves to BBB-2 and a new device appears: indices hold, the
	// new one is appended, the primary flag follows.
	snprintf(cur[0].serial, sizeof(cur[0].serial), "BBB-2");
	cur[0].primary = true;
	snprintf(cur[1].serial, sizeof(cur[1].serial), "AAA-0");
	cur[1].primary = false;
	snprintf(cur[2].serial, sizeof(cur[2].serial), "SRX1");
	cur[2].primary = false;
	n = leia_scam_merge_stable(reg, n, 4, cur, 3, here);
	CHECK(n == 3);
	CHECK(strcmp(reg[0].serial, "SRX1") == 0 && !reg[0].primary);
	CHECK(strcmp(reg[1].serial, "BBB-2") == 0 && reg[1].primary);
	CHECK(strcmp(reg[2].serial, "AAA-0") == 0 && !reg[2].primary);

	// A device goes away: it keeps its index, reported not present.
	n = leia_scam_merge_stable(reg, n, 4, cur, 1, here);
	CHECK(n == 3 && here[1] && !here[0] && !here[2]);
	CHECK(strcmp(reg[0].serial, "SRX1") == 0 && !reg[0].primary);

	// Full registry: a further device is not given an index.
	snprintf(cur[0].serial, sizeof(cur[0].serial), "D4");
	snprintf(cur[1].serial, sizeof(cur[1].serial), "E5");
	n = leia_scam_merge_stable(reg, n, 4, cur, 2, here);
	CHECK(n == 4 && strcmp(reg[3].serial, "D4") == 0 && here[3]);
}

static void
test_display_names(void)
{
	struct leia_scam_camera_slot c[4];
	memset(c, 0, sizeof(c));
	snprintf(c[0].serial, sizeof(c[0].serial), "SRX1");
	snprintf(c[1].serial, sizeof(c[1].serial), "BBB-2");
	snprintf(c[2].serial, sizeof(c[2].serial), "ZZZ-3");
	snprintf(c[3].serial, sizeof(c[3].serial), "Q4");
	char out[4][LEIA_SCAM_NAME_MAX];

	// A screen name when known, else the serial.
	const char *screens[4] = {"Acer SpatialLabs View", "", NULL, "Odyssey 3D"};
	leia_scam_display_names(c, screens, 4, out);
	CHECK(strcmp(out[0], "Leia SR camera - Acer SpatialLabs View") == 0);
	CHECK(strcmp(out[1], "Leia SR camera BBB-2") == 0);
	CHECK(strcmp(out[2], "Leia SR camera ZZZ-3") == 0);
	CHECK(strcmp(out[3], "Leia SR camera - Odyssey 3D") == 0);

	// Two panels of one model: both get their serial, the third is untouched.
	const char *same[3] = {"Odyssey 3D", "Odyssey 3D", "Other"};
	leia_scam_display_names(c, same, 3, out);
	CHECK(strcmp(out[0], "Leia SR camera - Odyssey 3D (SRX1)") == 0);
	CHECK(strcmp(out[1], "Leia SR camera - Odyssey 3D (BBB-2)") == 0);
	CHECK(strcmp(out[2], "Leia SR camera - Other") == 0);

	// No screens table: serial names, distinct by construction.
	leia_scam_display_names(c, NULL, 2, out);
	CHECK(strcmp(out[0], "Leia SR camera SRX1") == 0 && strcmp(out[1], "Leia SR camera BBB-2") == 0);

	// A screen name too long to fit keeps the serial whole when disambiguated.
	char longname[300];
	memset(longname, 'x', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	const char *lng[2] = {longname, longname};
	leia_scam_display_names(c, lng, 2, out);
	CHECK(strlen(out[0]) == LEIA_SCAM_NAME_MAX - 1 && strlen(out[1]) == LEIA_SCAM_NAME_MAX - 1);
	CHECK(strcmp(out[0] + strlen(out[0]) - 7, " (SRX1)") == 0);
	CHECK(strcmp(out[1] + strlen(out[1]) - 8, " (BBB-2)") == 0);
	CHECK(strcmp(out[0], out[1]) != 0);
}

static void
test_calibration(void)
{
	struct leia_scam_calibration c;
	char why[128] = {0};
	CHECK(leia_scam_parse_calibration(k_intr5, sizeof(k_intr5) - 1, k_extr_pos, sizeof(k_extr_pos) - 1, &c, why,
	                                  sizeof(why)));
	NEAR(c.K[0][0], 431.5, 1e-9);
	NEAR(c.K[0][2], 321.1, 1e-9);
	NEAR(c.K[1][4], 431.9, 1e-9);
	NEAR(c.D[0][4], -0.055, 1e-12);
	NEAR(c.D[1][1], 0.096, 1e-12);
	CHECK(c.d_count[0] == 5 && c.d_count[1] == 5);
	NEAR(c.T[0], 62.15, 1e-9);
	NEAR(c.R[4], 0.999948, 1e-9);

	// Eye order from the sign of T; overrides win.
	CHECK(leia_scam_decide_swap(&c, NULL));
	CHECK(leia_scam_decide_swap(&c, "auto"));
	CHECK(!leia_scam_decide_swap(&c, "0"));
	CHECK(leia_scam_decide_swap(&c, "1"));

	struct leia_scam_eye_calibration e;
	leia_scam_calibration_for_runtime(&c, true, &e);
	CHECK(e.model == 1);
	NEAR(e.k[0][0], 433.1, 1e-9); // eye 0 = camera 2 after the swap
	NEAR(e.k[1][0], 431.5, 1e-9);
	NEAR(e.d[0][0], -0.074, 1e-12);
	// x_R = R' x_L + T' with R' = R^T, T' = -R^T T: the runtime's contract
	// (right camera at +x of the left one => T'.x < 0), same baseline.
	CHECK(e.T[0] < 0.0);
	NEAR(sqrt(e.T[0] * e.T[0] + e.T[1] * e.T[1] + e.T[2] * e.T[2]),
	     sqrt(c.T[0] * c.T[0] + c.T[1] * c.T[1] + c.T[2] * c.T[2]), 1e-3); // R is orthonormal to ~1e-5 here
	NEAR(e.R[0][1], c.R[3], 1e-15);
	// Round trip: applying the swapped extrinsics to a camera-2 point and the
	// originals back gives the point.
	double x2[3] = {0.1, -0.2, 1.5}, x1[3], back[3];
	for (int i = 0; i < 3; i++) {
		x1[i] = e.R[i][0] * x2[0] + e.R[i][1] * x2[1] + e.R[i][2] * x2[2] + e.T[i];
	}
	for (int i = 0; i < 3; i++) {
		back[i] = c.R[i * 3] * x1[0] + c.R[i * 3 + 1] * x1[1] + c.R[i * 3 + 2] * x1[2] + c.T[i];
	}
	for (int i = 0; i < 3; i++) {
		NEAR(back[i], x2[i], 1e-3);
	}
	leia_scam_calibration_for_runtime(&c, false, &e);
	NEAR(e.k[0][0], 431.5, 1e-9);
	NEAR(e.T[0], 62.15, 1e-9);

	// Refusals.
	char bad[4096];
	snprintf(bad, sizeof(bad), "%s", k_extr_pos);
	char *t = strstr(bad, "6.2150000000000006e+01");
	memcpy(t, "6.2150000000000006e-02", 22); // metres, not mm
	CHECK(!leia_scam_parse_calibration(k_intr5, sizeof(k_intr5) - 1, bad, strlen(bad), &c, why, sizeof(why)));
	CHECK(strstr(why, "mm") != NULL);
	snprintf(bad, sizeof(bad), "%s", k_extr_pos);
	t = strstr(bad, "9.9998000000000000e-01");
	memcpy(t, "-9.999800000000000e-01", 22); // reflection
	CHECK(!leia_scam_parse_calibration(k_intr5, sizeof(k_intr5) - 1, bad, strlen(bad), &c, why, sizeof(why)));
	CHECK(!leia_scam_parse_calibration(k_intr5, sizeof(k_intr5) - 1, "%YAML:1.0\n", 10, &c, why, sizeof(why)));
	CHECK(strstr(why, "R") != NULL);

	// 14 coefficients: accepted when the thin-prism / tilt terms are zero.
	char i14[4096];
	snprintf(i14, sizeof(i14), "%s", k_intr5);
	t = strstr(i14, "   cols: 5\n   dt: d\n   data: [ -7.4e-02");
	CHECK(t != NULL);
	const char *d2_14 =
	    "D2: !!opencv-matrix\n   rows: 1\n   cols: 14\n   dt: d\n   data: [ -7.4e-02, 9.6e-02, "
	    "6.0e-04, -8.0e-04, -4.1e-02, 0.01, 0., 0., 0., 0., 0., 0., 0., 0. ]\n";
	char *d2 = strstr(i14, "D2:");
	snprintf(d2, sizeof(i14) - (size_t)(d2 - i14), "%s", d2_14);
	CHECK(leia_scam_parse_calibration(i14, strlen(i14), k_extr_pos, sizeof(k_extr_pos) - 1, &c, why, sizeof(why)));
	CHECK(c.d_count[1] == 8);
	leia_scam_calibration_for_runtime(&c, false, &e);
	CHECK(e.model == 2);
	d2_14 =
	    "D2: !!opencv-matrix\n   rows: 1\n   cols: 14\n   dt: d\n   data: [ -7.4e-02, 9.6e-02, "
	    "6.0e-04, -8.0e-04, -4.1e-02, 0., 0., 0., 0.002, 0., 0., 0., 0., 0. ]\n";
	snprintf(d2, sizeof(i14) - (size_t)(d2 - i14), "%s", d2_14);
	CHECK(!leia_scam_parse_calibration(i14, strlen(i14), k_extr_pos, sizeof(k_extr_pos) - 1, &c, why, sizeof(why)));
}

static void
test_swap_halves(void)
{
	uint8_t img[2 * 8];
	for (int i = 0; i < 16; i++) {
		img[i] = (uint8_t)i;
	}
	leia_scam_swap_halves_gray8(img, 8, 2, 8);
	const uint8_t want[16] = {4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15, 8, 9, 10, 11};
	CHECK(memcmp(img, want, 16) == 0);
}

static void
test_rate_and_pairs(void)
{
	// A 60 Hz source with +-2 ms arrival jitter (the field shape; the old
	// enumerate claimed 30 Hz).
	struct leia_scam_rate r;
	memset(&r, 0, sizeof(r));
	int64_t t = 1000000000;
	int done = 0;
	for (uint32_t i = 0; i <= LEIA_SCAM_RATE_WINDOW; i++) {
		int64_t j = (i == 0 || i == LEIA_SCAM_RATE_WINDOW) ? 0 : ((i & 1) ? 2000000 : -2000000);
		done += leia_scam_rate_push(&r, t + j) ? 1 : 0;
		t += 16666667;
	}
	CHECK(done == 1);
	NEAR(r.rate, 60.0, 0.05);
	// A suspension gap restarts the window.
	struct leia_scam_rate g;
	memset(&g, 0, sizeof(g));
	t = 1000000000;
	for (int i = 0; i < 20; i++, t += 33333333) {
		leia_scam_rate_push(&g, t);
	}
	t += 2000000000;
	for (uint32_t i = 0; i <= LEIA_SCAM_RATE_WINDOW; i++, t += 33333333) {
		leia_scam_rate_push(&g, t);
	}
	NEAR(g.rate, 30.0, 0.05);

	// Pair accounting: repeated predicts of one tracker frame count once.
	struct leia_scam_pair_count c;
	memset(&c, 0, sizeof(c));
	CHECK(leia_scam_pair_count_push(&c, 7, 600.0, 610.0));
	CHECK(!leia_scam_pair_count_push(&c, 7, 600.0, 610.0));
	CHECK(leia_scam_pair_count_push(&c, 8, 0.0, 0.0));     // no face: a frame, but no eyes
	CHECK(leia_scam_pair_count_push(&c, 0, 590.0, 600.0)); // frameId restart is still new
	CHECK(c.polls == 4);
	CHECK(c.new_frames == 3);
	CHECK(c.with_eyes == 2);
}

static void
test_platform_hint(void)
{
	uint16_t vid = 0, pid = 0;
	// Every spelling of the USB id a Windows stack produces.
	CHECK(leia_scam_usb_id("\\\\?\\usb#vid_04f2&pid_b70c&mi_00#6&1a2b3c&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global",
	                       &vid, &pid));
	CHECK(vid == 0x04f2 && pid == 0xb70c);
	CHECK(leia_scam_usb_id("USB\\VID_04F2&PID_B70C&MI_00\\6&1A2B3C&0&0000", &vid, &pid));
	CHECK(vid == 0x04f2 && pid == 0xb70c);
	CHECK(leia_scam_usb_id("04F2:B70C", &vid, &pid));
	CHECK(vid == 0x04f2 && pid == 0xb70c);
	CHECK(!leia_scam_usb_id("\\\\?\\root#camera#0000#{e5323777-f976-4f5b-9b55-b94699c46e44}", &vid, &pid));
	CHECK(!leia_scam_usb_id("vid_04f2&pid_b7", &vid, &pid));
	CHECK(!leia_scam_usb_id(NULL, &vid, &pid));

	const char *tracker = "\\\\?\\usb#vid_04f2&pid_b70c&mi_00#6&1a2b3c&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global";
	const char *webcam = "\\\\?\\usb#vid_0c45&pid_6366&mi_00#6&aaaa&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global";
	char hint[LEIA_SCAM_HINT_MAX];

	// Known VID:PID next to an ordinary webcam: the tracker, as "vvvv:pppp".
	struct leia_scam_video_device a[] = {{webcam, "Integrated Webcam"}, {tracker, "USB Camera"}};
	CHECK(leia_scam_pick_platform_hint(a, 2, hint, sizeof(hint)));
	CHECK(strcmp(hint, "04f2:b70c") == 0);

	// Unknown VID:PID, found by the driver's friendly name.
	struct leia_scam_video_device b[] = {{webcam, "Integrated Webcam"},
	                                     {"\\\\?\\usb#vid_1234&pid_abcd#x#{g}", "Some Tracking Camera"}};
	CHECK(leia_scam_pick_platform_hint(b, 2, hint, sizeof(hint)));
	CHECK(strcmp(hint, "1234:abcd") == 0);

	// The same camera seen under two interface classes is ONE candidate.
	struct leia_scam_video_device c[] = {
	    {tracker, "Tracking Camera"},
	    {"\\\\?\\usb#vid_04f2&pid_b70c&mi_00#6&1a2b3c&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global",
	     "Tracking Camera"}};
	CHECK(leia_scam_pick_platform_hint(c, 2, hint, sizeof(hint)));
	CHECK(strcmp(hint, "04f2:b70c") == 0);

	// Two different matching cameras: ambiguous, hide none.
	struct leia_scam_video_device d[] = {{tracker, NULL}, {"\\\\?\\usb#vid_5555&pid_6666#x#{g}", "tracking camera"}};
	CHECK(!leia_scam_pick_platform_hint(d, 2, hint, sizeof(hint)));
	CHECK(hint[0] == '\0');

	// No candidate at all (a webcam only, or nothing enumerated).
	struct leia_scam_video_device e[] = {{webcam, "Integrated Webcam"}};
	CHECK(!leia_scam_pick_platform_hint(e, 1, hint, sizeof(hint)));
	CHECK(hint[0] == '\0');
	CHECK(!leia_scam_pick_platform_hint(NULL, 0, hint, sizeof(hint)));

	// A non-USB candidate is named by its path; an unnamed one is ambiguous.
	struct leia_scam_video_device f[] = {{"\\\\?\\acpi#int3474#1#{g}", "Tracking Camera"}};
	CHECK(leia_scam_pick_platform_hint(f, 1, hint, sizeof(hint)));
	CHECK(strcmp(hint, "\\\\?\\acpi#int3474#1#{g}") == 0);
	struct leia_scam_video_device g[] = {{NULL, "Tracking Camera"}};
	CHECK(!leia_scam_pick_platform_hint(g, 1, hint, sizeof(hint)));
}

int
main(void)
{
	test_header_and_jpeg();
	test_serials();
	test_section_names();
	test_tracker_stats();
	test_camera_list();
	test_merge_stable();
	test_display_names();
	test_calibration();
	test_swap_halves();
	test_rate_and_pairs();
	test_platform_hint();
	if (g_fail) {
		fprintf(stderr, "test_stereo_camera_parse: %d failure(s)\n", g_fail);
		return 1;
	}
	printf("test_stereo_camera_parse: all checks passed\n");
	return 0;
}
