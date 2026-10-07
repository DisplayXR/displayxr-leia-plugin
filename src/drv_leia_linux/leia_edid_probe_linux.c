// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux panel detection over /sys/class/drm EDID blobs.
 *
 * The kernel exposes each DRM connector as /sys/class/drm/<card>-<conn>/edid;
 * the file is non-empty exactly when a monitor is connected. EDID bytes 8-11
 * carry the manufacturer + product IDs; both are matched as little-endian
 * 16-bit words, the same convention the frozen table uses on Windows
 * (e.g. Dell "DEL" = EDID bytes 10 AC -> 0xAC10 = 44048).
 *
 * No libdrm dependency — plain sysfs reads, usable from the plug-in probe
 * before any Vulkan/SDK state exists.
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#include "leia_edid_probe_linux.h"

#include "../drv_leia/leia_edid_table.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>

static const uint8_t EDID_MAGIC[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

bool
leia_lnx_edid_table_contains(uint16_t manufacturer_id, uint16_t product_id)
{
	for (size_t i = 0; i < LEIA_EDID_TABLE_LEN; i++) {
		if (leia_edid_table[i][0] == manufacturer_id && leia_edid_table[i][1] == product_id) {
			return true;
		}
	}
	return false;
}

bool
leia_lnx_edid_parse(const uint8_t *edid, size_t len, struct leia_lnx_edid_panel *out)
{
	if (edid == NULL || out == NULL || len < 128 || memcmp(edid, EDID_MAGIC, sizeof(EDID_MAGIC)) != 0) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->manufacturer_id = (uint16_t)(edid[8] | (edid[9] << 8));
	out->product_id = (uint16_t)(edid[10] | (edid[11] << 8));
	out->serial =
	    (uint32_t)edid[12] | ((uint32_t)edid[13] << 8) | ((uint32_t)edid[14] << 16) | ((uint32_t)edid[15] << 24);

	// Detailed timing #1 at byte 54 (pixel clock 0 = a display descriptor,
	// not a timing — every field below would be garbage).
	const uint16_t pixel_clock = (uint16_t)(edid[54] | (edid[55] << 8));
	if (pixel_clock != 0) {
		out->native_w = (uint32_t)edid[56] | ((uint32_t)(edid[58] & 0xF0) << 4);
		out->native_h = (uint32_t)edid[59] | ((uint32_t)(edid[61] & 0xF0) << 4);
		const uint32_t w_mm = (uint32_t)edid[66] | ((uint32_t)(edid[68] & 0xF0) << 4);
		const uint32_t h_mm = (uint32_t)edid[67] | ((uint32_t)(edid[68] & 0x0F) << 8);
		if (w_mm > 50 && h_mm > 50) {
			out->width_mm = w_mm;
			out->height_mm = h_mm;
		}
	}
	if (out->width_mm == 0 && edid[21] > 5 && edid[22] > 5) {
		out->width_mm = (uint32_t)edid[21] * 10u;
		out->height_mm = (uint32_t)edid[22] * 10u;
	}
	return true;
}

static bool
connector_is_internal(const char *name)
{
	return strstr(name, "eDP") != NULL || strstr(name, "LVDS") != NULL || strstr(name, "DSI") != NULL;
}

static int
panel_cmp_connector(const void *a, const void *b)
{
	const struct leia_lnx_edid_panel *pa = a, *pb = b;
	return strcmp(pa->connector, pb->connector);
}

static void
randr_fill_positions(struct leia_lnx_edid_panel *panels, uint32_t *count, uint32_t cap);

uint32_t
leia_lnx_edid_enumerate_panels(struct leia_lnx_edid_panel *out, uint32_t cap, bool with_positions)
{
	if (out == NULL || cap == 0) {
		return 0;
	}
	uint32_t n = 0;

	DIR *drm = opendir("/sys/class/drm");
	if (drm == NULL) {
		U_LOG_I("leia_lnx_edid: /sys/class/drm not available — no DRM probe");
	} else {
		struct dirent *entry;
		while (n < cap && (entry = readdir(drm)) != NULL) {
			// Connectors are card<N>-<TYPE>-<M>; skip cardN itself and misc nodes.
			const char *dash = strchr(entry->d_name, '-');
			if (strncmp(entry->d_name, "card", 4) != 0 || dash == NULL) {
				continue;
			}

			char path[512];
			snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", entry->d_name);
			FILE *f = fopen(path, "rb");
			if (f == NULL) {
				continue;
			}
			uint8_t edid[128];
			size_t len = fread(edid, 1, sizeof(edid), f);
			fclose(f);

			// Empty file = connector present but no monitor.
			struct leia_lnx_edid_panel p;
			if (!leia_lnx_edid_parse(edid, len, &p) ||
			    !leia_lnx_edid_table_contains(p.manufacturer_id, p.product_id)) {
				continue;
			}
			snprintf(p.connector, sizeof(p.connector), "%s", dash + 1);
			p.internal = connector_is_internal(p.connector);
			out[n++] = p;
		}
		closedir(drm);
	}

	qsort(out, n, sizeof(out[0]), panel_cmp_connector);

	if (with_positions) {
		randr_fill_positions(out, &n, cap);
	}

	for (uint32_t i = 0; i < n; i++) {
		U_LOG_I(
		    "leia_lnx_edid: Leia panel #%u on %s%s%s (manufacturer %u, product %u, serial %u, %ux%u px, "
		    "%ux%u mm)%s",
		    i, out[i].connector[0] ? out[i].connector : "(no DRM connector)",
		    out[i].randr_output[0] ? " / RandR " : "", out[i].randr_output, out[i].manufacturer_id,
		    out[i].product_id, out[i].serial, out[i].native_w, out[i].native_h, out[i].width_mm,
		    out[i].height_mm, out[i].has_position ? "" : " — no desktop position");
	}
	return n;
}

bool
leia_lnx_edid_panel_present(uint16_t *out_manufacturer_id, uint16_t *out_product_id)
{
	// Single-panel accessor on top of the list: the first panel in
	// connector order (behaviour unchanged for the one-panel case).
	struct leia_lnx_edid_panel panels[LEIA_LNX_EDID_MAX_PANELS];
	const uint32_t n = leia_lnx_edid_enumerate_panels(panels, LEIA_LNX_EDID_MAX_PANELS, false);
	if (n == 0) {
		return false;
	}
	if (out_manufacturer_id != NULL) {
		*out_manufacturer_id = panels[0].manufacturer_id;
	}
	if (out_product_id != NULL) {
		*out_product_id = panels[0].product_id;
	}
	return true;
}

/*! Parse physical size (metres) out of one 128-byte EDID block. Preferred:
 * detailed timing descriptor #1 image size in mm (bytes 66,67 low 8 bits;
 * byte 68 = H-upper-nibble<<4 | V-upper-nibble). Fallback: bytes 21/22, cm.
 * Returns false when both encodings are absent/implausible. */
static bool
edid_physical_size_m(const uint8_t *edid, float *out_w_m, float *out_h_m)
{
	// Detailed timing #1 at byte 54; a pixel-clock of 0 means it is a
	// display descriptor, not a timing — mm fields would be garbage.
	const uint16_t pixel_clock = (uint16_t)(edid[54] | (edid[55] << 8));
	if (pixel_clock != 0) {
		const uint32_t w_mm = (uint32_t)edid[66] | ((uint32_t)(edid[68] & 0xF0) << 4);
		const uint32_t h_mm = (uint32_t)edid[67] | ((uint32_t)(edid[68] & 0x0F) << 8);
		if (w_mm > 50 && h_mm > 50) {
			*out_w_m = (float)w_mm / 1000.0f;
			*out_h_m = (float)h_mm / 1000.0f;
			return true;
		}
	}
	// Basic max image size, cm (0 = undefined/aspect-ratio encoding).
	if (edid[21] > 5 && edid[22] > 5) {
		*out_w_m = (float)edid[21] / 100.0f;
		*out_h_m = (float)edid[22] / 100.0f;
		return true;
	}
	return false;
}

bool
leia_lnx_edid_panel_physical_size(float *out_width_m, float *out_height_m)
{
	DIR *drm = opendir("/sys/class/drm");
	if (drm == NULL) {
		return false;
	}

	float table_w = 0, table_h = 0;     // frozen-table-matched panel (preferred)
	float ext_w = 0, ext_h = 0;         // first connected external connector
	struct dirent *entry;
	while ((entry = readdir(drm)) != NULL) {
		if (strncmp(entry->d_name, "card", 4) != 0 || strchr(entry->d_name, '-') == NULL) {
			continue;
		}
		char path[512];
		snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", entry->d_name);
		FILE *f = fopen(path, "rb");
		if (f == NULL) {
			continue;
		}
		uint8_t edid[128];
		size_t n = fread(edid, 1, sizeof(edid), f);
		fclose(f);
		if (n < sizeof(edid) || memcmp(edid, EDID_MAGIC, sizeof(EDID_MAGIC)) != 0) {
			continue;
		}

		float w = 0, h = 0;
		if (!edid_physical_size_m(edid, &w, &h) || w < 0.05f || h < 0.05f) {
			continue;
		}

		const uint16_t man = (uint16_t)(edid[8] | (edid[9] << 8));
		const uint16_t prod = (uint16_t)(edid[10] | (edid[11] << 8));
		bool in_table = false;
		for (size_t i = 0; i < LEIA_EDID_TABLE_LEN; i++) {
			if (leia_edid_table[i][0] == man && leia_edid_table[i][1] == prod) {
				in_table = true;
				break;
			}
		}
		// Internal-panel connectors (eDP/LVDS/DSI) are never the 3D display.
		const bool internal = strstr(entry->d_name, "eDP") != NULL ||
		                      strstr(entry->d_name, "LVDS") != NULL ||
		                      strstr(entry->d_name, "DSI") != NULL;

		if (in_table && table_w == 0) {
			table_w = w;
			table_h = h;
			U_LOG_I("leia_lnx_edid: table-matched panel %s physical size %.3fx%.3f m",
			        entry->d_name, w, h);
		} else if (!internal && ext_w == 0) {
			ext_w = w;
			ext_h = h;
			U_LOG_I("leia_lnx_edid: external connector %s physical size %.3fx%.3f m",
			        entry->d_name, w, h);
		}
	}
	closedir(drm);

	const float w = table_w > 0 ? table_w : ext_w;
	const float h = table_h > 0 ? table_h : ext_h;
	if (w <= 0 || h <= 0) {
		return false;
	}
	if (out_width_m != NULL) {
		*out_width_m = w;
	}
	if (out_height_m != NULL) {
		*out_height_m = h;
	}
	return true;
}

/*! Read one RandR output's EDID property (first 128-byte block). */
static bool
randr_output_edid(xcb_connection_t *conn,
                  xcb_randr_output_t output,
                  xcb_atom_t edid_atom,
                  struct leia_lnx_edid_panel *out)
{
	// 32 longs = 128 bytes = one EDID block.
	xcb_randr_get_output_property_cookie_t cookie =
	    xcb_randr_get_output_property(conn, output, edid_atom, XCB_ATOM_NONE /* AnyPropertyType */, 0, 32, 0, 0);
	xcb_randr_get_output_property_reply_t *prop = xcb_randr_get_output_property_reply(conn, cookie, NULL);
	if (prop == NULL) {
		return false;
	}
	bool ok = false;
	if (prop->format == 8) {
		const uint8_t *edid = xcb_randr_get_output_property_data(prop);
		const int len = xcb_randr_get_output_property_data_length(prop);
		ok = len > 0 && leia_lnx_edid_parse(edid, (size_t)len, out);
	}
	free(prop);
	return ok;
}

/*!
 * Join the sysfs panel list to the X server's RandR outputs by EDID identity
 * (manufacturer, product, serial) — never by connector name, which drifts
 * between DRM and RandR ("HDMI-A-1" vs "HDMI-1" vs NVIDIA "HDMI-0"). Two
 * panels with byte-identical identity are paired in connector order.
 */
static void
randr_fill_positions(struct leia_lnx_edid_panel *panels, uint32_t *count, uint32_t cap)
{
	int screen_num = 0;
	xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
	if (conn == NULL || xcb_connection_has_error(conn)) {
		U_LOG_I("leia_lnx_edid: no X server — panel desktop position unresolved");
		if (conn != NULL) {
			xcb_disconnect(conn);
		}
		return;
	}

	xcb_randr_get_screen_resources_current_reply_t *res = NULL;

	const xcb_query_extension_reply_t *ext = xcb_get_extension_data(conn, &xcb_randr_id);
	if (ext == NULL || !ext->present) {
		U_LOG_I("leia_lnx_edid: X server lacks RandR — panel desktop position unresolved");
		goto done;
	}

	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
	for (int i = 0; i < screen_num && it.rem > 0; i++) {
		xcb_screen_next(&it);
	}
	if (it.data == NULL) {
		goto done;
	}

	res = xcb_randr_get_screen_resources_current_reply(
	    conn, xcb_randr_get_screen_resources_current(conn, it.data->root), NULL);
	if (res == NULL) {
		goto done;
	}

	xcb_intern_atom_reply_t *atom_reply =
	    xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 1 /* only_if_exists */, 4, "EDID"), NULL);
	const xcb_atom_t edid_atom = atom_reply != NULL ? atom_reply->atom : XCB_ATOM_NONE;
	free(atom_reply);
	if (edid_atom == XCB_ATOM_NONE) {
		goto done;
	}

	xcb_randr_output_t *outputs = xcb_randr_get_screen_resources_current_outputs(res);
	const int n_outputs = xcb_randr_get_screen_resources_current_outputs_length(res);
	for (int i = 0; i < n_outputs; i++) {
		xcb_randr_get_output_info_reply_t *oi = xcb_randr_get_output_info_reply(
		    conn, xcb_randr_get_output_info(conn, outputs[i], res->config_timestamp), NULL);
		if (oi == NULL) {
			continue;
		}
		struct leia_lnx_edid_panel rp;
		// Only connected outputs driven by a CRTC have a desktop position.
		if (oi->connection != XCB_RANDR_CONNECTION_CONNECTED || oi->crtc == XCB_NONE ||
		    !randr_output_edid(conn, outputs[i], edid_atom, &rp) ||
		    !leia_lnx_edid_table_contains(rp.manufacturer_id, rp.product_id)) {
			free(oi);
			continue;
		}
		xcb_randr_get_crtc_info_reply_t *ci = xcb_randr_get_crtc_info_reply(
		    conn, xcb_randr_get_crtc_info(conn, oi->crtc, res->config_timestamp), NULL);
		if (ci == NULL) {
			free(oi);
			continue;
		}

		// Join: first not-yet-positioned panel with the same identity.
		struct leia_lnx_edid_panel *p = NULL;
		for (uint32_t k = 0; k < *count; k++) {
			if (!panels[k].has_position && panels[k].manufacturer_id == rp.manufacturer_id &&
			    panels[k].product_id == rp.product_id && panels[k].serial == rp.serial) {
				p = &panels[k];
				break;
			}
		}
		if (p == NULL && *count < cap) {
			// Seen by RandR only (no sysfs entry): keep it, so the
			// single-panel position accessor behaves as before.
			p = &panels[(*count)++];
			*p = rp;
		}
		if (p != NULL) {
			const int name_len = xcb_randr_get_output_info_name_length(oi);
			snprintf(p->randr_output, sizeof(p->randr_output), "%.*s", name_len,
			         (const char *)xcb_randr_get_output_info_name(oi));
			p->has_position = true;
			p->left = ci->x;
			p->top = ci->y;
			p->crtc_w = ci->width;
			p->crtc_h = ci->height;
			U_LOG_I("leia_lnx_edid: panel %s on RandR output %s at (%d, %d) %ux%u",
			        p->connector[0] ? p->connector : "(no DRM connector)", p->randr_output, (int)ci->x,
			        (int)ci->y, (unsigned)ci->width, (unsigned)ci->height);
		}
		free(ci);
		free(oi);
	}

done:
	free(res);
	xcb_disconnect(conn);
}

bool
leia_lnx_edid_panel_desktop_position(int32_t *out_left, int32_t *out_top)
{
	// Single-panel accessor: the first panel (connector order) that sits on
	// an active RandR output — what the old first-match RandR scan returned
	// on a one-panel box.
	struct leia_lnx_edid_panel panels[LEIA_LNX_EDID_MAX_PANELS];
	const uint32_t n = leia_lnx_edid_enumerate_panels(panels, LEIA_LNX_EDID_MAX_PANELS, true);
	for (uint32_t i = 0; i < n; i++) {
		if (panels[i].has_position) {
			if (out_left != NULL) {
				*out_left = panels[i].left;
			}
			if (out_top != NULL) {
				*out_top = panels[i].top;
			}
			return true;
		}
	}
	U_LOG_I("leia_lnx_edid: no active RandR output matches the Leia panel table");
	return false;
}

/*
 * Shared panel cache (connector, ids, px, mm, RandR position). Replaces the
 * two per-function "the panel" statics (plug-in get_display_info + DP
 * get_display_pixel_info) so each panel resolves its own position. Resolved on
 * first use; refreshed by probe_displays' short-TTL snapshot (every runtime
 * registry rebuild) and invalidated by SR display connect/topology events, so
 * a hot-plugged or moved panel is picked up — never re-scanned from the
 * per-frame position accessor itself.
 */
static pthread_mutex_t g_pos_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_pos_resolved = false;
static bool g_pos_stale = false;
static int64_t g_pos_resolved_ns = 0;
static uint32_t g_pos_count = 0;
static struct leia_lnx_edid_panel g_pos_panels[LEIA_LNX_EDID_MAX_PANELS];

//! Caller holds g_pos_lock. Rescan when never resolved, invalidated, or older than @p max_age_ns.
static void
pos_resolve_locked(uint64_t max_age_ns)
{
	const int64_t now = os_monotonic_get_ns();
	const bool expired = max_age_ns != UINT64_MAX && g_pos_resolved && now - g_pos_resolved_ns >= 0 &&
	                     (uint64_t)(now - g_pos_resolved_ns) > max_age_ns;
	if (!g_pos_resolved || g_pos_stale || expired) {
		g_pos_count = leia_lnx_edid_enumerate_panels(g_pos_panels, LEIA_LNX_EDID_MAX_PANELS, true);
		g_pos_resolved = true;
		g_pos_stale = false;
		g_pos_resolved_ns = now;
	}
}

uint32_t
leia_lnx_edid_panels_snapshot(struct leia_lnx_edid_panel *out, uint32_t cap, uint64_t max_age_ns)
{
	pthread_mutex_lock(&g_pos_lock);
	pos_resolve_locked(max_age_ns);
	const uint32_t n = g_pos_count < cap ? g_pos_count : cap;
	if (out != NULL && n > 0) {
		memcpy(out, g_pos_panels, n * sizeof(out[0]));
	}
	pthread_mutex_unlock(&g_pos_lock);
	return out != NULL ? n : 0;
}

void
leia_lnx_edid_cache_invalidate(void)
{
	pthread_mutex_lock(&g_pos_lock);
	g_pos_stale = true;
	pthread_mutex_unlock(&g_pos_lock);
}

bool
leia_lnx_edid_panel_desktop_position_cached(const char *connector, int32_t *out_left, int32_t *out_top)
{
	pthread_mutex_lock(&g_pos_lock);
	pos_resolve_locked(UINT64_MAX);
	bool found = false;
	for (uint32_t i = 0; i < g_pos_count && !found; i++) {
		const struct leia_lnx_edid_panel *p = &g_pos_panels[i];
		// NULL = the first panel; otherwise the named DRM connector.
		const bool key_match = connector == NULL ? true : strcmp(p->connector, connector) == 0;
		if (!key_match) {
			continue;
		}
		if (connector == NULL && !p->has_position) {
			continue; // first panel *with* a desktop position, as before
		}
		if (p->has_position) {
			if (out_left != NULL) {
				*out_left = p->left;
			}
			if (out_top != NULL) {
				*out_top = p->top;
			}
			found = true;
		}
		break;
	}
	pthread_mutex_unlock(&g_pos_lock);
	return found;
}
