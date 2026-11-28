// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Platform-neutral half of the Android stereo camera's JPEG still: the
 *         RGBA -> NV12 conversion of one decoded eye.
 *
 * AImageDecoder (the NDK's only JPEG decoder) decodes to RGBA_8888, RGB_565,
 * A_8 or RGBA_F16, never to YCbCr, and the NDK exposes no libjpeg, so the
 * still is decoded to RGBA and converted here. This file is ALWAYS compiled
 * with -O2 (CMakeLists, per file), also in a Debug plug-in: at -O0 the
 * conversion of a 4096x3072 eye takes seconds. It holds no struct the
 * runtime ABI sees. A NEON path (aarch64) and a scalar path give bit-identical
 * results (tests/test_stereo_camera_android_still.c).
 *
 * @ingroup drv_leia_android
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Convert the @p w x @p h region at (@p ox, @p oy) of an RGBA_8888 image
 * (@p rgba, @p rgba_stride bytes per row) to NV12: luma at @p y_plane, CbCr
 * interleaved at @p uv_plane, both @p dst_stride bytes per row (an SBS eye
 * writes at its column offset into a stride twice its width). Full-range
 * BT.601 (what the camera's own YUV_420_888 is): Y = (77 R + 150 G + 29 B +
 * 128) >> 8, Cb / Cr from each 2x2 block's sums. @p w and @p h are even; alpha
 * is ignored. NEON on aarch64, scalar elsewhere; the same bytes either way.
 */
void
leia_scam_rgba_to_nv12(const uint8_t *rgba,
                       size_t rgba_stride,
                       uint32_t ox,
                       uint32_t oy,
                       uint32_t w,
                       uint32_t h,
                       uint8_t *y_plane,
                       uint8_t *uv_plane,
                       size_t dst_stride);

//! The scalar reference (what leia_scam_rgba_to_nv12 does without NEON), for the tests.
void
leia_scam_rgba_to_nv12_scalar(const uint8_t *rgba,
                              size_t rgba_stride,
                              uint32_t ox,
                              uint32_t oy,
                              uint32_t w,
                              uint32_t h,
                              uint8_t *y_plane,
                              uint8_t *uv_plane,
                              size_t dst_stride);

#ifdef __cplusplus
}
#endif
