// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  CPU-memory transport of the Leia conversion provider (POSIX arm):
 *         host-memory input -> the packed BGR8 frame NeurD's CPU path reads,
 *         and NeurD's BGR8 side-by-side views -> a host-memory RGBA8 / BGRA8
 *         target of any extent.
 *
 * The Windows arm moves pixels with D3D11 (leia_conv_d3d11.*); desktop Linux
 * has no D3D11, and NeurD's Linux backend (OpenVINO) only has a CPU entry
 * point, so this arm imports XRT_CONV_MEM_CPU only (the runtime copies GPU
 * images to host memory for such a provider, ADR-053 P5).
 *
 * Pure C: no NeurD, no runtime header (formats are passed as the
 * XRT_CONV_FMT_* values, mirrored below), so tests/test_conv_leia_cpu.c runs
 * it on every lane.
 *
 * @ingroup conv_leia
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! @name Mirrors of the XRT_CONV_FMT_* this transport reads / writes @{ */
#define LEIA_CONV_CPU_FMT_RGBA8_UNORM 1u
#define LEIA_CONV_CPU_FMT_RGBA8_SRGB 2u
#define LEIA_CONV_CPU_FMT_BGRA8_UNORM 3u
#define LEIA_CONV_CPU_FMT_BGRA8_SRGB 4u
#define LEIA_CONV_CPU_FMT_R32_FLOAT 8u
/*! @} */

//! True for the four 4-byte colour formats this transport handles.
bool
leia_conv_cpu_format_ok(uint32_t fmt);

/*!
 * Pack a host-memory RGBA8 / BGRA8 image into tightly packed BGR8 (stride
 * w * 3, the layout NeurD_convert_stream_cpu reads; NeurD ignores the input
 * stride). Alpha is dropped. @p dst must hold w * h * 3 bytes.
 *
 * @return false on a bad argument (null, zero extent, an unknown format, or
 *         a stride / byte_size too small for the image).
 */
bool
leia_conv_cpu_pack_bgr8(const void *src,
                        uint64_t src_bytes,
                        uint32_t w,
                        uint32_t h,
                        uint32_t row_stride,
                        uint32_t fmt,
                        uint8_t *dst);

/*!
 * Write @p views side-by-side BGR8 views (@p src, @p sw x @p sh, row stride
 * @p src_stride, view i = columns [i * sw / views, (i + 1) * sw / views)) into
 * a host-memory RGBA8 / BGRA8 target of @p dw x @p dh with the same
 * side-by-side layout. Each view is resampled bilinearly inside its own tile,
 * so no tile ever samples its neighbour. Alpha is written opaque.
 *
 * @return false on a bad argument (as above).
 */
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
                          uint32_t dst_fmt);

/*!
 * Write a float depth map (@p src, @p sw x @p sh floats, row stride @p src_stride
 * bytes; 0 = packed) over the whole of a host-memory R32_FLOAT target of @p dw x
 * @p dh (row stride @p dst_stride bytes; 0 = packed), point-sampled (depth is
 * never blended across an edge), values as is.
 *
 * @return false on a bad argument (null, zero extent, a format other than
 *         R32_FLOAT, or a stride / byte_size too small).
 */
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
                          uint32_t dst_fmt);

#ifdef __cplusplus
}
#endif
