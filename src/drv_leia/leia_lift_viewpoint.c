// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Lift viewpoint mapping (metres -> NeurD units). See the header.
 * @ingroup drv_leia
 */

#include "leia_lift_viewpoint.h"

#include <math.h>

static float
vp_clamp(float v, float lim)
{
	if (!(v == v)) { // NaN in -> centre out
		return 0.0f;
	}
	return v < -lim ? -lim : (v > lim ? lim : v);
}

void
leia_lift_vp_map(const struct leia_lift_vp_policy *p,
                 const struct leia_lift_vp_gains *g,
                 const float in_m[3],
                 float out_n[3])
{
	const float view_gain = g != NULL ? g->view_gain : 1.0f;
	const float y_gain = g != NULL ? g->y_gain : 1.0f;
	const float z_gain = g != NULL ? g->z_gain : 0.5f;

	if (p == NULL || !p->active) {
		// LEGACY. A lenticular panel has horizontal parallax only: passing the
		// raw panel-centred eye HEIGHT made NeurD render the frame as seen from
		// above/below the display centre — the lifted image shifted with head
		// height ("jumps") and a filled band appeared at the top edge (David,
		// panel, 2026-09-26). Head z has no defined reference here.
		out_n[0] = vp_clamp(view_gain * in_m[0] / LEIA_LIFT_VP_REF_BASELINE_M, LEIA_LIFT_VP_CLAMP);
		out_n[1] = 0.0f;
		out_n[2] = 0.0f;
		return;
	}

	// The unit is the NOMINAL 63 mm baseline, never the reported one: the
	// runtime already scaled the eyes by its ipd factor, so dividing by the
	// post-factor baseline would undo it (the pair would always land on ±0.5).
	const float u = LEIA_LIFT_VP_REF_BASELINE_M;
	// baseline_m only widens the clamp: the runtime clamps the eyes' MIDPOINT,
	// and each eye sits half the pair's separation off it.
	const float b = p->baseline_m > 0.0f ? p->baseline_m : LEIA_LIFT_VP_REF_BASELINE_M;
	const float lim =
	    p->max_offset_m > 0.0f ? fabsf(view_gain) * (p->max_offset_m + 0.5f * b) / u : LEIA_LIFT_VP_CLAMP;
	const uint32_t axis = (p->axis_mode == LEIA_LIFT_AXIS_XY || p->axis_mode == LEIA_LIFT_AXIS_XYZ)
	                          ? p->axis_mode
	                          : LEIA_LIFT_AXIS_X;

	out_n[0] = vp_clamp(view_gain * in_m[0] / u, lim);

	// The runtime let y through (rect-relative, recentred): honour it. y_gain
	// is the tuning knob in case vertical parallax still reads as jumps.
	out_n[1] = axis >= LEIA_LIFT_AXIS_XY ? vp_clamp(y_gain * view_gain * in_m[1] / u, fabsf(y_gain) * lim) : 0.0f;

	// z: distance from the rect plane, metres, +toward the viewer. Pinned axes
	// arrive AT the reference distance, so (z - ref) / ref is the fractional
	// distance change; z_gain maps it to NeurD's (unspecified) depth offset.
	if (axis == LEIA_LIFT_AXIS_XYZ) {
		const float ref = p->ref_z_m > 0.0f ? p->ref_z_m : LEIA_LIFT_VP_DEFAULT_REF_Z_M;
		out_n[2] = vp_clamp(z_gain * (in_m[2] - ref) / ref, LEIA_LIFT_VP_Z_CLAMP);
	} else {
		out_n[2] = 0.0f;
	}
}
