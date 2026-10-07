// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for multi-screen M0 on Linux: the list-returning EDID
 *         parser (leia_edid_probe_linux.h) and runtime-descriptor -> claim
 *         matching (leia_display_claims_linux.h).
 *
 * Hardware-free: EDID fixtures are synthesised in memory (an ACR DS1-style
 * panel with an EDID serial, a non-Leia eDP), panels and SR displays are
 * hand-built — no sysfs, no X server, no SR SDK.
 */

#include "leia_display_claims_linux.h"
#include "leia_edid_probe_linux.h"

#include <stddef.h>
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

#define ACR_ID 29188u  /* "ACR" as the table stores it */
#define DS1_PROD 1u    /* {29188, 1} = ACER_DS1 (alt) in leia_edid_table.h */
#define LGD_ID 0xE430u /* "LGD", not in the table */

/*! Build a 128-byte EDID base block: ids, serial, DTD #1 with px + mm. */
static void
make_edid(uint8_t *e, uint16_t man, uint16_t prod, uint32_t serial, uint32_t w, uint32_t h, uint32_t wmm, uint32_t hmm)
{
	static const uint8_t magic[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
	memset(e, 0, 128);
	memcpy(e, magic, 8);
	e[8] = (uint8_t)(man & 0xFF);
	e[9] = (uint8_t)(man >> 8);
	e[10] = (uint8_t)(prod & 0xFF);
	e[11] = (uint8_t)(prod >> 8);
	e[12] = (uint8_t)(serial & 0xFF);
	e[13] = (uint8_t)((serial >> 8) & 0xFF);
	e[14] = (uint8_t)((serial >> 16) & 0xFF);
	e[15] = (uint8_t)(serial >> 24);
	e[18] = 1;
	e[19] = 4;
	e[21] = (uint8_t)(wmm / 10);
	e[22] = (uint8_t)(hmm / 10);
	// DTD #1: pixel clock 594.00 MHz (59400 x 10 kHz), active w/h, image mm.
	e[54] = (uint8_t)(59400 & 0xFF);
	e[55] = (uint8_t)(59400 >> 8);
	e[56] = (uint8_t)(w & 0xFF);
	e[58] = (uint8_t)((w >> 8) << 4);
	e[59] = (uint8_t)(h & 0xFF);
	e[61] = (uint8_t)((h >> 8) << 4);
	e[66] = (uint8_t)(wmm & 0xFF);
	e[67] = (uint8_t)(hmm & 0xFF);
	e[68] = (uint8_t)(((wmm >> 8) << 4) | ((hmm >> 8) & 0x0F));
}

static void
test_edid_parse(void)
{
	uint8_t e[128];
	struct leia_lnx_edid_panel p;

	// ACR DS1-style: 3840x2160, 344x194 mm, EDID serial 0x01020304.
	make_edid(e, ACR_ID, DS1_PROD, 0x01020304u, 3840, 2160, 344, 194);
	CHECK(leia_lnx_edid_parse(e, sizeof(e), &p));
	CHECK(p.manufacturer_id == ACR_ID);
	CHECK(p.product_id == DS1_PROD);
	CHECK(p.serial == 0x01020304u);
	CHECK(p.native_w == 3840 && p.native_h == 2160);
	CHECK(p.width_mm == 344 && p.height_mm == 194);
	CHECK(p.connector[0] == '\0' && !p.has_position);
	CHECK(leia_lnx_edid_table_contains(p.manufacturer_id, p.product_id));

	// Non-Leia eDP: parses, not in the table.
	make_edid(e, LGD_ID, 0x0601, 0, 2560, 1600, 302, 189);
	CHECK(leia_lnx_edid_parse(e, sizeof(e), &p));
	CHECK(p.native_w == 2560 && p.native_h == 1600);
	CHECK(!leia_lnx_edid_table_contains(p.manufacturer_id, p.product_id));

	// DTD #1 is a display descriptor (pixel clock 0): cm fallback, no px.
	make_edid(e, ACR_ID, DS1_PROD, 0, 3840, 2160, 600, 340);
	e[54] = e[55] = 0;
	CHECK(leia_lnx_edid_parse(e, sizeof(e), &p));
	CHECK(p.native_w == 0 && p.width_mm == 600 && p.height_mm == 340);

	// Rejects: short blob, wrong magic, NULL.
	CHECK(!leia_lnx_edid_parse(e, 16, &p));
	e[0] = 0x42;
	CHECK(!leia_lnx_edid_parse(e, sizeof(e), &p));
	CHECK(!leia_lnx_edid_parse(NULL, 128, &p));
}

static void
test_pnp(void)
{
	CHECK(leia_lnx_pnp_to_manufacturer_id("ACR") == ACR_ID);
	CHECK(leia_lnx_pnp_to_manufacturer_id("DEL") == 44048u);
	CHECK(leia_lnx_pnp_to_manufacturer_id("SAM") == 11596u);
	CHECK(leia_lnx_pnp_to_manufacturer_id("ac") == 0);
	CHECK(leia_lnx_pnp_to_manufacturer_id(NULL) == 0);
}

static struct xrt_display_descriptor
desc(uint64_t id, uint16_t man, uint16_t prod, uint32_t w, uint32_t h, int32_t l, int32_t t)
{
	struct xrt_display_descriptor d;
	memset(&d, 0, sizeof(d));
	d.struct_size = sizeof(d);
	d.monitor_id = id;
	d.edid_manufacturer = man;
	d.edid_product = prod;
	d.pixel_width = w;
	d.pixel_height = h;
	d.refresh_mhz = 60000;
	d.screen_left = l;
	d.screen_top = t;
	return d;
}

static struct leia_lnx_edid_panel
panel(const char *conn, uint32_t serial, bool pos, int32_t l, int32_t t)
{
	uint8_t e[128];
	struct leia_lnx_edid_panel p;
	make_edid(e, ACR_ID, DS1_PROD, serial, 3840, 2160, 344, 194);
	(void)leia_lnx_edid_parse(e, sizeof(e), &p);
	snprintf(p.connector, sizeof(p.connector), "%s", conn);
	p.has_position = pos;
	p.left = l;
	p.top = t;
	p.crtc_w = pos ? 3840 : 0;
	p.crtc_h = pos ? 2160 : 0;
	return p;
}

static void
test_claims_edid_ids(void)
{
	struct xrt_display_descriptor d[2] = {
	    desc(0xA, LGD_ID, 0x0601, 2560, 1600, 0, 0),
	    desc(0xB, ACR_ID, DS1_PROD, 3840, 2160, 2560, 0),
	};
	struct leia_lnx_edid_panel p = panel("HDMI-A-1", 7, true, 2560, 0);
	struct leia_lnx_claim_inputs in = {
	    .panels = &p, .panel_count = 1, .sr_display_count = -1, .supported_apis = XRT_DP_API_BIT_VK};
	struct xrt_display_claim c[4];
	struct leia_lnx_claim_binding b[4];

	// 1.38 without a live context: EDID confidence, no serial.
	uint32_t n = leia_lnx_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 1);
	CHECK(c[0].monitor_id == 0xB);
	CHECK(c[0].confidence == XRT_DISPLAY_CLAIM_EDID);
	CHECK(c[0].supported_apis == XRT_DP_API_BIT_VK);
	CHECK(c[0].serial[0] == '\0');
	CHECK(strcmp(b[0].connector, "HDMI-A-1") == 0 && b[0].sr_display_id == 0);

	// 1.38 with a live context's lens serial: one panel -> VERIFIED + serial.
	in.legacy_fpc_serial = "QI012321D10117";
	n = leia_lnx_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 1 && c[0].confidence == XRT_DISPLAY_CLAIM_VERIFIED);
	CHECK(strcmp(c[0].serial, "QI012321D10117") == 0);

	// ... but two panels: the serial cannot be attributed -> both EDID.
	struct xrt_display_descriptor d2[2] = {
	    desc(0xB, ACR_ID, DS1_PROD, 3840, 2160, 0, 0),
	    desc(0xC, ACR_ID, DS1_PROD, 3840, 2160, 3840, 0),
	};
	struct leia_lnx_edid_panel p2[2] = {panel("DP-1", 1, true, 3840, 0), panel("HDMI-A-1", 2, true, 0, 0)};
	in.panels = p2;
	in.panel_count = 2;
	n = leia_lnx_compute_claims(d2, 2, &in, c, b, 4);
	CHECK(n == 2);
	CHECK(c[0].confidence == XRT_DISPLAY_CLAIM_EDID && c[1].confidence == XRT_DISPLAY_CLAIM_EDID);
	// Identical twins are told apart by the RandR origin, not list order.
	CHECK(c[0].monitor_id == 0xB && strcmp(b[0].connector, "HDMI-A-1") == 0);
	CHECK(c[1].monitor_id == 0xC && strcmp(b[1].connector, "DP-1") == 0);

	// max_claims is honoured.
	n = leia_lnx_compute_claims(d2, 2, &in, c, b, 1);
	CHECK(n == 1);

	// No Leia panel at all: nothing claimed.
	in.panel_count = 0;
	CHECK(leia_lnx_compute_claims(d, 2, &in, c, b, 4) == 0);
}

static void
test_claims_no_ids(void)
{
	// The runtime's XWayland path: descriptors carry no EDID ids (#251).
	struct leia_lnx_claim_inputs in = {.sr_display_count = -1, .supported_apis = XRT_DP_API_BIT_VK};
	struct xrt_display_claim c[4];

	// eDP and DS1 share 3840x2160; the RandR origin picks the panel.
	struct xrt_display_descriptor d[2] = {
	    desc(0xA, 0, 0, 3840, 2160, 0, 0),
	    desc(0xB, 0, 0, 3840, 2160, 3840, 0),
	};
	struct leia_lnx_edid_panel p = panel("HDMI-A-1", 7, true, 3840, 0);
	in.panels = &p;
	in.panel_count = 1;
	uint32_t n = leia_lnx_compute_claims(d, 2, &in, c, NULL, 4);
	CHECK(n == 1 && c[0].monitor_id == 0xB);

	// Same sizes, no RandR origin: ambiguous -> claim nothing (never the eDP).
	p.has_position = false;
	CHECK(leia_lnx_compute_claims(d, 2, &in, c, NULL, 4) == 0);

	// Different eDP size: the size match is unambiguous -> claim the panel.
	d[0] = desc(0xA, 0, 0, 2560, 1600, 0, 0);
	n = leia_lnx_compute_claims(d, 2, &in, c, NULL, 4);
	CHECK(n == 1 && c[0].monitor_id == 0xB && c[0].confidence == XRT_DISPLAY_CLAIM_EDID);
}

static void
test_claims_new_api(void)
{
	struct xrt_display_descriptor d[2] = {
	    desc(0xA, LGD_ID, 0x0601, 2560, 1600, 0, 0),
	    desc(0xB, ACR_ID, DS1_PROD, 3840, 2160, 2560, 0),
	};
	struct leia_lnx_edid_panel p = panel("HDMI-A-1", 7, true, 2560, 0);
	struct leia_lnx_sr_display sr;
	memset(&sr, 0, sizeof(sr));
	sr.display_id = 0xD15D;
	sr.fpc_verified = true;
	snprintf(sr.serial, sizeof(sr.serial), "QI012321D10117");
	sr.manufacturer_id = ACR_ID;
	sr.product_id = DS1_PROD;
	sr.edid_serial = 99; // differs: the connector is the join key here
	snprintf(sr.connector, sizeof(sr.connector), "HDMI-A-1");

	struct leia_lnx_claim_inputs in = {.panels = &p,
	                                   .panel_count = 1,
	                                   .sr_displays = &sr,
	                                   .sr_display_count = 1,
	                                   .legacy_fpc_serial = "IGNORED-ON-NEW-API",
	                                   .supported_apis = XRT_DP_API_BIT_VK};
	struct xrt_display_claim c[4];
	struct leia_lnx_claim_binding b[4];

	uint32_t n = leia_lnx_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 1 && c[0].monitor_id == 0xB);
	CHECK(c[0].confidence == XRT_DISPLAY_CLAIM_VERIFIED);
	CHECK(strcmp(c[0].serial, "QI012321D10117") == 0);
	CHECK(b[0].sr_display_id == 0xD15D);

	// EDID-only in SR's eyes: EDID confidence, but displayId still kept.
	sr.fpc_verified = false;
	sr.serial[0] = '\0';
	n = leia_lnx_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 1 && c[0].confidence == XRT_DISPLAY_CLAIM_EDID && c[0].serial[0] == '\0');
	CHECK(b[0].sr_display_id == 0xD15D);

	// No connector: join by EDID (manufacturer, product, serial).
	sr.fpc_verified = true;
	sr.connector[0] = '\0';
	sr.edid_serial = 7;
	n = leia_lnx_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 1 && c[0].confidence == XRT_DISPLAY_CLAIM_VERIFIED && b[0].sr_display_id == 0xD15D);

	// SR lists the panel but the EDID scan does not (frozen table stale):
	// still claimed, on SR's authority.
	in.panel_count = 0;
	n = leia_lnx_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 1 && c[0].monitor_id == 0xB && c[0].confidence == XRT_DISPLAY_CLAIM_VERIFIED);

	// SR enumeration works and reports nothing: the table panel is claimed
	// at EDID confidence (the 1.38 serial is not consulted on the new API).
	in.panel_count = 1;
	in.sr_display_count = 0;
	n = leia_lnx_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 1 && c[0].confidence == XRT_DISPLAY_CLAIM_EDID);
}

/* Identical twins without RandR positions (the XWayland norm): the panel list
 * is in DRM connector order, the descriptors in RandR order, so nothing pairs
 * them. Both monitors are claimed at EDID confidence with NO identity — never
 * one twin's serial / SR display / connector on the other monitor. */
static void
test_claims_ambiguous_twins(void)
{
	struct xrt_display_descriptor d[2] = {
	    desc(0xB, ACR_ID, DS1_PROD, 3840, 2160, 0, 0),
	    desc(0xC, ACR_ID, DS1_PROD, 3840, 2160, 3840, 0),
	};
	struct leia_lnx_edid_panel p2[2] = {panel("DP-1", 1, false, 0, 0), panel("HDMI-A-1", 2, false, 0, 0)};
	struct leia_lnx_sr_display sr[2];
	memset(sr, 0, sizeof(sr));
	for (int i = 0; i < 2; i++) {
		sr[i].display_id = 0xD150 + (uint64_t)i;
		sr[i].fpc_verified = i == 0;
		snprintf(sr[i].serial, sizeof(sr[i].serial), "%s", i == 0 ? "QI012321D10117" : "");
		sr[i].manufacturer_id = ACR_ID;
		sr[i].product_id = DS1_PROD;
		sr[i].edid_serial = (uint32_t)(i + 1);
	}
	snprintf(sr[0].connector, sizeof(sr[0].connector), "DP-1");
	snprintf(sr[1].connector, sizeof(sr[1].connector), "HDMI-A-1");

	struct leia_lnx_claim_inputs in = {
	    .panels = p2, .panel_count = 2, .sr_display_count = -1, .supported_apis = XRT_DP_API_BIT_VK};
	struct xrt_display_claim c[4];
	struct leia_lnx_claim_binding b[4];

	// 1.38: both claimed, both EDID, no serial, no connector.
	uint32_t n = leia_lnx_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 2);
	for (uint32_t i = 0; i < n; i++) {
		CHECK(c[i].confidence == XRT_DISPLAY_CLAIM_EDID && c[i].serial[0] == '\0');
		CHECK(b[i].connector[0] == '\0' && b[i].sr_display_id == 0);
	}

	// New API: SR's per-connector identity cannot be attributed either.
	in.sr_displays = sr;
	in.sr_display_count = 2;
	n = leia_lnx_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 2);
	for (uint32_t i = 0; i < n; i++) {
		CHECK(c[i].confidence == XRT_DISPLAY_CLAIM_EDID && c[i].serial[0] == '\0');
		CHECK(b[i].sr_display_id == 0 && b[i].connector[0] == '\0');
	}

	// SR lists the twins but the EDID scan does not: still ambiguous.
	in.panel_count = 0;
	n = leia_lnx_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 2 && c[0].serial[0] == '\0' && b[0].sr_display_id == 0 && b[1].sr_display_id == 0);

	// One twin placed by RandR: it pairs by origin, the other is then
	// the only candidate left for the other monitor — unambiguous.
	in.panel_count = 2;
	in.sr_display_count = -1;
	p2[1].has_position = true;
	p2[1].left = 0;
	p2[1].top = 0;
	n = leia_lnx_compute_claims(d, 2, &in, c, b, 4);
	CHECK(n == 2);
	CHECK(c[0].monitor_id == 0xB && strcmp(b[0].connector, "HDMI-A-1") == 0);
	CHECK(c[1].monitor_id == 0xC && strcmp(b[1].connector, "DP-1") == 0);
}

/*! A future runtime appended fields: walk with ITS stride, not ours. */
struct bigger_descriptor
{
	struct xrt_display_descriptor base;
	uint32_t appended[6];
};

static void
test_claims_stride(void)
{
	struct bigger_descriptor d[2];
	memset(d, 0, sizeof(d));
	d[0].base = desc(0xA, LGD_ID, 0x0601, 2560, 1600, 0, 0);
	d[1].base = desc(0xB, ACR_ID, DS1_PROD, 3840, 2160, 2560, 0);
	d[0].base.struct_size = d[1].base.struct_size = sizeof(struct bigger_descriptor);
	struct leia_lnx_edid_panel p = panel("HDMI-A-1", 7, true, 2560, 0);
	struct leia_lnx_claim_inputs in = {
	    .panels = &p, .panel_count = 1, .sr_display_count = -1, .supported_apis = XRT_DP_API_BIT_VK};
	struct xrt_display_claim c[2];
	const uint32_t n = leia_lnx_compute_claims(&d[0].base, 2, &in, c, NULL, 2);
	CHECK(n == 1 && c[0].monitor_id == 0xB);
}

static void
test_binding_table(void)
{
	struct leia_lnx_claim_binding b[2] = {
	    {.monitor_id = 1, .sr_display_id = 0x11, .connector = "HDMI-A-1"},
	    {.monitor_id = 2, .sr_display_id = 0, .connector = "DP-1"},
	};
	struct leia_lnx_claim_binding out;
	leia_lnx_claims_store(b, 2);
	CHECK(leia_lnx_claims_lookup(1, &out) && out.sr_display_id == 0x11);
	CHECK(leia_lnx_claims_lookup(2, &out) && strcmp(out.connector, "DP-1") == 0);
	CHECK(!leia_lnx_claims_lookup(3, &out));
	leia_lnx_claims_store(NULL, 0);
	CHECK(!leia_lnx_claims_lookup(1, &out));
}

int
main(void)
{
	test_edid_parse();
	test_pnp();
	test_claims_edid_ids();
	test_claims_no_ids();
	test_claims_new_api();
	test_claims_ambiguous_twins();
	test_claims_stride();
	test_binding_table();
	if (g_failures != 0) {
		fprintf(stderr, "test_display_claims_linux: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	printf("test_display_claims_linux: all checks passed\n");
	return EXIT_SUCCESS;
}
