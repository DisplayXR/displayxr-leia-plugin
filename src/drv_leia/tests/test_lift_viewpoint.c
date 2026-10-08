// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the lift viewpoint mapping (leia_lift_viewpoint.h):
 *         metres -> NeurD units under the legacy contract and the runtime's
 *         viewpoint policy (runtime ADR-048).
 *
 * Hardware-free and host-runnable (the Linux CI job builds it for the Windows
 * arm): pure arithmetic, no NeurD / SR / D3D.
 */

#include "leia_lift_viewpoint.h"

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

static void
map(const struct leia_lift_vp_policy *p, const struct leia_lift_vp_gains *g, float x, float y, float z, float out[3])
{
	const float in[3] = {x, y, z};
	leia_lift_vp_map(p, g, in, out);
}

static void
test_legacy(void)
{
	const struct leia_lift_vp_gains g1 = {1.0f, 1.0f, 0.5f};
	float o[3];

	// NULL policy = legacy: a centred 63 mm pair is NeurD's default ±0.5.
	map(NULL, &g1, -0.0315f, 0.10f, 0.60f, o);
	CHECK(NEAR(o[0], -0.5f) && o[1] == 0.0f && o[2] == 0.0f);
	map(NULL, &g1, 0.0315f, -0.2f, 0.4f, o);
	CHECK(NEAR(o[0], 0.5f) && o[1] == 0.0f && o[2] == 0.0f);

	// Inactive policy struct = legacy too, whatever its fields say.
	struct leia_lift_vp_policy off = {false, 0.03f, LEIA_LIFT_AXIS_XYZ, 0.01f, 0.6f};
	map(&off, &g1, 0.063f, 0.063f, 0.9f, o);
	CHECK(NEAR(o[0], 1.0f) && o[1] == 0.0f && o[2] == 0.0f);

	// ±3 clamp; view_gain scales.
	map(NULL, &g1, 1.0f, 0, 0, o);
	CHECK(NEAR(o[0], 3.0f));
	map(NULL, &g1, -1.0f, 0, 0, o);
	CHECK(NEAR(o[0], -3.0f));
	const struct leia_lift_vp_gains g2 = {2.0f, 1.0f, 0.5f};
	map(NULL, &g2, 0.063f, 0, 0, o);
	CHECK(NEAR(o[0], 2.0f));
	const struct leia_lift_vp_gains g0 = {0.0f, 1.0f, 0.5f};
	map(NULL, &g0, 0.2f, 0, 0, o);
	CHECK(o[0] == 0.0f);

	// NaN never reaches NeurD.
	map(NULL, &g1, NAN, 0, 0, o);
	CHECK(o[0] == 0.0f);
}

static void
test_policy_unit_and_axes(void)
{
	const struct leia_lift_vp_gains g1 = {1.0f, 1.0f, 0.5f};
	float o[3];

	// The unit is the NOMINAL 63 mm: a 63 mm pair is NeurD's default ±0.5.
	struct leia_lift_vp_policy p = {true, 0.063f, LEIA_LIFT_AXIS_X, 0.0f, 0.6f};
	map(&p, &g1, -0.0315f, 0.05f, 0.7f, o);
	CHECK(NEAR(o[0], -0.5f) && o[1] == 0.0f && o[2] == 0.0f); // X: y/z dropped
	map(&p, &g1, 0.0315f + 0.063f, 0, 0.6f, o);               // midpoint 63 mm right
	CHECK(NEAR(o[0], 1.5f));

	// ipdFactor shows through: the runtime scaled the pair to 32 mm, so each
	// eye maps to ±0.016/0.063 = ±0.254·ViewGain (softer stereo), NOT ±0.5.
	p.baseline_m = 0.032f;
	map(&p, &g1, -0.016f, 0, 0.6f, o);
	CHECK(fabsf(o[0] + 0.254f) < 1e-3f);
	map(&p, &g1, 0.016f, 0, 0.6f, o);
	CHECK(fabsf(o[0] - 0.254f) < 1e-3f);
	const struct leia_lift_vp_gains g2x = {2.0f, 1.0f, 0.5f};
	map(&p, &g2x, 0.016f, 0, 0.6f, o);
	CHECK(fabsf(o[0] - 0.508f) < 1e-3f);

	// Unknown baseline (0) changes nothing about the unit.
	p.baseline_m = 0.0f;
	map(&p, &g1, 0.0315f, 0, 0.6f, o);
	CHECK(NEAR(o[0], 0.5f));

	// XY: y honoured with y_gain·view_gain / 0.063.
	p.baseline_m = 0.063f;
	p.axis_mode = LEIA_LIFT_AXIS_XY;
	map(&p, &g1, 0.0f, 0.0315f, 0.9f, o);
	CHECK(NEAR(o[0], 0.0f) && NEAR(o[1], 0.5f) && o[2] == 0.0f); // z still pinned
	const struct leia_lift_vp_gains gy = {2.0f, 0.25f, 0.5f};
	map(&p, &gy, 0.0f, 0.063f, 0.6f, o);
	CHECK(NEAR(o[1], 0.5f)); // 0.25 * 2 * 1

	// XYZ: z relative to the reference distance, fractional, z_gain, clamped ±1.
	p.axis_mode = LEIA_LIFT_AXIS_XYZ;
	p.ref_z_m = 0.6f;
	map(&p, &g1, 0, 0, 0.6f, o);
	CHECK(o[2] == 0.0f); // at the reference distance -> 0
	map(&p, &g1, 0, 0, 0.3f, o);
	CHECK(NEAR(o[2], -0.25f)); // half the distance: 0.5 * (-0.5)
	map(&p, &g1, 0, 0, 0.9f, o);
	CHECK(NEAR(o[2], 0.25f));
	const struct leia_lift_vp_gains gz = {1.0f, 1.0f, -2.0f};
	map(&p, &gz, 0, 0, 3.0f, o);
	CHECK(NEAR(o[2], -1.0f)); // negative gain flips, clamp holds
	p.ref_z_m = 0.0f;         // unknown reference -> 0.5 m
	map(&p, &g1, 0, 0, 0.75f, o);
	CHECK(NEAR(o[2], 0.25f));

	// Unknown axis mode reads as X.
	p.axis_mode = 0;
	map(&p, &g1, 0, 0.1f, 0.9f, o);
	CHECK(o[1] == 0.0f && o[2] == 0.0f);
}

static void
test_policy_clamp(void)
{
	const struct leia_lift_vp_gains g1 = {1.0f, 1.0f, 0.5f};
	float o[3];

	// Runtime clamp 63 mm, 63 mm pair: L = (0.063 + 0.0315)/0.063 = 1.5 — the
	// midpoint may sit 1 unit off and each eye still keeps its half-pair.
	struct leia_lift_vp_policy p = {true, 0.063f, LEIA_LIFT_AXIS_XY, 0.063f, 0.6f};
	map(&p, &g1, 0.063f + 0.0315f, 0, 0.6f, o); // right eye of a midpoint at the clamp
	CHECK(NEAR(o[0], 1.5f));
	map(&p, &g1, 0.063f - 0.0315f, 0, 0.6f, o); // left eye
	CHECK(NEAR(o[0], 0.5f));
	map(&p, &g1, 0.5f, 0.5f, 0.6f, o); // a glitch far past the runtime clamp
	CHECK(NEAR(o[0], 1.5f) && NEAR(o[1], 1.5f));
	map(&p, &g1, -0.5f, -0.5f, 0.6f, o);
	CHECK(NEAR(o[0], -1.5f) && NEAR(o[1], -1.5f));

	// A 32 mm pair at the same clamp: the margin is that pair's half, 16 mm.
	p.baseline_m = 0.032f;
	map(&p, &g1, 0.063f + 0.016f, 0, 0.6f, o); // right eye at the clamp: unclipped
	CHECK(fabsf(o[0] - 0.079f / 0.063f) < 1e-3f);
	map(&p, &g1, 0.5f, 0, 0.6f, o);
	CHECK(fabsf(o[0] - 0.079f / 0.063f) < 1e-3f);
	p.baseline_m = 0.063f;

	// The clamp scales with view_gain (and y_gain for y).
	const struct leia_lift_vp_gains g2 = {2.0f, 0.5f, 0.5f};
	map(&p, &g2, 0.5f, 0.5f, 0.6f, o);
	CHECK(NEAR(o[0], 3.0f) && NEAR(o[1], 1.5f));

	// A runtime clamp wider than ±3 is honoured (the runtime owns the clamp).
	p.max_offset_m = 0.252f; // L = (0.252 + 0.0315)/0.063 = 4.5
	map(&p, &g1, 0.5f, 0, 0.6f, o);
	CHECK(NEAR(o[0], 4.5f));

	// Unclamped runtime (0) -> the legacy ±3 guard.
	p.max_offset_m = 0.0f;
	map(&p, &g1, 0.5f, -0.5f, 0.6f, o);
	CHECK(NEAR(o[0], 3.0f) && NEAR(o[1], -3.0f));
}

int
main(void)
{
	test_legacy();
	test_policy_unit_and_axes();
	test_policy_clamp();
	if (g_failures != 0) {
		fprintf(stderr, "test_lift_viewpoint: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_lift_viewpoint: OK\n");
	return 0;
}
