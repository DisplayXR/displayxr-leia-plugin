// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Host test of the Leia conversion provider's pure decisions
 *         (leia_conv_policy.c): the readiness gate (too old -> FAILED(VERSION),
 *         missing -> ABSENT(MODEL)), the licence class from the module's own
 *         words, output extents and the decoded-convergence mapping. Plain C,
 *         no runtime header: runs on every CI lane.
 */

#include "leia_conv_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		g_checks++;                                                                                            \
		if (!(cond)) {                                                                                         \
			g_fail++;                                                                                      \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
		}                                                                                                      \
	} while (0)

static struct leia_conv_readiness
rd(struct leia_conv_module_view v)
{
	struct leia_conv_readiness r;
	memset(&r, 0xA5, sizeof(r));
	leia_conv_readiness_of(&v, &r);
	return r;
}

static void
test_readiness_gate(void)
{
	struct leia_conv_module_view v;
	struct leia_conv_readiness r;

	// Never activated, nothing found: ABSENT(MODEL), retryable (installed later).
	memset(&v, 0, sizeof(v));
	v.enabled = true;
	r = rd(v);
	CHECK(r.state == LEIA_CONV_STATE_ABSENT);
	CHECK(r.failure_reason == LEIA_CONV_FAIL_MODEL);
	CHECK(r.retryable == 1u);

	// Found, version resource older than the floor: FAILED(VERSION) — never MODEL, never ABSENT.
	v.found = true;
	v.too_old = true;
	r = rd(v);
	CHECK(r.state == LEIA_CONV_STATE_FAILED);
	CHECK(r.failure_reason == LEIA_CONV_FAIL_VERSION);
	CHECK(r.failure_reason != LEIA_CONV_FAIL_MODEL);
	CHECK(r.retryable == 1u);

	// Found and new enough, cold: INSTALLED (warm makes it READY).
	v.too_old = false;
	r = rd(v);
	CHECK(r.state == LEIA_CONV_STATE_INSTALLED);
	CHECK(r.failure_reason == LEIA_CONV_FAIL_NONE);

	// A warm in progress: WARMING, whatever presence said.
	v.warming = true;
	r = rd(v);
	CHECK(r.state == LEIA_CONV_STATE_WARMING);
	v.warming = false;

	// After activation the module's status is authoritative.
	v.module_status = LEIA_CONV_MODULE_READY;
	CHECK(rd(v).state == LEIA_CONV_STATE_READY);

	v.module_status = LEIA_CONV_MODULE_TOO_OLD; // refused after the load (no version resource)
	r = rd(v);
	CHECK(r.state == LEIA_CONV_STATE_FAILED && r.failure_reason == LEIA_CONV_FAIL_VERSION && r.retryable == 1u);

	v.module_status = LEIA_CONV_MODULE_ABSENT;
	r = rd(v);
	CHECK(r.state == LEIA_CONV_STATE_ABSENT && r.failure_reason == LEIA_CONV_FAIL_MODEL && r.retryable == 1u);

	v.module_status = LEIA_CONV_MODULE_UNLICENSED;
	r = rd(v);
	CHECK(r.state == LEIA_CONV_STATE_UNLICENSED && r.failure_reason == LEIA_CONV_FAIL_LICENCE);

	v.module_status = LEIA_CONV_MODULE_LOAD_FAILED;
	v.recheckable = true;
	r = rd(v);
	CHECK(r.state == LEIA_CONV_STATE_FAILED && r.failure_reason == LEIA_CONV_FAIL_LOAD && r.retryable == 1u);
	v.recheckable = false;
	CHECK(rd(v).retryable == 0u);

	// Licence activation needs the network: FAILED(NETWORK), retryable.
	v.module_status = LEIA_CONV_MODULE_ACTIVATING;
	v.network_retry = true;
	r = rd(v);
	CHECK(r.state == LEIA_CONV_STATE_FAILED && r.failure_reason == LEIA_CONV_FAIL_NETWORK && r.retryable == 1u);
	v.network_retry = false;
	CHECK(rd(v).state == LEIA_CONV_STATE_WARMING);

	// Device loss outranks a READY module.
	v.module_status = LEIA_CONV_MODULE_READY;
	v.device_lost = true;
	r = rd(v);
	CHECK(r.state == LEIA_CONV_STATE_FAILED && r.failure_reason == LEIA_CONV_FAIL_DEVICE_LOST && r.retryable == 1u);
	v.device_lost = false;

	// Disabled by configuration, either way it is said.
	v.enabled = false;
	CHECK(rd(v).state == LEIA_CONV_STATE_DISABLED);
	v.enabled = true;
	v.module_status = LEIA_CONV_MODULE_DISABLED;
	CHECK(rd(v).state == LEIA_CONV_STATE_DISABLED);

	// NULL-safe.
	leia_conv_readiness_of(NULL, NULL);
}

static void
test_licence(void)
{
	CHECK(leia_conv_licence_class(NULL, NULL) == LEIA_CONV_LICENCE_UNKNOWN);
	CHECK(leia_conv_licence_class("", "") == LEIA_CONV_LICENCE_UNKNOWN);
	CHECK(leia_conv_licence_class("", "Some Module") == LEIA_CONV_LICENCE_UNKNOWN);
	CHECK(leia_conv_licence_class("License: internal use only", NULL) == LEIA_CONV_LICENCE_RESEARCH_ONLY);
	CHECK(leia_conv_licence_class("", "Module-INTERNAL") == LEIA_CONV_LICENCE_RESEARCH_ONLY);
	CHECK(leia_conv_licence_class("License: non-commercial", "") == LEIA_CONV_LICENCE_RESEARCH_ONLY);
	CHECK(leia_conv_licence_class("License: research", "") == LEIA_CONV_LICENCE_RESEARCH_ONLY);
	CHECK(leia_conv_licence_class("License: evaluation copy", "") == LEIA_CONV_LICENCE_RESEARCH_ONLY);
	CHECK(leia_conv_licence_class("License: commercial", "") == LEIA_CONV_LICENCE_COMMERCIAL);
	CHECK(leia_conv_licence_class("", "Module (Commercial)") == LEIA_CONV_LICENCE_COMMERCIAL);
	// The licence line, once printed, wins over the file's product name.
	CHECK(leia_conv_licence_class("License: commercial", "Module-internal") == LEIA_CONV_LICENCE_COMMERCIAL);
	CHECK(leia_conv_licence_class("License: internal use only", "Module (commercial)") ==
	      LEIA_CONV_LICENCE_RESEARCH_ONLY);
}

static void
test_extents(void)
{
	uint32_t w = 0, h = 0;
	leia_conv_views_extent(1920, 1080, 2, false, 16384, &w, &h);
	CHECK(w == 3840 && h == 1080);
	leia_conv_views_extent(1920, 1080, 2, true, 16384, &w, &h);
	CHECK(w == 1920 && h == 1080);
	// 2 x 10000 does not fit 16384 in a row: scaled, aspect kept.
	leia_conv_views_extent(10000, 5000, 2, false, 16384, &w, &h);
	CHECK(w <= 16384 && w == 2 * 8192 && h == 4096);
	leia_conv_views_extent(0, 1080, 2, false, 16384, &w, &h);
	CHECK(w == 0 && h == 0);

	leia_conv_depth_extent(1920, 1080, 1280, &w, &h);
	CHECK(w == 1280 && h == 720);
	leia_conv_depth_extent(640, 480, 1280, &w, &h); // never upscaled
	CHECK(w == 640 && h == 480);
	leia_conv_depth_extent(1080, 1920, 1024, &w, &h); // portrait: long edge capped
	CHECK(h == 1024 && w == 576);
	leia_conv_depth_extent(100000, 1, 1024, &w, &h); // never 0
	CHECK(w == 1024 && h == 1);
}

static void
test_convergence(void)
{
	float rel = 0.0f, metric = 0.0f;
	leia_conv_convergence(LEIA_CONV_CONVERGENCE_AUTO, 0.3f, LEIA_CONV_DEPTH_AFFINE_INVERSE, &rel, &metric);
	CHECK(rel < 0.0f && metric == 0.0f);

	// Relative aux depth decodes to the disparity h: the glass at h0 = 0.75 is
	// normalised depth 0.25.
	leia_conv_convergence(LEIA_CONV_CONVERGENCE_DECODED, 0.75f, LEIA_CONV_DEPTH_AFFINE_INVERSE, &rel, &metric);
	CHECK(fabsf(rel - 0.25f) < 1e-6f && metric == 0.0f);
	leia_conv_convergence(LEIA_CONV_CONVERGENCE_DECODED, 1.5f, LEIA_CONV_DEPTH_AFFINE_INVERSE, &rel, &metric);
	CHECK(rel == 0.0f); // clamped

	// Metric aux depth: metres, converted by the module with its own scale.
	leia_conv_convergence(LEIA_CONV_CONVERGENCE_DECODED, 2.5f, LEIA_CONV_DEPTH_METRIC, &rel, &metric);
	CHECK(rel < 0.0f && metric == 2.5f);
	leia_conv_convergence(LEIA_CONV_CONVERGENCE_DECODED, -1.0f, LEIA_CONV_DEPTH_METRIC, &rel, &metric);
	CHECK(rel < 0.0f && metric == 0.0f);

	// No depth returned yet (kind unknown), or a NaN plane: AUTO.
	leia_conv_convergence(LEIA_CONV_CONVERGENCE_DECODED, 0.5f, LEIA_CONV_DEPTH_UNKNOWN, &rel, &metric);
	CHECK(rel < 0.0f && metric == 0.0f);
	leia_conv_convergence(LEIA_CONV_CONVERGENCE_DECODED, NAN, LEIA_CONV_DEPTH_AFFINE_INVERSE, &rel, &metric);
	CHECK(rel < 0.0f && metric == 0.0f);
}

static void
test_depth_kind(void)
{
	CHECK(leia_conv_depth_kind(0u, 1u) == LEIA_CONV_DEPTH_AFFINE_INVERSE); // relative model: raw disparity
	CHECK(leia_conv_depth_kind(1u, 0u) == LEIA_CONV_DEPTH_METRIC);         // metric model: metres
	CHECK(leia_conv_depth_kind(1u, 1u) == LEIA_CONV_DEPTH_METRIC_INVERSE);
	CHECK(leia_conv_depth_kind(0u, 0u) == LEIA_CONV_DEPTH_AFFINE);
}

int
main(void)
{
	test_readiness_gate();
	test_licence();
	test_extents();
	test_convergence();
	test_depth_kind();
	printf("test_conv_leia_policy: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
