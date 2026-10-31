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

//! Phone-class front block (rounded): lenses stacked along the sensor's y axis.
static void
make_phone_block(uint8_t *b, size_t len)
{
	memset(b, 0, len);
	b[0] = 1;
	const double main_[12] = {2247.25, 2246.78, 2098.22, 1565.56, -0.02419, -0.01066, -0.000158, -0.000591, 0.01362, 0, 0, 0};
	const double sub_[12] = {2244.79, 2245.12, 2094.78, 1568.07, -0.02302, -0.01037, -0.0000881, -0.000896, 0.01253, 0, 0, 0};
	for (int i = 0; i < 12; i++) {
		wd(b, 1 + 8 * (size_t)i, main_[i]);
		wd(b, 129 + 8 * (size_t)i, sub_[i]);
	}
	const double R[9] = {0.999849, -0.00311775, -0.0170664, 0.00310547, 0.999995, -0.000746275, 0.0170686, 0.000693164, 0.999854};
	for (int i = 0; i < 9; i++) {
		wd(b, 257 + 8 * (size_t)i, R[i]);
	}
	wd(b, 329, 0.367011);
	wd(b, 337, 14.8931);
	wd(b, 345, -0.481515);
}

//! OpenCV RADTAN8 projection of camera-space point P through fx fy cx cy + d.
static void
project(const double k[4], const double d[8], const double P[3], double *u, double *v)
{
	const double x = P[0] / P[2], y = P[1] / P[2], r2 = x * x + y * y;
	const double rad = (1 + d[0] * r2 + d[1] * r2 * r2 + d[4] * r2 * r2 * r2) /
	                   (1 + d[5] * r2 + d[6] * r2 * r2 + d[7] * r2 * r2 * r2);
	const double xd = x * rad + 2 * d[2] * x * y + d[3] * (r2 + 2 * x * x);
	const double yd = y * rad + d[2] * (r2 + 2 * y * y) + 2 * d[3] * x * y;
	*u = k[0] * xd + k[2];
	*v = k[1] * yd + k[3];
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
	CHECK(leia_scam_stream_calib_from_factory(&f, 3264, 2448, 0, 0, 1280, 720, 0, &s));
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
		CHECK(leia_scam_stream_calib_from_factory(&f, 3264, 2448, 0, 0, 1280, 720, 0, &d));
		CHECK(leia_scam_stream_calib_from_factory(&f, 3264, 2448, 1280, 960, 1280, 720, 0, &two));
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
		CHECK(leia_scam_stream_calib_from_factory(&f, 4576, 3168, 1280, 960, 1280, 720, 0, &two));
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
	CHECK(leia_scam_stream_calib_from_factory(&f, 3264, 2448, 0, 0, 1280, 720, 0, &s));
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

	// Phone-class: a vertical pair whose block is expressed at the largest 4:3
	// output (4192x3144), not at the 4576x3168 array.
	{
		uint8_t pb[400];
		make_phone_block(pb, sizeof(pb));
		struct leia_scam_factory_calib pf, pa;
		CHECK(leia_scam_factory_calib_parse(pb, sizeof(pb), &pf));
		CHECK(leia_scam_factory_is_vertical(&pf));
		const uint32_t sizes[] = {4192, 3144, 4160, 3120, 4096, 3072, 1280, 960, 4608, 3456};
		uint32_t rw = 0, rh = 0;
		const double off = leia_scam_pick_calib_reference(&pf, 4576, 3168, sizes, 5, &rw, &rh);
		CHECK(rw == 4192 && rh == 3144);
		CHECK(off < 0.003);
		leia_scam_factory_to_array(&pf, rw, rh, 4576, 3168, &pa);
		// HAL: 4:3 region 4224x3168 of the array, scaled by 4192/4224.
		NEAR(pa.main.fx, 2247.25 * 4224.0 / 4192.0, 1e-6);
		NEAR(pa.main.cx, 2098.22 * 4224.0 / 4192.0 + 176.0, 1e-6);
		NEAR(pa.main.cy, (1565.56 - 1572.0) * 4224.0 / 4192.0 + 1584.0, 1e-6);
		CHECK(fabs(pa.main.cx - 2288.0) < 5.0 && fabs(pa.main.cy - 1584.0) < 8.0);

		// Unrotated, the published pair is vertical; turned by 270 it is side by side.
		struct leia_scam_stream_calib s0, s270, s90;
		CHECK(leia_scam_stream_calib_from_factory(&pa, 4576, 3168, 1280, 960, 1280, 720, 0, &s0));
		CHECK(fabs(s0.T_rl_mm[1]) > fabs(s0.T_rl_mm[0]));
		CHECK(leia_scam_stream_calib_from_factory(&pa, 4576, 3168, 1280, 960, 1280, 720, 270, &s270));
		CHECK(s270.width == 720 && s270.height == 1280);
		CHECK(fabs(s270.T_rl_mm[0]) > 14.0 && fabs(s270.T_rl_mm[1]) < 1.0);
		CHECK(s270.T_rl_mm[0] < 0.0); // right camera at +x of the left one
		CHECK(leia_scam_stream_calib_from_factory(&pa, 4576, 3168, 1280, 960, 1280, 720, 90, &s90));
		CHECK(s90.T_rl_mm[0] < 0.0);
		CHECK(s90.left_is_sub != s270.left_is_sub); // a half turn swaps the lenses
		CHECK(!leia_scam_stream_calib_from_factory(&pa, 4576, 3168, 1280, 960, 1280, 720, 45, &s90));

		// Consistency: a 3D point seen through the unrotated eye, its pixel turned
		// 270 deg clockwise (x' = y, y' = W - 1 - x), equals the projection through
		// the turned calibration of the same point in the turned camera frame.
		const double pts[3][3] = {{120.0, -80.0, 600.0}, {-200.0, 150.0, 450.0}, {10.0, 5.0, 2000.0}};
		for (int e = 0; e < 2; e++) {
			const int lens0 = (e == 0) == s0.left_is_sub;       // 1 = sub lens in s0's eye e
			const int e270 = (lens0 == s270.left_is_sub) ? 0 : 1; // the same lens in s270
			for (int i = 0; i < 3; i++) {
				double u, v, u2, v2;
				project(s0.k[e], s0.d[e], pts[i], &u, &v);
				const double Pr[3] = {pts[i][1], -pts[i][0], pts[i][2]}; // Q_ccw P
				project(s270.k[e270], s270.d[e270], Pr, &u2, &v2);
				NEAR(u2, v, 1e-6);
				NEAR(v2, 1280.0 - 1.0 - u, 1e-6);
			}
		}
		// Extrinsics: x_R = R x_L + T maps consistently after the turn.
		{
			const double P[3] = {50.0, -30.0, 500.0};
			double xr0[3], xr[3], Pl[3] = {P[1], -P[0], P[2]};
			for (int i = 0; i < 3; i++) {
				xr0[i] = s0.T_rl_mm[i];
				xr[i] = s270.T_rl_mm[i];
				for (int j = 0; j < 3; j++) {
					xr0[i] += s0.R_rl[i][j] * P[j];
					xr[i] += s270.R_rl[i][j] * Pl[j];
				}
			}
			// Only when the eye order is the same in both is the right camera the same lens.
			if (s0.left_is_sub == s270.left_is_sub) {
				NEAR(xr[0], xr0[1], 1e-9);
				NEAR(xr[1], -xr0[0], 1e-9);
				NEAR(xr[2], xr0[2], 1e-9);
			}
		}

		// Eye order from the HAL's lens poses (device frame, metres). The
		// phone-class device publishes lens 0 ("sub") at (-15.3, -0.75) mm of lens 1
		// ("main"): sub on the user's left = the front camera's own right, so main
		// is the SBS left eye. Read in this file's convention the block says the
		// opposite after the 270 turn; the poses win and the convention is swapped.
		{
			const double p_main[3] = {0.0, 0.0, 0.0};
			const double p_sub[3] = {-0.01530, -0.00075, 0.0};
			const double p_none[3] = {0.0, 0.0, 0.0};
			CHECK(s270.left_is_sub); // the block alone
			CHECK(leia_scam_pose_sub_side(p_main, p_sub, true, 270, 270) == 1);
			struct leia_scam_factory_calib po = pa;
			CHECK(leia_scam_factory_orient_by_pose(&po, p_main, p_sub, true, 270, 270) == 1);
			struct leia_scam_stream_calib sp;
			CHECK(leia_scam_stream_calib_from_factory(&po, 4576, 3168, 1280, 960, 1280, 720, 270, &sp));
			CHECK(!sp.left_is_sub); // (a) left = main
			CHECK(sp.T_rl_mm[0] < -14.0 && fabs(sp.T_rl_mm[1]) < 1.0);
			NEAR(sp.k[0][0], s270.k[1][0], 1e-9); // the eyes trade places, intrinsics follow
			// Agreeing again is a no-op; swapping twice is the identity.
			CHECK(leia_scam_factory_orient_by_pose(&po, p_main, p_sub, true, 270, 270) == 0);
			struct leia_scam_factory_calib twice = pa;
			leia_scam_factory_swap_convention(&twice);
			leia_scam_factory_swap_convention(&twice);
			for (int i = 0; i < 3; i++) {
				NEAR(twice.T[i], pa.T[i], 1e-4); // the rounded R is orthonormal to ~1e-6
				for (int j = 0; j < 3; j++) {
					NEAR(twice.R[i][j], pa.R[i][j], 1e-12);
				}
			}
			// No poses (both at the origin, the usual "not published"): block unchanged.
			po = pa;
			CHECK(leia_scam_pose_sub_side(p_none, p_none, true, 270, 270) == 0);
			CHECK(leia_scam_factory_orient_by_pose(&po, p_none, p_none, true, 270, 270) == -1);
			CHECK(memcmp(&po, &pa, sizeof(po)) == 0);
			// A pose along the published y axis decides nothing either.
			const double p_y[3] = {0.0, 0.015, 0.0};
			CHECK(leia_scam_pose_sub_side(p_main, p_y, true, 270, 270) == 0);
			CHECK(leia_scam_pose_sub_side(p_main, p_sub, true, 45, 270) == 0);

			// (c) The same block on a front pair mounted the other way up (sensor
			// orientation 90, turned 90): the block alone flips the order relative to
			// 270, the poses decide it the same way for the same physical layout.
			CHECK(leia_scam_pose_sub_side(p_main, p_sub, true, 90, 90) == 1);
			po = pa;
			CHECK(leia_scam_factory_orient_by_pose(&po, p_main, p_sub, true, 90, 90) == 0);
			CHECK(leia_scam_stream_calib_from_factory(&po, 4576, 3168, 1280, 960, 1280, 720, 90, &sp));
			CHECK(!sp.left_is_sub && sp.T_rl_mm[0] < 0.0);
			const double p_sub_r[3] = {0.01530, 0.00075, 0.0}; // mirrored layout
			po = pa;
			CHECK(leia_scam_factory_orient_by_pose(&po, p_main, p_sub_r, true, 90, 90) == 1);
			CHECK(leia_scam_stream_calib_from_factory(&po, 4576, 3168, 1280, 960, 1280, 720, 90, &sp));
			CHECK(sp.left_is_sub && sp.T_rl_mm[0] < 0.0);
			// A rear lens sees the device's +x on its own right: the same offset flips.
			CHECK(leia_scam_pose_sub_side(p_main, p_sub, false, 270, 270) == -1);
		}

		// (b) Tablets: horizontal pairs, never turned. Front (sensor 270): the block
		// puts the sub lens on the left; poses that agree, or no poses, keep it.
		{
			struct leia_scam_factory_calib tb;
			CHECK(leia_scam_factory_calib_parse(blk, sizeof(blk), &tb));
			struct leia_scam_stream_calib st;
			const double z[3] = {0.0, 0.0, 0.0};
			const double below[3] = {0.0, -0.015, 0.0};
			struct leia_scam_factory_calib to = tb;
			CHECK(leia_scam_factory_orient_by_pose(&to, z, z, true, 270, 0) == -1);
			CHECK(leia_scam_factory_orient_by_pose(&to, z, below, true, 270, 0) == 0);
			CHECK(memcmp(&to, &tb, sizeof(to)) == 0);
			CHECK(leia_scam_stream_calib_from_factory(&to, 3264, 2448, 0, 0, 1280, 720, 0, &st));
			CHECK(st.left_is_sub);
			// Rear (sensor 90), main on the left (T = -15 mm): unchanged likewise.
			memcpy(bad, blk, sizeof(bad));
			wd(bad, 329, -14.989);
			CHECK(leia_scam_factory_calib_parse(bad, sizeof(bad), &tb));
			to = tb;
			CHECK(leia_scam_factory_orient_by_pose(&to, z, z, false, 90, 0) == -1);
			CHECK(leia_scam_factory_orient_by_pose(&to, z, below, false, 90, 0) == 0);
			CHECK(leia_scam_stream_calib_from_factory(&to, 3264, 2448, 0, 0, 1280, 720, 0, &st));
			CHECK(!st.left_is_sub);
		}

		// The tablets' blocks centre on their array: the array is kept.
		struct leia_scam_factory_calib tf;
		CHECK(leia_scam_factory_calib_parse(blk, sizeof(blk), &tf));
		CHECK(!leia_scam_factory_is_vertical(&tf));
		const uint32_t tsz[] = {3264, 2448, 3200, 2400};
		leia_scam_pick_calib_reference(&tf, 3264, 2448, tsz, 2, &rw, &rh);
		CHECK(rw == 3264 && rh == 2448);
	}

	// Frame orientation (runtime XR_DXR_stereo_camera v7): what remains after the turn.
	CHECK(leia_scam_frame_orientation_deg(true, 270, 0) == 270); // tablet front, unturned
	CHECK(leia_scam_frame_orientation_deg(true, 90, 0) == 90);   // tablet rear, unturned
	CHECK(leia_scam_frame_orientation_deg(true, 270, 270) == 0); // phone-class front, turned upright
	CHECK(leia_scam_frame_orientation_deg(true, 90, 90) == 0);
	CHECK(leia_scam_frame_orientation_deg(true, 0, 90) == 270); // vertical pair, no usable mounting
	CHECK(leia_scam_frame_orientation_deg(true, 270, 90) == 180);
	CHECK(leia_scam_frame_orientation_deg(true, 0, 0) == 0); // landscape-natural, unturned
	CHECK(leia_scam_frame_orientation_deg(false, 0, 0) == -1);
	CHECK(leia_scam_frame_orientation_deg(true, 45, 0) == -1);
	CHECK(leia_scam_frame_orientation_deg(true, 360, 0) == -1);
	CHECK(leia_scam_frame_orientation_deg(true, 270, 45) == -1);

	CHECK(leia_scam_block_hash(blk, sizeof(blk)) == leia_scam_block_hash(blk, sizeof(blk)));
	CHECK(leia_scam_block_hash(blk, sizeof(blk)) != leia_scam_block_hash(bad, sizeof(bad)));

	if (g_fail == 0) {
		printf("test_stereo_camera_android_calib: all passed\n");
	}
	return g_fail == 0 ? 0 : 1;
}
