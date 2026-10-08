// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Pure math for NeurD's depth export and its metric off-axis camera:
 *         depth metadata -> the runtime's vendor-neutral depth description, and
 *         runtime viewpoints + rect -> NeurD's screen / per-view eye positions.
 *         See docs/lift-neurd.md § "Depth export" and § "Off-axis camera".
 *
 * Platform-neutral C with no NeurD / SR / D3D dependency (the NeurD structs are
 * mirrored by plug-in-owned ones), so it is host-tested on the Linux CI lane
 * (tests/test_lift_depth.c) for the Windows arm.
 *
 * NeurD's depth vocabulary (media_sdk, read from StretchCS / OffAxisCS / #559):
 *  - h  = the 8-bit "disparity" the view renderer reads, sample / 255, in
 *         [0, 1], larger = NEARER. Relative video model: min-max normalised
 *         inverse depth. Metric video model: raw / 255 with raw =
 *         inverse_depth_scale / depth_m (clamped to 255, i.e. depth >= near_m).
 *  - n  = 1 - h, the renderer's normalised depth (0 nearest, 1 farthest).
 *  - Legacy (dimensionless) renderer: zero disparity at n = 5 * convergence +
 *    0.5, i.e. h0 = 0.5 - 5 * convergence (NEURD_PROP_CONVERGENCE, +-0.2).
 *  - Off-axis renderer: n sits at z = C - n * D in front of the screen
 *    (C = convergence_depth_m, D = depth_scale_m), so h0 = 1 - C / D.
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Values identical to the runtime's XRT_DP_LIFT_DEPTH_RELATIVE / _METRIC.
#define LEIA_LIFT_DEPTH_UNITS_RELATIVE 0u
#define LEIA_LIFT_DEPTH_UNITS_METRIC 1u
//! Values identical to the runtime's XRT_DP_LIFT_DEPTH_ENCODING_*.
#define LEIA_LIFT_DEPTH_ENCODING_LINEAR 0u
#define LEIA_LIFT_DEPTH_ENCODING_INVERSE 1u
//! Values identical to the runtime's XRT_DP_LIFT_VIEWPOINTS_*.
#define LEIA_LIFT_VIEWPOINTS_TRACKED 0u
#define LEIA_LIFT_VIEWPOINTS_EXPLICIT 1u
#define LEIA_LIFT_VIEWPOINTS_DISPLAY_RIG 2u
#define LEIA_LIFT_VIEWPOINTS_CAMERA_RIG 3u

//! NeurD's nominal pinhole: focal = 0.78 * max(w, h) source pixels (#559,
//! borrowed from its CUDA tile path). Used only when NeurD reports no focal.
#define LEIA_LIFT_NEURD_FOCAL_FACTOR 0.78f
//! Viewing distance when neither the runtime nor the panel gives one.
#define LEIA_LIFT_OA_DEFAULT_NOMINAL_Z_M 0.5f
//! Eye separation assumed for the default (untracked) off-axis pair.
#define LEIA_LIFT_OA_DEFAULT_BASELINE_M 0.063f
//! The nearest relief plane is kept at most this fraction of the way to the
//! nearest eye (and the nominal source camera): NeurD rejects eye_z <= C.
#define LEIA_LIFT_OA_POPOUT_LIMIT 0.9f
//! Smallest relief thickness passed to NeurD (it rejects depth_scale_m <= 0).
#define LEIA_LIFT_OA_MIN_RELIEF_M 0.001f

/*!
 * Plug-in mirror of NeurD_depth_info (media_sdk #559, NeurD >= 0.4.9).
 */
struct leia_lift_nd_depth_info
{
	uint32_t units; //!< 0 relative, 1 metric metres (NeurD_depth_units)
	int32_t width, height;
	int32_t source_width, source_height;
	float focal_px_x, focal_px_y; //!< depth-texture texels; <= 0 = not reported
	float principal_px_x, principal_px_y;
	float near_m, far_m;
	//! METRIC: raw model output = inverse_depth_scale / depth_m. RELATIVE: 0.
	float inverse_depth_scale;
};

/*!
 * The vendor-neutral description of one depth map (mirrors the metadata
 * fields of the runtime's xrt_dp_lift_depth).
 */
struct leia_lift_depth_meta
{
	uint32_t units;    //!< LEIA_LIFT_DEPTH_UNITS_*
	uint32_t encoding; //!< LEIA_LIFT_DEPTH_ENCODING_*
	float value_scale;
	float value_offset;
	float focal_x_px, focal_y_px; //!< depth-map pixels; <= 0 = unknown
	float principal_x_px, principal_y_px;
	float near_depth, far_depth; //!< decoded, in units; 0 = unknown
	float convergence_depth;     //!< decoded depth at zero disparity; 0 = unknown
};

/*!
 * h at zero disparity of NeurD's LEGACY (dimensionless) renderer for
 * NEURD_PROP_CONVERGENCE @p nd_convergence: 0.5 - 5 * c (StretchCS: the ray
 * crosses its source column at normalised depth 5c + 0.5).
 */
float
leia_lift_nd_legacy_zero_h(float nd_convergence);

/*!
 * Decoded depth for an 8-bit disparity @p h (in units of @p units): metric =
 * inverse_depth_scale / (255 h) metres, relative = 1 / h (the value the depth
 * map decodes to with value_scale 1/255, INVERSE). Returns 0 (unknown) for
 * h <= 0 (zero disparity at infinity / beyond) or a metric scale <= 0.
 */
float
leia_lift_depth_from_h(uint32_t units, float inverse_depth_scale, float h);

/*!
 * Inverse of leia_lift_depth_from_h for METRIC depth: h = S / (255 d); 0 for
 * d <= 0 or S <= 0. Not clamped to [0, 1].
 */
float
leia_lift_h_from_metric_depth(float inverse_depth_scale, float depth_m);

/*!
 * Fill @p out from NeurD's depth info.
 *
 * The R32F map NeurD returns (#559, RawDepthToR32FCS) holds:
 *  - METRIC model: depth in METRES (the shader already computed
 *    inverse_depth_scale / raw; no-signal texels = far_m) -> LINEAR, scale 1.
 *  - RELATIVE model: the raw model value (0..255, larger = nearer) ->
 *    INVERSE, scale 1/255 (decoded d = h, depth = 1/h in relative units).
 * Intrinsics are passed through (NeurD reports them in depth texels); when
 * NeurD reports no focal, the nominal 0.78 * max(source) pinhole is rescaled to
 * the depth map. @p zero_h = h at zero disparity for this frame (legacy:
 * leia_lift_nd_legacy_zero_h, off-axis: leia_lift_oa_build's out_zero_h);
 * convergence_depth = leia_lift_depth_from_h(units, S, zero_h).
 */
void
leia_lift_depth_meta_from_info(const struct leia_lift_nd_depth_info *in,
                               float zero_h,
                               struct leia_lift_depth_meta *out);

/*!
 * Inputs of the off-axis camera mapping.
 */
struct leia_lift_oa_in
{
	float rect_width_m;  //!< lifted rect = ONE output view (NeurD screen), metres;
	                     //!< > 0
	float rect_height_m; //!< > 0
	//! Reference viewing distance (runtime nominal_z_m, else the panel's),
	//! metres;
	//! <= 0 = 0.5. Becomes NeurD's nominal source camera distance.
	float nominal_z_m;
	//! XRT_DP_LIFT_VIEWPOINTS_*: DISPLAY_RIG / CAMERA_RIG = the app's rig eyes,
	//! reproduced exactly (no gain); TRACKED / EXPLICIT get view_gain / y_gain.
	uint32_t viewpoint_source;
	float view_gain; //!< ViewGain (x and y), tracked / explicit only
	float y_gain;    //!< YGain (extra y), tracked / explicit only
	//! Pair separation for the default eyes when none are given; <= 0 = 63 mm.
	float baseline_m;
	//! strength (1 = calibrated / metric; scales the relief thickness); < 0 = 1.
	float strength;

	//! Requested relative convergence (runtime: 0 nearest on the glass ... 1
	//! farthest), i.e. the normalised depth n0 to put on the screen; < 0 = auto.
	float convergence;
	//! AUTO: the module's own auto-convergence as h0 (the last conversion's,
	//! from NeurD); outside (0, 1] = unknown -> mid-depth (n0 = 0.5).
	float auto_zero_h;

	//! Metric model AND its scale known (inverse_depth_scale > 0).
	bool metric;
	float inverse_depth_scale; //!< METRIC: raw = S / depth_m
	float relief_m;            //!< ReliefDepthM: relief thickness for relative depth, metres
	float metric_relief_max_m; //!< MetricReliefMaxM: cap on the metric relief,
	                           //!< metres
};

//! Mirror of NeurD_screen_desc's payload.
struct leia_lift_oa_screen
{
	float screen_width_m;
	float screen_height_m;
	float depth_scale_m;
	float convergence_depth_m;
	float nominal_eye_z_m;
};

/*!
 * Map the runtime's viewpoints onto NeurD's off-axis camera.
 *
 * Eyes: rect-relative metres (x right, y up, z = distance from the rect plane,
 * toward the viewer — the runtime's RECT frame), written to @p out_eyes as
 * (eye_x_m, eye_y_m, eye_z_m) — the same frame as NeurD_view_frustum. z <= 0
 * (unknown) = the nominal distance. @p n_eyes == 0 = the default pair
 * (±baseline/2, 0, nominal). Rig sources pass through untouched; tracked /
 * explicit ones get x *= view_gain, y *= view_gain * y_gain.
 *
 * Depth (n = normalised depth, 0 nearest .. 1 farthest; z = C - n D):
 *  - RELATIVE: D = relief_m * strength, n0 = convergence (auto: from
 *    auto_zero_h, else 0.5), C = n0 D.
 *  - METRIC: d0 = scene depth at the screen (from n0 via h0 = 1 - n0, or
 *    the auto h0); D = 255 N d0 / S * strength — the relief whose depth
 *    gradient at the screen equals a metric scene scaled so that, seen from the
 *    nominal eye at distance N, d0 lands on the screen (z = N (1 - d/d0));
 *    capped at metric_relief_max_m. Exact at the screen plane, first-order
 *    elsewhere: NeurD's relief is linear in h (inverse depth), a metric scene
 *    is not.
 *  - Either way C is limited to POPOUT_LIMIT x min(eye z, N) (NeurD needs
 *    every eye in front of the nearest relief plane); n0 then moves.
 *
 * @param out_eyes  3 * max(n_eyes, 2) floats.
 * @param out_n     views written (n_eyes, or 2 for the default pair).
 * @param out_zero_h h at zero disparity actually used (1 - C / D).
 * @return false on an unusable rect (<= 0) — the caller keeps the legacy path.
 */
bool
leia_lift_oa_build(const struct leia_lift_oa_in *in,
                   const float *eyes_m,
                   uint32_t n_eyes,
                   struct leia_lift_oa_screen *out_screen,
                   float *out_eyes,
                   uint32_t *out_n,
                   float *out_zero_h);

#ifdef __cplusplus
}
#endif
