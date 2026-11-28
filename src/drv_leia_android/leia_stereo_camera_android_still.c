// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  RGBA -> NV12 for the JPEG still. See the header.
 * @ingroup drv_leia_android
 */

#include "leia_stereo_camera_android_still.h"

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define LEIA_SCAM_STILL_NEON 1
#endif

//! 2x2 block at column @p x of rows @p r0 / @p r1 (RGBA) into Y (two rows) and one CbCr pair.
static inline void
block_scalar(const uint8_t *r0, const uint8_t *r1, uint8_t *y0, uint8_t *y1, uint8_t *c)
{
	const uint8_t *px[4] = {r0, r0 + 4, r1, r1 + 4};
	uint8_t *py[4] = {y0, y0 + 1, y1, y1 + 1};
	int sr = 0, sg = 0, sb = 0;
	for (int k = 0; k < 4; k++) {
		const int R = px[k][0], G = px[k][1], B = px[k][2];
		*py[k] = (uint8_t)((77 * R + 150 * G + 29 * B + 128) >> 8);
		sr += R;
		sg += G;
		sb += B;
	}
	// The sums' averages, then Cb / Cr with the +128 offset folded in (never negative).
	const int cb = (-43 * sr - 85 * sg + 128 * sb + 4 * 32768 + 512) >> 10;
	const int cr = (128 * sr - 107 * sg - 21 * sb + 4 * 32768 + 512) >> 10;
	c[0] = (uint8_t)(cb < 0 ? 0 : (cb > 255 ? 255 : cb));
	c[1] = (uint8_t)(cr < 0 ? 0 : (cr > 255 ? 255 : cr));
}

static void
rows_scalar(const uint8_t *r0, const uint8_t *r1, uint8_t *y0, uint8_t *y1, uint8_t *c, uint32_t x_begin, uint32_t w)
{
	for (uint32_t x = x_begin; x < w; x += 2) {
		block_scalar(r0 + 4 * (size_t)x, r1 + 4 * (size_t)x, y0 + x, y1 + x, c + x);
	}
}

void
leia_scam_rgba_to_nv12_scalar(const uint8_t *rgba,
                              size_t rgba_stride,
                              uint32_t ox,
                              uint32_t oy,
                              uint32_t w,
                              uint32_t h,
                              uint8_t *y_plane,
                              uint8_t *uv_plane,
                              size_t dst_stride)
{
	for (uint32_t y = 0; y < h; y += 2) {
		const uint8_t *r0 = rgba + (size_t)(y + oy) * rgba_stride + (size_t)ox * 4;
		rows_scalar(r0, r0 + rgba_stride, y_plane + (size_t)y * dst_stride, y_plane + (size_t)(y + 1) * dst_stride,
		            uv_plane + (size_t)(y / 2) * dst_stride, 0, w);
	}
}

#ifdef LEIA_SCAM_STILL_NEON
//! Luma of 16 pixels: (77 R + 150 G + 29 B + 128) >> 8, exactly.
static inline uint8x16_t
luma16(uint8x16x4_t p)
{
	uint16x8_t lo = vmull_u8(vget_low_u8(p.val[0]), vdup_n_u8(77));
	lo = vmlal_u8(lo, vget_low_u8(p.val[1]), vdup_n_u8(150));
	lo = vmlal_u8(lo, vget_low_u8(p.val[2]), vdup_n_u8(29));
	uint16x8_t hi = vmull_u8(vget_high_u8(p.val[0]), vdup_n_u8(77));
	hi = vmlal_u8(hi, vget_high_u8(p.val[1]), vdup_n_u8(150));
	hi = vmlal_u8(hi, vget_high_u8(p.val[2]), vdup_n_u8(29));
	// max 255 * 256 + 128 < 65536: the rounding narrow is exact.
	return vcombine_u8(vrshrn_n_u16(lo, 8), vrshrn_n_u16(hi, 8));
}

//! One chroma value per 2x2 block: (k0 a + k1 b + k2 c + 131584) >> 10 on the 4-pixel sums, saturated to u8.
static inline uint8x8_t
chroma8(int16x8_t a, int16x8_t b, int16x8_t c, int16_t k0, int16_t k1, int16_t k2)
{
	int32x4_t lo = vdupq_n_s32(4 * 32768 + 512);
	lo = vmlal_n_s16(lo, vget_low_s16(a), k0);
	lo = vmlal_n_s16(lo, vget_low_s16(b), k1);
	lo = vmlal_n_s16(lo, vget_low_s16(c), k2);
	int32x4_t hi = vdupq_n_s32(4 * 32768 + 512);
	hi = vmlal_n_s16(hi, vget_high_s16(a), k0);
	hi = vmlal_n_s16(hi, vget_high_s16(b), k1);
	hi = vmlal_n_s16(hi, vget_high_s16(c), k2);
	lo = vshrq_n_s32(lo, 10);
	hi = vshrq_n_s32(hi, 10);
	return vqmovn_u16(vcombine_u16(vqmovun_s32(lo), vqmovun_s32(hi)));
}
#endif

void
leia_scam_rgba_to_nv12(const uint8_t *rgba,
                       size_t rgba_stride,
                       uint32_t ox,
                       uint32_t oy,
                       uint32_t w,
                       uint32_t h,
                       uint8_t *y_plane,
                       uint8_t *uv_plane,
                       size_t dst_stride)
{
#ifdef LEIA_SCAM_STILL_NEON
	const uint32_t w16 = w & ~15u;
	for (uint32_t y = 0; y < h; y += 2) {
		const uint8_t *r0 = rgba + (size_t)(y + oy) * rgba_stride + (size_t)ox * 4;
		const uint8_t *r1 = r0 + rgba_stride;
		uint8_t *y0 = y_plane + (size_t)y * dst_stride, *y1 = y0 + dst_stride;
		uint8_t *c = uv_plane + (size_t)(y / 2) * dst_stride;
		for (uint32_t x = 0; x < w16; x += 16) {
			const uint8x16x4_t p0 = vld4q_u8(r0 + 4 * (size_t)x);
			const uint8x16x4_t p1 = vld4q_u8(r1 + 4 * (size_t)x);
			vst1q_u8(y0 + x, luma16(p0));
			vst1q_u8(y1 + x, luma16(p1));
			// 2x2 sums: horizontal pairs of each row, then the two rows (<= 1020).
			const int16x8_t sr = vreinterpretq_s16_u16(vaddq_u16(vpaddlq_u8(p0.val[0]), vpaddlq_u8(p1.val[0])));
			const int16x8_t sg = vreinterpretq_s16_u16(vaddq_u16(vpaddlq_u8(p0.val[1]), vpaddlq_u8(p1.val[1])));
			const int16x8_t sb = vreinterpretq_s16_u16(vaddq_u16(vpaddlq_u8(p0.val[2]), vpaddlq_u8(p1.val[2])));
			uint8x8x2_t cbcr;
			cbcr.val[0] = chroma8(sr, sg, sb, -43, -85, 128);
			cbcr.val[1] = chroma8(sr, sg, sb, 128, -107, -21);
			vst2_u8(c + x, cbcr);
		}
		rows_scalar(r0, r1, y0, y1, c, w16, w);
	}
#else
	leia_scam_rgba_to_nv12_scalar(rgba, rgba_stride, ox, oy, w, h, y_plane, uv_plane, dst_stride);
#endif
}
