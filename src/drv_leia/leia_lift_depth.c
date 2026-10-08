// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Lift depth metadata + off-axis camera mapping. See the header.
 * @ingroup drv_leia
 */

#include "leia_lift_depth.h"

#include <math.h>

static float
ld_clamp(float v, float lo, float hi)
{
	if (!(v == v)) { // NaN
		return lo;
	}
	return v < lo ? lo : (v > hi ? hi : v);
}

static bool
ld_finite(float v)
{
	return v == v && v > -INFINITY && v < INFINITY;
}

float
leia_lift_nd_legacy_zero_h(float nd_convergence)
{
	return 0.5f - 5.0f * nd_convergence;
}

float
leia_lift_depth_from_h(uint32_t units, float inverse_depth_scale, float h)
{
	if (!ld_finite(h) || h <= 0.0f) {
		return 0.0f;
	}
	if (units == LEIA_LIFT_DEPTH_UNITS_METRIC) {
		if (!(inverse_depth_scale > 0.0f)) {
			return 0.0f;
		}
		return inverse_depth_scale / (255.0f * h);
	}
	return 1.0f / h;
}

float
leia_lift_h_from_metric_depth(float inverse_depth_scale, float depth_m)
{
	if (!(inverse_depth_scale > 0.0f) || !(depth_m > 0.0f) || !ld_finite(depth_m)) {
		return 0.0f;
	}
	return inverse_depth_scale / (255.0f * depth_m);
}

void
leia_lift_depth_meta_from_info(const struct leia_lift_nd_depth_info *in, float zero_h, struct leia_lift_depth_meta *out)
{
	struct leia_lift_depth_meta m = {0};
	const bool metric = in->units == LEIA_LIFT_DEPTH_UNITS_METRIC && in->inverse_depth_scale > 0.0f;

	if (metric) {
		// NeurD's RawDepthToR32FCS already wrote metres (S / raw, far_m where
		// raw carries no signal): the samples ARE depth.
		m.units = LEIA_LIFT_DEPTH_UNITS_METRIC;
		m.encoding = LEIA_LIFT_DEPTH_ENCODING_LINEAR;
		m.value_scale = 1.0f;
		m.near_depth = in->near_m > 0.0f ? in->near_m : 0.0f;
		m.far_depth = in->far_m > 0.0f ? in->far_m : 0.0f;
	} else {
		// Relative model: the raw 0..255 inverse depth (larger = nearer). With
		// scale 1/255 the decoded value is the renderer's h, depth = 1/h in
		// relative units — the same units convergence_depth is given in. The
		// range is per-frame min-max normalised, so no meaningful near/far.
		m.units = LEIA_LIFT_DEPTH_UNITS_RELATIVE;
		m.encoding = LEIA_LIFT_DEPTH_ENCODING_INVERSE;
		m.value_scale = 1.0f / 255.0f;
	}
	m.value_offset = 0.0f;

	if (in->focal_px_x > 0.0f && in->focal_px_y > 0.0f) {
		m.focal_x_px = in->focal_px_x;
		m.focal_y_px = in->focal_px_y;
		m.principal_x_px = in->principal_px_x;
		m.principal_y_px = in->principal_px_y;
	} else if (in->width > 0 && in->height > 0 && in->source_width > 0 && in->source_height > 0) {
		// NeurD's nominal pinhole in source pixels, rescaled per axis (the
		// model stretches the source to its input size: texels are not square).
		const float f_src =
		    LEIA_LIFT_NEURD_FOCAL_FACTOR *
		    (float)(in->source_width > in->source_height ? in->source_width : in->source_height);
		m.focal_x_px = f_src * (float)in->width / (float)in->source_width;
		m.focal_y_px = f_src * (float)in->height / (float)in->source_height;
		m.principal_x_px = 0.5f * (float)in->width;
		m.principal_y_px = 0.5f * (float)in->height;
	}

	m.convergence_depth = leia_lift_depth_from_h(m.units, in->inverse_depth_scale, zero_h);
	*out = m;
}

bool
leia_lift_oa_build(const struct leia_lift_oa_in *in,
                   const float *eyes_m,
                   uint32_t n_eyes,
                   struct leia_lift_oa_screen *out_screen,
                   float *out_eyes,
                   uint32_t *out_n,
                   float *out_zero_h)
{
	if (in == NULL || !(in->rect_width_m > 0.0f) || !(in->rect_height_m > 0.0f) || !ld_finite(in->rect_width_m) ||
	    !ld_finite(in->rect_height_m)) {
		return false;
	}
	const float nz =
	    (in->nominal_z_m > 0.0f && ld_finite(in->nominal_z_m)) ? in->nominal_z_m : LEIA_LIFT_OA_DEFAULT_NOMINAL_Z_M;

	// ---- Eyes.
	const bool rig = in->viewpoint_source == LEIA_LIFT_VIEWPOINTS_DISPLAY_RIG ||
	                 in->viewpoint_source == LEIA_LIFT_VIEWPOINTS_CAMERA_RIG;
	const float gx = rig ? 1.0f : in->view_gain;
	const float gy = rig ? 1.0f : in->view_gain * in->y_gain;
	uint32_t n = 0;
	if (eyes_m != NULL && n_eyes > 0) {
		for (uint32_t i = 0; i < n_eyes; i++) {
			const float x = eyes_m[i * 3 + 0], y = eyes_m[i * 3 + 1], z = eyes_m[i * 3 + 2];
			out_eyes[i * 3 + 0] = ld_finite(x) ? gx * x : 0.0f;
			out_eyes[i * 3 + 1] = ld_finite(y) ? gy * y : 0.0f;
			out_eyes[i * 3 + 2] = (ld_finite(z) && z > 0.0f) ? z : nz;
		}
		n = n_eyes;
	} else {
		const float b = in->baseline_m > 0.0f ? in->baseline_m : LEIA_LIFT_OA_DEFAULT_BASELINE_M;
		const float e[6] = {-0.5f * b, 0.0f, nz, 0.5f * b, 0.0f, nz};
		for (int i = 0; i < 6; i++) {
			out_eyes[i] = e[i];
		}
		n = 2;
	}
	float zmin = nz;
	for (uint32_t i = 0; i < n; i++) {
		if (out_eyes[i * 3 + 2] < zmin) {
			zmin = out_eyes[i * 3 + 2];
		}
	}

	// ---- Depth: n0 (normalised depth on the screen) and D (relief thickness).
	const float strength = in->strength >= 0.0f ? in->strength : 1.0f;
	float h0;
	if (in->convergence >= 0.0f) {
		h0 = 1.0f - ld_clamp(in->convergence, 0.0f, 1.0f);
	} else if (in->auto_zero_h > 0.0f && in->auto_zero_h <= 1.0f) {
		h0 = in->auto_zero_h;
	} else {
		h0 = 0.5f; // mid-depth
	}
	const float n0 = ld_clamp(1.0f - h0, 0.0f, 1.0f);

	float D;
	if (in->metric && in->inverse_depth_scale > 0.0f) {
		const float cap = in->metric_relief_max_m > LEIA_LIFT_OA_MIN_RELIEF_M ? in->metric_relief_max_m
		                                                                      : LEIA_LIFT_OA_MIN_RELIEF_M;
		// d0 = S / (255 h0); D = 255 N d0 / S = N / h0. (h0 -> 0: d0 -> infinity,
		// capped.)
		D = (h0 > 0.0f) ? nz / h0 : cap;
		D *= strength;
		if (!(D < cap)) {
			D = cap;
		}
	} else {
		D = in->relief_m * strength;
	}
	if (!(D > LEIA_LIFT_OA_MIN_RELIEF_M) || !ld_finite(D)) {
		D = LEIA_LIFT_OA_MIN_RELIEF_M;
	}

	float C = n0 * D;
	const float c_max = LEIA_LIFT_OA_POPOUT_LIMIT * zmin;
	if (C > c_max) {
		C = c_max; // nothing may come out past (most of the way to) the nearest eye
	}

	out_screen->screen_width_m = in->rect_width_m;
	out_screen->screen_height_m = in->rect_height_m;
	out_screen->depth_scale_m = D;
	out_screen->convergence_depth_m = C;
	out_screen->nominal_eye_z_m = nz;
	*out_n = n;
	*out_zero_h = 1.0f - C / D;
	return true;
}
