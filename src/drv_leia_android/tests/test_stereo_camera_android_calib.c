// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Host test for leia_stereo_camera_android_calib.c (Linux CI).
 *
 * The vector is a Lume Pad 2 front-pair block, values rounded (not a byte
 * copy of any one unit): f ~1727 px at 3264x2448, 15 mm baseline, ~0.46 deg
 * of roll between the lenses.
 */

#include "leia_stereo_camera_android_calib.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail = 0;
#define CHECK(c)                                                                                                       \
	do {                                                                                                           \
		if (!(c)) {                                                                                            \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                            \
			g_fail++;                                                                                      \
		}                                                                                                      \
	} while (0)
#define NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

static void
wd(uint8_t *b, size_t off, double v)
{
	uint64_t u;
	memcpy(&u, &v, sizeof(u));
	for (int i = 0; i < 8; i++) {
		b[off + (size_t)i] = (uint8_t)(u >> (8 * i));
	}
}

static void
make_block(uint8_t *b, size_t len)
{
	memset(b, 0, len);
	b[0] = 1;
	const double main_[12] = {1726.8, 1762.4, 1624.2, 1204.5, -0.0216, 9.741, -0.000555, 0.0000857, 23.20, 0, 9.644, 23.43};
	const double sub_[12] = {1724.2, 1759.1, 1628.7, 1209.3, -0.00409, -1.481, 0.000602, 0.000449, 2.410, 0, -1.488, 2.419};
	for (int i = 0; i < 12; i++) {
		wd(b, 1 + 8 * (size_t)i, main_[i]);
		wd(b, 129 + 8 * (size_t)i, sub_[i]);
	}
	// A pure roll of 0.4616 deg about z.
	const double a = 0.4616 * M_PI / 180.0;
	const double R[9] = {cos(a), -sin(a), 0, sin(a), cos(a), 0, 0, 0, 1};
	for (int i = 0; i < 9; i++) {
		wd(b, 257 + 8 * (size_t)i, R[i]);
	}
	wd(b, 329, 14.989);
	wd(b, 337, 0.0);
	wd(b, 345, 0.0);
}

int
main(void)
{
	uint8_t blk[690];
	make_block(blk, sizeof(blk));

	struct leia_scam_factory_calib f;
	CHECK(leia_scam_factory_calib_parse(blk, sizeof(blk), &f));
	NEAR(f.main.fx, 1726.8, 1e-9);
	NEAR(f.sub.cy, 1209.3, 1e-9);
	NEAR(f.main.d[7], 23.43, 1e-12);
	NEAR(leia_scam_factory_baseline_mm(&f), 14.989, 1e-9);

	// Rejections.
	CHECK(!leia_scam_factory_calib_parse(blk, 352, &f));
	uint8_t bad[690];
	// Byte 0 differs per device (1 on Lume Pad 2, 128/27 on nubia Pad 3D II):
	// it must not decide.
	memcpy(bad, blk, sizeof(bad));
	bad[0] = 128;
	CHECK(leia_scam_factory_calib_parse(bad, sizeof(bad), &f));
	memcpy(bad, blk, sizeof(bad));
	wd(bad, 1, 0.0); // fx = 0
	CHECK(!leia_scam_factory_calib_parse(bad, sizeof(bad), &f));
	memcpy(bad, blk, sizeof(bad));
	wd(bad, 257, 2.0); // R not orthonormal
	CHECK(!leia_scam_factory_calib_parse(bad, sizeof(bad), &f));
	memcpy(bad, blk, sizeof(bad));
	wd(bad, 329, 0.1); // baseline 0.1 mm
	CHECK(!leia_scam_factory_calib_parse(bad, sizeof(bad), &f));

	CHECK(leia_scam_factory_calib_parse(blk, sizeof(blk), &f));

	// 1280x720 from the 4:3 3264x2448 array: scale 1280/3264, centre-crop rows.
	struct leia_scam_stream_calib s;
	CHECK(leia_scam_stream_calib_from_factory(&f, 3264, 2448, 0, 0, 1280, 720, &s));
	const double sc = 1280.0 / 3264.0;
	// T = (+15,0,0) puts the sub lens at -x of main: sub = SBS left.
	CHECK(s.left_is_sub);
	NEAR(s.k[0][0], 1724.2 * sc, 1e-9);                       // left = sub
	NEAR(s.k[1][0], 1726.8 * sc, 1e-9);                       // right = main
	NEAR(s.k[1][3], (1204.5 - 1224.0) * sc + 360.0, 1e-9);    // cy with the row crop
	NEAR(s.d[0][1], -1.481, 1e-12);
	// Right camera at +x of left: c = -R_rl^T T_rl must have x = +|T|.
	double cx = 0.0;
	for (int k = 0; k < 3; k++) {
		cx -= s.R_rl[k][0] * s.T_rl_mm[k];
	}
	NEAR(cx, 14.989, 1e-9);
	NEAR(s.T_rl_mm[0], -14.989 * cos(0.4616 * M_PI / 180.0), 1e-9);
	// R_rl = R^T.
	NEAR(s.R_rl[0][1], f.R[1][0], 1e-15);

	// The same with the plug-in's real pipeline on a 4:3 sensor: capture
	// 1280x960, crop the centre 1280x720. Must equal the direct 16:9 stream.
	{
		struct leia_scam_stream_calib d, two;
		CHECK(leia_scam_stream_calib_from_factory(&f, 3264, 2448, 0, 0, 1280, 720, &d));
		CHECK(leia_scam_stream_calib_from_factory(&f, 3264, 2448, 1280, 960, 1280, 720, &two));
		for (int e = 0; e < 2; e++) {
			for (int k = 0; k < 4; k++) {
				NEAR(two.k[e][k], d.k[e][k], 1e-9);
			}
		}
	}
	// A non-4:3 sensor (phone-class panel, 4576x3168): the 1280x960 capture
	// is the centred 4:3 region scaled to the height (crop columns), then the
	// plug-in crops rows to 720. Not the same as 1280x720 straight from the
	// array (scale to the width).
	{
		struct leia_scam_stream_calib two;
		CHECK(leia_scam_stream_calib_from_factory(&f, 4576, 3168, 1280, 960, 1280, 720, &two));
		const double s43 = 960.0 / 3168.0;
		const double main_cx = (1624.2 - 4576.0 / 2.0) * s43 + 640.0; // column crop in step 1
		const double main_cy = 1204.5 * s43 - 120.0;                  // row crop in step 2
		NEAR(two.k[two.left_is_sub ? 1 : 0][0], 1726.8 * s43, 1e-9);
		NEAR(two.k[two.left_is_sub ? 1 : 0][2], main_cx, 1e-9);
		NEAR(two.k[two.left_is_sub ? 1 : 0][3], main_cy, 1e-9);
	}

	// Flipped baseline sign: main is then the left eye and R/T pass through.
	memcpy(bad, blk, sizeof(bad));
	wd(bad, 329, -14.989);
	CHECK(leia_scam_factory_calib_parse(bad, sizeof(bad), &f));
	CHECK(leia_scam_stream_calib_from_factory(&f, 3264, 2448, 0, 0, 1280, 720, &s));
	CHECK(!s.left_is_sub);
	NEAR(s.T_rl_mm[0], -14.989, 1e-12);

	// Tracker intrinsics: 640x480 from 4:3 is a pure scale.
	struct leia_scam_lens t;
	leia_scam_lens_for_stream(&f.main, 3264, 2448, 640, 480, &t);
	NEAR(t.fx, 1726.8 * 640.0 / 3264.0, 1e-9);
	NEAR(t.cy, 1204.5 * 480.0 / 2448.0, 1e-9);
	// Narrower than the array (e.g. 1:1): scale to height, crop columns.
	leia_scam_lens_for_stream(&f.main, 3264, 2448, 480, 480, &t);
	NEAR(t.cx, (1624.2 - 1632.0) * (480.0 / 2448.0) + 240.0, 1e-9);

	CHECK(leia_scam_block_hash(blk, sizeof(blk)) == leia_scam_block_hash(blk, sizeof(blk)));
	CHECK(leia_scam_block_hash(blk, sizeof(blk)) != leia_scam_block_hash(bad, sizeof(bad)));

	if (g_fail == 0) {
		printf("test_stereo_camera_android_calib: all passed\n");
	}
	return g_fail == 0 ? 0 : 1;
}
