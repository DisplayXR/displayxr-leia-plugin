// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Lightweight SR hardware probe — detects SR display presence
 *         and caches display properties for the Leia builder and device.
 *
 * On Windows the probe is a thin veneer over the ONE process-wide geometry
 * resolver in leia_sr_ready.cpp: it waits (within the shared startup budget)
 * for the SR platform to identify the panel, queries a FRESH display handle,
 * verifies it and caches the result. A probe that fails because the panel is
 * not identified yet is NOT latched — the next call retries, cheaply, and the
 * late-identification watcher publishes into the same cache.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_interface.h"
#include "util/u_logging.h"

#ifdef XRT_HAVE_LEIA_SR_D3D11

#include "leia_sr_ready.h"

bool
leiasr_probe_display(double timeout_seconds)
{
	// Blocks at most for what is left of the shared startup budget (plus a
	// bounded verification spin), returns fast once that budget is spent.
	return leiasr_geometry_resolve(timeout_seconds, "SR runtime probe");
}

bool
leiasr_get_probe_results(struct leiasr_probe_result *out)
{
	if (out == nullptr) {
		return false;
	}
	struct leiasr_geometry g = {};
	if (!leiasr_geometry_get(&g)) {
		*out = {};
		return false;
	}
	*out = {};
	out->hw_found = true;
	out->pixel_w = g.pixel_w;
	out->pixel_h = g.pixel_h;
	out->refresh_hz = g.refresh_hz;
	out->display_w_m = g.width_m;
	out->display_h_m = g.height_m;
	out->nominal_z_m = g.nominal_z_m;
	return true;
}

#else // !XRT_HAVE_LEIA_SR_D3D11

// Stub implementations for non-SR builds.

bool
leiasr_probe_display(double timeout_seconds)
{
	(void)timeout_seconds;
	return false;
}

bool
leiasr_get_probe_results(struct leiasr_probe_result *out)
{
	(void)out;
	return false;
}

#endif // XRT_HAVE_LEIA_SR_D3D11
