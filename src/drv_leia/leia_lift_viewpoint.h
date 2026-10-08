// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Pure mapping: lift viewpoints in metres -> NeurD's dimensionless
 *         viewpoint units. See docs/lift-neurd.md § "Tracked eyes -> NeurD
 *         viewpoints".
 *
 * Platform-neutral C with no NeurD / SR / D3D dependency, so it is host-tested
 * on the Linux CI lane (tests/test_lift_viewpoint.c) for the Windows arm.
 *
 * Two contracts:
 *  - LEGACY (a runtime without the viewpoint policy, or eyes the plug-in read
 *    from its own tracker): panel-centred eyes, x only, a fixed 63 mm unit,
 *    a ±3 clamp. Byte-for-byte the pre-policy behaviour.
 *  - POLICY (runtime ADR-048, XRT_DP_LIFT_HAS_VIEWPOINT_POLICY): the runtime
 *    has already rebased the eyes to the lifted rect, applied the ipd /
 *    parallax factors, the axis mask, its clamp and the recentering ease. The
 *    plug-in only translates units: one NeurD x unit = the NOMINAL 63 mm eye
 *    baseline (NeurD's ±0.5 default pair), so the runtime's ipd factor shows
 *    through; every axis the runtime let through is honoured.
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Values identical to the runtime's XRT_DP_LIFT_AXIS_*.
#define LEIA_LIFT_AXIS_X 1u
#define LEIA_LIFT_AXIS_XY 2u
#define LEIA_LIFT_AXIS_XYZ 3u

//! The nominal eye baseline: one NeurD x unit in both contracts (NeurD's
//! default ±0.5 pair == a 63 mm IPD). Also the pair separation assumed for the
//! clamp margin when the runtime reports none (baseline_m == 0).
#define LEIA_LIFT_VP_REF_BASELINE_M 0.063f
//! Legacy x clamp, and the policy x/y clamp when the runtime applied none.
#define LEIA_LIFT_VP_CLAMP 3.0f
//! Clamp for the mapped z (fractional distance change times ZGain).
#define LEIA_LIFT_VP_Z_CLAMP 1.0f
//! Reference viewing distance when neither the runtime nor the panel gives one
//! (equals the runtime's own nominal-distance default).
#define LEIA_LIFT_VP_DEFAULT_REF_Z_M 0.5f

/*!
 * What the runtime told us about the viewpoints of one convert.
 */
struct leia_lift_vp_policy
{
	//! True = POLICY contract (the runtime filled the viewpoint-policy fields
	//! AND the viewpoints came from the runtime). False = LEGACY.
	bool active;
	float baseline_m;   //!< Outermost-viewpoint distance (eye separation), metres; 0 = unknown. Clamp margin only.
	uint32_t axis_mode; //!< LEIA_LIFT_AXIS_*; anything else is read as X.
	float max_offset_m; //!< The runtime's x/y midpoint clamp, metres; 0 = unclamped.
	float ref_z_m;      //!< Reference viewing distance (the eyes' z when z is pinned); <= 0 = default.
};

/*!
 * Per-stream gains (knobs snapshotted at stream create).
 */
struct leia_lift_vp_gains
{
	float view_gain; //!< ViewGain: x (and y) scale, [0, 10].
	float y_gain;    //!< YGain: extra y scale on top of view_gain, [0, 10].
	float z_gain;    //!< ZGain: z scale, [-2, 2] (negative flips the sign).
};

/*!
 * Map one viewpoint (x, y, z metres) to NeurD units.
 *
 * LEGACY: x_n = clamp(view_gain·x / 0.063, ±3), y_n = z_n = 0.
 *
 * POLICY, with b = baseline_m (0.063 when 0):
 *   x_n = clamp(view_gain·x / 0.063, ±L)
 *   y_n = clamp(y_gain·view_gain·y / 0.063, ±y_gain·L)  when axis_mode >= XY, else 0
 *   z_n = clamp(z_gain·(z − ref_z) / ref_z, ±1)          when axis_mode == XYZ, else 0
 *   L   = view_gain·(max_offset_m + b/2) / 0.063         when max_offset_m > 0, else 3
 * The unit stays the nominal 63 mm: the runtime already applied its ipd
 * factor to the eyes, so a 32 mm pair maps to ±0.254 (softer stereo), not
 * ±0.5. The b/2 is the half-pair an eye sits off the clamped midpoint: the
 * runtime clamps the MIDPOINT, so clamping each eye at the midpoint's limit
 * would collapse the pair at the edge.
 *
 * NULL @p p = LEGACY; NULL @p g = gains 1 / 1 / 0.5.
 */
void
leia_lift_vp_map(const struct leia_lift_vp_policy *p,
                 const struct leia_lift_vp_gains *g,
                 const float in_m[3],
                 float out_n[3]);

#ifdef __cplusplus
}
#endif
