// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Per-monitor display claims for the Linux arm — see
 *         leia_display_claims_linux.h.
 *
 * Matching rules (multi-screen plan M0):
 *
 *  Runtime monitor -> Leia panel (EDID scan):
 *   - descriptor carries EDID ids: the panels with the same (manufacturer,
 *     product). Two identical panels are told apart by the RandR origin
 *     (== descriptor screen_left/top); failing that, connector order.
 *   - descriptor carries no EDID ids (the runtime's XWayland path, #251 /
 *     runtime#1579): a panel whose RandR origin AND size equal the
 *     descriptor's; else a size-only match (native or CRTC px), accepted only
 *     when it is unambiguous on BOTH sides — exactly one panel of that size
 *     and exactly one runtime monitor of that size — so an eDP that happens
 *     to share the panel's resolution is never claimed.
 *
 *  Leia panel -> SR display (new API only): same DRM connector; else EDID
 *  (manufacturer, product, serial); else (manufacturer, product) when that is
 *  unique in SR's list. A runtime monitor that matches no table panel but
 *  whose EDID ids SR lists is still claimed — SR's product-code registry is
 *  authoritative and the frozen table drifts (the Windows arm's "table miss"
 *  deferral, here with a real per-monitor answer instead of "the primary").
 *
 *  Confidence:
 *   - new API: VERIFIED when SR reports the display FPC_VERIFIED (serial =
 *     FPC serial), else EDID;
 *   - SR 1.38: VERIFIED only when exactly one Leia panel is connected AND a
 *     live SR context's lens returned an FPC serial (the serial is
 *     system-global, so with two panels there is no way to say whose it is);
 *     else EDID.
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#include "leia_display_claims_linux.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

uint16_t
leia_lnx_pnp_to_manufacturer_id(const char *pnp)
{
	if (pnp == NULL) {
		return 0;
	}
	uint16_t be = 0;
	for (int i = 0; i < 3; i++) {
		const char c = pnp[i];
		if (c < 'A' || c > 'Z') {
			return 0;
		}
		be = (uint16_t)((be << 5) | (uint16_t)(c - 'A' + 1));
	}
	// EDID stores it big-endian in bytes 8-9; the table reads those two
	// bytes as a little-endian word.
	return (uint16_t)((be >> 8) | (be << 8));
}

/*! The fields of a runtime descriptor this file reads, clamped to the
 *  runtime's struct_size (zero when the runtime's struct is shorter). */
struct desc_view
{
	uint64_t monitor_id;
	uint16_t man, prod;
	uint32_t px_w, px_h;
	int32_t left, top;
	bool has_ids;
};

#define DESC_HAS(d, field)                                                                                             \
	((d)->struct_size >=                                                                                           \
	 offsetof(struct xrt_display_descriptor, field) + sizeof(((struct xrt_display_descriptor *)0)->field))

static const struct xrt_display_descriptor *
desc_at(const struct xrt_display_descriptor *displays, uint32_t i)
{
	// Walk with the runtime's stride: a newer runtime appends fields, and
	// indexing with OUR sizeof would land mid-struct from entry 1 on.
	size_t stride = displays[0].struct_size;
	if (stride < offsetof(struct xrt_display_descriptor, edid_manufacturer)) {
		stride = sizeof(struct xrt_display_descriptor); // pre-struct_size caller
	}
	return (const struct xrt_display_descriptor *)((const uint8_t *)displays + (size_t)i * stride);
}

static struct desc_view
desc_read(const struct xrt_display_descriptor *d)
{
	struct desc_view v = {0};
	if (DESC_HAS(d, monitor_id)) {
		v.monitor_id = d->monitor_id;
	}
	if (DESC_HAS(d, edid_product)) {
		v.man = d->edid_manufacturer;
		v.prod = d->edid_product;
	}
	if (DESC_HAS(d, pixel_height)) {
		v.px_w = d->pixel_width;
		v.px_h = d->pixel_height;
	}
	if (DESC_HAS(d, screen_top)) {
		v.left = d->screen_left;
		v.top = d->screen_top;
	}
	v.has_ids = v.man != 0 || v.prod != 0;
	return v;
}

static bool
panel_size_is(const struct leia_lnx_edid_panel *p, uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0) {
		return false;
	}
	return (p->native_w == w && p->native_h == h) || (p->has_position && p->crtc_w == w && p->crtc_h == h);
}

//! match_panel(): identical twins this descriptor cannot be told apart from.
#define MATCH_AMBIGUOUS (-2)

static int32_t
match_panel(const struct desc_view *v,
            const struct xrt_display_descriptor *displays,
            uint32_t display_count,
            const struct leia_lnx_claim_inputs *in,
            const bool *panel_taken)
{
	if (v->has_ids) {
		// Candidates: free panels with the descriptor's EDID ids that are not
		// known to sit somewhere ELSE on the desktop. A panel at the
		// descriptor's origin wins outright (identical twins: the origin
		// decides). Two or more candidates with no origin to decide between
		// them — twins without RandR positions, the XWayland norm — are
		// AMBIGUOUS: the panel list is in DRM connector order and the
		// runtime's descriptors in RandR order, so pairing them by order would
		// swap serial / sr_display_id / connector between the two monitors.
		int32_t cand = -1;
		uint32_t cand_count = 0;
		for (uint32_t k = 0; k < in->panel_count; k++) {
			const struct leia_lnx_edid_panel *p = &in->panels[k];
			if (panel_taken[k] || p->manufacturer_id != v->man || p->product_id != v->prod) {
				continue;
			}
			if (p->has_position) {
				if (p->left == v->left && p->top == v->top) {
					return (int32_t)k;
				}
				continue; // placed elsewhere: not this monitor
			}
			cand = (int32_t)k;
			cand_count++;
		}
		if (cand_count > 1) {
			return MATCH_AMBIGUOUS;
		}
		return cand;
	}

	// No EDID ids on the descriptor: origin + size first.
	for (uint32_t k = 0; k < in->panel_count; k++) {
		const struct leia_lnx_edid_panel *p = &in->panels[k];
		if (!panel_taken[k] && p->has_position && p->left == v->left && p->top == v->top &&
		    panel_size_is(p, v->px_w, v->px_h)) {
			return (int32_t)k;
		}
	}
	// Size only — and only when unambiguous on both sides.
	int32_t hit = -1;
	uint32_t panel_hits = 0;
	for (uint32_t k = 0; k < in->panel_count; k++) {
		if (panel_size_is(&in->panels[k], v->px_w, v->px_h)) {
			panel_hits++;
			hit = (int32_t)k;
		}
	}
	if (panel_hits != 1 || panel_taken[hit]) {
		return -1;
	}
	uint32_t monitor_hits = 0;
	for (uint32_t i = 0; i < display_count; i++) {
		const struct desc_view o = desc_read(desc_at(displays, i));
		if (o.px_w == v->px_w && o.px_h == v->px_h) {
			monitor_hits++;
		}
	}
	return monitor_hits == 1 ? hit : -1;
}

static int32_t
match_sr_for_panel(const struct leia_lnx_edid_panel *p, const struct leia_lnx_claim_inputs *in, const bool *sr_taken)
{
	if (in->sr_display_count <= 0) {
		return -1;
	}
	const uint32_t n = (uint32_t)in->sr_display_count;
	if (p->connector[0] != '\0') {
		for (uint32_t j = 0; j < n; j++) {
			if (!sr_taken[j] && strcmp(in->sr_displays[j].connector, p->connector) == 0) {
				return (int32_t)j;
			}
		}
	}
	for (uint32_t j = 0; j < n; j++) {
		const struct leia_lnx_sr_display *s = &in->sr_displays[j];
		if (!sr_taken[j] && s->manufacturer_id == p->manufacturer_id && s->product_id == p->product_id &&
		    s->edid_serial == p->serial) {
			return (int32_t)j;
		}
	}
	int32_t hit = -1;
	uint32_t hits = 0;
	for (uint32_t j = 0; j < n; j++) {
		const struct leia_lnx_sr_display *s = &in->sr_displays[j];
		if (s->manufacturer_id == p->manufacturer_id && s->product_id == p->product_id) {
			hits++;
			hit = (int32_t)j;
		}
	}
	return (hits == 1 && !sr_taken[hit]) ? hit : -1;
}

static int32_t
match_sr_for_ids(const struct desc_view *v, const struct leia_lnx_claim_inputs *in, const bool *sr_taken)
{
	if (in->sr_display_count <= 0 || !v->has_ids) {
		return -1;
	}
	// Same rule as match_panel: two free SR displays with these ids and
	// nothing to tell them apart is ambiguous, not "the first one".
	int32_t hit = -1;
	uint32_t hits = 0;
	for (uint32_t j = 0; j < (uint32_t)in->sr_display_count; j++) {
		const struct leia_lnx_sr_display *s = &in->sr_displays[j];
		if (!sr_taken[j] && s->manufacturer_id == v->man && s->product_id == v->prod) {
			hit = (int32_t)j;
			hits++;
		}
	}
	return hits > 1 ? MATCH_AMBIGUOUS : hit;
}

uint32_t
leia_lnx_compute_claims(const struct xrt_display_descriptor *displays,
                        uint32_t display_count,
                        const struct leia_lnx_claim_inputs *in,
                        struct xrt_display_claim *out_claims,
                        struct leia_lnx_claim_binding *out_bindings,
                        uint32_t max_claims)
{
	if (displays == NULL || display_count == 0 || in == NULL || out_claims == NULL || max_claims == 0) {
		return 0;
	}

	bool panel_taken[LEIA_LNX_EDID_MAX_PANELS] = {0};
	bool sr_taken[LEIA_LNX_SR_MAX_DISPLAYS] = {0};
	struct leia_lnx_claim_inputs clamped = *in;
	if (clamped.panel_count > LEIA_LNX_EDID_MAX_PANELS) {
		clamped.panel_count = LEIA_LNX_EDID_MAX_PANELS;
	}
	if (clamped.sr_display_count > LEIA_LNX_SR_MAX_DISPLAYS) {
		clamped.sr_display_count = LEIA_LNX_SR_MAX_DISPLAYS;
	}
	in = &clamped;

	const bool legacy_verified = in->sr_display_count < 0 && in->panel_count == 1 &&
	                             in->legacy_fpc_serial != NULL && in->legacy_fpc_serial[0] != '\0';

	uint32_t n = 0;
	for (uint32_t i = 0; i < display_count && n < max_claims; i++) {
		const struct desc_view v = desc_read(desc_at(displays, i));

		const int32_t pk = match_panel(&v, displays, display_count, in, panel_taken);
		const int32_t sj_ids = pk == -1 ? match_sr_for_ids(&v, in, sr_taken) : -1;
		if (pk == MATCH_AMBIGUOUS || sj_ids == MATCH_AMBIGUOUS) {
			// One of several identical Leia panels, but which one is unknown:
			// claim the monitor (it IS a Leia panel) at EDID confidence with
			// no serial, no SR display and no connector — never someone
			// else's identity.
			struct xrt_display_claim *c = &out_claims[n];
			memset(c, 0, sizeof(*c));
			c->monitor_id = v.monitor_id;
			c->supported_apis = in->supported_apis;
			c->confidence = (uint32_t)XRT_DISPLAY_CLAIM_EDID;
			if (out_bindings != NULL) {
				memset(&out_bindings[n], 0, sizeof(out_bindings[n]));
				out_bindings[n].monitor_id = v.monitor_id;
			}
			n++;
			continue;
		}
		int32_t sj = pk >= 0 ? match_sr_for_panel(&in->panels[pk], in, sr_taken) : sj_ids;
		if (pk < 0 && sj < 0) {
			continue;
		}

		struct xrt_display_claim *c = &out_claims[n];
		memset(c, 0, sizeof(*c));
		c->monitor_id = v.monitor_id;
		c->supported_apis = in->supported_apis;
		c->confidence = (uint32_t)XRT_DISPLAY_CLAIM_EDID;

		if (sj >= 0) {
			const struct leia_lnx_sr_display *s = &in->sr_displays[sj];
			sr_taken[sj] = true;
			if (s->fpc_verified) {
				c->confidence = (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED;
				snprintf(c->serial, sizeof(c->serial), "%s", s->serial);
			}
		} else if (legacy_verified) {
			c->confidence = (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED;
			snprintf(c->serial, sizeof(c->serial), "%s", in->legacy_fpc_serial);
		}

		if (out_bindings != NULL) {
			struct leia_lnx_claim_binding *b = &out_bindings[n];
			memset(b, 0, sizeof(*b));
			b->monitor_id = v.monitor_id;
			b->sr_display_id = sj >= 0 ? in->sr_displays[sj].display_id : 0;
			const char *conn = pk >= 0 ? in->panels[pk].connector : in->sr_displays[sj].connector;
			snprintf(b->connector, sizeof(b->connector), "%s", conn);
		}
		if (pk >= 0) {
			panel_taken[pk] = true;
		}
		n++;
	}
	return n;
}

bool
leia_lnx_fallback_claim(const struct xrt_display_descriptor *displays,
                        uint32_t display_count,
                        const struct leia_lnx_edid_panel *panels,
                        uint32_t panel_count,
                        uint32_t supported_apis,
                        struct xrt_display_claim *out_claim,
                        struct leia_lnx_claim_binding *out_binding)
{
	if (displays == NULL || display_count == 0 || out_claim == NULL) {
		return false;
	}
	if (panel_count > LEIA_LNX_EDID_MAX_PANELS) {
		panel_count = LEIA_LNX_EDID_MAX_PANELS;
	}

	int32_t pick = -1;
	const char *connector = "";
	// 1. The bound panel, by pixel size.
	for (uint32_t k = 0; k < panel_count && pick < 0 && panels != NULL; k++) {
		for (uint32_t i = 0; i < display_count; i++) {
			const struct desc_view v = desc_read(desc_at(displays, i));
			if (panel_size_is(&panels[k], v.px_w, v.px_h)) {
				pick = (int32_t)i;
				connector = panels[k].connector;
				break;
			}
		}
	}
	// 2. The primary monitor.
	for (uint32_t i = 0; i < display_count && pick < 0; i++) {
		const struct xrt_display_descriptor *d = desc_at(displays, i);
		if (DESC_HAS(d, flags) && (d->flags & 1u) != 0) {
			pick = (int32_t)i;
		}
	}
	// 3. The first monitor.
	if (pick < 0) {
		pick = 0;
	}

	const struct desc_view v = desc_read(desc_at(displays, (uint32_t)pick));
	memset(out_claim, 0, sizeof(*out_claim));
	out_claim->monitor_id = v.monitor_id;
	out_claim->confidence = (uint32_t)XRT_DISPLAY_CLAIM_EDID;
	out_claim->supported_apis = supported_apis;
	if (out_binding != NULL) {
		memset(out_binding, 0, sizeof(*out_binding));
		out_binding->monitor_id = v.monitor_id;
		snprintf(out_binding->connector, sizeof(out_binding->connector), "%s", connector);
	}
	return true;
}


/*
 *
 * Plug-in-private monitor table.
 *
 */

#define LEIA_LNX_MAX_BINDINGS 16

static pthread_mutex_t g_bind_lock = PTHREAD_MUTEX_INITIALIZER;
static struct leia_lnx_claim_binding g_bindings[LEIA_LNX_MAX_BINDINGS];
static uint32_t g_binding_count;

void
leia_lnx_claims_store(const struct leia_lnx_claim_binding *bindings, uint32_t count)
{
	pthread_mutex_lock(&g_bind_lock);
	if (count > LEIA_LNX_MAX_BINDINGS) {
		count = LEIA_LNX_MAX_BINDINGS;
	}
	if (bindings != NULL && count > 0) {
		memcpy(g_bindings, bindings, count * sizeof(bindings[0]));
	}
	g_binding_count = bindings != NULL ? count : 0;
	pthread_mutex_unlock(&g_bind_lock);
}

bool
leia_lnx_claims_lookup(uint64_t monitor_id, struct leia_lnx_claim_binding *out)
{
	bool found = false;
	pthread_mutex_lock(&g_bind_lock);
	for (uint32_t i = 0; i < g_binding_count; i++) {
		if (g_bindings[i].monitor_id == monitor_id) {
			if (out != NULL) {
				*out = g_bindings[i];
			}
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_bind_lock);
	return found;
}
