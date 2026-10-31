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

int
main(void)
{
	test_sha256();
	test_parse();
	test_check();
	if (g_fail != 0) {
		printf("%d check(s) failed\n", g_fail);
		return 1;
	}
	printf("test_stereo_camera_android_user_calib: OK\n");
	return 0;
}
