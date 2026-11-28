// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Host test for leia_stereo_camera_android_still.c (Linux CI): the
 *         still's RGBA -> NV12 conversion is bit-identical to the scalar
 *         reference (the NEON path on aarch64 hosts), on random data, flat
 *         extremes, a crop offset and a width that is not a multiple of 16;
 *         and it agrees with the BT.601 full-range formula. Prints the time of
 *         one 4096x3072 eye (informational).
 */

#include "leia_stereo_camera_android_still.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail = 0;
#define CHECK(c)                                                                                                       \
	do {                                                                                                           \
		if (!(c)) {                                                                                            \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                            \
			g_fail++;                                                                                      \
		}                                                                                                      \
	} while (0)

static uint32_t g_rng = 12345u;
static uint8_t
rnd(void)
{
	g_rng = g_rng * 1664525u + 1013904223u;
	return (uint8_t)(g_rng >> 24);
}

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/*!
 * Convert a cap_w x cap_h RGBA image's centred w x h with both paths into an
 * SBS layout (dst stride 2w, written at column x0) and compare every byte.
 */
static void
compare(uint32_t cap_w, uint32_t cap_h, uint32_t w, uint32_t h, int fill)
{
	const size_t stride = (size_t)cap_w * 4 + 64; // a padded stride, as AImageDecoder may give
	uint8_t *rgba = malloc(stride * cap_h);
	for (size_t i = 0; i < stride * cap_h; i++) {
		rgba[i] = fill < 0 ? rnd() : (uint8_t)fill;
	}
	const uint32_t ox = ((cap_w - w) / 2u) & ~1u, oy = ((cap_h - h) / 2u) & ~1u;
	const size_t ds = (size_t)2 * w, size = ds * h * 3 / 2;
	uint8_t *a = malloc(size), *b = malloc(size);
	memset(a, 7, size);
	memset(b, 7, size);
	for (uint32_t x0 = 0; x0 <= w; x0 += w) { // left eye, then right eye
		leia_scam_rgba_to_nv12(rgba, stride, ox, oy, w, h, a + x0, a + ds * h + x0, ds);
		leia_scam_rgba_to_nv12_scalar(rgba, stride, ox, oy, w, h, b + x0, b + ds * h + x0, ds);
	}
	CHECK(memcmp(a, b, size) == 0);
	if (fill >= 0) {
		// Grey: Y = fill, Cb = Cr = 128 exactly.
		CHECK(a[0] == (uint8_t)((256 * fill + 128) >> 8) && a[ds * h] == 128 && a[ds * h + 1] == 128);
	} else {
		// One pixel against the formula.
		const uint8_t *p = rgba + (size_t)oy * stride + (size_t)ox * 4;
		CHECK(a[0] == (uint8_t)((77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8));
	}
	free(rgba);
	free(a);
	free(b);
}

int
main(void)
{
	compare(64, 48, 64, 48, -1);
	compare(4096, 3072, 4096, 3072, -1); // the rear still
	compare(4208, 3120, 4160, 3120, -1); // a column-cropped output
	compare(130, 64, 98, 40, -1);        // tail of 2 columns past 16s, row offset
	compare(64, 32, 64, 32, 0);
	compare(64, 32, 64, 32, 255);
	compare(64, 32, 64, 32, 128);

	// Saturated colours (the chroma clamp): pure blue / red / green blocks.
	{
		const uint8_t cols[3][4] = {{0, 0, 255, 255}, {255, 0, 0, 255}, {0, 255, 0, 255}};
		for (int k = 0; k < 3; k++) {
			uint8_t rgba[32 * 2 * 4];
			for (int i = 0; i < 64; i++) {
				memcpy(rgba + 4 * i, cols[k], 4);
			}
			uint8_t a[64 * 3 / 2], b[64 * 3 / 2];
			leia_scam_rgba_to_nv12(rgba, 128, 0, 0, 32, 2, a, a + 64, 32);
			leia_scam_rgba_to_nv12_scalar(rgba, 128, 0, 0, 32, 2, b, b + 64, 32);
			CHECK(memcmp(a, b, sizeof(a)) == 0);
		}
		// Pure blue: Cb = (128*1020 + 131584) >> 10 = 256 -> clamped to 255.
	}

	// Timing of one 4096x3072 eye (informational, not a check).
	{
		const uint32_t w = 4096, h = 3072;
		const size_t stride = (size_t)w * 4;
		uint8_t *rgba = malloc(stride * h);
		for (size_t i = 0; i < stride * h; i++) {
			rgba[i] = rnd();
		}
		uint8_t *dst = malloc((size_t)w * h * 3 / 2);
		double t0 = now_ms();
		leia_scam_rgba_to_nv12_scalar(rgba, stride, 0, 0, w, h, dst, dst + (size_t)w * h, w);
		double t1 = now_ms();
		leia_scam_rgba_to_nv12(rgba, stride, 0, 0, w, h, dst, dst + (size_t)w * h, w);
		double t2 = now_ms();
		printf("4096x3072 eye RGBA->NV12: scalar %.1f ms, fast path %.1f ms\n", t1 - t0, t2 - t1);
		free(rgba);
		free(dst);
	}

	if (g_fail != 0) {
		printf("test_stereo_camera_android_still: %d failure(s)\n", g_fail);
		return 1;
	}
	printf("test_stereo_camera_android_still: all passed\n");
	return 0;
}
