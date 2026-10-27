// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the multi-screen M6 Windows weaver plan
 *         (leia_sr_multi_win.h). Hardware-free and call-free: no SR loader,
 *         no SR runtime, no panel.
 */

#include "leia_sr_multi_win.h"

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

#define AL_ID 0x1ull
#define DS1_ID 0x2ull

//! LeiaSR 1.38.0+2056 (phases A-D): both honoured, two bindable displays.
static const struct leia_win_sr_multi_caps k_caps_2056 = {
    .known = true, .external_routing = true, .keep_drag_snap = true, .display_binding = true, .max_bound_displays = 2};
//! A runtime under new headers that answers the query but supports nothing.
static const struct leia_win_sr_multi_caps k_caps_none = {.known = true};
//! Query failed / old SDK.
static const struct leia_win_sr_multi_caps k_caps_unknown = {.known = false};

static void
test_unbound_dp_never_changes_routing(void)
{
	// The pre-M6 single-screen DP: SDK routing, no binding, whatever the caps.
	struct leia_win_sr_weaver_plan p = leia_win_sr_plan_weaver(&k_caps_2056, false, true, AL_ID);
	CHECK(!p.routing_external);
	CHECK(!p.keep_drag_snap);
	CHECK(!p.bind_display);
	CHECK(!leia_win_sr_plan_needs_present_origin(&p, true));
}

static void
test_majority_segment_keeps_hwnd_and_snap(void)
{
	struct leia_win_sr_weaver_plan p = leia_win_sr_plan_weaver(&k_caps_2056, true, true, AL_ID);
	CHECK(p.routing_external);
	CHECK(p.keep_drag_snap);
	CHECK(p.bind_display && p.display_id == AL_ID);
	// With the real window the SDK derives the phase itself.
	CHECK(!leia_win_sr_plan_needs_present_origin(&p, true));
}

static void
test_secondary_segment_is_windowless_with_origin(void)
{
	struct leia_win_sr_weaver_plan p = leia_win_sr_plan_weaver(&k_caps_2056, true, false, DS1_ID);
	CHECK(p.routing_external);
	CHECK(!p.keep_drag_snap); // no window: nothing to snap
	CHECK(p.bind_display && p.display_id == DS1_ID);
	CHECK(leia_win_sr_plan_needs_present_origin(&p, false));
}

static void
test_unknown_display_binds_nothing(void)
{
	struct leia_win_sr_weaver_plan p = leia_win_sr_plan_weaver(&k_caps_2056, true, false, 0);
	CHECK(p.routing_external);
	CHECK(!p.bind_display);
}

static void
test_caps_gate(void)
{
	struct leia_win_sr_weaver_plan p = leia_win_sr_plan_weaver(&k_caps_none, true, true, DS1_ID);
	CHECK(!p.routing_external && !p.keep_drag_snap && !p.bind_display);
	p = leia_win_sr_plan_weaver(&k_caps_unknown, true, true, DS1_ID);
	CHECK(!p.routing_external && !p.bind_display);
	// Routing without binding support: EXTERNAL on the active display only.
	const struct leia_win_sr_multi_caps routing_only = {.known = true, .external_routing = true};
	p = leia_win_sr_plan_weaver(&routing_only, true, false, DS1_ID);
	CHECK(p.routing_external && !p.bind_display && !p.keep_drag_snap);
}

static void
test_fallback_order(void)
{
	struct leia_win_sr_weaver_plan p = leia_win_sr_plan_weaver(&k_caps_2056, true, true, DS1_ID);
	CHECK(leia_win_sr_plan_weaver_fallback(&p));
	CHECK(p.routing_external && p.keep_drag_snap && !p.bind_display && p.display_id == 0);
	CHECK(leia_win_sr_plan_weaver_fallback(&p));
	CHECK(!p.routing_external && !p.keep_drag_snap && !p.bind_display);
	CHECK(!leia_win_sr_plan_weaver_fallback(&p));
}

static void
test_bound_lens(void)
{
	// D3 runtime (2079+): a bound DP binds and drives its own lens.
	const struct leia_win_sr_multi_caps d3 = {.known = true,
	                                          .external_routing = true,
	                                          .keep_drag_snap = true,
	                                          .display_binding = true,
	                                          .max_bound_displays = 2,
	                                          .lens_per_device = true};
	struct leia_win_sr_weaver_plan p = leia_win_sr_plan_weaver(&d3, true, false, DS1_ID);
	CHECK(leia_win_sr_plan_bind_lens(&d3, &p));
	// Pre-D3 runtime: never — an unbound lens would switch the active panel.
	p = leia_win_sr_plan_weaver(&k_caps_2056, true, false, DS1_ID);
	CHECK(!leia_win_sr_plan_bind_lens(&k_caps_2056, &p));
	// D3 runtime but the display did not bind (fallback dropped it): no lens.
	p = leia_win_sr_plan_weaver(&d3, true, false, DS1_ID);
	(void)leia_win_sr_plan_weaver_fallback(&p);
	CHECK(!leia_win_sr_plan_bind_lens(&d3, &p));
	// An unbound (pre-M6) DP has an empty plan: never a bound lens.
	p = leia_win_sr_plan_weaver(&d3, false, true, AL_ID);
	CHECK(!leia_win_sr_plan_bind_lens(&d3, &p));
}

static void
test_segment_tracked(void)
{
	const struct leia_win_sr_multi_caps d4 = {.known = true,
	                                          .external_routing = true,
	                                          .display_binding = true,
	                                          .max_bound_displays = 2,
	                                          .lens_per_device = true,
	                                          .eye_tracker_per_device = true};
	// D4 on: the DS1-bound weaver tracks its own camera — no pin.
	CHECK(leia_win_sr_plan_segment_tracked(&d4, DS1_ID, AL_ID));
	CHECK(!leia_win_sr_plan_pin_simulated_viewer(DS1_ID, AL_ID) || leia_win_sr_plan_segment_tracked(&d4, DS1_ID, AL_ID));
	// Gate off / pre-D4: not tracked, the pin stays.
	CHECK(!leia_win_sr_plan_segment_tracked(&k_caps_2056, DS1_ID, AL_ID));
	// The active display is always tracked; unknown active = assume tracked.
	CHECK(leia_win_sr_plan_segment_tracked(&k_caps_2056, AL_ID, AL_ID));
	CHECK(leia_win_sr_plan_segment_tracked(&k_caps_unknown, DS1_ID, 0));
	CHECK(leia_win_sr_plan_segment_tracked(&k_caps_2056, 0, AL_ID));
}

static void
test_simulated_viewer_pin(void)
{
	// The DS1-bound weaver on a rig whose tracker follows the AL: pin.
	CHECK(leia_win_sr_plan_pin_simulated_viewer(DS1_ID, AL_ID));
	// The AL-bound weaver: tracked, never pin (a pin would weave with no viewer).
	CHECK(!leia_win_sr_plan_pin_simulated_viewer(AL_ID, AL_ID));
	// Unknown active display or unbound weaver: never pin.
	CHECK(!leia_win_sr_plan_pin_simulated_viewer(DS1_ID, 0));
	CHECK(!leia_win_sr_plan_pin_simulated_viewer(0, AL_ID));
}

static void
test_window_handle_on_the_untracked_screen(void)
{
	/*
	 * ADR-047 Amendment 2, shared by the D3D11 and Vulkan per-screen DPs: the
	 * window's majority moved to the DS1, so the DS1's DP holds the HWND and
	 * the AL (the active, tracked panel) weaves windowless.
	 */
	// The DS1 DP with the window: EXTERNAL + drag snap, bound, phases from
	// the window — and still pins its viewer (its display has no tracker).
	struct leia_win_sr_weaver_plan ds1 = leia_win_sr_plan_weaver(&k_caps_2056, true, true, DS1_ID);
	CHECK(ds1.routing_external && ds1.keep_drag_snap);
	CHECK(ds1.bind_display && ds1.display_id == DS1_ID);
	CHECK(!leia_win_sr_plan_needs_present_origin(&ds1, true));
	CHECK(!leia_win_sr_plan_segment_tracked(&k_caps_2056, DS1_ID, AL_ID));
	CHECK(leia_win_sr_plan_pin_simulated_viewer(DS1_ID, AL_ID));

	// The AL's windowless replacement: EXTERNAL, no snap, phase from the
	// present origin, tracked by the active display's camera — no pin.
	struct leia_win_sr_weaver_plan al = leia_win_sr_plan_weaver(&k_caps_2056, true, false, AL_ID);
	CHECK(al.routing_external && !al.keep_drag_snap);
	CHECK(al.bind_display && al.display_id == AL_ID);
	CHECK(leia_win_sr_plan_needs_present_origin(&al, false));
	CHECK(leia_win_sr_plan_segment_tracked(&k_caps_2056, AL_ID, AL_ID));
	CHECK(!leia_win_sr_plan_pin_simulated_viewer(AL_ID, AL_ID));
}

static void
test_screen_origin_binding_wins_over_a_transient_sr_location(void)
{
	/*
	 * runtime #2311, W10-2: the DS1 (3840x2160, non-main at (-3840, 0)) was
	 * re-seated; its per-screen DP was bound to (-3840, 0), but its SR context
	 * was created while Windows listed the panel alone at (0, 0). The DP must
	 * report the binding's origin, and say once that SR disagreed.
	 */
	const struct leia_win_screen_origin o = leia_win_screen_origin_from_binding(-3840, 0, 3840, 2160);
	CHECK(o.bound && o.left == -3840 && o.top == 0);
	int32_t left = 1, top = 1;
	CHECK(leia_win_screen_origin_pick(&o, 0, 0, &left, &top));
	CHECK(left == -3840 && top == 0);
	// Once SR agrees (a DP created after the panel is back): same answer, no log.
	CHECK(!leia_win_screen_origin_pick(&o, -3840, 0, &left, &top));
	CHECK(left == -3840 && top == 0);
}

static void
test_screen_origin_zero_is_a_real_origin(void)
{
	// A panel that IS the primary display sits at (0, 0): still bound, and it
	// wins over an SR location elsewhere.
	const struct leia_win_screen_origin o = leia_win_screen_origin_from_binding(0, 0, 3840, 2160);
	CHECK(o.bound);
	int32_t left = 1, top = 1;
	CHECK(leia_win_screen_origin_pick(&o, -3840, 0, &left, &top));
	CHECK(left == 0 && top == 0);
	// Negative top (a panel above the main display).
	const struct leia_win_screen_origin above = leia_win_screen_origin_from_binding(0, -2160, 3840, 2160);
	CHECK(!leia_win_screen_origin_pick(&above, 0, -2160, &left, &top));
	CHECK(left == 0 && top == -2160);
}

static void
test_screen_origin_unbound_keeps_the_sr_location(void)
{
	int32_t left = 1, top = 1;
	// A window-bound DP (create_dp_d3d11 etc.) has no binding: unchanged.
	const struct leia_win_screen_origin none = {0};
	CHECK(!leia_win_screen_origin_pick(&none, -3840, 0, &left, &top));
	CHECK(left == -3840 && top == 0);
	CHECK(!leia_win_screen_origin_pick(NULL, 0, 0, &left, &top));
	CHECK(left == 0 && top == 0);
	// A binding without a rect: nothing to trust, keep the SR location.
	struct leia_win_screen_origin o = leia_win_screen_origin_from_binding(-3840, 0, 0, 2160);
	CHECK(!o.bound);
	CHECK(!leia_win_screen_origin_pick(&o, 5, 6, &left, &top));
	CHECK(left == 5 && top == 6);
	o = leia_win_screen_origin_from_binding(-3840, 0, 3840, 0);
	CHECK(!o.bound);
}

int
main(void)
{
	test_unbound_dp_never_changes_routing();
	test_majority_segment_keeps_hwnd_and_snap();
	test_secondary_segment_is_windowless_with_origin();
	test_unknown_display_binds_nothing();
	test_caps_gate();
	test_fallback_order();
	test_bound_lens();
	test_segment_tracked();
	test_simulated_viewer_pin();
	test_window_handle_on_the_untracked_screen();
	test_screen_origin_binding_wins_over_a_transient_sr_location();
	test_screen_origin_zero_is_a_real_origin();
	test_screen_origin_unbound_keeps_the_sr_location();
	if (g_failures != 0) {
		fprintf(stderr, "test_sr_multi_win: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	printf("test_sr_multi_win: OK\n");
	return EXIT_SUCCESS;
}
