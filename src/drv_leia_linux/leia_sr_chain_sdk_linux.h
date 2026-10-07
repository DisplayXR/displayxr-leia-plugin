// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The SR-typed half of leia_sr_routing_linux.h: build the weaver
 *         create-info pNext chain from a plan, and the capability query
 *         chain (multi-screen M4).
 *
 * Header-only and call-free (SR types and the SDK's compound-literal macros,
 * no SR function), so tests/test_sr_chain_linux.c can check the chains with
 * nothing but the SDK headers — no loader, no SR runtime, no panel. Only
 * usable when CMake found the structs (DXR_LEIA_LNX_HAVE_SR_ROUTING): the
 * LeiaSR 1.38 headers predate them.
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#pragma once

#ifndef DXR_LEIA_LNX_HAVE_SR_ROUTING
#error "leia_sr_chain_sdk_linux.h needs SR SDK headers with SrWeaverRoutingInfo (DXR_LEIA_LNX_HAVE_SR_ROUTING)"
#endif

#include "leia_sr_routing_linux.h"

#include <sr/sr.h>
#include <sr/sr_vk.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! Storage for the extension structs one weaver create chains. Must outlive
 *  the srCreateWeaver* call (it may live on the caller's stack). */
struct leia_lnx_sr_weaver_chain
{
	SrWeaverRoutingInfo routing;
	SrDisplayBindingInfo binding;
};

/*!
 * Chain what @p plan asks for in front of @p tail (the create-info's existing
 * pNext, usually NULL) and return the new head, to be stored in the
 * create-info's pNext. Order: routing -> binding -> tail. Neither struct is
 * position-sensitive (SR walks the chain by sType), but a fixed order keeps
 * the logs and the test deterministic.
 */
static inline const void *
leia_lnx_sr_build_weaver_chain(const struct leia_lnx_sr_weaver_plan *plan,
                               struct leia_lnx_sr_weaver_chain *storage,
                               const void *tail)
{
	const void *head = tail;
	if (plan->bind_display) {
		storage->binding = SrDisplayBindingInfo(.displayId = plan->display_id);
		storage->binding.pNext = head;
		head = &storage->binding;
	}
	if (plan->routing_external) {
		storage->routing = SrWeaverRoutingInfo(.mode = SR_WEAVER_ROUTING_EXTERNAL);
		storage->routing.flags = 0; // no KEEP_DRAG_SNAP: windowless on Linux
		storage->routing.pNext = head;
		head = &storage->routing;
	}
	return head;
}

/*! Storage for the capability query: SrRuntimeCapabilities -> routing caps -> binding caps. */
struct leia_lnx_sr_caps_chain
{
	SrRuntimeCapabilities caps;
	SrWeaverRoutingCapabilities routing;
	SrDisplayBindingCapabilities binding;
};

/*!
 * Initialise @p c for srGetRuntimeCapabilities(instance, &c->caps): every
 * extension struct at "not supported", since a runtime that predates one
 * leaves it untouched.
 */
static inline void
leia_lnx_sr_caps_chain_init(struct leia_lnx_sr_caps_chain *c)
{
	c->binding = SrDisplayBindingCapabilities();
	c->routing = SrWeaverRoutingCapabilities();
	c->caps = SrRuntimeCapabilities();
	c->routing.pNext = &c->binding;
	c->caps.pNext = &c->routing;
}

//! Reduce a filled query (@p res = the srGetRuntimeCapabilities result) to the plain-C caps.
static inline struct leia_lnx_sr_multi_caps
leia_lnx_sr_caps_chain_reduce(SrResult res, const struct leia_lnx_sr_caps_chain *c)
{
	struct leia_lnx_sr_multi_caps out = {0};
	if (SR_FAILED(res)) {
		return out;
	}
	out.known = true;
	out.external_routing = c->routing.externalRouting != SR_FALSE;
	out.display_binding = c->binding.displayBinding != SR_FALSE;
	out.max_bound_displays = c->binding.maxBoundDisplays;
	return out;
}

#ifdef __cplusplus
}
#endif
