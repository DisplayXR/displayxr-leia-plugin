// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for multi-screen M4 per-DP screen state
 *         (leia_screen_linux.h): binding -> what one DP answers.
 *
 * Hardware-free: bindings, the EDID panel list, the SR display query and the
 * claim table entries are hand-built fixtures — no SDK, no Vulkan device, no
 * sysfs, no X server. The box this targets (ds1-linux): the laptop eDP at
 * (0,0) owned by sim_display, the Acer DS1 on HDMI-A-1 to its right, owned by
 * leia-sr; plus a second Leia panel for the "not the SR panel" rules.
 */

#include "leia_screen_linux.h"
#include "leia_sr_linux.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

#define DS1_SR_ID 0x5352000000000001ull
#define OTHER_SR_ID 0x5352000000000002ull

//! The DS1 on HDMI-A-1, right of a 2880-px-wide eDP.
static struct leia_lnx_edid_panel
ds1_panel(void)
{
	struct leia_lnx_edid_panel p = {0};
	snprintf(p.connector, sizeof(p.connector), "HDMI-A-1");
	snprintf(p.randr_output, sizeof(p.randr_output), "HDMI-1");
	p.manufacturer_id = 29188;
	p.product_id = 1;
	p.width_mm = 344;
	p.height_mm = 194;
	p.native_w = 3840;
	p.native_h = 2160;
	p.has_position = true;
	p.left = 2880;
	p.top = 0;
	return p;
}

//! A second Leia panel (LeiaSR phase D territory) on DP-2, below the DS1.
static struct leia_lnx_edid_panel
second_panel(void)
{
	struct leia_lnx_edid_panel p = {0};
	snprintf(p.connector, sizeof(p.connector), "DP-2");
	p.manufacturer_id = 29188;
	p.product_id = 1;
	p.width_mm = 597;
	p.height_mm = 336;
	p.native_w = 3840;
	p.native_h = 2160;
	p.has_position = true;
	p.left = 2880;
	p.top = 2160;
	return p;
}

//! What the SR context reports for the DS1 (calibrated, not EDID-rounded).
static struct leiasr_lnx_display_info
sr_ds1(void)
{
	struct leiasr_lnx_display_info i = {0};
	i.valid = true;
	i.width_m = 0.3442f;
	i.height_m = 0.1936f;
	i.pixel_width = 3840;
	i.pixel_height = 2160;
	i.recommended_view_width = 1920;
	i.recommended_view_height = 1080;
	i.refresh_mhz = 59940;
	i.nominal_viewer_y_m = 0.01f;
	i.nominal_viewer_z_m = 0.60f;
	return i;
}

static struct leia_lnx_screen_binding
ds1_binding(void)
{
	struct leia_lnx_screen_binding b = {0};
	b.monitor_id = 0xD51;
	b.desktop_left = 2880;
	b.desktop_top = 0;
	b.desktop_width = 2560; // a 150 % scaled desktop: logical px ...
	b.desktop_height = 1440;
	b.native_width = 3840; // ... vs the device mode
	b.native_height = 2160;
	b.width_mm = 344;
	b.height_mm = 194;
	snprintf(b.device_name, sizeof(b.device_name), "HDMI-1");
	return b;
}

/* The DS1 binding resolves to the SR-driven panel with SR's numbers, the
 * binding's origin and the device-mode pixels. */
static void
test_ds1_binding_is_the_sr_panel(void)
{
	const struct leia_lnx_edid_panel panels[1] = {ds1_panel()};
	const struct leiasr_lnx_display_info sr = sr_ds1();
	const struct leia_lnx_screen_binding b = ds1_binding();

	struct leia_lnx_screen s;
	leia_lnx_screen_resolve(&b, NULL, panels, 1, &sr, 0, &s);
	CHECK(s.valid);
	CHECK(s.monitor_id == 0xD51);
	CHECK(strcmp(s.connector, "HDMI-A-1") == 0); // joined by RandR output name
	CHECK(s.is_sr_panel);                        // SR 1.38 rule: the first panel
	CHECK(s.pixel_width == 3840 && s.pixel_height == 2160);
	CHECK(s.screen_left == 2880 && s.screen_top == 0);
	CHECK(NEAR(s.width_m, 0.3442f) && NEAR(s.height_m, 0.1936f)); // SR's, not EDID mm
	CHECK(NEAR(s.nominal_viewer_z_m, 0.60f) && NEAR(s.nominal_viewer_y_m, 0.01f));
	CHECK(s.recommended_view_width == 1920 && s.recommended_view_height == 1080);
	CHECK(s.refresh_mhz == 59940);
}

/* The claim's connector wins over every heuristic; its displayId is carried. */
static void
test_claim_connector_and_display_id(void)
{
	const struct leia_lnx_edid_panel panels[2] = {ds1_panel(), second_panel()};
	const struct leiasr_lnx_display_info sr = sr_ds1();
	struct leia_lnx_screen_binding b = ds1_binding();
	snprintf(b.device_name, sizeof(b.device_name), "%s", ""); // nothing to join by name
	b.desktop_left = 9999;                                    // nor by origin

	struct leia_lnx_claim_binding claim = {.monitor_id = 0xD51, .sr_display_id = DS1_SR_ID};
	snprintf(claim.connector, sizeof(claim.connector), "HDMI-A-1");

	struct leia_lnx_screen s;
	leia_lnx_screen_resolve(&b, &claim, panels, 2, &sr, DS1_SR_ID, &s);
	CHECK(strcmp(s.connector, "HDMI-A-1") == 0);
	CHECK(s.sr_display_id == DS1_SR_ID);
	CHECK(s.is_sr_panel);         // by id
	CHECK(s.screen_left == 9999); // origin is the binding's, always

	// The binding's device name outranks the claim's connector (review B):
	// a claim made by list order could name the twin.
	snprintf(b.device_name, sizeof(b.device_name), "DP-2");
	leia_lnx_screen_resolve(&b, &claim, panels, 2, &sr, DS1_SR_ID, &s);
	CHECK(strcmp(s.connector, "DP-2") == 0);
	snprintf(b.device_name, sizeof(b.device_name), "%s", "");

	// The runtime's opaque display_id wins over the claim table.
	b.display_id = OTHER_SR_ID;
	leia_lnx_screen_resolve(&b, &claim, panels, 2, &sr, DS1_SR_ID, &s);
	CHECK(s.sr_display_id == OTHER_SR_ID);
	CHECK(!s.is_sr_panel); // ids known on both sides and different
}

/* A second Leia panel is not the SR-driven one: EDID facts, no SR numbers. */
static void
test_second_panel_is_not_the_sr_panel(void)
{
	const struct leia_lnx_edid_panel panels[2] = {ds1_panel(), second_panel()};
	const struct leiasr_lnx_display_info sr = sr_ds1();
	struct leia_lnx_screen_binding b = {0};
	b.monitor_id = 0x2;
	b.desktop_left = 2880;
	b.desktop_top = 2160;
	b.desktop_width = 3840;
	b.desktop_height = 2160; // no device mode, no mm in the binding

	struct leia_lnx_screen s;
	leia_lnx_screen_resolve(&b, NULL, panels, 2, &sr, 0, &s);
	CHECK(s.valid);
	CHECK(strcmp(s.connector, "DP-2") == 0);                    // joined by origin
	CHECK(!s.is_sr_panel);                                      // not the first panel
	CHECK(NEAR(s.width_m, 0.597f) && NEAR(s.height_m, 0.336f)); // EDID mm
	CHECK(s.pixel_width == 3840 && s.pixel_height == 2160);     // EDID native
	CHECK(s.recommended_view_width == 0 && s.refresh_mhz == 0); // unknown, not SR's
	// Centred viewer at SR's distance-to-height ratio.
	CHECK(NEAR(s.nominal_viewer_x_m, 0.0f) && NEAR(s.nominal_viewer_y_m, 0.0f));
	CHECK(NEAR(s.nominal_viewer_z_m, 0.60f * (0.336f / 0.1936f)));
}

/* Two DPs, two screens: each instance holds its own answer; resolving the
 * second never touches the first (no shared/static state). */
static void
test_two_instances_do_not_share_state(void)
{
	const struct leia_lnx_edid_panel panels[2] = {ds1_panel(), second_panel()};
	const struct leiasr_lnx_display_info sr = sr_ds1();
	const struct leia_lnx_screen_binding b1 = ds1_binding();
	struct leia_lnx_screen_binding b2 = {0};
	b2.monitor_id = 0x2;
	b2.desktop_left = 2880;
	b2.desktop_top = 2160;
	b2.native_width = 3840;
	b2.native_height = 2160;
	b2.width_mm = 597;
	b2.height_mm = 336;

	struct leia_lnx_screen s1, s2;
	leia_lnx_screen_resolve(&b1, NULL, panels, 2, &sr, 0, &s1);
	const struct leia_lnx_screen s1_before = s1;
	leia_lnx_screen_resolve(&b2, NULL, panels, 2, &sr, 0, &s2);

	CHECK(memcmp(&s1, &s1_before, sizeof(s1)) == 0);
	CHECK(s1.monitor_id != s2.monitor_id);
	CHECK(s1.screen_top == 0 && s2.screen_top == 2160);
	CHECK(s1.is_sr_panel && !s2.is_sr_panel);
	CHECK(!NEAR(s1.width_m, s2.width_m));

	// And re-resolving the first afterwards gives the identical answer.
	struct leia_lnx_screen s1_again;
	leia_lnx_screen_resolve(&b1, NULL, panels, 2, &sr, 0, &s1_again);
	CHECK(memcmp(&s1, &s1_again, sizeof(s1)) == 0);
}

/* No SR display (headless / SR down): physical facts only, never "the SR panel". */
static void
test_no_sr(void)
{
	const struct leia_lnx_edid_panel panels[1] = {ds1_panel()};
	const struct leia_lnx_screen_binding b = ds1_binding();
	struct leia_lnx_screen s;
	leia_lnx_screen_resolve(&b, NULL, panels, 1, NULL, 0, &s);
	CHECK(s.valid);
	CHECK(!s.is_sr_panel);
	CHECK(NEAR(s.width_m, 0.344f) && NEAR(s.height_m, 0.194f)); // binding mm
	CHECK(NEAR(s.nominal_viewer_z_m, 0.65f));                   // desktop default

	struct leiasr_lnx_display_info invalid = sr_ds1();
	invalid.valid = false;
	leia_lnx_screen_resolve(&b, NULL, panels, 1, &invalid, 0, &s);
	CHECK(!s.is_sr_panel);
}

/* Forced probe with no EDID match at all: the one-panel assumption stands. */
static void
test_no_panel_list(void)
{
	const struct leiasr_lnx_display_info sr = sr_ds1();
	struct leia_lnx_screen_binding b = ds1_binding();
	b.native_width = 0;
	b.native_height = 0;
	struct leia_lnx_screen s;
	leia_lnx_screen_resolve(&b, NULL, NULL, 0, &sr, 0, &s);
	CHECK(s.valid);
	CHECK(s.is_sr_panel);
	CHECK(s.connector[0] == '\0');
	// No device mode in the binding and no panel: the desktop size.
	CHECK(s.pixel_width == 2560 && s.pixel_height == 1440);
}

/* Nothing usable: invalid, and the DP getters then report false. */
static void
test_incomplete_is_invalid(void)
{
	struct leia_lnx_screen_binding b = {0};
	b.monitor_id = 7;
	struct leia_lnx_screen s;
	leia_lnx_screen_resolve(&b, NULL, NULL, 0, NULL, 0, &s);
	CHECK(!s.valid);
	CHECK(s.monitor_id == 7);
}

int
main(void)
{
	test_ds1_binding_is_the_sr_panel();
	test_claim_connector_and_display_id();
	test_second_panel_is_not_the_sr_panel();
	test_two_instances_do_not_share_state();
	test_no_sr();
	test_no_panel_list();
	test_incomplete_is_invalid();
	if (g_failures != 0) {
		fprintf(stderr, "test_screen_linux: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	printf("test_screen_linux: all checks passed\n");
	return EXIT_SUCCESS;
}
