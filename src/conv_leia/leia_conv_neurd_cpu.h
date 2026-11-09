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
 * leia_conv_neurd_posix.cpp and converts through NeurD's CPU entry points
 * (packed BGR8 in, BGR8 side-by-side views out): NeurD_convert_stream_cpu_ex
 * when the loaded NeurD has it (>= 0.5.1) — the Windows module's camera and
 * float depth, built by the same helpers — else NeurD_convert_stream_cpu (a
 * fixed pair: views only, no look-around).
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
 * Applies @p params and @p viewpoints_m (the runtime's two viewpoints, metres,
 * rect-relative when params->has_policy; NULL = none) the way the Windows
 * module's leia_lift_neurd_convert does: the metric off-axis camera
 * (leia_lift_oa_build) when the runtime passed its rect-relative policy, else
 * the dimensionless pattern (leia_lift_vp_map); strength x DepthGain ->
 * NEURD_PROP_GAIN_MULTIPLIER, convergence < 0 = NeurD's own auto-convergence,
 * else the relative depth at the glass mapped through ConvGain (or the metric
 * plane through the stream's model scale); inpaint != 0 = the blur fill.
 * Without NeurD_convert_stream_cpu_ex: NeurD's fixed pair with the eyes'
 * separation as a gain factor. The depth of the same inference (streams created
 * with LEIA_LIFT_AUX_DEPTH) is then read with leia_lift_neurd_get_depth, whose
 * resource is a host float32 array (row stride width * 4, format 0) owned by
 * NeurD until the next convert on the stream.
 *
 * [S] only (the runtime serialises it). Never throws.
 */
bool
leia_lift_neurd_convert_cpu(struct leia_lift_neurd *lift,
                            uint64_t id,
                            const struct leia_conv_bgr8_image *in,
                            const struct leia_lift_neurd_params *params,
                            const float *viewpoints_m,
                            uint32_t viewpoint_floats,
                            struct leia_conv_bgr8_image *out);

#ifdef __cplusplus
}
#endif
