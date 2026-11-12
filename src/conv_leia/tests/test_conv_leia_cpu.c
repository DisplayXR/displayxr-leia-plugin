// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The POSIX arm's CPU transport (leia_conv_cpu.c): channel order both
 *         ways, strides, the per-view resample never bleeding across the SBS
 *         seam, and every argument check. Plain C, every lane.
 */

#include "leia_conv_cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		g_checks++;                                                                                            \
		if (!(cond)) {                                                                                         \
			g_fail++;                                                                                      \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
		}                                                                                                      \
	} while (0)

static void
test_pack(void)
{
	// 2x2 RGBA with a padded stride (12 bytes per row, 8 used).
	uint8_t src[24];
	memset(src, 0xEE, sizeof(src));
	const uint8_t px[4][4] = {{10, 20, 30, 40}, {50, 60, 70, 80}, {90, 100, 110, 120}, {130, 140, 150, 160}};
	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			memcpy(src + y * 12 + x * 4, px[y * 2 + x], 4);
		}
	}
	uint8_t bgr[12];
	CHECK(leia_conv_cpu_pack_bgr8(src, sizeof(src), 2, 2, 12, LEIA_CONV_CPU_FMT_RGBA8_UNORM, bgr));
	for (int i = 0; i < 4; i++) {
		CHECK(bgr[i * 3 + 0] == px[i][2]); // B
		CHECK(bgr[i * 3 + 1] == px[i][1]); // G
		CHECK(bgr[i * 3 + 2] == px[i][0]); // R
	}
	CHECK(leia_conv_cpu_pack_bgr8(src, sizeof(src), 2, 2, 12, LEIA_CONV_CPU_FMT_BGRA8_SRGB, bgr));
	for (int i = 0; i < 4; i++) {
		CHECK(bgr[i * 3 + 0] == px[i][0]);
		CHECK(bgr[i * 3 + 2] == px[i][2]);
	}
	// Argument checks: stride too small, buffer too small, unknown format, nulls.
	CHECK(!leia_conv_cpu_pack_bgr8(src, sizeof(src), 2, 2, 7, LEIA_CONV_CPU_FMT_RGBA8_UNORM, bgr));
	CHECK(!leia_conv_cpu_pack_bgr8(src, 19, 2, 2, 12, LEIA_CONV_CPU_FMT_RGBA8_UNORM, bgr));
	CHECK(leia_conv_cpu_pack_bgr8(src, 20, 2, 2, 12, LEIA_CONV_CPU_FMT_RGBA8_UNORM, bgr)); // last row unpadded
	CHECK(!leia_conv_cpu_pack_bgr8(src, sizeof(src), 2, 2, 12, 5u /* NV12 */, bgr));
	CHECK(!leia_conv_cpu_pack_bgr8(NULL, sizeof(src), 2, 2, 12, LEIA_CONV_CPU_FMT_RGBA8_UNORM, bgr));
	CHECK(!leia_conv_cpu_pack_bgr8(src, sizeof(src), 0, 2, 12, LEIA_CONV_CPU_FMT_RGBA8_UNORM, bgr));
	// Stride 0 = packed.
	CHECK(leia_conv_cpu_pack_bgr8(src, 0, 2, 1, 0, LEIA_CONV_CPU_FMT_RGBA8_UNORM, bgr));
}

static void
test_views_identity_and_order(void)
{
	// 4x1 BGR source: left view = two blue texels, right view = two red texels.
	const uint8_t src[12] = {255, 0, 0, 255, 0, 0, 0, 0, 255, 0, 0, 255};
	uint8_t dst[16];
	memset(dst, 0, sizeof(dst));
	CHECK(leia_conv_cpu_write_views(src, 4, 1, 12, 2, dst, sizeof(dst), 4, 1, 16, LEIA_CONV_CPU_FMT_RGBA8_UNORM));
	for (int x = 0; x < 4; x++) {
		const uint8_t *d = dst + x * 4;
		if (x < 2) {
			CHECK(d[0] == 0 && d[1] == 0 && d[2] == 255); // RGBA blue
		} else {
			CHECK(d[0] == 255 && d[1] == 0 && d[2] == 0); // RGBA red
		}
		CHECK(d[3] == 255);
	}
	memset(dst, 0, sizeof(dst));
	CHECK(leia_conv_cpu_write_views(src, 4, 1, 12, 2, dst, sizeof(dst), 4, 1, 16, LEIA_CONV_CPU_FMT_BGRA8_UNORM));
	CHECK(dst[0] == 255 && dst[2] == 0);    // BGRA blue
	CHECK(dst[8] == 0 && dst[10] == 255);   // BGRA red
}

static void
test_views_no_seam_bleed(void)
{
	// Left view all 0, right view all 200, upscaled 3x with bilinear: the
	// texels next to the seam must stay exactly 0 / 200.
	const uint32_t sw = 8, sh = 4, dw = 24, dh = 12;
	uint8_t *src = malloc(sw * sh * 3);
	for (uint32_t y = 0; y < sh; y++) {
		for (uint32_t x = 0; x < sw; x++) {
			memset(src + (y * sw + x) * 3, x < sw / 2 ? 0 : 200, 3);
		}
	}
	const uint32_t stride = dw * 4 + 8; // padded target rows
	uint8_t *dst = malloc((size_t)stride * dh);
	memset(dst, 7, (size_t)stride * dh);
	CHECK(leia_conv_cpu_write_views(src, sw, sh, 0, 2, dst, (uint64_t)stride * dh, dw, dh, stride,
	                                LEIA_CONV_CPU_FMT_RGBA8_SRGB));
	int bad = 0;
	for (uint32_t y = 0; y < dh; y++) {
		for (uint32_t x = 0; x < dw; x++) {
			const uint8_t *d = dst + (size_t)y * stride + x * 4;
			const uint8_t want = x < dw / 2 ? 0 : 200;
			bad += d[0] != want || d[1] != want || d[2] != want || d[3] != 255;
		}
		bad += dst[(size_t)y * stride + dw * 4] != 7; // padding untouched
	}
	CHECK(bad == 0);
	// Target too small for its own stride / extent.
	CHECK(!leia_conv_cpu_write_views(src, sw, sh, 0, 2, dst, 10, dw, dh, stride, LEIA_CONV_CPU_FMT_RGBA8_SRGB));
	CHECK(!leia_conv_cpu_write_views(src, sw, sh, 0, 2, dst, (uint64_t)stride * dh, dw, dh, dw * 3,
	                                 LEIA_CONV_CPU_FMT_RGBA8_SRGB));
	CHECK(!leia_conv_cpu_write_views(src, sw, sh, sw * 2, 2, dst, (uint64_t)stride * dh, dw, dh, stride,
	                                 LEIA_CONV_CPU_FMT_RGBA8_SRGB)); // source stride too small
	CHECK(!leia_conv_cpu_write_views(src, sw, sh, 0, 0, dst, (uint64_t)stride * dh, dw, dh, stride,
	                                 LEIA_CONV_CPU_FMT_RGBA8_SRGB));
	free(src);
	free(dst);
}

static void
test_views_downscale_gradient(void)
{
	// A horizontal ramp per view, downscaled 2x: monotone and in range.
	const uint32_t sw = 16, sh = 2, dw = 8, dh = 1;
	uint8_t src[16 * 2 * 3];
	for (uint32_t y = 0; y < sh; y++) {
		for (uint32_t x = 0; x < sw; x++) {
			memset(src + (y * sw + x) * 3, (int)((x % 8) * 30), 3);
		}
	}
	uint8_t dst[8 * 4];
	CHECK(leia_conv_cpu_write_views(src, sw, sh, 0, 2, dst, sizeof(dst), dw, dh, 0, LEIA_CONV_CPU_FMT_RGBA8_UNORM));
	for (int v = 0; v < 2; v++) {
		for (int x = 1; x < 4; x++) {
			CHECK(dst[(v * 4 + x) * 4] > dst[(v * 4 + x - 1) * 4]);
		}
		CHECK(dst[(v * 4) * 4] <= 30);
		CHECK(dst[(v * 4 + 3) * 4] >= 180);
	}
}

static void
test_depth(void)
{
	// 4x2 -> 2x1 and 8x4: point-sampled, values untouched, argument checks.
	const float src[8] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f};
	float half[2] = {0};
	CHECK(leia_conv_cpu_write_depth(src, 4, 2, 0, half, sizeof(half), 2, 1, 0, LEIA_CONV_CPU_FMT_R32_FLOAT));
	CHECK(half[0] == 1.0f || half[0] == 16.0f || half[0] == 0.5f || half[0] == 8.0f);
	CHECK(half[1] == 4.0f || half[1] == 64.0f || half[1] == 2.0f || half[1] == 32.0f);
	float up[8 * 4];
	CHECK(leia_conv_cpu_write_depth(src, 4, 2, 16, up, sizeof(up), 8, 4, 0, LEIA_CONV_CPU_FMT_R32_FLOAT));
	CHECK(up[0] == 0.5f && up[1] == 0.5f && up[7] == 4.0f);
	CHECK(up[3 * 8 + 0] == 8.0f && up[3 * 8 + 7] == 64.0f);
	// Every value written is a source value (no blending).
	for (int i = 0; i < 32; i++) {
		int found = 0;
		for (int j = 0; j < 8; j++) {
			found |= up[i] == src[j];
		}
		CHECK(found);
	}
	CHECK(!leia_conv_cpu_write_depth(NULL, 4, 2, 0, up, sizeof(up), 8, 4, 0, LEIA_CONV_CPU_FMT_R32_FLOAT));
	CHECK(!leia_conv_cpu_write_depth(src, 4, 2, 0, up, sizeof(up), 8, 4, 0, LEIA_CONV_CPU_FMT_RGBA8_UNORM));
	CHECK(!leia_conv_cpu_write_depth(src, 4, 2, 8, up, sizeof(up), 8, 4, 0, LEIA_CONV_CPU_FMT_R32_FLOAT));
	CHECK(!leia_conv_cpu_write_depth(src, 4, 2, 0, up, 16, 8, 4, 0, LEIA_CONV_CPU_FMT_R32_FLOAT));
}

int
main(void)
{
	test_depth();
	test_pack();
	test_views_identity_and_order();
	test_views_no_seam_bleed();
	test_views_downscale_gradient();
	if (g_fail != 0) {
		fprintf(stderr, "test_conv_leia_cpu: %d of %d checks FAILED\n", g_fail, g_checks);
		return 1;
	}
	printf("test_conv_leia_cpu: all %d checks passed\n", g_checks);
	return 0;
}
