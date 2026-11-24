// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Host test for leia_stereo_camera_android_user_calib.c (Linux CI):
 *         the calibration app's schema-1 record, SHA-256 of the factory block,
 *         and the checks that decide whether the record's R is used.
 *
 * The record is the shape the calibration app saves (values from a real save,
 * device id elided).
 */

#include "leia_stereo_camera_android_user_calib.h"

#include <ctype.h>
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

static const char *kRecord =
    "{\"schema\":1,\"device\":{\"model\":\"x\",\"id\":\"x\"},"
    "\"pair\":{\"logicalCameraId\":\"2\",\"lensIds\":[\"1\",\"0\"],\"facing\":\"front\"},\n"
    "\"factory\":{\"blockSha256\":\"34D042746D0851912E72E9EE0C88ABAD7489590A3DD53D248A507F0D0FFCC507\",\"firstByte\":1},"
    "\"T_source\":\"factory\",\"intrinsicsSource\":\"factory\",\"imageSize\":{\"width\":4576,\"height\":3168},"
    "\"lens0\":{\"fx\":2247.1,\"fy\":2247.9,\"cx\":2098.2,\"cy\":1565.6,\"dist\":[0.1,-0.2,0,0,0.01,0,0,0]},"
    "\"lens1\":{\"fx\":2246.0,\"fy\":2246.5,\"cx\":2094.8,\"cy\":1568.1,\"dist\":[0.1,-0.2,0,0,0.01,0,0,0]},"
    "\"R_1_from_0\":[[0.99998838,0.00204836,0.00436285],[-0.00203835,0.99999529,-0.00229582],"
    "[-0.00436753,0.00228690,0.99998785]],"
    "\"R_rodrigues_deg\":[0.13,0.25,-0.12],\"deltaFromFactory_deg\":[0.0907,1.2283,-0.2942],"
    "\"T_mm_1_from_0\":[0.367,14.893,-0.482],\"method\":\"infinity-epipolar-v1\","
    "\"metrics\":{\"inliers\":3497,\"epipolarRmsPx\":0.515,\"infinityDisparityPx\":-0.286,\"nearRowResidualPx\":1.16,"
    "\"farSceneDistanceMinM\":20,\"matchingFrameRotationDeg\":[0,0],\"flag\":true,\"none\":null,"
    "\"esc\":\"a\\\"b\\u00e9\"},"
    "\"timestamp\":\"2026-10-31T11:45:57+09:00\",\"appVersion\":\"2.0.0-skeleton\"}";

static void
block(uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		b[i] = (uint8_t)((i * 7 + 3) & 255);
	}
}

static void
rot_y(double deg, double R[3][3])
{
	const double a = deg * M_PI / 180.0;
	const double M[3][3] = {{cos(a), 0, sin(a)}, {0, 1, 0}, {-sin(a), 0, cos(a)}};
	memcpy(R, M, sizeof(M));
}

static void
test_sha256(void)
{
	uint8_t d[32];
	char hex[65];
	leia_scam_sha256((const uint8_t *)"", 0, d);
	leia_scam_sha256_hex(d, hex);
	CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
	leia_scam_sha256((const uint8_t *)"abc", 3, d);
	leia_scam_sha256_hex(d, hex);
	CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
	// 56 bytes: the padding spills into a second block.
	const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	leia_scam_sha256((const uint8_t *)m, strlen(m), d);
	leia_scam_sha256_hex(d, hex);
	CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
	// A factory-block-sized buffer (690 bytes, as on the phone-class device).
	uint8_t b[690];
	block(b, sizeof(b));
	leia_scam_sha256(b, sizeof(b), d);
	leia_scam_sha256_hex(d, hex);
	CHECK(strcmp(hex, "34d042746d0851912e72e9ee0c88abad7489590a3dd53d248a507f0d0ffcc507") == 0);
}

static void
test_parse(void)
{
	struct leia_scam_user_calib u;
	CHECK(leia_scam_user_calib_parse(kRecord, strlen(kRecord), &u));
	CHECK(u.schema == 1);
	CHECK(strcmp(u.logical_id, "2") == 0);
	CHECK(u.has_lens_ids && strcmp(u.lens_ids[0], "1") == 0 && strcmp(u.lens_ids[1], "0") == 0);
	CHECK(strcmp(u.block_sha256, "34d042746d0851912e72e9ee0c88abad7489590a3dd53d248a507f0d0ffcc507") == 0);
	CHECK(u.has_R);
	NEAR(u.R[0][0], 0.99998838, 1e-12);
	NEAR(u.R[1][2], -0.00229582, 1e-12);
	NEAR(u.R[2][0], -0.00436753, 1e-12);
	CHECK(strcmp(u.timestamp, "2026-10-31T11:45:57+09:00") == 0);
	CHECK(strcmp(u.method, "infinity-epipolar-v1") == 0);

	// A length that counts the terminator is accepted.
	CHECK(leia_scam_user_calib_parse(kRecord, strlen(kRecord) + 1, &u));

	// Truncated / not an object / wrong types.
	CHECK(!leia_scam_user_calib_parse(kRecord, strlen(kRecord) - 1, &u));
	CHECK(!leia_scam_user_calib_parse(kRecord, 200, &u));
	CHECK(!leia_scam_user_calib_parse("[]", 2, &u));
	CHECK(!leia_scam_user_calib_parse("", 0, &u));
	const char *bad_r = "{\"schema\":1,\"R_1_from_0\":[[1,0,0],[0,1,0]]}";
	CHECK(!leia_scam_user_calib_parse(bad_r, strlen(bad_r), &u));
	const char *bad_id = "{\"pair\":{\"logicalCameraId\":2}}";
	CHECK(!leia_scam_user_calib_parse(bad_id, strlen(bad_id), &u));
	const char *junk = "{\"schema\":1} x";
	CHECK(!leia_scam_user_calib_parse(junk, strlen(junk), &u));

	// Missing fields parse, and are rejected by the check.
	const char *empty = "{}";
	CHECK(leia_scam_user_calib_parse(empty, strlen(empty), &u));
	CHECK(u.schema == -1 && !u.has_R && !u.has_lens_ids);
}

static void
test_check(void)
{
	uint8_t b[690], sha[32], other[32];
	block(b, sizeof(b));
	leia_scam_sha256(b, sizeof(b), sha);
	b[0] ^= 1; // the factory block changed (e.g. a re-burn)
	leia_scam_sha256(b, sizeof(b), other);

	struct leia_scam_user_calib u;
	CHECK(leia_scam_user_calib_parse(kRecord, strlen(kRecord), &u));

	double Rf[3][3];
	rot_y(1.2, Rf);
	double d = -1;
	CHECK(leia_scam_user_calib_check(&u, "2", "1", "0", sha, Rf, &d) == LEIA_SCAM_USER_CALIB_OK);
	CHECK(d > 0.5 && d < 2.0);
	// No lens ids from the caller: the lens check is skipped.
	CHECK(leia_scam_user_calib_check(&u, "2", NULL, NULL, sha, Rf, NULL) == LEIA_SCAM_USER_CALIB_OK);

	CHECK(leia_scam_user_calib_check(&u, "3", "1", "0", sha, Rf, &d) == LEIA_SCAM_USER_CALIB_OTHER_PAIR);
	CHECK(leia_scam_user_calib_check(&u, "2", "0", "1", sha, Rf, &d) == LEIA_SCAM_USER_CALIB_OTHER_LENSES);
	CHECK(leia_scam_user_calib_check(&u, "2", "1", "0", other, Rf, &d) == LEIA_SCAM_USER_CALIB_BLOCK_CHANGED);

	struct leia_scam_user_calib v = u;
	v.schema = 2;
	CHECK(leia_scam_user_calib_check(&v, "2", "1", "0", sha, Rf, &d) == LEIA_SCAM_USER_CALIB_BAD_SCHEMA);

	v = u;
	v.has_R = false;
	CHECK(leia_scam_user_calib_check(&v, "2", "1", "0", sha, Rf, &d) == LEIA_SCAM_USER_CALIB_NO_ROTATION);

	v = u;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			v.R[i][j] *= 1.01; // scaled: not orthonormal
		}
	}
	CHECK(leia_scam_user_calib_check(&v, "2", "1", "0", sha, Rf, &d) == LEIA_SCAM_USER_CALIB_NOT_ROTATION);

	v = u;
	for (int j = 0; j < 3; j++) {
		v.R[2][j] = -v.R[2][j]; // a reflection: orthonormal, det = -1
	}
	CHECK(leia_scam_user_calib_check(&v, "2", "1", "0", sha, Rf, &d) == LEIA_SCAM_USER_CALIB_NOT_ROTATION);

	v = u;
	v.R[0][0] = NAN;
	CHECK(leia_scam_user_calib_check(&v, "2", "1", "0", sha, Rf, &d) == LEIA_SCAM_USER_CALIB_NOT_ROTATION);

	v = u;
	rot_y(7.0, v.R);
	CHECK(leia_scam_user_calib_check(&v, "2", "1", "0", sha, Rf, &d) == LEIA_SCAM_USER_CALIB_TOO_FAR);
	NEAR(d, 5.8, 1e-6);
	rot_y(5.9, v.R); // 4.7 deg from the factory R: in range
	CHECK(leia_scam_user_calib_check(&v, "2", "1", "0", sha, Rf, &d) == LEIA_SCAM_USER_CALIB_OK);
	NEAR(d, 4.7, 1e-6);

	CHECK(strlen(leia_scam_user_calib_verdict_str(LEIA_SCAM_USER_CALIB_BLOCK_CHANGED)) > 0);
}

//! The provider vs cache choice; the cache passes the same gate as a fresh record.
static void
test_choose(void)
{
	uint8_t b[690], sha[32], other[32];
	block(b, sizeof(b));
	leia_scam_sha256(b, sizeof(b), sha);
	b[0] ^= 1; // a re-burned block
	leia_scam_sha256(b, sizeof(b), other);
	double Rf[3][3];
	rot_y(1.2, Rf);
	const size_t n = strlen(kRecord);
	struct leia_scam_user_calib_choice c;

	// Provider answers with a good record: use it, refresh the cache.
	leia_scam_user_calib_choose(LEIA_SCAM_PROVIDER_RECORD, kRecord, n, NULL, 0, "2", "1", "0", sha, Rf, &c);
	CHECK(c.source == LEIA_SCAM_USER_CALIB_SRC_PROVIDER && c.cache_write && !c.cache_drop);
	CHECK(c.verdict == LEIA_SCAM_USER_CALIB_OK && c.delta_deg > 0.5);
	NEAR(c.rec.R[0][0], 0.99998838, 1e-12);

	// Provider answers with a record for a changed block: factory, and the cache goes.
	leia_scam_user_calib_choose(LEIA_SCAM_PROVIDER_RECORD, kRecord, n, kRecord, n, "2", "1", "0", other, Rf, &c);
	CHECK(c.source == LEIA_SCAM_USER_CALIB_SRC_FACTORY && c.cache_drop && !c.cache_write);
	CHECK(c.considered && c.verdict == LEIA_SCAM_USER_CALIB_BLOCK_CHANGED);

	// Provider answers "nothing saved" (restored to factory): factory, the cache goes,
	// even though a valid cache exists.
	leia_scam_user_calib_choose(LEIA_SCAM_PROVIDER_NO_RECORD, NULL, 0, kRecord, n, "2", "1", "0", sha, Rf, &c);
	CHECK(c.source == LEIA_SCAM_USER_CALIB_SRC_FACTORY && c.cache_drop && !c.considered);

	// Provider unreachable, valid cache: the cache is used, and not rewritten.
	leia_scam_user_calib_choose(LEIA_SCAM_PROVIDER_UNAVAILABLE, NULL, 0, kRecord, n, "2", "1", "0", sha, Rf, &c);
	CHECK(c.source == LEIA_SCAM_USER_CALIB_SRC_CACHE && !c.cache_write && !c.cache_drop);
	CHECK(c.verdict == LEIA_SCAM_USER_CALIB_OK);

	// Provider unreachable, cache saved against another block (SHA mismatch): ignored.
	leia_scam_user_calib_choose(LEIA_SCAM_PROVIDER_UNAVAILABLE, NULL, 0, kRecord, n, "2", "1", "0", other, Rf, &c);
	CHECK(c.source == LEIA_SCAM_USER_CALIB_SRC_FACTORY && !c.cache_write);
	CHECK(c.considered && c.verdict == LEIA_SCAM_USER_CALIB_BLOCK_CHANGED);

	// Provider unreachable, cache for another pair / unparseable / absent: factory.
	leia_scam_user_calib_choose(LEIA_SCAM_PROVIDER_UNAVAILABLE, NULL, 0, kRecord, n, "3", "1", "0", sha, Rf, &c);
	CHECK(c.source == LEIA_SCAM_USER_CALIB_SRC_FACTORY && c.verdict == LEIA_SCAM_USER_CALIB_OTHER_PAIR);
	leia_scam_user_calib_choose(LEIA_SCAM_PROVIDER_UNAVAILABLE, NULL, 0, kRecord, n - 5, "2", "1", "0", sha, Rf, &c);
	CHECK(c.source == LEIA_SCAM_USER_CALIB_SRC_FACTORY && c.parse_failed);
	leia_scam_user_calib_choose(LEIA_SCAM_PROVIDER_UNAVAILABLE, NULL, 0, NULL, 0, "2", "1", "0", sha, Rf, &c);
	CHECK(c.source == LEIA_SCAM_USER_CALIB_SRC_FACTORY && !c.considered && !c.cache_drop);
}

/*
 * The served record (display config "stereoCalibrations"): the real-shaped
 * tablet entry from leia-calibration-suite
 * android/app/src/test/resources/configs/k68-tablet-saved-old-layout.json
 * (mirror Save: logical 5, lensIds ["1","3"], imageSize 1280x960, factory null).
 */
static struct leia_scam_served_stereo
tablet_served(void)
{
	struct leia_scam_served_stereo s;
	memset(&s, 0, sizeof(s));
	snprintf(s.logical_id, sizeof(s.logical_id), "5");
	snprintf(s.lens_ids[0], sizeof(s.lens_ids[0]), "1");
	snprintf(s.lens_ids[1], sizeof(s.lens_ids[1]), "3");
	s.width = 1280;
	s.height = 960;
	s.lens[0].fx = 693.3099546541497;
	s.lens[0].fy = 692.2241582736079;
	s.lens[0].cx = 641.2852896328532;
	s.lens[0].cy = 481.7170643688129;
	s.lens[0].dist[0] = 0.03416474974987201;
	s.lens[0].dist[1] = -0.05696495902361015;
	s.lens[1].fx = 692.9834930279095;
	s.lens[1].fy = 692.1524860734401;
	s.lens[1].cx = 638.3147356512378;
	s.lens[1].cy = 479.4969527726019;
	s.lens[1].dist[0] = 0.02981495872017688;
	s.lens[1].dist[1] = -0.05159669312255205;
	const double R[9] = {0.9999969921006638,    -0.0011835956661088538, -0.0021482297186240446,
	                     0.0011858200711528134, 0.9999987618387237,     0.0010344814055983885,
	                     0.002147002651060855,  -0.0010370257079002423, 0.9999971574746088};
	memcpy(s.R, R, sizeof(s.R));
	s.T_mm[0] = -25.015269088222244;
	s.T_mm[1] = 0.013857612014705938;
	s.T_mm[2] = -0.002862206078678098;
	snprintf(s.timestamp, sizeof(s.timestamp), "2026-11-20T15:02:11+09:00");
	return s;
}

static void
test_served(void)
{
	const struct leia_scam_served_stereo s = tablet_served();
	struct leia_scam_factory_calib f;
	double d = -1.0;

	// Tablet front: main = 1, sub = 3 (lensIds order) -> full model, as is.
	// Front array 3264x2448 (4:3): the 1280x960 stream is the whole array.
	CHECK(leia_scam_served_to_factory(&s, "1", "3", 3264, 2448, NULL, NULL, &f, &d) == LEIA_SCAM_SERVED_FULL);
	const double sc = 3264.0 / 1280.0;
	NEAR(f.main.fx, 693.3099546541497 * sc, 1e-9);
	NEAR(f.main.cx, (641.2852896328532 + 0.5) * sc - 0.5, 1e-9);
	NEAR(f.sub.cy, (479.4969527726019 + 0.5) * (2448.0 / 960.0) - 0.5, 1e-9);
	NEAR(f.T[0], -25.015269088222244, 0.0);
	NEAR(f.main.d[1], -0.05696495902361015, 0.0);
	// Round trip through the stream pipeline: at the 1280x960 capture the eyes
	// carry exactly the record's numbers (T.x < 0: lens1 = sub sits at +x, so
	// main = the SBS left eye).
	struct leia_scam_stream_calib sc960;
	CHECK(leia_scam_stream_calib_from_factory(&f, 3264, 2448, 1280, 960, 1280, 960, 0, &sc960));
	CHECK(!sc960.left_is_sub);
	NEAR(sc960.k[0][0], 693.3099546541497, 1e-9);
	NEAR(sc960.k[0][2], 641.2852896328532, 1e-9);
	NEAR(sc960.k[0][3], 481.7170643688129, 1e-9);
	NEAR(sc960.k[1][1], 692.1524860734401, 1e-9);
	NEAR(sc960.k[1][2], 638.3147356512378, 1e-9);
	NEAR(sc960.T_rl_mm[0], -25.015269088222244, 0.0);
	// The default 1280x720 eye: the centre rows, cy - 120.
	CHECK(leia_scam_stream_calib_from_factory(&f, 3264, 2448, 1280, 960, 1280, 720, 0, &sc960));
	NEAR(sc960.k[0][3], 481.7170643688129 - 120.0, 1e-9);

	// Per axis against the REGION (rear-like 4208x3120 array, not 4:3): the
	// 1280x960 stream is the 4160x3120 centred region, so both ratios are 3.25
	// and cx gains the region's 24-column offset; never 4208/1280 on x.
	CHECK(leia_scam_served_to_factory(&s, "1", "3", 4208, 3120, NULL, NULL, &f, NULL) == LEIA_SCAM_SERVED_FULL);
	NEAR(f.main.fx, 693.3099546541497 * 3.25, 1e-9);
	NEAR(f.main.fy, 692.2241582736079 * 3.25, 1e-9);
	NEAR(f.main.cx, 24.0 + (641.2852896328532 + 0.5) * 3.25 - 0.5, 1e-9);
	NEAR(f.main.cy, (481.7170643688129 + 0.5) * 3.25 - 0.5, 1e-9);
	// A stated size a hair off the region's aspect: each axis by its own ratio.
	{
		struct leia_scam_served_stereo s2 = s;
		s2.width = 1276; // 1276/960 vs the 4:3 region
		CHECK(leia_scam_served_to_factory(&s2, "1", "3", 3264, 2448, NULL, NULL, &f, NULL) == LEIA_SCAM_SERVED_FULL);
		const double rw = 2448.0 * 1276.0 / 960.0, x0 = (3264.0 - rw) / 2.0;
		NEAR(f.main.fx, 693.3099546541497 * rw / 1276.0, 1e-9);
		NEAR(f.main.fy, 692.2241582736079 * 2448.0 / 960.0, 1e-9);
		NEAR(f.main.cx, x0 + (641.2852896328532 + 0.5) * rw / 1276.0 - 0.5, 1e-9);
	}

	// Lens order reversed relative to the block (lensIds = [sub, main]): the
	// lenses swap and the extrinsics invert (x_main = R^T x_sub - R^T T).
	CHECK(leia_scam_served_to_factory(&s, "3", "1", 3264, 2448, NULL, NULL, &f, NULL) == LEIA_SCAM_SERVED_FULL);
	NEAR(f.main.fx, 692.9834930279095 * sc, 1e-9); // lens1 ("3") is the main lens now
	NEAR(f.sub.fx, 693.3099546541497 * sc, 1e-9);
	NEAR(f.R[0][1], s.R[1][0], 0.0);
	{
		double t = 0.0;
		for (int j = 0; j < 3; j++) {
			t -= s.R[j][0] * s.T_mm[j];
		}
		NEAR(f.T[0], t, 1e-12); // ~ +25.0
	}
	// Either order describes the same pair: the SBS left eye is the same lens.
	{
		struct leia_scam_factory_calib a, b;
		struct leia_scam_stream_calib ca, cb;
		leia_scam_served_to_factory(&s, "1", "3", 3264, 2448, NULL, NULL, &a, NULL);
		leia_scam_served_to_factory(&s, "3", "1", 3264, 2448, NULL, NULL, &b, NULL);
		leia_scam_stream_calib_from_factory(&a, 3264, 2448, 1280, 960, 1280, 720, 0, &ca);
		leia_scam_stream_calib_from_factory(&b, 3264, 2448, 1280, 960, 1280, 720, 0, &cb);
		for (int e = 0; e < 2; e++) {
			for (int k = 0; k < 4; k++) {
				NEAR(ca.k[e][k], cb.k[e][k], 1e-9);
			}
		}
		NEAR(ca.T_rl_mm[0], cb.T_rl_mm[0], 1e-9);
	}

	// Phone order ["1","0"] (suite MirrorStereoSaveTest: T = (-0.31, -14.92, 0.44),
	// a vertical pair): main = 1, sub = 0 -> as is; the pipeline turns it.
	{
		struct leia_scam_served_stereo p = s;
		snprintf(p.lens_ids[0], sizeof(p.lens_ids[0]), "1");
		snprintf(p.lens_ids[1], sizeof(p.lens_ids[1]), "0");
		p.T_mm[0] = -0.31;
		p.T_mm[1] = -14.92;
		p.T_mm[2] = 0.44;
		CHECK(leia_scam_served_to_factory(&p, "1", "0", 4576, 3168, NULL, NULL, &f, NULL) == LEIA_SCAM_SERVED_FULL);
		CHECK(leia_scam_factory_is_vertical(&f));
		NEAR(f.main.fx, 693.3099546541497 / (960.0 / 3168.0), 1e-9); // the 4224x3168 region
		CHECK(leia_scam_served_to_factory(&p, "0", "1", 4576, 3168, NULL, NULL, &f, NULL) == LEIA_SCAM_SERVED_FULL);
		NEAR(f.main.fx, 692.9834930279095 / (960.0 / 3168.0), 1e-9);
	}

	// Ids that name other lenses; ids not named = [main, sub].
	CHECK(leia_scam_served_to_factory(&s, "0", "2", 3264, 2448, NULL, NULL, &f, NULL) ==
	      LEIA_SCAM_SERVED_OTHER_LENSES);
	{
		struct leia_scam_served_stereo n = s;
		n.lens_ids[0][0] = n.lens_ids[1][0] = '\0';
		CHECK(leia_scam_served_to_factory(&n, "1", "3", 3264, 2448, NULL, NULL, &f, NULL) == LEIA_SCAM_SERVED_FULL);
		NEAR(f.main.fx, 693.3099546541497 * sc, 1e-9);
	}
	// Implausible values.
	{
		struct leia_scam_served_stereo b = s;
		b.R[0][0] = 2.0;
		CHECK(leia_scam_served_to_factory(&b, "1", "3", 3264, 2448, NULL, NULL, &f, NULL) ==
		      LEIA_SCAM_SERVED_NOT_ROTATION);
		b = s;
		b.T_mm[0] = 0.2;
		b.T_mm[1] = b.T_mm[2] = 0.0;
		CHECK(leia_scam_served_to_factory(&b, "1", "3", 3264, 2448, NULL, NULL, &f, NULL) ==
		      LEIA_SCAM_SERVED_BAD_BASELINE);
		b = s;
		b.lens[1].cx = 1400.0; // outside the 1280 stated width
		CHECK(leia_scam_served_to_factory(&b, "1", "3", 3264, 2448, NULL, NULL, &f, NULL) == LEIA_SCAM_SERVED_BAD_LENS);
		b = s;
		b.width = 0;
		CHECK(leia_scam_served_to_factory(&b, "1", "3", 3264, 2448, NULL, NULL, &f, NULL) == LEIA_SCAM_SERVED_BAD_SIZE);
	}
	// Against a factory block: R within 5 deg passes, further is refused.
	{
		struct leia_scam_factory_calib blk;
		memset(&blk, 0, sizeof(blk));
		blk.R[0][0] = blk.R[1][1] = blk.R[2][2] = 1.0;
		CHECK(leia_scam_served_to_factory(&s, "1", "3", 3264, 2448, &blk, NULL, &f, &d) == LEIA_SCAM_SERVED_FULL);
		CHECK(d > 0.0 && d < 0.2);
		const double a = 8.0 * 3.14159265358979323846 / 180.0;
		blk.R[0][0] = cos(a);
		blk.R[0][1] = -sin(a);
		blk.R[1][0] = sin(a);
		blk.R[1][1] = cos(a);
		CHECK(leia_scam_served_to_factory(&s, "1", "3", 3264, 2448, &blk, NULL, &f, &d) == LEIA_SCAM_SERVED_TOO_FAR);
	}
	// A user record (it names a factory block): R only, on that block only.
	{
		struct leia_scam_factory_calib blk;
		memset(&blk, 0, sizeof(blk));
		blk.R[0][0] = blk.R[1][1] = blk.R[2][2] = 1.0;
		blk.main.fx = 1726.8;
		blk.T[0] = 14.989;
		uint8_t sha[32];
		const char *bytes = "factory block bytes";
		leia_scam_sha256((const uint8_t *)bytes, strlen(bytes), sha);
		struct leia_scam_served_stereo u = s;
		leia_scam_sha256_hex(sha, u.block_sha256);
		u.block_sha256[3] = (char)toupper((unsigned char)u.block_sha256[3]); // compared case-insensitively
		CHECK(leia_scam_served_to_factory(&u, "1", "3", 3264, 2448, &blk, sha, &f, NULL) == LEIA_SCAM_SERVED_ROTATION);
		NEAR(f.main.fx, 1726.8, 0.0); // the block's intrinsics and T stay
		NEAR(f.T[0], 14.989, 0.0);
		NEAR(f.R[0][1], s.R[0][1], 0.0);
		uint8_t other[32];
		memcpy(other, sha, sizeof(other));
		other[0] ^= 1;
		CHECK(leia_scam_served_to_factory(&u, "1", "3", 3264, 2448, &blk, other, &f, NULL) ==
		      LEIA_SCAM_SERVED_BLOCK_CHANGED);
		CHECK(leia_scam_served_to_factory(&u, "1", "3", 3264, 2448, NULL, NULL, &f, NULL) ==
		      LEIA_SCAM_SERVED_BLOCK_CHANGED);
	}
	CHECK(strcmp(leia_scam_served_verdict_str(LEIA_SCAM_SERVED_FULL), "?") != 0);
}

int
main(void)
{
	test_served();
	test_sha256();
	test_parse();
	test_check();
	test_choose();
	if (g_fail != 0) {
		printf("%d check(s) failed\n", g_fail);
		return 1;
	}
	printf("test_stereo_camera_android_user_calib: OK\n");
	return 0;
}
