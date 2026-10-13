// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Pure decisions of the Leia conversion provider (no Windows, no D3D,
 *         no runtime header): the module status -> capability readiness gate
 *         (FAILED(VERSION) for a module older than the floor, ABSENT(MODEL)
 *         when there is none), the licence class from the module's own
 *         strings, output extents and the decoded-convergence mapping.
 *
 * Host-tested by tests/test_conv_leia_policy.c on every CI lane. The values
 * below mirror the runtime's xrt_conversion_plugin.h (ABI 1); the provider
 * static_asserts every pair where it includes that header.
 *
 * @ingroup conv_leia
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! @name Capabilities this provider declares (cap_id, stable). @{ */
#define LEIA_CONV_CAP_DEPTH_MONO 1u
#define LEIA_CONV_CAP_DEPTH_VIDEO 2u
#define LEIA_CONV_CAP_VIEWS_MONO 3u
#define LEIA_CONV_CAP_COUNT 3u
/*! @} */

/*! @name Mirrors of XRT_CONV_STATE_* @{ */
#define LEIA_CONV_STATE_UNKNOWN 0u
#define LEIA_CONV_STATE_READY 1u
#define LEIA_CONV_STATE_WARMING 2u
#define LEIA_CONV_STATE_ABSENT 3u
#define LEIA_CONV_STATE_INCOMPATIBLE 4u
#define LEIA_CONV_STATE_UNLICENSED 5u
#define LEIA_CONV_STATE_FAILED 6u
#define LEIA_CONV_STATE_DISABLED 7u
#define LEIA_CONV_STATE_INSTALLED 8u
/*! @} */

/*! @name Mirrors of XRT_CONV_FAIL_* @{ */
#define LEIA_CONV_FAIL_NONE 0u
#define LEIA_CONV_FAIL_LICENCE 1u
#define LEIA_CONV_FAIL_LOAD 2u
#define LEIA_CONV_FAIL_NETWORK 4u
#define LEIA_CONV_FAIL_VERSION 5u
#define LEIA_CONV_FAIL_MODEL 7u
#define LEIA_CONV_FAIL_DEVICE_LOST 8u
/*! @} */

/*! @name Mirrors of XRT_CONV_LICENCE_* @{ */
#define LEIA_CONV_LICENCE_UNKNOWN 0u
#define LEIA_CONV_LICENCE_COMMERCIAL 2u
#define LEIA_CONV_LICENCE_RESEARCH_ONLY 3u
/*! @} */

/*! @name Mirrors of XRT_CONV_DEPTH_* and XRT_CONV_CONVERGENCE_* @{ */
#define LEIA_CONV_DEPTH_UNKNOWN 0u
#define LEIA_CONV_DEPTH_METRIC 1u
#define LEIA_CONV_DEPTH_METRIC_INVERSE 2u
#define LEIA_CONV_DEPTH_AFFINE_INVERSE 3u
#define LEIA_CONV_DEPTH_AFFINE 5u
#define LEIA_CONV_CONVERGENCE_AUTO 0u
#define LEIA_CONV_CONVERGENCE_DECODED 1u
/*! @} */

/*! @name Mirrors of the module's LEIA_LIFT_MODULE_* (leia_lift_neurd.h) @{ */
#define LEIA_CONV_MODULE_UNKNOWN 0u //!< not activated yet: presence decides
#define LEIA_CONV_MODULE_READY 1u
#define LEIA_CONV_MODULE_ACTIVATING 2u
#define LEIA_CONV_MODULE_ABSENT 3u
#define LEIA_CONV_MODULE_TOO_OLD 4u
#define LEIA_CONV_MODULE_UNLICENSED 5u
#define LEIA_CONV_MODULE_LOAD_FAILED 6u
#define LEIA_CONV_MODULE_DISABLED 7u
/*! @} */

//! Everything the readiness of a capability depends on, as last observed.
struct leia_conv_module_view
{
	bool enabled; //!< setting Enable != 0
	//! Presence (consulted only while the module was never activated):
	bool found;    //!< a vendor module file was found
	bool too_old;  //!< ...and its version resource is older than the floor
	uint32_t module_status; //!< LEIA_CONV_MODULE_*; UNKNOWN = never activated
	bool network_retry;     //!< ACTIVATING because the licence needs the network
	bool recheckable;       //!< LOAD_FAILED for a file-rooted reason
	bool warming;           //!< a warm() is running right now
	bool device_lost;       //!< the provider's own D3D11 device was removed
};

struct leia_conv_readiness
{
	uint32_t state;          //!< LEIA_CONV_STATE_*
	uint32_t failure_reason; //!< LEIA_CONV_FAIL_*
	uint32_t retryable;      //!< 1: a later warm() may succeed
};

/*!
 * The readiness of every capability (they share one module). The version gate
 * reports FAILED(VERSION) — never FAILED(MODEL) and never ABSENT — so the
 * dashboard says "too old", not "missing"; no module is ABSENT(MODEL).
 */
void
leia_conv_readiness_of(const struct leia_conv_module_view *in, struct leia_conv_readiness *out);

/*!
 * Licence class (LEIA_CONV_LICENCE_*) from the module's OWN words: its licence
 * line once it printed one, else its file's product name. A module that calls
 * itself internal / non-commercial / research / evaluation is RESEARCH_ONLY
 * (selected only with the runtime's AllowResearchOnly opt-in); one that says
 * commercial (and none of those) is COMMERCIAL; anything else is UNKNOWN,
 * which the runtime also gates. Never invented: no words, UNKNOWN.
 */
uint32_t
leia_conv_licence_class(const char *licence_line, const char *product_name);

/*!
 * Views target extent for an input of in_w x in_h: per view the input size,
 * scaled down (aspect kept) so the whole target fits @p max_dim texels per
 * edge — N views side by side for ROW, one view per layer for ARRAY.
 */
void
leia_conv_views_extent(
    uint32_t in_w, uint32_t in_h, uint32_t views, bool array_layout, uint32_t max_dim, uint32_t *out_w, uint32_t *out_h);

//! Depth target extent: the input size with its long edge capped at @p max_edge (never upscaled, never 0).
void
leia_conv_depth_extent(uint32_t in_w, uint32_t in_h, uint32_t max_edge, uint32_t *out_w, uint32_t *out_h);

/*!
 * The runtime's convergence (params.convergence_mode / convergence, in the
 * DECODED units of the depth this stream last returned, of kind
 * @p depth_kind) -> the module's terms: *out_rel in [0,1] (relative depth at
 * the glass, 0 nearest) or -1 for the module's own auto-convergence, and
 * *out_metric_m > 0 when the plane is a metric depth the module converts
 * with its own scale.
 */
void
leia_conv_convergence(uint32_t mode, float value, uint32_t depth_kind, float *out_rel, float *out_metric_m);

/*!
 * Depth kind (LEIA_CONV_DEPTH_*) of the module's float depth, from its units
 * (0 relative, 1 metric) and encoding (0 linear, 1 inverse).
 */
uint32_t
leia_conv_depth_kind(uint32_t units, uint32_t encoding);

#ifdef __cplusplus
}
#endif
