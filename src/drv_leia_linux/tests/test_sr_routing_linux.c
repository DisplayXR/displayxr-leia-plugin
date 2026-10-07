// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the multi-screen M4 SR weaver chain: the plan
 *         (leia_sr_routing_linux.h, everywhere) and — when the SDK headers
 *         carry the structs — the real pNext chains built from it
 *         (leia_sr_chain_sdk_linux.h).
 *
 * Hardware-free and call-free: no SR loader, no SR runtime, no panel. The
 * chain half uses only SR types and the SDK's compound-literal macros.
 */

#include "leia_sr_routing_linux.h"
#ifdef DXR_LEIA_LNX_HAVE_SR_ROUTING
#include "leia_sr_chain_sdk_linux.h"
#endif

#include <stdio.h>
#include <stdlib.h>

static int g_failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

#define DS1_ID 0x5352000000000001ull

//! LeiaSR 876620d62 (phases A-C): both honoured, one bindable display.
static const struct leia_lnx_sr_multi_caps k_caps_new = {
    .known = true, .external_routing = true, .display_binding = true, .max_bound_displays = 1};
//! LeiaSR 1.38 runtime under new headers: the query succeeds, structs untouched.
static const struct leia_lnx_sr_multi_caps k_caps_138 = {.known = true};

static void
test_plan_new_runtime(void)
{
	struct leia_lnx_sr_weaver_plan p = leia_lnx_sr_plan_weaver(&k_caps_new, true, DS1_ID);
	CHECK(p.routing_external);
	CHECK(p.bind_display && p.display_id == DS1_ID);

	// No display id known (plain factory / SR does not list the monitor).
	p = leia_lnx_sr_plan_weaver(&k_caps_new, true, 0);
	CHECK(p.routing_external && !p.bind_display && p.display_id == 0);

	// DXR_LEIA_SR_EXTERNAL_ROUTING=0: SDK routing, binding unaffected.
	p = leia_lnx_sr_plan_weaver(&k_caps_new, false, DS1_ID);
	CHECK(!p.routing_external && p.bind_display);
}

static void
test_plan_old_or_unknown_runtime(void)
{
	struct leia_lnx_sr_weaver_plan p = leia_lnx_sr_plan_weaver(&k_caps_138, true, DS1_ID);
	CHECK(!p.routing_external && !p.bind_display);

	p = leia_lnx_sr_plan_weaver(NULL, true, DS1_ID);
	CHECK(!p.routing_external && !p.bind_display);

	const struct leia_lnx_sr_multi_caps unknown = {
	    .known = false, .external_routing = true, .display_binding = true, .max_bound_displays = 1};
	p = leia_lnx_sr_plan_weaver(&unknown, true, DS1_ID);
	CHECK(!p.routing_external && !p.bind_display);

	// Binding honoured but zero bindable displays: no binding.
	const struct leia_lnx_sr_multi_caps none_bindable = {.known = true, .display_binding = true};
	p = leia_lnx_sr_plan_weaver(&none_bindable, true, DS1_ID);
	CHECK(!p.bind_display);
}

static void
test_fallback_order(void)
{
	struct leia_lnx_sr_weaver_plan p = leia_lnx_sr_plan_weaver(&k_caps_new, true, DS1_ID);
	struct leia_lnx_sr_weaver_plan n;
	// 1. drop the binding (EDID-only / unknown id refused) ...
	CHECK(leia_lnx_sr_plan_weaver_fallback(&p, &n));
	CHECK(n.routing_external && !n.bind_display && n.display_id == 0);
	// 2. ... then the routing ...
	p = n;
	CHECK(leia_lnx_sr_plan_weaver_fallback(&p, &n));
	CHECK(!n.routing_external && !n.bind_display);
	// 3. ... then nothing left: the plain create failed, give up.
	p = n;
	CHECK(!leia_lnx_sr_plan_weaver_fallback(&p, &n));
}

#ifdef DXR_LEIA_LNX_HAVE_SR_ROUTING
static void
test_chain_full(void)
{
	const struct leia_lnx_sr_weaver_plan p = leia_lnx_sr_plan_weaver(&k_caps_new, true, DS1_ID);
	struct leia_lnx_sr_weaver_chain storage;
	SrWeaverCreateInfoVulkan wci = SrWeaverCreateInfoVulkan(.window = 0);
	wci.pNext = leia_lnx_sr_build_weaver_chain(&p, &storage, wci.pNext);

	const SrWeaverRoutingInfo *r = (const SrWeaverRoutingInfo *)wci.pNext;
	CHECK(r == &storage.routing);
	CHECK(r->sType == SR_TYPE_WEAVER_ROUTING_INFO && (int)r->sType == 22);
	CHECK(r->mode == SR_WEAVER_ROUTING_EXTERNAL);
	CHECK(r->flags == 0); // no KEEP_DRAG_SNAP on Linux
	const SrDisplayBindingInfo *b = (const SrDisplayBindingInfo *)r->pNext;
	CHECK(b == &storage.binding);
	CHECK(b->sType == SR_TYPE_DISPLAY_BINDING_INFO && (int)b->sType == 23);
	CHECK(b->displayId == DS1_ID);
	CHECK(b->pNext == NULL);
	CHECK(wci.window == 0);
}

static void
test_chain_partial_and_empty(void)
{
	struct leia_lnx_sr_weaver_chain storage;
	int tail_marker = 0;

	// Routing only, onto an existing tail.
	struct leia_lnx_sr_weaver_plan p = leia_lnx_sr_plan_weaver(&k_caps_new, true, 0);
	const void *head = leia_lnx_sr_build_weaver_chain(&p, &storage, &tail_marker);
	CHECK(head == &storage.routing);
	CHECK(storage.routing.pNext == &tail_marker);

	// Binding only (routing opted out).
	p = leia_lnx_sr_plan_weaver(&k_caps_new, false, DS1_ID);
	head = leia_lnx_sr_build_weaver_chain(&p, &storage, NULL);
	CHECK(head == &storage.binding);
	CHECK(storage.binding.pNext == NULL && storage.binding.displayId == DS1_ID);

	// Nothing (1.38 runtime): the create-info is untouched.
	p = leia_lnx_sr_plan_weaver(&k_caps_138, true, DS1_ID);
	head = leia_lnx_sr_build_weaver_chain(&p, &storage, &tail_marker);
	CHECK(head == &tail_marker);
	head = leia_lnx_sr_build_weaver_chain(&p, &storage, NULL);
	CHECK(head == NULL);
}

static void
test_caps_chain(void)
{
	struct leia_lnx_sr_caps_chain c;
	leia_lnx_sr_caps_chain_init(&c);
	CHECK(c.caps.sType == SR_TYPE_RUNTIME_CAPABILITIES);
	CHECK(c.caps.pNext == &c.routing);
	CHECK(c.routing.sType == SR_TYPE_WEAVER_ROUTING_CAPABILITIES && (int)c.routing.sType == 24);
	CHECK(c.routing.pNext == &c.binding);
	CHECK(c.binding.sType == SR_TYPE_DISPLAY_BINDING_CAPABILITIES && (int)c.binding.sType == 26);
	CHECK(c.binding.pNext == NULL);
	// Initialised to "not supported": a runtime that predates them leaves them so.
	struct leia_lnx_sr_multi_caps m = leia_lnx_sr_caps_chain_reduce(SR_SUCCESS, &c);
	CHECK(m.known && !m.external_routing && !m.display_binding && m.max_bound_displays == 0);

	// What a phase A-C runtime writes.
	c.routing.externalRouting = SR_TRUE;
	c.binding.displayBinding = SR_TRUE;
	c.binding.maxBoundDisplays = 1;
	m = leia_lnx_sr_caps_chain_reduce(SR_SUCCESS, &c);
	CHECK(m.known && m.external_routing && m.display_binding && m.max_bound_displays == 1);

	// A failed query is "unknown", whatever the structs hold.
	m = leia_lnx_sr_caps_chain_reduce(SR_ERROR_VALIDATION_FAILURE, &c);
	CHECK(!m.known && !m.external_routing && !m.display_binding);
}
#endif

int
main(void)
{
	test_plan_new_runtime();
	test_plan_old_or_unknown_runtime();
	test_fallback_order();
#ifdef DXR_LEIA_LNX_HAVE_SR_ROUTING
	test_chain_full();
	test_chain_partial_and_empty();
	test_caps_chain();
	printf("test_sr_routing_linux: plan + SR pNext chains checked\n");
#else
	printf("test_sr_routing_linux: plan checked (SR chain half compiled out: SDK headers lack the structs)\n");
#endif
	if (g_failures != 0) {
		fprintf(stderr, "test_sr_routing_linux: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	printf("test_sr_routing_linux: all checks passed\n");
	return EXIT_SUCCESS;
}
