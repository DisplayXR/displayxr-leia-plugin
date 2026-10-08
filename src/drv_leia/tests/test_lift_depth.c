// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the lift depth metadata conversion and the off-axis
 *         camera mapping (leia_lift_depth.h).
 *
 * Hardware-free and host-runnable (the Linux CI job builds it for the Windows
 * arm): pure arithmetic, no NeurD / SR / D3D.
 */

#include "leia_lift_depth.h"

#include <math.h>
#include <stdio.h>

static int g_failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)
#define NEAR_REL(a, b) (fabsf((a) - (b)) <= 1e-4f * fabsf(b))

static void
test_zero_h_and_depth(void)
{
	// Legacy renderer: convergence 0 -> zero disparity at mid-depth (h 0.5);
	// +-0.1 -> the ends of the normalised range.
	CHECK(NEAR(leia_lift_nd_legacy_zero_h(0.0f), 0.5f));
	CHECK(NEAR(leia_lift_nd_legacy_zero_h(0.1f), 0.0f));
	CHECK(NEAR(leia_lift_nd_legacy_zero_h(-0.1f), 1.0f));

	// Metric (S = 100): h 0.5 -> raw 127.5 -> 0.784 m; h 1 -> near (0.39 m).
	CHECK(NEAR_REL(leia_lift_depth_from_h(LEIA_LIFT_DEPTH_UNITS_METRIC, 100.0f, 0.5f), 100.0f / 127.5f));
	CHECK(NEAR_REL(leia_lift_depth_from_h(LEIA_LIFT_DEPTH_UNITS_METRIC, 100.0f, 1.0f), 100.0f / 255.0f));
	// Relative: 1/h.
	CHECK(NEAR(leia_lift_depth_from_h(LEIA_LIFT_DEPTH_UNITS_RELATIVE, 0.0f, 0.25f), 4.0f));
	// Unknown: zero disparity at/behind infinity, or a metric map without scale.
	CHECK(leia_lift_depth_from_h(LEIA_LIFT_DEPTH_UNITS_METRIC, 100.0f, 0.0f) == 0.0f);
	CHECK(leia_lift_depth_from_h(LEIA_LIFT_DEPTH_UNITS_RELATIVE, 0.0f, -0.2f) == 0.0f);
	CHECK(leia_lift_depth_from_h(LEIA_LIFT_DEPTH_UNITS_METRIC, 0.0f, 0.5f) == 0.0f);

	// Round trip.
	const float h = leia_lift_h_from_metric_depth(100.0f, 2.0f);
	CHECK(NEAR_REL(leia_lift_depth_from_h(LEIA_LIFT_DEPTH_UNITS_METRIC, 100.0f, h), 2.0f));
	CHECK(leia_lift_h_from_metric_depth(100.0f, 0.0f) == 0.0f);
}

static void
test_meta_metric(void)
{
	// What #559 reports for the metric video model at 640x416 from a 1280x720
	// frame.
	struct leia_lift_nd_depth_info in = {0};
	in.units = 1;
	in.width = 640;
	in.height = 416;
	in.source_width = 1280;
	in.source_height = 720;
	in.focal_px_x = 0.78f * 1280.0f * 640.0f / 1280.0f; // 499.2
	in.focal_px_y = 0.78f * 1280.0f * 416.0f / 720.0f;  // 576.9
	in.principal_px_x = 320.0f;
	in.principal_px_y = 208.0f;
	in.near_m = 100.0f / 255.0f;
	in.far_m = 1000.0f;
	in.inverse_depth_scale = 100.0f;

	struct leia_lift_depth_meta m;
	leia_lift_depth_meta_from_info(&in, leia_lift_nd_legacy_zero_h(0.0f), &m);
	// NeurD's shader already wrote metres: LINEAR, scale 1, no offset.
	CHECK(m.units == LEIA_LIFT_DEPTH_UNITS_METRIC);
	CHECK(m.encoding == LEIA_LIFT_DEPTH_ENCODING_LINEAR);
	CHECK(m.value_scale == 1.0f && m.value_offset == 0.0f);
	CHECK(NEAR(m.focal_x_px, in.focal_px_x) && NEAR(m.focal_y_px, in.focal_px_y));
	CHECK(NEAR(m.principal_x_px, 320.0f) && NEAR(m.principal_y_px, 208.0f));
	CHECK(NEAR(m.near_depth, in.near_m) && NEAR(m.far_depth, 1000.0f));
	CHECK(NEAR_REL(m.convergence_depth, 100.0f / 127.5f));

	// Metric units but no scale reported: fall back to relative semantics.
	in.inverse_depth_scale = 0.0f;
	leia_lift_depth_meta_from_info(&in, 0.5f, &m);
	CHECK(m.units == LEIA_LIFT_DEPTH_UNITS_RELATIVE && m.encoding == LEIA_LIFT_DEPTH_ENCODING_INVERSE);
}

static void
test_meta_relative(void)
{
	struct leia_lift_nd_depth_info in = {0};
	in.units = 0;
	in.width = 640;
	in.height = 416;
	in.source_width = 1920;
	in.source_height = 1080;
	// No focal reported -> NeurD's nominal pinhole rescaled per axis.
	struct leia_lift_depth_meta m;
	leia_lift_depth_meta_from_info(&in, 0.25f, &m);
	CHECK(m.units == LEIA_LIFT_DEPTH_UNITS_RELATIVE);
	CHECK(m.encoding == LEIA_LIFT_DEPTH_ENCODING_INVERSE);
	CHECK(NEAR(m.value_scale, 1.0f / 255.0f));
	CHECK(NEAR(m.focal_x_px, 0.78f * 1920.0f * 640.0f / 1920.0f));
	CHECK(NEAR(m.focal_y_px, 0.78f * 1920.0f * 416.0f / 1080.0f));
	CHECK(NEAR(m.principal_x_px, 320.0f) && NEAR(m.principal_y_px, 208.0f));
	CHECK(m.near_depth == 0.0f && m.far_depth == 0.0f);
	// Decodes consistently: a sample of 63.75 (h 0.25) is depth 4 = convergence.
	CHECK(NEAR(m.convergence_depth, 4.0f));
	const float sample = 63.75f;
	CHECK(NEAR(1.0f / (m.value_scale * sample + m.value_offset), m.convergence_depth));

	// Zero disparity at infinity -> unknown.
	leia_lift_depth_meta_from_info(&in, 0.0f, &m);
	CHECK(m.convergence_depth == 0.0f);
}

static struct leia_lift_oa_in
base_in(void)
{
	struct leia_lift_oa_in in = {0};
	in.rect_width_m = 0.32f;
	in.rect_height_m = 0.18f;
	in.nominal_z_m = 0.6f;
	in.viewpoint_source = LEIA_LIFT_VIEWPOINTS_TRACKED;
	in.view_gain = 1.0f;
	in.y_gain = 1.0f;
	in.strength = 1.0f;
	in.convergence = -1.0f;
	in.auto_zero_h = 0.0f;
	in.relief_m = 0.08f;
	in.metric_relief_max_m = 0.3f;
	return in;
}

static void
test_oa_eyes(void)
{
	struct leia_lift_oa_in in = base_in();
	struct leia_lift_oa_screen s;
	float e[3 * 4];
	uint32_t n = 0;
	float h0 = 0.0f;

	// Rect-relative eyes pass through: x, y, z (distance from the rect plane).
	const float eyes[6] = {-0.03f, 0.01f, 0.55f, 0.033f, 0.01f, 0.56f};
	CHECK(leia_lift_oa_build(&in, eyes, 2, &s, e, &n, &h0));
	CHECK(n == 2);
	CHECK(NEAR(e[0], -0.03f) && NEAR(e[1], 0.01f) && NEAR(e[2], 0.55f));
	CHECK(NEAR(e[3], 0.033f) && NEAR(e[4], 0.01f) && NEAR(e[5], 0.56f));
	CHECK(NEAR(s.screen_width_m, 0.32f) && NEAR(s.screen_height_m, 0.18f));
	CHECK(NEAR(s.nominal_eye_z_m, 0.6f));

	// Tracked: view_gain on x and y, y_gain on y only; z untouched.
	in.view_gain = 2.0f;
	in.y_gain = 0.5f;
	CHECK(leia_lift_oa_build(&in, eyes, 2, &s, e, &n, &h0));
	CHECK(NEAR(e[0], -0.06f) && NEAR(e[1], 0.01f) && NEAR(e[2], 0.55f));

	// A rig source is reproduced EXACTLY, gains ignored.
	in.viewpoint_source = LEIA_LIFT_VIEWPOINTS_CAMERA_RIG;
	CHECK(leia_lift_oa_build(&in, eyes, 2, &s, e, &n, &h0));
	CHECK(NEAR(e[0], -0.03f) && NEAR(e[1], 0.01f) && NEAR(e[2], 0.55f));
	in.viewpoint_source = LEIA_LIFT_VIEWPOINTS_DISPLAY_RIG;
	CHECK(leia_lift_oa_build(&in, eyes, 2, &s, e, &n, &h0));
	CHECK(NEAR(e[3], 0.033f));

	// Unknown z (0) -> the nominal distance.
	const float flat[3] = {0.01f, 0.0f, 0.0f};
	CHECK(leia_lift_oa_build(&in, flat, 1, &s, e, &n, &h0));
	CHECK(n == 1 && NEAR(e[2], 0.6f));

	// No eyes -> default pair at the nominal distance, the given baseline.
	in.baseline_m = 0.05f;
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	CHECK(n == 2 && NEAR(e[0], -0.025f) && NEAR(e[3], 0.025f) && NEAR(e[2], 0.6f) && NEAR(e[5], 0.6f));

	// No nominal distance at all -> 0.5 m.
	in.nominal_z_m = 0.0f;
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	CHECK(NEAR(s.nominal_eye_z_m, 0.5f) && NEAR(e[2], 0.5f));

	// Unusable rect -> refused (caller keeps the legacy path).
	in.rect_width_m = 0.0f;
	CHECK(!leia_lift_oa_build(&in, eyes, 2, &s, e, &n, &h0));
}

static void
test_oa_relative_depth(void)
{
	struct leia_lift_oa_in in = base_in();
	struct leia_lift_oa_screen s;
	float e[6];
	uint32_t n = 0;
	float h0 = 0.0f;

	// Auto with no module estimate -> mid-depth: C = D / 2.
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	CHECK(NEAR(s.depth_scale_m, 0.08f) && NEAR(s.convergence_depth_m, 0.04f) && NEAR(h0, 0.5f));

	// Auto with the module's estimate (h0 0.25 -> n0 0.75).
	in.auto_zero_h = 0.25f;
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	CHECK(NEAR(s.convergence_depth_m, 0.06f) && NEAR(h0, 0.25f));

	// Explicit convergence (runtime: 0 = nearest on the glass, 1 = farthest).
	in.convergence = 0.0f;
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	CHECK(NEAR(s.convergence_depth_m, 0.0f) && NEAR(h0, 1.0f));
	in.convergence = 1.0f;
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	CHECK(NEAR(s.convergence_depth_m, 0.08f) && NEAR(h0, 0.0f));

	// strength scales the relief; 0 -> NeurD's minimum (it rejects <= 0).
	in.convergence = 0.5f;
	in.strength = 2.0f;
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	CHECK(NEAR(s.depth_scale_m, 0.16f) && NEAR(s.convergence_depth_m, 0.08f));
	in.strength = 0.0f;
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	CHECK(s.depth_scale_m > 0.0f && NEAR(s.depth_scale_m, LEIA_LIFT_OA_MIN_RELIEF_M));

	// Pop-out limit: the nearest relief plane stays in front of the nearest eye.
	in.strength = 1.0f;
	in.relief_m = 2.0f;
	in.convergence = 1.0f; // C would be 2 m
	const float close[6] = {-0.03f, 0.0f, 0.4f, 0.03f, 0.0f, 0.45f};
	CHECK(leia_lift_oa_build(&in, close, 2, &s, e, &n, &h0));
	CHECK(NEAR(s.convergence_depth_m, 0.9f * 0.4f));
	CHECK(s.convergence_depth_m < e[2] && s.convergence_depth_m < s.nominal_eye_z_m);
	CHECK(NEAR(h0, 1.0f - (0.9f * 0.4f) / 2.0f));
}

static void
test_oa_metric_depth(void)
{
	struct leia_lift_oa_in in = base_in();
	struct leia_lift_oa_screen s;
	float e[6];
	uint32_t n = 0;
	float h0 = 0.0f;
	in.metric = true;
	in.inverse_depth_scale = 100.0f;
	in.metric_relief_max_m = 10.0f; // uncapped for the formula checks

	// Auto: the module put 2 m on the screen (h0 = 100 / 510).
	in.auto_zero_h = leia_lift_h_from_metric_depth(100.0f, 2.0f);
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	// D = 255 N d0 / S = N / h0 = 0.6 * 5.1 = 3.06 m; C = (1 - h0) D, then the
	// pop-out limit.
	CHECK(NEAR_REL(s.depth_scale_m, 0.6f * 255.0f * 2.0f / 100.0f));
	CHECK(NEAR(s.convergence_depth_m, 0.9f * 0.6f));
	// The zero-disparity h actually used maps back to a metric depth.
	CHECK(h0 > 0.0f && h0 < 1.0f);

	// The metric gradient at the screen: a scene point dd behind d0 lands
	// N dd / d0 behind the screen (scaled world seen from the nominal eye).
	{
		const float d0 = 2.0f, dd = 0.01f;
		const float h_a = leia_lift_h_from_metric_depth(100.0f, d0);
		const float h_b = leia_lift_h_from_metric_depth(100.0f, d0 + dd);
		const float dz = (h_a - h_b) * s.depth_scale_m; // z = C - n D, n = 1 - h
		CHECK(fabsf(dz - 0.6f * dd / d0) < 0.03f * (0.6f * dd / d0));
	}

	// Capped by MetricReliefMaxM.
	in.metric_relief_max_m = 0.3f;
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	CHECK(NEAR(s.depth_scale_m, 0.3f));
	CHECK(NEAR(s.convergence_depth_m, (1.0f - in.auto_zero_h) * 0.3f));
	CHECK(NEAR(h0, in.auto_zero_h));

	// Explicit far convergence (h0 -> 0): d0 -> infinity, relief = the cap.
	in.convergence = 1.0f;
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	CHECK(NEAR(s.depth_scale_m, 0.3f) && NEAR(s.convergence_depth_m, 0.3f));

	// Metric flagged but no scale -> relative relief.
	in.inverse_depth_scale = 0.0f;
	in.convergence = 0.5f;
	CHECK(leia_lift_oa_build(&in, NULL, 0, &s, e, &n, &h0));
	CHECK(NEAR(s.depth_scale_m, 0.08f));
}

int
main(void)
{
	test_zero_h_and_depth();
	test_meta_metric();
	test_meta_relative();
	test_oa_eyes();
	test_oa_relative_depth();
	test_oa_metric_depth();
	if (g_failures != 0) {
		fprintf(stderr, "test_lift_depth: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_lift_depth: OK\n");
	return 0;
}
