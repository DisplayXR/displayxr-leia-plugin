// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  CPU-memory transport of the Leia conversion provider (POSIX arm).
 * @ingroup conv_leia
 */

#include "leia_conv_cpu.h"

#include <math.h>

bool
leia_conv_cpu_format_ok(uint32_t fmt)
{
	return fmt == LEIA_CONV_CPU_FMT_RGBA8_UNORM || fmt == LEIA_CONV_CPU_FMT_RGBA8_SRGB ||
	       fmt == LEIA_CONV_CPU_FMT_BGRA8_UNORM || fmt == LEIA_CONV_CPU_FMT_BGRA8_SRGB;
}

static bool
is_bgra(uint32_t fmt)
{
	return fmt == LEIA_CONV_CPU_FMT_BGRA8_UNORM || fmt == LEIA_CONV_CPU_FMT_BGRA8_SRGB;
}

//! The image fits its buffer: (h - 1) full rows plus one packed row of 4-byte texels.
static bool
fits(uint64_t bytes, uint32_t w, uint32_t h, uint32_t stride)
{
	if (w == 0 || h == 0 || stride < (uint64_t)w * 4u) {
		return false;
	}
	const uint64_t need = (uint64_t)stride * (uint64_t)(h - 1u) + (uint64_t)w * 4u;
	return bytes == 0 || bytes >= need; // byte_size 0 = unknown: trust the stride
}

bool
leia_conv_cpu_pack_bgr8(const void *src,
                        uint64_t src_bytes,
                        uint32_t w,
                        uint32_t h,
                        uint32_t row_stride,
                        uint32_t fmt,
                        uint8_t *dst)
{
	if (src == NULL || dst == NULL || !leia_conv_cpu_format_ok(fmt)) {
		return false;
	}
	if (row_stride == 0) {
		row_stride = w * 4u;
	}
	if (!fits(src_bytes, w, h, row_stride)) {
		return false;
	}
	// Source byte order -> BGR: RGBA reads (2, 1, 0), BGRA reads (0, 1, 2).
	const uint32_t b = is_bgra(fmt) ? 0u : 2u;
	const uint32_t r = is_bgra(fmt) ? 2u : 0u;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *s = (const uint8_t *)src + (size_t)y * row_stride;
		uint8_t *d = dst + (size_t)y * w * 3u;
		for (uint32_t x = 0; x < w; x++, s += 4, d += 3) {
			d[0] = s[b];
			d[1] = s[1];
			d[2] = s[r];
		}
	}
	return true;
}

bool
leia_conv_cpu_write_views(const uint8_t *src,
                          uint32_t sw,
                          uint32_t sh,
                          uint32_t src_stride,
                          uint32_t views,
                          void *dst,
                          uint64_t dst_bytes,
                          uint32_t dw,
                          uint32_t dh,
                          uint32_t dst_stride,
                          uint32_t dst_fmt)
{
	if (src == NULL || dst == NULL || views == 0 || sw < views || sh == 0 || dw < views || dh == 0 ||
	    !leia_conv_cpu_format_ok(dst_fmt)) {
		return false;
	}
	if (src_stride == 0) {
		src_stride = sw * 3u;
	}
	if (src_stride < sw * 3u) {
		return false;
	}
	if (dst_stride == 0) {
		dst_stride = dw * 4u;
	}
	if (!fits(dst_bytes, dw, dh, dst_stride)) {
		return false;
	}
	// BGR source -> target byte order.
	const uint32_t ob = is_bgra(dst_fmt) ? 0u : 2u;
	const uint32_t orr = is_bgra(dst_fmt) ? 2u : 0u;
	if (sw == dw && sh == dh) {
		// Same extent (the usual case: NeurD's views at the input size): a swizzle, no resample.
		for (uint32_t y = 0; y < dh; y++) {
			const uint8_t *s = src + (size_t)y * src_stride;
			uint8_t *d = (uint8_t *)dst + (size_t)y * dst_stride;
			for (uint32_t x = 0; x < dw; x++, s += 3, d += 4) {
				d[ob] = s[0];
				d[1] = s[1];
				d[orr] = s[2];
				d[3] = 255u;
			}
		}
		return true;
	}
	const float sy_scale = (float)sh / (float)dh;
	for (uint32_t v = 0; v < views; v++) {
		const uint32_t sx0 = (uint32_t)(((uint64_t)sw * v) / views);
		const uint32_t sx1 = (uint32_t)(((uint64_t)sw * (v + 1u)) / views);
		const uint32_t dx0 = (uint32_t)(((uint64_t)dw * v) / views);
		const uint32_t dx1 = (uint32_t)(((uint64_t)dw * (v + 1u)) / views);
		const uint32_t stw = sx1 - sx0;
		const uint32_t dtw = dx1 - dx0;
		if (stw == 0 || dtw == 0) {
			continue;
		}
		const float sx_scale = (float)stw / (float)dtw;
		for (uint32_t y = 0; y < dh; y++) {
			float fy = ((float)y + 0.5f) * sy_scale - 0.5f;
			fy = fy < 0.0f ? 0.0f : (fy > (float)(sh - 1u) ? (float)(sh - 1u) : fy);
			const uint32_t y0 = (uint32_t)fy;
			const uint32_t y1 = y0 + 1u < sh ? y0 + 1u : y0;
			const float wy = fy - (float)y0;
			const uint8_t *r0 = src + (size_t)y0 * src_stride;
			const uint8_t *r1 = src + (size_t)y1 * src_stride;
			uint8_t *d = (uint8_t *)dst + (size_t)y * dst_stride + (size_t)dx0 * 4u;
			for (uint32_t x = 0; x < dtw; x++, d += 4) {
				float fx = ((float)x + 0.5f) * sx_scale - 0.5f;
				fx = fx < 0.0f ? 0.0f : (fx > (float)(stw - 1u) ? (float)(stw - 1u) : fx);
				const uint32_t x0 = (uint32_t)fx;
				const uint32_t x1 = x0 + 1u < stw ? x0 + 1u : x0;
				const float wx = fx - (float)x0;
				const uint8_t *p00 = r0 + (size_t)(sx0 + x0) * 3u;
				const uint8_t *p01 = r0 + (size_t)(sx0 + x1) * 3u;
				const uint8_t *p10 = r1 + (size_t)(sx0 + x0) * 3u;
				const uint8_t *p11 = r1 + (size_t)(sx0 + x1) * 3u;
				uint8_t bgr[3];
				for (int c = 0; c < 3; c++) {
					const float top = (float)p00[c] + ((float)p01[c] - (float)p00[c]) * wx;
					const float bot = (float)p10[c] + ((float)p11[c] - (float)p10[c]) * wx;
					const float val = top + (bot - top) * wy;
					bgr[c] = (uint8_t)(val + 0.5f);
				}
				d[ob] = bgr[0];
				d[1] = bgr[1];
				d[orr] = bgr[2];
				d[3] = 255u;
			}
		}
	}
	return true;
}

bool
leia_conv_cpu_write_depth(const float *src,
                          uint32_t sw,
                          uint32_t sh,
                          uint32_t src_stride,
                          void *dst,
                          uint64_t dst_bytes,
                          uint32_t dw,
                          uint32_t dh,
                          uint32_t dst_stride,
                          uint32_t dst_fmt)
{
	if (src == NULL || dst == NULL || sw == 0 || sh == 0 || dst_fmt != LEIA_CONV_CPU_FMT_R32_FLOAT) {
		return false;
	}
	if (src_stride == 0) {
		src_stride = sw * 4u;
	}
	if (dst_stride == 0) {
		dst_stride = dw * 4u;
	}
	// One float per texel: the 4-byte rule of fits() applies as is.
	if (src_stride < sw * 4u || !fits(dst_bytes, dw, dh, dst_stride)) {
		return false;
	}
	for (uint32_t y = 0; y < dh; y++) {
		uint32_t sy = (uint32_t)(((uint64_t)y * sh * 2u + sh) / (2u * (uint64_t)dh));
		sy = sy < sh ? sy : sh - 1u;
		const float *row = (const float *)((const uint8_t *)src + (size_t)sy * src_stride);
		float *d = (float *)((uint8_t *)dst + (size_t)y * dst_stride);
		for (uint32_t x = 0; x < dw; x++) {
			uint32_t sx = (uint32_t)(((uint64_t)x * sw * 2u + sw) / (2u * (uint64_t)dw));
			sx = sx < sw ? sx : sw - 1u;
			d[x] = row[sx];
		}
	}
	return true;
}
