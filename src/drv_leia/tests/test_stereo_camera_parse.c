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
