// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The NeurD module of the conversion provider's POSIX arm: the
 *         leia_lift_neurd.h lifecycle (create / probe / activate / status /
 *         streams) over libNeurD.so, plus a CPU convert.
 *
 * The Windows arm compiles ../drv_leia/leia_lift_neurd.cpp, whose convert is
 * D3D11 end to end. NeurD's Linux build has no D3D11 and no CUDA-free GPU entry
 * point, so this arm implements the same lifecycle API in
 * leia_conv_neurd_posix.cpp and converts through NeurD_convert_stream_cpu
 * (packed BGR8 in, BGR8 side-by-side views out). That path returns views only:
 * no float depth, no off-axis camera, no explicit viewpoints.
 *
 * @ingroup conv_leia
 */

#pragma once

#include "leia_lift_neurd.h"

#ifdef __cplusplus
extern "C" {
#endif

//! A packed BGR8 frame in host memory (stride in bytes, >= width * 3).
struct leia_conv_bgr8_image
{
	const uint8_t *data;
	uint32_t width, height, stride;
};

/*!
 * Convert one packed BGR8 frame on stream @p id into two side-by-side BGR8
 * views (@p out, owned by NeurD, valid until the next convert on any stream).
 * Applies @p params the way the Windows module does: strength x DepthGain ->
 * NEURD_PROP_GAIN_MULTIPLIER, convergence < 0 = NeurD's own
 * auto-convergence, else the relative depth at the glass mapped through
 * ConvGain; inpaint != 0 = the blur fill. Per-stream properties when the
 * loaded NeurD has them (>= 0.4.9), process-wide ones otherwise.
 *
 * [S] only (the runtime serialises it). Never throws.
 */
bool
leia_lift_neurd_convert_cpu(struct leia_lift_neurd *lift,
                            uint64_t id,
                            const struct leia_conv_bgr8_image *in,
                            const struct leia_lift_neurd_params *params,
                            struct leia_conv_bgr8_image *out);

#ifdef __cplusplus
}
#endif
