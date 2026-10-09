// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Per-monitor display claims for the Windows arm — see
 *         leia_display_claims_win.h.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_claims_win.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

uint16_t
leia_win_pnp_to_manufacturer_id(const char *pnp)
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
	int32_t left, top;
	bool has_pos;
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
	if (DESC_HAS(d, screen_top)) {
		v.left = d->screen_left;
		v.top = d->screen_top;
		v.has_pos = true;
	}
	return v;
}

//! match_sr(): identical twins this descriptor cannot be told apart from.
#define MATCH_AMBIGUOUS (-2)

static int32_t
match_sr(const struct desc_view *v, const struct leia_win_claim_inputs *in, const bool *sr_taken)
{
	if (in->sr_display_count <= 0) {
		return -1;
	}
	// Candidates: free SR displays with the descriptor's EDID ids that are
	// not known to sit somewhere ELSE on the desktop. One at the descriptor's
	// origin wins outright (identical twins: the origin decides). Two or more
	// candidates with no origin to decide between them are AMBIGUOUS, not
	// "the first one": pairing by list order would swap serial / displayId
	// between the two monitors.
	int32_t cand = -1;
	uint32_t cand_count = 0;
	for (uint32_t j = 0; j < (uint32_t)in->sr_display_count; j++) {
		const struct leia_win_sr_display *s = &in->sr_displays[j];
		if (sr_taken[j] || s->manufacturer_id != v->man || s->product_id != v->prod) {
			continue;
		}
		if (s->location_is_desktop_global && v->has_pos) {
			if (s->left == v->left && s->top == v->top) {
				return (int32_t)j;
			}
			continue; // placed elsewhere: not this monitor
		}
		cand = (int32_t)j;
		cand_count++;
	}
	if (cand_count > 1) {
		return MATCH_AMBIGUOUS;
	}
	return cand;
}

//! join_target(): SR's monitor list says this monitor is not an SR display.
#define JOIN_NOT_SR (-3)

/*!
 * SR's own join for one descriptor: its row in SR's monitor list (by
 * HMONITOR, else by desktop origin), mapped to an index into SR's display
 * list. -1 = no usable row (absent, contradicting EDID ids, or an SR display
 * id the display list does not carry): the heuristic decides. JOIN_NOT_SR =
 * SR lists the monitor as not an SR display.
 */
static int32_t
join_target(const struct desc_view *v, const struct leia_win_claim_inputs *in)
{
	if (in->sr_display_count < 0 || in->sr_monitors == NULL || in->sr_monitor_count <= 0 || !v->has_pos) {
		return -1;
	}
	const uint64_t hmon = in->monitor_at != NULL ? in->monitor_at(v->left, v->top) : 0;
	const struct leia_win_sr_monitor *row = NULL;
	for (uint32_t k = 0; k < (uint32_t)in->sr_monitor_count; k++) {
		const struct leia_win_sr_monitor *m = &in->sr_monitors[k];
		if (hmon != 0 ? m->hmonitor == hmon : (m->x == v->left && m->y == v->top)) {
			row = m;
			break;
		}
	}
	if (row == NULL) {
		return -1;
	}
	// Both sides know the panel's EDID ids and they differ: the topology
	// changed between the runtime's enumeration and SR's. Not this monitor.
	if (v->man != 0 && row->edid_manufacturer != 0 &&
	    (row->edid_manufacturer != v->man || row->edid_product != v->prod)) {
		return -1;
	}
	if (row->sr_display_id == 0) {
		return JOIN_NOT_SR;
	}
	for (uint32_t j = 0; j < (uint32_t)in->sr_display_count; j++) {
		if (in->sr_displays[j].display_id == row->sr_display_id) {
			return (int32_t)j;
		}
	}
	return -1;
}

uint32_t
leia_win_compute_claims(const struct xrt_display_descriptor *displays,
                        uint32_t display_count,
                        const struct leia_win_claim_inputs *in,
                        struct xrt_display_claim *out_claims,
                        struct leia_win_claim_binding *out_bindings,
                        uint32_t max_claims)
{
	if (displays == NULL || display_count == 0 || in == NULL || out_claims == NULL || max_claims == 0) {
		return 0;
	}

	bool sr_taken[LEIA_WIN_SR_MAX_DISPLAYS] = {0};
	struct leia_win_claim_inputs clamped = *in;
	if (clamped.sr_display_count > LEIA_WIN_SR_MAX_DISPLAYS) {
		clamped.sr_display_count = LEIA_WIN_SR_MAX_DISPLAYS;
	}
	if (clamped.sr_monitor_count > LEIA_WIN_SR_MAX_MONITORS) {
		clamped.sr_monitor_count = LEIA_WIN_SR_MAX_MONITORS;
	}
	in = &clamped;
	const bool sr_available = in->sr_display_count >= 0;

	// SR's own join first: each SR display it ties to a monitor belongs to
	// the first descriptor that joins it, and is taken before the heuristic
	// runs. No join (list unavailable / empty) leaves everything untouched.
	int32_t join_owner[LEIA_WIN_SR_MAX_DISPLAYS];
	for (uint32_t j = 0; j < LEIA_WIN_SR_MAX_DISPLAYS; j++) {
		join_owner[j] = -1;
	}
	for (uint32_t i = 0; i < display_count; i++) {
		const struct desc_view v = desc_read(desc_at(displays, i));
		const int32_t jt = join_target(&v, in);
		if (jt >= 0 && join_owner[jt] < 0) {
			join_owner[jt] = (int32_t)i;
			sr_taken[jt] = true;
		}
	}

	uint32_t n = 0;
	for (uint32_t i = 0; i < display_count && n < max_claims; i++) {
		const struct desc_view v = desc_read(desc_at(displays, i));
		const bool in_table = in->table_contains != NULL && in->table_contains(v.man, v.prod);

		struct xrt_display_claim *c = &out_claims[n];
		struct leia_win_claim_binding b;
		memset(c, 0, sizeof(*c));
		memset(&b, 0, sizeof(b));
		c->monitor_id = v.monitor_id;
		c->supported_apis = in->supported_apis;
		c->confidence = (uint32_t)XRT_DISPLAY_CLAIM_EDID;
		b.monitor_id = v.monitor_id;

		if (!sr_available) {
			// Older SR: the frozen table is the only per-monitor evidence.
			if (!in_table) {
				continue;
			}
			if (in->legacy_table_verified) {
				c->confidence = (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED;
			}
		} else {
			const int32_t jt = join_target(&v, in);
			int32_t sj;
			if (jt >= 0 && join_owner[jt] == (int32_t)i) {
				sj = jt; // SR's own join: bound, no heuristic
				b.sr_joined = true;
			} else if (jt == JOIN_NOT_SR) {
				sj = -1; // SR says it is not an SR display: table hit only
			} else {
				sj = match_sr(&v, in, sr_taken);
			}
			if (sj == MATCH_AMBIGUOUS) {
				// One of several identical Leia panels, but which one is
				// unknown: EDID confidence, no serial, no displayId.
			} else if (sj >= 0) {
				const struct leia_win_sr_display *s = &in->sr_displays[sj];
				sr_taken[sj] = true;
				if (s->fpc_verified) {
					c->confidence = (uint32_t)XRT_DISPLAY_CLAIM_VERIFIED;
					snprintf(c->serial, sizeof(c->serial), "%s", s->serial);
				}
				b.sr_display_id = s->display_id;
				b.hmonitor = s->hmonitor;
				snprintf(b.device_name, sizeof(b.device_name), "%s", s->device_name);
				b.width_mm = s->width_mm;
				b.height_mm = s->height_mm;
				b.native_w = s->native_w;
				b.native_h = s->native_h;
			} else if (!in_table) {
				continue; // neither SR nor the table knows it
			}
			// Table hit that SR does not list: a Leia panel SR has no
			// calibration for (or the SR list is stale) — EDID confidence.
		}

		if (out_bindings != NULL) {
			out_bindings[n] = b;
		}
		n++;
	}
	return n;
}


/*
 *
 * Plug-in-private monitor table.
 *
 */

#define LEIA_WIN_MAX_BINDINGS 16

#ifdef _WIN32
static SRWLOCK g_bind_lock = SRWLOCK_INIT;
#define BIND_LOCK() AcquireSRWLockExclusive(&g_bind_lock)
#define BIND_UNLOCK() ReleaseSRWLockExclusive(&g_bind_lock)
#else
static pthread_mutex_t g_bind_lock = PTHREAD_MUTEX_INITIALIZER;
#define BIND_LOCK() pthread_mutex_lock(&g_bind_lock)
#define BIND_UNLOCK() pthread_mutex_unlock(&g_bind_lock)
#endif

static struct leia_win_claim_binding g_bindings[LEIA_WIN_MAX_BINDINGS];
static uint32_t g_binding_count;

void
leia_win_claims_store(const struct leia_win_claim_binding *bindings, uint32_t count)
{
	BIND_LOCK();
	if (count > LEIA_WIN_MAX_BINDINGS) {
		count = LEIA_WIN_MAX_BINDINGS;
	}
	if (bindings != NULL && count > 0) {
		memcpy(g_bindings, bindings, count * sizeof(bindings[0]));
	}
	g_binding_count = bindings != NULL ? count : 0;
	BIND_UNLOCK();
}

bool
leia_win_claims_lookup(uint64_t monitor_id, struct leia_win_claim_binding *out)
{
	bool found = false;
	BIND_LOCK();
	for (uint32_t i = 0; i < g_binding_count; i++) {
		if (g_bindings[i].monitor_id == monitor_id) {
			if (out != NULL) {
				*out = g_bindings[i];
			}
			found = true;
			break;
		}
	}
	BIND_UNLOCK();
	return found;
}


/*
 *
 * SR side stubs — the real implementation lives in leia_sr_v2_common.cpp
 * when the SDK headers declare srEnumerateDisplays.
 *
 */

uint64_t
leia_win_monitor_at(int32_t left, int32_t top)
{
#ifdef _WIN32
	POINT pt;
	pt.x = left;
	pt.y = top;
	return (uint64_t)(uintptr_t)MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
#else
	(void)left;
	(void)top;
	return 0;
#endif
}

#ifndef DXR_LEIA_HAS_SR_MONITOR_ENUM
int32_t
leia_win_sr_enumerate_monitors(struct leia_win_sr_monitor *out, uint32_t cap)
{
	(void)out;
	(void)cap;
	return -1;
}
#endif

#ifndef DXR_LEIA_HAS_SR_DISPLAY_ENUM
int32_t
leia_win_sr_enumerate_displays(struct leia_win_sr_display *out, uint32_t cap)
{
	(void)out;
	(void)cap;
	return -1;
}

void
leia_win_sr_enumerate_shutdown(void)
{
}
#endif
