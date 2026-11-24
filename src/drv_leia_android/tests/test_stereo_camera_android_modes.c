// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Host test for leia_stereo_camera_android_modes.c (Linux CI): the
 *         spec v8 mode list on the tablets' numbers, the calibration of every
 *         listed mode against the runtime's derivation rule, the metering
 *         point mapping, the control caps / request plan, and the capture
 *         result conversion.
 *
 * Tablet numbers (Lume Pad 2 class): rear lenses 4208x3120, front lenses
 * 3264x2448; the default eye is the centre 1280x720 of a 1280x960 capture.
 * The rear size list is the one stage 0 read from the NP02J's rear pair
 * (identical for YUV, PRIVATE and JPEG, every size at 33.33 ms); the front
 * list is a typical one for its 3264x2448 lens.
 */

#include "leia_stereo_camera_android_calib.h"
#include "leia_stereo_camera_android_modes.h"

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

/*!
 * The runtime's rule, copied verbatim from displayxr-runtime
 * src/xrt/auxiliary/util/u_stereo_camera.c u_stereo_camera_mode_intrinsics
 * (XR_DXR_stereo_camera v8 §5.5): the intrinsics of a mode from the default's.
 */
static void
runtime_mode_intrinsics(double fx,
                        double fy,
                        double cx,
                        double cy,
                        uint32_t from_w,
                        uint32_t from_h,
                        uint32_t to_w,
                        uint32_t to_h,
                        double out[4])
{
	const double s = from_w > 0 ? (double)to_w / (double)from_w : 1.0;
	out[0] = fx * s;
	out[1] = fy * s;
	out[2] = (cx + 0.5) * s - 0.5;
	out[3] = (cy + 0.5 - (double)from_h * 0.5) * s + (double)to_h * 0.5 - 0.5;
}

static const struct leia_scam_eye_geom kDefault = {1280, 960, 1280, 720, 0};

#define MS(ms) ((int64_t)((ms)*1000000.0))

//! Rear lenses: stage 0's list (docs/STAGE0.md §1), YUV = JPEG.
static const struct leia_scam_size kRearSizes[] = {
    {4208, 3120, MS(33.33)}, {4160, 3120, MS(33.33)}, {4096, 3072, MS(33.33)}, {4080, 3072, MS(33.33)},
    {4000, 3000, MS(33.33)}, {4208, 2630, MS(33.33)}, {2976, 2976, MS(33.33)}, {4096, 2160, MS(33.33)},
    {3840, 2160, MS(33.33)}, {3280, 2460, MS(33.33)}, {3264, 2448, MS(33.33)}, {4080, 1836, MS(33.33)},
    {2592, 1944, MS(33.33)}, {2560, 1920, MS(33.33)}, {2688, 1512, MS(33.33)}, {2304, 1728, MS(33.33)},
    {2048, 1536, MS(33.33)}, {1920, 1440, MS(33.33)}, {1920, 1200, MS(33.33)}, {1920, 1080, MS(33.33)},
    {1600, 1200, MS(33.33)}, {1440, 1080, MS(33.33)}, {1280, 960, MS(33.33)},  {1560, 720, MS(33.33)},
    {1024, 1024, MS(33.33)}, {1280, 720, MS(33.33)},  {1024, 768, MS(33.33)},  {800, 600, MS(33.33)},
    {864, 480, MS(33.33)},   {800, 480, MS(33.33)},   {720, 480, MS(33.33)},   {640, 480, MS(33.33)},
    {640, 360, MS(33.33)},   {352, 288, MS(33.33)},   {320, 240, MS(33.33)},   {176, 144, MS(33.33)},
};
#define N_REAR (sizeof(kRearSizes) / sizeof(kRearSizes[0]))

static const struct leia_scam_size kFrontSizes[] = {
    {3264, 2448, MS(33.33)}, {2560, 1920, MS(33.33)}, {1920, 1440, MS(33.33)}, {1920, 1080, MS(33.33)},
    {1600, 1200, MS(33.33)}, {1280, 960, MS(33.33)},  {1280, 720, MS(33.33)},  {640, 480, MS(33.33)},
};
#define N_FRONT (sizeof(kFrontSizes) / sizeof(kFrontSizes[0]))

//! The .cpp's defaults: rear = reconfigure + JPEG still output, front = neither.
static struct leia_scam_mode_policy
policy(bool shared)
{
	struct leia_scam_mode_policy p;
	memset(&p, 0, sizeof(p));
	p.max_stream_w = shared ? 1280 : 1920;
	p.max_stream_h = shared ? 960 : 1440;
	p.max_still_w = 4096;
	p.reconfigure_stream = !shared;
	p.still_output = !shared;
	p.default_rate = 29.80f;
	p.max_stream_frame_ns = MS(33.4);
	return p;
}

static size_t
build(uint32_t aw, uint32_t ah, const struct leia_scam_eye_geom *def, const struct leia_scam_size *sz, size_t n,
      const struct leia_scam_mode_policy *pol, struct leia_scam_mode *m, size_t cap)
{
	return leia_scam_build_modes(aw, ah, def, sz, n, sz, n, pol, m, cap);
}

static void
wd(uint8_t *b, size_t off, double v)
{
	uint64_t u;
	memcpy(&u, &v, sizeof(u));
	for (int i = 0; i < 8; i++) {
		b[off + (size_t)i] = (uint8_t)(u >> (8 * i));
	}
}

/*!
 * A plausible factory block at @p aw x @p ah (rounded values, not a unit's):
 * principal points a few pixels off centre, a small roll, @p tx mm baseline.
 */
static void
make_block(uint8_t *b, size_t len, double aw, double ah, double f, double tx)
{
	memset(b, 0, len);
	b[0] = 1;
	const double main_[12] = {f, f * 1.02, aw / 2 + 7.8, ah / 2 - 19.5, -0.0216, 9.741, -0.000555, 0.0000857, 23.20, 0, 9.644, 23.43};
	const double sub_[12] = {f * 0.998, f * 1.019, aw / 2 + 12.3, ah / 2 - 14.7, -0.00409, -1.481, 0.000602, 0.000449, 2.410, 0, -1.488, 2.419};
	for (int i = 0; i < 12; i++) {
		wd(b, 1 + 8 * (size_t)i, main_[i]);
		wd(b, 129 + 8 * (size_t)i, sub_[i]);
	}
	const double a = 0.3 * M_PI / 180.0;
	const double R[9] = {cos(a), -sin(a), 0, sin(a), cos(a), 0, 0, 0, 1};
	for (int i = 0; i < 9; i++) {
		wd(b, 257 + 8 * (size_t)i, R[i]);
	}
	wd(b, 329, tx);
	wd(b, 337, 0.12);
	wd(b, 345, -0.05);
}

/*!
 * For every listed mode: the runtime's derivation applied to the default
 * mode's calibration (what the plug-in returns from get_calibration) equals
 * the calibration computed directly for the mode's own capture + crop from
 * the factory block. Distortion and extrinsics are the same for every mode.
 */
static void
check_modes_calibration(const struct leia_scam_factory_calib *f,
                        uint32_t aw,
                        uint32_t ah,
                        const struct leia_scam_mode *modes,
                        size_t n)
{
	struct leia_scam_stream_calib def;
	CHECK(leia_scam_stream_calib_from_factory(f, aw, ah, kDefault.cap_w, kDefault.cap_h, kDefault.crop_w,
	                                          kDefault.crop_h, 0, &def));
	CHECK(def.width == 1280 && def.height == 720);
	for (size_t i = 0; i < n; i++) {
		const struct leia_scam_mode *m = &modes[i];
		struct leia_scam_stream_calib direct;
		CHECK(leia_scam_stream_calib_from_factory(f, aw, ah, m->geom.cap_w, m->geom.cap_h, m->geom.crop_w,
		                                          m->geom.crop_h, m->geom.rot, &direct));
		CHECK(direct.width == m->eye_w && direct.height == m->eye_h);
		CHECK(direct.left_is_sub == def.left_is_sub);
		for (int e = 0; e < 2; e++) {
			double d[4];
			runtime_mode_intrinsics(def.k[e][0], def.k[e][1], def.k[e][2], def.k[e][3], def.width, def.height,
			                        m->eye_w, m->eye_h, d);
			for (int k = 0; k < 4; k++) {
				NEAR(d[k], direct.k[e][k], 1e-9);
			}
			for (int k = 0; k < 8; k++) {
				NEAR(def.d[e][k], direct.d[e][k], 0.0);
			}
		}
		for (int r = 0; r < 3; r++) {
			NEAR(def.T_rl_mm[r], direct.T_rl_mm[r], 0.0);
			for (int c = 0; c < 3; c++) {
				NEAR(def.R_rl[r][c], direct.R_rl[r][c], 0.0);
			}
		}
		// The runtime's own drop rule (§5.5): relative height in [1/2, 2] and
		// the derived principal point inside the frame.
		const double rel = ((double)m->eye_h / m->eye_w) / (720.0 / 1280.0);
		CHECK(rel >= 0.5 && rel <= 2.0);
		for (int e = 0; e < 2; e++) {
			CHECK(direct.k[e][2] > 0 && direct.k[e][2] < m->eye_w && direct.k[e][3] > 0 &&
			      direct.k[e][3] < m->eye_h);
		}
	}
}

static const struct leia_scam_mode *
find(const struct leia_scam_mode *m, size_t n, uint32_t w, uint32_t h)
{
	for (size_t i = 0; i < n; i++) {
		if (m[i].eye_w == w && m[i].eye_h == h) {
			return &m[i];
		}
	}
	return NULL;
}

static void
test_regions(void)
{
	struct leia_scam_region r;
	// 4:3 from the 4208x3120 rear array (aspect 1.349): every row, 4160 centred columns.
	leia_scam_stream_region(4208, 3120, 1280, 960, &r);
	NEAR(r.w, 4160.0, 1e-9);
	NEAR(r.x0, 24.0, 1e-9);
	NEAR(r.h, 3120.0, 1e-9);
	// 16:9 is wider than the array: every column, centred rows.
	leia_scam_stream_region(4208, 3120, 1920, 1080, &r);
	NEAR(r.w, 4208.0, 1e-9);
	NEAR(r.h, 4208.0 * 1080.0 / 1920.0, 1e-9);
	// Same aspect: the whole array.
	leia_scam_stream_region(4208, 3120, 4208, 3120, &r);
	NEAR(r.x0, 0.0, 0.0);
	NEAR(r.w, 4208.0, 0.0);
}

static void
test_rear_modes(void)
{
	const struct leia_scam_mode_policy pol = policy(false);
	struct leia_scam_mode m[LEIA_SCAM_MODES_MAX];
	const size_t n = build(4208, 3120, &kDefault, kRearSizes, N_REAR, &pol, m, LEIA_SCAM_MODES_MAX);
	// The lead's list: default, 1280x960 (same capture), 1920x1080 + 1920x1440
	// (one 1920x1440 capture), and the 4096x3072 still from the JPEG output.
	CHECK(n == 5);
	CHECK(m[0].eye_w == 1280 && m[0].eye_h == 720 && m[0].source == LEIA_SCAM_SRC_DEFAULT_CAPTURE);
	CHECK(m[0].flags == (LEIA_SCAM_MODE_STREAM | LEIA_SCAM_MODE_STILL));
	NEAR(m[0].max_frame_rate, 29.80, 1e-6);
	CHECK(m[1].eye_w == 1280 && m[1].eye_h == 960 && m[1].source == LEIA_SCAM_SRC_DEFAULT_CAPTURE);
	CHECK(m[1].flags == (LEIA_SCAM_MODE_STREAM | LEIA_SCAM_MODE_STILL));
	CHECK(m[1].geom.cap_w == 1280 && m[1].geom.cap_h == 960);
	CHECK(m[2].eye_w == 1920 && m[2].eye_h == 1080 && m[2].flags == LEIA_SCAM_MODE_STREAM);
	CHECK(m[2].source == LEIA_SCAM_SRC_OWN_CAPTURE && m[2].geom.cap_w == 1920 && m[2].geom.cap_h == 1440);
	CHECK(m[2].geom.crop_h == 1080);
	NEAR(m[2].max_frame_rate, 0.0, 0.0); // not measured yet: unknown, never the HAL's claimed 30
	CHECK(m[3].eye_w == 1920 && m[3].eye_h == 1440 && m[3].flags == LEIA_SCAM_MODE_STREAM);
	CHECK(m[3].geom.cap_w == 1920 && m[3].geom.cap_h == 1440);
	CHECK(m[4].eye_w == 4096 && m[4].eye_h == 3072 && m[4].flags == LEIA_SCAM_MODE_STILL);
	CHECK(m[4].source == LEIA_SCAM_SRC_STILL_OUTPUT && m[4].geom.cap_w == 4096);
	// 16:9 sizes see the whole array width: never a mode of their own.
	CHECK(find(m, n, 3840, 2160) == NULL);
	CHECK(leia_scam_find_still_mode(m, n, 0, 0) == 4);
	CHECK(leia_scam_find_still_mode(m, n, 1280, 960) == 1);
	CHECK(leia_scam_find_still_mode(m, n, 1920, 1440) == -1); // a stream mode, not a still size

	// A measured rate of the own capture is reported for its modes.
	{
		struct leia_scam_mode_policy p2 = pol;
		p2.own_rate = 29.80f;
		struct leia_scam_mode m2[LEIA_SCAM_MODES_MAX];
		build(4208, 3120, &kDefault, kRearSizes, N_REAR, &p2, m2, LEIA_SCAM_MODES_MAX);
		NEAR(m2[3].max_frame_rate, 29.80, 1e-6);
	}
	// Without 4096x3072 / under a lower still cap, the largest 4:3 at or under it.
	{
		struct leia_scam_mode_policy p2 = pol;
		p2.max_still_w = 3300;
		struct leia_scam_mode m2[LEIA_SCAM_MODES_MAX];
		const size_t n2 = build(4208, 3120, &kDefault, kRearSizes, N_REAR, &p2, m2, LEIA_SCAM_MODES_MAX);
		CHECK(m2[n2 - 1].eye_w == 3280 && m2[n2 - 1].eye_h == 2460);
		// No cap at all: the 4208x3120 output cropped to the default's 4160 columns,
		// or the listed 4160x3120 (no crop) for the same eye: the smaller output wins.
		p2.max_still_w = 0;
		const size_t n3 = build(4208, 3120, &kDefault, kRearSizes, N_REAR, &p2, m2, LEIA_SCAM_MODES_MAX);
		CHECK(m2[n3 - 1].eye_w == 4160 && m2[n3 - 1].eye_h == 3120 && m2[n3 - 1].geom.cap_w == 4160);
		struct leia_scam_size only[] = {{4208, 3120, MS(33.33)}, {1280, 960, MS(33.33)}};
		const size_t n4 = leia_scam_build_modes(4208, 3120, &kDefault, only, 2, only, 2, &p2, m2,
		                                        LEIA_SCAM_MODES_MAX);
		CHECK(m2[n4 - 1].eye_w == 4160 && m2[n4 - 1].geom.cap_w == 4208 && m2[n4 - 1].geom.crop_w == 4160);
	}
	// debug.dxr.leia.scam_max_stream=3264x2448: the largest full-rate YUV capture.
	{
		struct leia_scam_mode_policy p2 = pol;
		p2.max_stream_w = 3264;
		p2.max_stream_h = 2448;
		struct leia_scam_mode m2[LEIA_SCAM_MODES_MAX];
		const size_t n2 = build(4208, 3120, &kDefault, kRearSizes, N_REAR, &p2, m2, LEIA_SCAM_MODES_MAX);
		const struct leia_scam_mode *a = find(m2, n2, 3264, 2448), *b = find(m2, n2, 3264, 1836);
		CHECK(a != NULL && a->flags == LEIA_SCAM_MODE_STREAM && b != NULL && b->geom.cap_h == 2448);
		CHECK(find(m2, n2, 1920, 1440) == NULL); // ONE further capture only
		// A size whose minimum frame duration is longer than 1/30 s is no stream capture.
		struct leia_scam_size s3[] = {{1920, 1440, MS(40)}, {1280, 960, MS(33.33)}};
		const size_t n3 = leia_scam_build_modes(4208, 3120, &kDefault, s3, 2, NULL, 0, &p2, m2,
		                                        LEIA_SCAM_MODES_MAX);
		CHECK(n3 == 2);
		s3[0].min_frame_duration_ns = 0; // unknown: no either
		CHECK(leia_scam_build_modes(4208, 3120, &kDefault, s3, 2, NULL, 0, &p2, m2, LEIA_SCAM_MODES_MAX) == 2);
	}

	// Calibration: every listed mode, default and raised caps.
	uint8_t blk[400];
	make_block(blk, sizeof(blk), 4208, 3120, 3310.0, -30.03);
	struct leia_scam_factory_calib f;
	CHECK(leia_scam_factory_calib_parse(blk, sizeof(blk), &f));
	check_modes_calibration(&f, 4208, 3120, m, n);
	{
		struct leia_scam_mode_policy p2 = pol;
		p2.max_stream_w = 3264;
		p2.max_stream_h = 2448;
		p2.max_still_w = 0;
		struct leia_scam_mode m2[LEIA_SCAM_MODES_MAX];
		const size_t n2 = build(4208, 3120, &kDefault, kRearSizes, N_REAR, &p2, m2, LEIA_SCAM_MODES_MAX);
		check_modes_calibration(&f, 4208, 3120, m2, n2);
		struct leia_scam_size only[] = {{4208, 3120, MS(33.33)}, {1280, 960, MS(33.33)}};
		const size_t n4 = leia_scam_build_modes(4208, 3120, &kDefault, only, 2, only, 2, &p2, m2,
		                                        LEIA_SCAM_MODES_MAX);
		check_modes_calibration(&f, 4208, 3120, m2, n4); // the column-cropped 4208 output
	}
}

static void
test_front_modes(void)
{
	// Tracking-shared: no reconfigure, no still output -> the default's capture only.
	struct leia_scam_mode_policy pol = policy(true);
	struct leia_scam_mode m[LEIA_SCAM_MODES_MAX];
	size_t n = build(3264, 2448, &kDefault, kFrontSizes, N_FRONT, &pol, m, LEIA_SCAM_MODES_MAX);
	CHECK(n == 2);
	CHECK(m[0].eye_w == 1280 && m[0].eye_h == 720);
	CHECK(m[1].eye_w == 1280 && m[1].eye_h == 960 && m[1].geom.cap_h == 960);
	CHECK(m[1].flags == (LEIA_SCAM_MODE_STREAM | LEIA_SCAM_MODE_STILL));
	// Even a raised stream cap adds nothing on a tracking-shared pair.
	pol.max_stream_w = 3264;
	pol.max_stream_h = 2448;
	CHECK(build(3264, 2448, &kDefault, kFrontSizes, N_FRONT, &pol, m, LEIA_SCAM_MODES_MAX) == 2);

	uint8_t blk[400];
	make_block(blk, sizeof(blk), 3264, 2448, 1726.8, 14.989);
	struct leia_scam_factory_calib f;
	CHECK(leia_scam_factory_calib_parse(blk, sizeof(blk), &f));
	check_modes_calibration(&f, 3264, 2448, m, n);
	// The same front lens if it were not tracking-shared (calibration of every kind of mode).
	pol = policy(false);
	n = build(3264, 2448, &kDefault, kFrontSizes, N_FRONT, &pol, m, LEIA_SCAM_MODES_MAX);
	CHECK(n == 5 && m[4].eye_w == 3264 && m[4].eye_h == 2448);
	check_modes_calibration(&f, 3264, 2448, m, n);
}

static void
test_turned_and_degenerate(void)
{
	// A phone held upright (eyes turned 270): mode 0 only.
	const struct leia_scam_eye_geom turned = {1280, 960, 1280, 720, 270};
	const struct leia_scam_mode_policy pol = policy(false);
	struct leia_scam_mode m[LEIA_SCAM_MODES_MAX];
	CHECK(build(4576, 3168, &turned, kRearSizes, N_REAR, &pol, m, LEIA_SCAM_MODES_MAX) == 1);
	CHECK(m[0].eye_w == 720 && m[0].eye_h == 1280);
	// The default captured directly at 16:9 (no 4:3 listed): its field is the
	// whole 4208 width, so only full-width sizes can be modes.
	const struct leia_scam_eye_geom direct = {1280, 720, 1280, 720, 0};
	const size_t n = build(4208, 3120, &direct, kRearSizes, N_REAR, &pol, m, LEIA_SCAM_MODES_MAX);
	for (size_t i = 1; i < n; i++) {
		struct leia_scam_region r;
		leia_scam_stream_region(4208, 3120, m[i].geom.cap_w, m[i].geom.cap_h, &r);
		NEAR(r.w * m[i].geom.crop_w / m[i].geom.cap_w, 4208.0, 1e-9);
	}
	// Capacity 1: the default alone.
	CHECK(build(4208, 3120, &kDefault, kRearSizes, N_REAR, &pol, m, 1) == 1);
}

//! Copy of pack_eye_turned's src_of (published pixel -> crop pixel, pixel centres).
static void
src_of(uint32_t rot, uint32_t X, uint32_t Y, uint32_t W, uint32_t H, uint32_t *x, uint32_t *y)
{
	if (rot == 90) {
		*x = Y;
		*y = H - 1 - X;
	} else if (rot == 270) {
		*x = W - 1 - Y;
		*y = X;
	} else if (rot == 180) {
		*x = W - 1 - X;
		*y = H - 1 - Y;
	} else {
		*x = X;
		*y = Y;
	}
}

static void
test_metering(void)
{
	double nx, ny;
	// Rear default: the eye's centre is the array's centre.
	CHECK(leia_scam_metering_to_array(0.5, 0.5, 4208, 3120, &kDefault, &nx, &ny));
	NEAR(nx, 0.5, 1e-12);
	NEAR(ny, 0.5, 1e-12);
	// Top-left of the 16:9 eye: 24 columns in (the 4:3 region), 120 capture rows down.
	CHECK(leia_scam_metering_to_array(0.0, 0.0, 4208, 3120, &kDefault, &nx, &ny));
	NEAR(nx * 4208.0, 24.0, 1e-9);
	NEAR(ny * 3120.0, 120.0 * 3120.0 / 960.0, 1e-9);
	// The same scene point in mode 0 and in mode 1 (1280x960) maps to the same array point.
	const struct leia_scam_eye_geom m1 = {1280, 960, 1280, 960, 0};
	double ax, ay;
	CHECK(leia_scam_metering_to_array(0.25, 0.75, 4208, 3120, &kDefault, &nx, &ny));
	CHECK(leia_scam_metering_to_array(0.25, (0.75 * 720.0 + 120.0) / 960.0, 4208, 3120, &m1, &ax, &ay));
	NEAR(nx, ax, 1e-12);
	NEAR(ny, ay, 1e-12);
	// Out-of-range input is clamped, not extrapolated.
	CHECK(leia_scam_metering_to_array(-1.0, 2.0, 4208, 3120, &kDefault, &nx, &ny));
	NEAR(nx * 4208.0, 24.0, 1e-9);

	// Every turn, pixel by pixel: the centre of published pixel (X, Y) maps to
	// the centre of the crop pixel pack_eye_turned reads it from.
	for (uint32_t rot = 0; rot < 360; rot += 90) {
		const struct leia_scam_eye_geom g = {1280, 960, 1280, 720, rot};
		uint32_t ew, eh;
		leia_scam_eye_size(&g, &ew, &eh);
		struct leia_scam_region r;
		leia_scam_stream_region(4576, 3168, 1280, 960, &r);
		const uint32_t pts[4][2] = {{0, 0}, {ew - 1, 0}, {17, eh - 3}, {ew / 2 + 5, eh / 3}};
		for (int i = 0; i < 4; i++) {
			uint32_t x, y;
			src_of(rot, pts[i][0], pts[i][1], g.crop_w, g.crop_h, &x, &y);
			CHECK(leia_scam_metering_to_array((pts[i][0] + 0.5) / ew, (pts[i][1] + 0.5) / eh, 4576, 3168, &g, &nx,
			                                  &ny));
			const double ex = r.x0 + (x + 0.5) * r.w / 1280.0;
			const double ey = r.y0 + (y + 0.5 + 120.0) * r.h / 960.0;
			NEAR(nx * 4576.0, ex, 1e-6);
			NEAR(ny * 3168.0, ey, 1e-6);
		}
	}
	CHECK(!leia_scam_metering_to_array(0.5, 0.5, 4208, 3120, &(struct leia_scam_eye_geom){1280, 960, 1280, 720, 45},
	                                   &nx, &ny));

	// The metering rectangle: centred, 5 % of the short side, kept inside.
	int32_t rg[5];
	leia_scam_metering_region(0.5, 0.5, 4208, 3120, 0.05, rg);
	CHECK(rg[2] - rg[0] == 156 && rg[3] - rg[1] == 156 && rg[4] == 1000);
	CHECK(rg[0] == 2104 - 78 && rg[1] == 1560 - 78);
	leia_scam_metering_region(0.0, 1.0, 4208, 3120, 0.05, rg);
	CHECK(rg[0] == 0 && rg[3] == 3120 && rg[1] == 3120 - 156);
}

static const uint8_t kRearAf[] = {0, 1, 3, 4}; // OFF AUTO CONTINUOUS_VIDEO CONTINUOUS_PICTURE
static const uint8_t kFrontAf[] = {0};

static struct leia_scam_hal_caps
rear_hal(void)
{
	struct leia_scam_hal_caps h;
	memset(&h, 0, sizeof(h));
	h.af_modes = kRearAf;
	h.n_af_modes = sizeof(kRearAf);
	h.min_focus_distance = 9.09f; // ~11 cm (stage 0: logical camera)
	h.have_min_focus_distance = true;
	h.focus_distance_calibration = 1; // APPROXIMATE
	h.manual_sensor = true;
	h.max_regions_ae = 1;
	h.max_regions_af = 1;
	h.ae_comp_min = -12;
	h.ae_comp_max = 12;
	h.ae_comp_step_num = 1;
	h.ae_comp_step_den = 6;
	return h;
}

static void
test_controls(void)
{
	const struct leia_scam_hal_caps rear = rear_hal();
	struct leia_scam_control_caps caps;
	// Default for a logical pair: no HAL AF -> MANUAL + LOCKED only.
	leia_scam_control_caps_from_hal(&rear, false, false, false, &caps);
	CHECK(caps.supported_controls == (LEIA_SCAM_CTL_FOCUS_MODE | LEIA_SCAM_CTL_FOCUS_DISTANCE |
	                                  LEIA_SCAM_CTL_METERING_POINT | LEIA_SCAM_CTL_EXPOSURE_COMPENSATION));
	CHECK(caps.supported_focus_modes == ((1u << LEIA_SCAM_FOCUS_MANUAL) | (1u << LEIA_SCAM_FOCUS_LOCKED)));
	NEAR(caps.min_focus_distance_diopters, 9.09, 1e-6);
	CHECK(caps.focus_distance_calibration == 1);
	NEAR(caps.exposure_compensation_min_ev, -2.0, 1e-6);
	NEAR(caps.exposure_compensation_max_ev, 2.0, 1e-6);
	NEAR(caps.exposure_compensation_step_ev, 1.0 / 6.0, 1e-6);
	// debug.dxr.leia.scam_hal_af=1: every mode.
	{
		struct leia_scam_control_caps c2;
		leia_scam_control_caps_from_hal(&rear, true, false, false, &c2);
		CHECK(c2.supported_focus_modes == ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4)));
	}
	// No MANUAL_SENSOR and no HAL AF: no focus control at all.
	{
		struct leia_scam_hal_caps h = rear;
		h.manual_sensor = false;
		struct leia_scam_control_caps c2;
		leia_scam_control_caps_from_hal(&h, false, false, false, &c2);
		CHECK((c2.supported_controls & (LEIA_SCAM_CTL_FOCUS_MODE | LEIA_SCAM_CTL_FOCUS_DISTANCE)) == 0);
	}
	// The front pair: fixed focus, tracking-shared -> nothing by default.
	{
		struct leia_scam_hal_caps h = rear;
		h.af_modes = kFrontAf;
		h.n_af_modes = 1;
		h.min_focus_distance = 0.0f;
		h.focus_distance_calibration = 0;
		h.max_regions_af = 0;
		struct leia_scam_control_caps c2;
		leia_scam_control_caps_from_hal(&h, false, true, false, &c2);
		CHECK(c2.supported_controls == 0 && c2.supported_focus_modes == 0);
		NEAR(c2.min_focus_distance_diopters, 0.0, 0.0);
		// debug.dxr.leia.scam_front_controls=1: metering + compensation, still no focus.
		leia_scam_control_caps_from_hal(&h, false, true, true, &c2);
		CHECK(c2.supported_controls == (LEIA_SCAM_CTL_METERING_POINT | LEIA_SCAM_CTL_EXPOSURE_COMPENSATION));
		h.ae_comp_min = h.ae_comp_max = 0;
		leia_scam_control_caps_from_hal(&h, false, false, false, &c2);
		CHECK((c2.supported_controls & LEIA_SCAM_CTL_EXPOSURE_COMPENSATION) == 0);
	}

	const int32_t region[5] = {10, 20, 166, 176, 1000};
	struct leia_scam_focus_policy fp = {false, 0.5f, 3.0f};
	struct leia_scam_controls c;
	memset(&c, 0, sizeof(c));
	struct leia_scam_request_plan p;
	uint32_t seen = 0;

	// valid_controls == 0: AF OFF at the default distance (0.5 D = 2 m), other defaults.
	leia_scam_plan_controls(&c, &caps, &rear, &fp, true, region, &seen, &p);
	CHECK(!p.af_mode_default && p.af_mode == LEIA_SCAM_AF_OFF && p.set_focus_distance && !p.af_trigger_start);
	NEAR(p.focus_distance, 0.5, 0.0);
	CHECK(p.regions_default && p.ae_comp_default);

	// MANUAL, dioptres: clamped to [0, min focus distance].
	c.valid_controls = LEIA_SCAM_CTL_FOCUS_MODE | LEIA_SCAM_CTL_FOCUS_DISTANCE;
	c.focus_mode = LEIA_SCAM_FOCUS_MANUAL;
	c.focus_distance_diopters = 2.0f; // 50 cm
	leia_scam_plan_controls(&c, &caps, &rear, &fp, true, region, &seen, &p);
	CHECK(p.af_mode == LEIA_SCAM_AF_OFF && p.set_focus_distance);
	NEAR(p.focus_distance, 2.0, 0.0);
	c.focus_distance_diopters = 12.0f;
	leia_scam_plan_controls(&c, &caps, &rear, &fp, true, region, &seen, &p);
	NEAR(p.focus_distance, 9.09, 1e-6);
	c.focus_distance_diopters = -1.0f;
	leia_scam_plan_controls(&c, &caps, &rear, &fp, true, region, &seen, &p);
	NEAR(p.focus_distance, 0.0, 0.0); // infinity
	// MANUAL without a distance, and LOCKED: stay at the current distance.
	c.valid_controls = LEIA_SCAM_CTL_FOCUS_MODE;
	leia_scam_plan_controls(&c, &caps, &rear, &fp, true, region, &seen, &p);
	CHECK(p.af_mode == LEIA_SCAM_AF_OFF);
	NEAR(p.focus_distance, 3.0, 0.0);
	c.focus_mode = LEIA_SCAM_FOCUS_LOCKED;
	leia_scam_plan_controls(&c, &caps, &rear, &fp, true, region, &seen, &p);
	CHECK(p.af_mode == LEIA_SCAM_AF_OFF && !p.af_trigger_start);
	NEAR(p.focus_distance, 3.0, 0.0);
	// AUTO_ONCE is not advertised without HAL AF: ignored, the default distance stays.
	c.focus_mode = LEIA_SCAM_FOCUS_AUTO_ONCE;
	c.focus_trigger = 7;
	leia_scam_plan_controls(&c, &caps, &rear, &fp, true, region, &seen, &p);
	CHECK(p.af_mode == LEIA_SCAM_AF_OFF && !p.af_trigger_start);
	NEAR(p.focus_distance, 0.5, 0.0);

	// Metering without HAL AF: AE regions only.
	c.valid_controls = LEIA_SCAM_CTL_METERING_POINT;
	leia_scam_plan_controls(&c, &caps, &rear, &fp, true, region, &seen, &p);
	CHECK(!p.regions_default && p.set_ae_regions && !p.set_af_regions && p.region[2] == 166);
	leia_scam_plan_controls(&c, &caps, &rear, &fp, false, region, &seen, &p);
	CHECK(p.regions_default);

	// With HAL AF (opt-in): tap to focus = AUTO + regions + one START per tap.
	{
		struct leia_scam_control_caps hc;
		leia_scam_control_caps_from_hal(&rear, true, false, false, &hc);
		const struct leia_scam_focus_policy hp = {true, 0.5f, 0.0f};
		seen = 0;
		c.valid_controls = LEIA_SCAM_CTL_FOCUS_MODE | LEIA_SCAM_CTL_METERING_POINT;
		c.focus_mode = LEIA_SCAM_FOCUS_AUTO_ONCE;
		c.focus_trigger = 1;
		leia_scam_plan_controls(&c, &hc, &rear, &hp, true, region, &seen, &p);
		CHECK(p.af_mode == LEIA_SCAM_AF_AUTO && p.af_trigger_start && p.set_af_regions && p.set_ae_regions);
		leia_scam_plan_controls(&c, &hc, &rear, &hp, true, region, &seen, &p);
		CHECK(!p.af_trigger_start); // re-sent unchanged: no new search
		c.focus_trigger = 2;
		leia_scam_plan_controls(&c, &hc, &rear, &hp, true, region, &seen, &p);
		CHECK(p.af_trigger_start);
		c.focus_mode = LEIA_SCAM_FOCUS_CONTINUOUS_AUTO;
		leia_scam_plan_controls(&c, &hc, &rear, &hp, true, region, &seen, &p);
		CHECK(p.af_mode == LEIA_SCAM_AF_CONTINUOUS_PICTURE);
		memset(&c, 0, sizeof(c));
		leia_scam_plan_controls(&c, &hc, &rear, &hp, true, region, &seen, &p);
		CHECK(p.af_mode_default && !p.set_focus_distance); // HAL AF: the template's own AF mode
	}

	// Exposure compensation: rounded to the 1/6 EV step, clamped.
	memset(&c, 0, sizeof(c));
	c.valid_controls = LEIA_SCAM_CTL_EXPOSURE_COMPENSATION;
	c.exposure_compensation_ev = 0.9f;
	leia_scam_plan_controls(&c, &caps, &rear, &fp, true, region, &seen, &p);
	CHECK(!p.ae_comp_default && p.ae_comp_index == 5);
	c.exposure_compensation_ev = -5.0f;
	leia_scam_plan_controls(&c, &caps, &rear, &fp, true, region, &seen, &p);
	CHECK(p.ae_comp_index == -12);
	CHECK(leia_scam_ev_to_index(1.0f / 3.0f, &rear) == 2);

	// A camera with no controls (the front): nothing applied, the template stays.
	{
		struct leia_scam_control_caps none;
		memset(&none, 0, sizeof(none));
		c.valid_controls = LEIA_SCAM_CTL_FOCUS_MODE | LEIA_SCAM_CTL_EXPOSURE_COMPENSATION;
		c.focus_mode = LEIA_SCAM_FOCUS_MANUAL;
		leia_scam_plan_controls(&c, &none, &rear, &fp, true, region, &seen, &p);
		CHECK(p.af_mode_default && p.ae_comp_default && p.regions_default && !p.set_focus_distance);
	}
}

static void
test_results(void)
{
	struct leia_scam_hal_result left, right, logical;
	memset(&left, 0, sizeof(left));
	memset(&right, 0, sizeof(right));
	memset(&logical, 0, sizeof(logical));
	left.have_exposure = true;
	left.exposure_ns = 8000000;
	left.have_iso = true;
	left.iso = 400;
	left.have_focus_distance = true;
	left.focus_distance = 2.004f;
	left.have_lens_state = right.have_lens_state = true;
	left.have_timestamp = right.have_timestamp = true;
	left.timestamp = right.timestamp = 5000;
	logical.have_focus_distance = true;
	logical.focus_distance = 1.984f; // the logical result is the one reported
	logical.have_exposure = true;
	logical.exposure_ns = 1; // the left lens's own value wins
	struct leia_scam_capture_state s;
	leia_scam_capture_state_from_hal(&left, &right, &logical, false, false, false, false, 1000, &s);
	CHECK(s.valid_fields == (LEIA_SCAM_RES_FOCUS_DISTANCE | LEIA_SCAM_RES_FOCUS_STATE | LEIA_SCAM_RES_EXPOSURE_TIME |
	                         LEIA_SCAM_RES_SENSITIVITY)); // no eye times: not independent
	CHECK(s.exposure_time_ns == 8000000 && s.sensitivity_iso == 400 && s.focus_state == LEIA_SCAM_FOCUS_STATE_LOCKED);
	NEAR(s.focus_distance_diopters, 1.984, 1e-6);
	// Either lens MOVING: SEARCHING.
	right.lens_state = 1;
	leia_scam_capture_state_from_hal(&left, &right, &logical, false, false, false, false, 0, &s);
	CHECK(s.focus_state == LEIA_SCAM_FOCUS_STATE_SEARCHING);
	// One lens's state alone says nothing about the pair.
	leia_scam_capture_state_from_hal(&left, NULL, &logical, false, false, false, false, 0, &s);
	CHECK((s.valid_fields & LEIA_SCAM_RES_FOCUS_STATE) == 0);
	// Independent stamps: per-eye times, converted to the runtime clock.
	right.timestamp = 5300;
	leia_scam_capture_state_from_hal(&left, &right, &logical, false, false, false, true, 1000, &s);
	CHECK((s.valid_fields & LEIA_SCAM_RES_EYE_TIMES) != 0 && s.eye_time_ns[0] == 4000 && s.eye_time_ns[1] == 4300);
	// HAL AF (opt-in): the logical AF state.
	logical.have_af_state = true;
	logical.af_state = 3; // ACTIVE_SCAN
	leia_scam_capture_state_from_hal(NULL, NULL, &logical, false, true, false, false, 0, &s);
	CHECK(s.focus_state == LEIA_SCAM_FOCUS_STATE_SEARCHING);
	// Nothing reported: no bits.
	leia_scam_capture_state_from_hal(NULL, NULL, NULL, false, false, false, true, 0, &s);
	CHECK(s.valid_fields == 0);
	// Fixed focus (the front lenses): FIXED.
	leia_scam_capture_state_from_hal(NULL, NULL, &logical, true, false, false, false, 0, &s);
	CHECK((s.valid_fields & LEIA_SCAM_RES_FOCUS_STATE) != 0 && s.focus_state == LEIA_SCAM_FOCUS_STATE_FIXED);

	// AF states.
	CHECK(leia_scam_focus_state_from_af(0, false) == LEIA_SCAM_FOCUS_STATE_UNKNOWN);
	CHECK(leia_scam_focus_state_from_af(0, true) == LEIA_SCAM_FOCUS_STATE_LOCKED);
	CHECK(leia_scam_focus_state_from_af(2, false) == LEIA_SCAM_FOCUS_STATE_LOCKED);
	CHECK(leia_scam_focus_state_from_af(5, false) == LEIA_SCAM_FOCUS_STATE_FAILED);
	CHECK(leia_scam_focus_state_from_af(42, false) == LEIA_SCAM_FOCUS_STATE_UNKNOWN);

	// Eye-time independence: stage 0's HAL (one synthesized stamp, 99.7 %
	// equal) never qualifies; a pair with real per-sensor stamps does.
	struct leia_scam_eye_times_tracker t;
	memset(&t, 0, sizeof(t));
	for (int i = 0; i < 298; i++) {
		leia_scam_eye_times_observe(&t, 1000 + i, i == 100 ? 999 : 1000 + i);
	}
	CHECK(!leia_scam_eye_times_independent(&t));
	memset(&t, 0, sizeof(t));
	for (int i = 0; i < 59; i++) {
		leia_scam_eye_times_observe(&t, 1000, 1400);
	}
	CHECK(!leia_scam_eye_times_independent(&t)); // not enough frames yet
	leia_scam_eye_times_observe(&t, 1000, 1400);
	CHECK(leia_scam_eye_times_independent(&t));
	leia_scam_eye_times_observe(&t, 0, 1400); // a missing stamp is not counted
	CHECK(t.both == 60);
}

static void
test_pairing(void)
{
	const int64_t ms = 1000000;
	// 29.80 fps: the 8 ms default stands (cap 10.07 ms); a larger setting is capped.
	CHECK(leia_scam_pair_tolerance_ns(8 * ms, 33558000) == 8 * ms);
	CHECK(leia_scam_pair_tolerance_ns(20 * ms, 33558000) == 33558000 * 3 / 10);
	CHECK(leia_scam_pair_tolerance_ns(20 * ms, 0) == 20 * ms); // period unknown
	// A constant -6.5 ms lens offset pairs; the next frame (one period on) never does.
	const int64_t t0 = 1000 * ms;
	const int64_t other[3] = {t0 - 33558000 - 6495000, t0 - 6495000, t0 + 33558000 - 6495000};
	const int64_t tol = leia_scam_pair_tolerance_ns(8 * ms, 33558000);
	CHECK(leia_scam_pair_pick(other, 3, t0, tol) == 1);
	// -39.1 ms (one period + 5.6 ms, the run after a still): pairs with the
	// frame 5.6 ms away, never with the one 39.1 ms away.
	const int64_t far[2] = {t0 - 39133000, t0 - 39133000 + 33558000};
	CHECK(leia_scam_pair_pick(far, 2, t0, tol) == 1);
	const int64_t none[1] = {t0 - 39133000};
	CHECK(leia_scam_pair_pick(none, 1, t0, tol) == -1);
	// Closest wins.
	const int64_t two[2] = {t0 + 3 * ms, t0 - 1 * ms};
	CHECK(leia_scam_pair_pick(two, 2, t0, 8 * ms) == 1);
	CHECK(leia_scam_pair_pick(NULL, 0, t0, tol) == -1);

	static struct leia_scam_skew_window w;
	leia_scam_skew_reset(&w);
	int64_t lo, med, hi;
	CHECK(!leia_scam_skew_summary(&w, &lo, &med, &hi));
	for (int i = 0; i < 101; i++) {
		leia_scam_skew_push(&w, (i % 2 == 0) ? -6495000 : -6495000 + i);
	}
	CHECK(leia_scam_skew_summary(&w, &lo, &med, &hi));
	CHECK(lo == -6495000 && hi == -6495000 + 99 && med == -6495000);
	for (int i = 0; i < 1000; i++) {
		leia_scam_skew_push(&w, i); // the window keeps the last 512
	}
	CHECK(w.n == LEIA_SCAM_SKEW_WINDOW && leia_scam_skew_summary(&w, &lo, &med, &hi) && lo == 488 && hi == 999);
}

static void
feed_n(struct leia_scam_phase_lock *l, int64_t gap, int64_t period, uint32_t n, enum leia_scam_phase_action *last)
{
	for (uint32_t i = 0; i < n; i++) {
		*last = leia_scam_phase_feed(l, gap, period);
	}
}

static void
test_phase_lock(void)
{
	const int64_t P = 33558000, ms = 1000000;
	const struct leia_scam_phase_cfg rear = {12, 3 * ms, 4, true, true, false};
	const struct leia_scam_phase_cfg front = {12, 3 * ms, 4, true, false, false};
	struct leia_scam_phase_lock l;
	enum leia_scam_phase_action a = LEIA_SCAM_PHASE_MEASURING;

	// The device runs' four phases (R - L, ms) on the rear defaults.
	// 0: locked at once, no shift.
	leia_scam_phase_init(&l, &rear);
	feed_n(&l, 0, P, 11, &a);
	CHECK(a == LEIA_SCAM_PHASE_MEASURING && leia_scam_phase_holding(&l));
	feed_n(&l, 0, P, 1, &a);
	CHECK(a == LEIA_SCAM_PHASE_LOCKED && l.shift == 0 && l.residual_ns == 0 && !leia_scam_phase_holding(&l));
	// -6.5: re-pairing cannot help (n = 0), over 3 ms -> reopen.
	leia_scam_phase_init(&l, &rear);
	feed_n(&l, -6495000, P, 12, &a);
	CHECK(a == LEIA_SCAM_PHASE_REOPEN && l.shift == 0 && l.reopens == 1 && leia_scam_phase_holding(&l));
	// -40.6: re-pair one frame later on the right -> -7.06 ms, still over 3 -> reopen.
	leia_scam_phase_init(&l, &rear);
	feed_n(&l, -40615000, P, 12, &a);
	CHECK(l.shift == 1 && l.residual_ns == -40615000 + P && a == LEIA_SCAM_PHASE_REOPEN);
	// -31.0: re-paired to +2.56 ms -> locked without a restart.
	leia_scam_phase_init(&l, &rear);
	feed_n(&l, -31000 * 1000, P, 12, &a);
	CHECK(a == LEIA_SCAM_PHASE_LOCKED && l.shift == 1 && l.residual_ns == -31000000 + P);
	// +31.0: the other way (right eye one frame earlier).
	leia_scam_phase_init(&l, &rear);
	feed_n(&l, 31000000, P, 12, &a);
	CHECK(a == LEIA_SCAM_PHASE_LOCKED && l.shift == -1 && l.residual_ns == 31000000 - P);
	// A sequence of restarts: -40.6, -6.5, +12, then 1.2 -> locked on the 4th session.
	leia_scam_phase_init(&l, &rear);
	const int64_t seq[4] = {-40615000, -6495000, 12000000, 1200000};
	for (int i = 0; i < 4; i++) {
		if (i > 0) {
			leia_scam_phase_restart(&l);
		}
		feed_n(&l, seq[i], P, 12, &a);
	}
	CHECK(a == LEIA_SCAM_PHASE_LOCKED && l.reopens == 3 && l.residual_ns == 1200000);
	CHECK(l.have_best && l.best_abs_ns == 1200000);
	// Never under the threshold: 4 restarts, then the 5th session is accepted as is.
	leia_scam_phase_init(&l, &rear);
	for (int i = 0; i < 5; i++) {
		if (i > 0) {
			leia_scam_phase_restart(&l);
		}
		feed_n(&l, -6000000 - i * 1000000, P, 12, &a);
	}
	CHECK(a == LEIA_SCAM_PHASE_ACCEPTED && l.reopens == 4 && l.residual_ns == -10000000);
	CHECK(l.best_abs_ns == 6000000);
	// The front default: re-pair only, never a restart.
	leia_scam_phase_init(&l, &front);
	feed_n(&l, -40615000, P, 12, &a);
	CHECK(a == LEIA_SCAM_PHASE_ACCEPTED && l.shift == 1 && l.reopens == 0);
	// Median, not mean: one outlier stamp (stage 0 saw single half-frame jumps).
	leia_scam_phase_init(&l, &rear);
	feed_n(&l, 0, P, 11, &a);
	feed_n(&l, -16000000, P, 1, &a);
	CHECK(a == LEIA_SCAM_PHASE_LOCKED && l.gap_ns == 0);
	// Re-pairing off: the raw gap decides.
	{
		struct leia_scam_phase_cfg c = rear;
		c.repair = false;
		leia_scam_phase_init(&l, &c);
		feed_n(&l, -31000000, P, 12, &a);
		CHECK(a == LEIA_SCAM_PHASE_REOPEN && l.shift == 0);
	}
	// A camera with same-request stills (rear JPEG outputs): judged on the raw
	// gap. -31 ms re-pairs to +2.56 ms for the stream but a still would be 31 ms
	// apart: restart; 1.5 ms raw locks.
	{
		struct leia_scam_phase_cfg c = rear;
		c.judge_raw_gap = true;
		leia_scam_phase_init(&l, &c);
		feed_n(&l, -31000000, P, 12, &a);
		CHECK(a == LEIA_SCAM_PHASE_REOPEN && l.shift == 1 && l.residual_ns == -31000000 + P);
		leia_scam_phase_restart(&l);
		feed_n(&l, 1500000, P, 12, &a);
		CHECK(a == LEIA_SCAM_PHASE_LOCKED && l.shift == 0);
	}
	// Unknown period: no re-pair.
	leia_scam_phase_init(&l, &front);
	feed_n(&l, -31000000, 0, 12, &a);
	CHECK(l.shift == 0 && a == LEIA_SCAM_PHASE_ACCEPTED);
	// No per-lens stamps at all: give up after 36 pairs, no shift.
	leia_scam_phase_init(&l, &rear);
	for (int i = 0; i < 35; i++) {
		a = leia_scam_phase_pair_without_sample(&l);
	}
	CHECK(a == LEIA_SCAM_PHASE_MEASURING);
	a = leia_scam_phase_pair_without_sample(&l);
	CHECK(a == LEIA_SCAM_PHASE_UNKNOWN && !leia_scam_phase_holding(&l) && l.shift == 0);
	// A decided lock ignores further samples.
	CHECK(leia_scam_phase_feed(&l, -40000000, P) == LEIA_SCAM_PHASE_UNKNOWN);

	// Nearest-frame pairing with the shift (synthesized, periodic image stamps):
	// shift +1: left k (stamp t) pairs with right k+1 (stamp t + P), and the
	// right image at t + P looks for the left at t.
	const int64_t t = 5000 * ms;
	CHECK(leia_scam_phase_partner_ts(t, true, 1, P) == t + P);
	CHECK(leia_scam_phase_partner_ts(t + P, false, 1, P) == t);
	CHECK(leia_scam_phase_partner_ts(t, true, -1, P) == t - P);
	CHECK(leia_scam_phase_partner_ts(t, true, 0, P) == t);
	{
		// The right queue holds frames k-1, k, k+1: the shift picks k+1, never k.
		const int64_t right[3] = {t - P, t, t + P};
		const int64_t tol = leia_scam_pair_tolerance_ns(8 * ms, P);
		CHECK(leia_scam_pair_pick(right, 3, leia_scam_phase_partner_ts(t, true, 1, P), tol) == 2);
		CHECK(leia_scam_pair_pick(right, 3, leia_scam_phase_partner_ts(t, true, 0, P), tol) == 1);
		// Stamps a little jittered (1.3 ms, stage 0's interval std at 1920x1440): still the right one.
		const int64_t jit[3] = {t - P + 1300000, t - 1300000, t + P - 1300000};
		CHECK(leia_scam_pair_pick(jit, 3, leia_scam_phase_partner_ts(t, true, 1, P), tol) == 2);
	}
}

int
main(void)
{
	test_phase_lock();
	test_pairing();
	test_regions();
	test_rear_modes();
	test_front_modes();
	test_turned_and_degenerate();
	test_metering();
	test_controls();
	test_results();
	if (g_fail != 0) {
		printf("test_stereo_camera_android_modes: %d failure(s)\n", g_fail);
		return 1;
	}
	printf("test_stereo_camera_android_modes: all passed\n");
	return 0;
}
