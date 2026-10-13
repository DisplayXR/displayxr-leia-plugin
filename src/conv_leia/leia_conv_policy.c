// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Pure decisions of the Leia conversion provider (see leia_conv_policy.h).
 * @ingroup conv_leia
 */

#include "leia_conv_policy.h"

#include <math.h>
#include <stddef.h>

void
leia_conv_readiness_of(const struct leia_conv_module_view *in, struct leia_conv_readiness *out)
{
	struct leia_conv_readiness r = {LEIA_CONV_STATE_UNKNOWN, LEIA_CONV_FAIL_NONE, 0u};
	if (in == NULL || out == NULL) {
		if (out != NULL) {
			*out = r;
		}
		return;
	}
	if (!in->enabled || in->module_status == LEIA_CONV_MODULE_DISABLED) {
		r.state = LEIA_CONV_STATE_DISABLED;
	} else if (in->device_lost) {
		// The provider's own device: the runtime re-warms on its backoff.
		r.state = LEIA_CONV_STATE_FAILED;
		r.failure_reason = LEIA_CONV_FAIL_DEVICE_LOST;
		r.retryable = 1u;
	} else if (in->warming) {
		r.state = LEIA_CONV_STATE_WARMING;
	} else {
		switch (in->module_status) {
		case LEIA_CONV_MODULE_READY: r.state = LEIA_CONV_STATE_READY; break;
		case LEIA_CONV_MODULE_ACTIVATING:
			if (in->network_retry) {
				// The licence activation needs the network: FAILED(NETWORK),
				// retryable — the runtime's backoff owns the retry.
				r.state = LEIA_CONV_STATE_FAILED;
				r.failure_reason = LEIA_CONV_FAIL_NETWORK;
				r.retryable = 1u;
			} else {
				r.state = LEIA_CONV_STATE_WARMING;
			}
			break;
		case LEIA_CONV_MODULE_ABSENT:
			r.state = LEIA_CONV_STATE_ABSENT;
			r.failure_reason = LEIA_CONV_FAIL_MODEL;
			r.retryable = 1u; // installed later: the next warm finds it
			break;
		case LEIA_CONV_MODULE_TOO_OLD:
			// The version gate: present but older than the floor. FAILED(VERSION),
			// never FAILED(MODEL) — "too old", not "missing". Retryable: an update
			// replaces the file and the next warm re-checks it.
			r.state = LEIA_CONV_STATE_FAILED;
			r.failure_reason = LEIA_CONV_FAIL_VERSION;
			r.retryable = 1u;
			break;
		case LEIA_CONV_MODULE_UNLICENSED:
			r.state = LEIA_CONV_STATE_UNLICENSED;
			r.failure_reason = LEIA_CONV_FAIL_LICENCE;
			break;
		case LEIA_CONV_MODULE_LOAD_FAILED:
			r.state = LEIA_CONV_STATE_FAILED;
			r.failure_reason = LEIA_CONV_FAIL_LOAD;
			r.retryable = in->recheckable ? 1u : 0u;
			break;
		default: // never activated: presence alone
			if (!in->found) {
				r.state = LEIA_CONV_STATE_ABSENT;
				r.failure_reason = LEIA_CONV_FAIL_MODEL;
				r.retryable = 1u;
			} else if (in->too_old) {
				r.state = LEIA_CONV_STATE_FAILED;
				r.failure_reason = LEIA_CONV_FAIL_VERSION;
				r.retryable = 1u;
			} else {
				r.state = LEIA_CONV_STATE_INSTALLED; // present and cold; warm() makes it READY
			}
			break;
		}
	}
	*out = r;
}

static char
lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

//! Case-insensitive substring test (ASCII).
static bool
contains_ci(const char *hay, const char *needle)
{
	if (hay == NULL || needle == NULL || needle[0] == '\0') {
		return false;
	}
	for (const char *h = hay; *h != '\0'; h++) {
		const char *a = h;
		const char *b = needle;
		while (*a != '\0' && *b != '\0' && lower(*a) == lower(*b)) {
			a++;
			b++;
		}
		if (*b == '\0') {
			return true;
		}
	}
	return false;
}

uint32_t
leia_conv_licence_class(const char *licence_line, const char *product_name)
{
	const char *text = (licence_line != NULL && licence_line[0] != '\0') ? licence_line : product_name;
	if (text == NULL || text[0] == '\0') {
		return LEIA_CONV_LICENCE_UNKNOWN;
	}
	static const char *const k_restricted[] = {"internal",     "non-commercial", "noncommercial", "non commercial",
	                                           "research",     "evaluation",     "trial",         "not for resale",
	                                           "personal use"};
	for (size_t i = 0; i < sizeof(k_restricted) / sizeof(k_restricted[0]); i++) {
		if (contains_ci(text, k_restricted[i])) {
			return LEIA_CONV_LICENCE_RESEARCH_ONLY;
		}
	}
	if (contains_ci(text, "commercial")) {
		return LEIA_CONV_LICENCE_COMMERCIAL;
	}
	return LEIA_CONV_LICENCE_UNKNOWN;
}

void
leia_conv_views_extent(
    uint32_t in_w, uint32_t in_h, uint32_t views, bool array_layout, uint32_t max_dim, uint32_t *out_w, uint32_t *out_h)
{
	if (out_w == NULL || out_h == NULL) {
		return;
	}
	*out_w = *out_h = 0;
	if (in_w == 0 || in_h == 0 || views == 0 || max_dim == 0) {
		return;
	}
	const uint32_t across = array_layout ? 1u : views;
	// Largest per-view scale that keeps across * w and h within max_dim.
	double s = 1.0;
	const double sw = (double)max_dim / ((double)across * (double)in_w);
	const double sh = (double)max_dim / (double)in_h;
	if (sw < s) {
		s = sw;
	}
	if (sh < s) {
		s = sh;
	}
	uint32_t vw = (uint32_t)floor((double)in_w * s);
	uint32_t vh = (uint32_t)floor((double)in_h * s);
	vw = vw == 0 ? 1u : vw;
	vh = vh == 0 ? 1u : vh;
	*out_w = vw * across;
	*out_h = vh;
}

void
leia_conv_depth_extent(uint32_t in_w, uint32_t in_h, uint32_t max_edge, uint32_t *out_w, uint32_t *out_h)
{
	if (out_w == NULL || out_h == NULL) {
		return;
	}
	*out_w = *out_h = 0;
	if (in_w == 0 || in_h == 0) {
		return;
	}
	const uint32_t edge = in_w > in_h ? in_w : in_h;
	if (max_edge == 0 || edge <= max_edge) {
		*out_w = in_w;
		*out_h = in_h;
		return;
	}
	const double s = (double)max_edge / (double)edge;
	uint32_t w = (uint32_t)floor((double)in_w * s + 0.5);
	uint32_t h = (uint32_t)floor((double)in_h * s + 0.5);
	*out_w = w == 0 ? 1u : (w > max_edge ? max_edge : w);
	*out_h = h == 0 ? 1u : (h > max_edge ? max_edge : h);
}

void
leia_conv_convergence(uint32_t mode, float value, uint32_t depth_kind, float *out_rel, float *out_metric_m)
{
	float rel = -1.0f;
	float metric = 0.0f;
	if (mode == LEIA_CONV_CONVERGENCE_DECODED && isfinite(value)) {
		if (depth_kind == LEIA_CONV_DEPTH_AFFINE_INVERSE) {
			// Decoded = the module's normalised disparity h (larger = nearer);
			// its "relative depth at the glass" is the normalised depth 1 - h.
			rel = 1.0f - value;
			rel = rel < 0.0f ? 0.0f : (rel > 1.0f ? 1.0f : rel);
		} else if (depth_kind == LEIA_CONV_DEPTH_METRIC && value > 0.0f) {
			metric = value; // metres; the module converts with its own scale
		}
		// Any other kind: no mapping the module could honour — AUTO.
	}
	if (out_rel != NULL) {
		*out_rel = rel;
	}
	if (out_metric_m != NULL) {
		*out_metric_m = metric;
	}
}

uint32_t
leia_conv_depth_kind(uint32_t units, uint32_t encoding)
{
	const bool metric = units == 1u;
	const bool inverse = encoding == 1u;
	if (metric) {
		return inverse ? LEIA_CONV_DEPTH_METRIC_INVERSE : LEIA_CONV_DEPTH_METRIC;
	}
	return inverse ? LEIA_CONV_DEPTH_AFFINE_INVERSE : LEIA_CONV_DEPTH_AFFINE;
}
