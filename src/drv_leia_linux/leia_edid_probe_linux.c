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

#include "util/u_logging.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>

static const uint8_t EDID_MAGIC[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

bool
leia_lnx_edid_panel_present(uint16_t *out_manufacturer_id, uint16_t *out_product_id)
{
	DIR *drm = opendir("/sys/class/drm");
	if (drm == NULL) {
		U_LOG_I("leia_lnx_edid: /sys/class/drm not available — no DRM probe");
		return false;
	}

	bool found = false;
	struct dirent *entry;
	while (!found && (entry = readdir(drm)) != NULL) {
		// Connectors are card<N>-<TYPE>-<M>; skip cardN itself and misc nodes.
		if (strncmp(entry->d_name, "card", 4) != 0 || strchr(entry->d_name, '-') == NULL) {
			continue;
		}

		char path[512];
		snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", entry->d_name);
		FILE *f = fopen(path, "rb");
		if (f == NULL) {
			continue;
		}
		uint8_t edid[16];
		size_t n = fread(edid, 1, sizeof(edid), f);
		fclose(f);

		// Empty file = connector present but no monitor.
		if (n < sizeof(edid) || memcmp(edid, EDID_MAGIC, sizeof(EDID_MAGIC)) != 0) {
			continue;
		}

		const uint16_t man = (uint16_t)(edid[8] | (edid[9] << 8));
		const uint16_t prod = (uint16_t)(edid[10] | (edid[11] << 8));

		for (size_t i = 0; i < LEIA_EDID_TABLE_LEN; i++) {
			if (leia_edid_table[i][0] == man && leia_edid_table[i][1] == prod) {
				U_LOG_I("leia_lnx_edid: known Leia panel on %s (manufacturer %u, product %u)",
				        entry->d_name, man, prod);
				if (out_manufacturer_id != NULL) {
					*out_manufacturer_id = man;
				}
				if (out_product_id != NULL) {
					*out_product_id = prod;
				}
				found = true;
				break;
			}
		}
	}
	closedir(drm);
	return found;
}

/*! Match one RandR output's EDID property against the frozen panel table. */
static bool
randr_output_is_leia_panel(xcb_connection_t *conn, xcb_randr_output_t output, xcb_atom_t edid_atom)
{
	// 32 longs = 128 bytes = one EDID block; only bytes 8-11 are needed.
	xcb_randr_get_output_property_cookie_t cookie =
	    xcb_randr_get_output_property(conn, output, edid_atom, XCB_ATOM_NONE /* AnyPropertyType */, 0, 32, 0, 0);
	xcb_randr_get_output_property_reply_t *prop = xcb_randr_get_output_property_reply(conn, cookie, NULL);
	if (prop == NULL) {
		return false;
	}

	bool match = false;
	const uint8_t *edid = xcb_randr_get_output_property_data(prop);
	const int len = xcb_randr_get_output_property_data_length(prop);
	if (prop->format == 8 && len >= 16 && memcmp(edid, EDID_MAGIC, sizeof(EDID_MAGIC)) == 0) {
		const uint16_t man = (uint16_t)(edid[8] | (edid[9] << 8));
		const uint16_t prod = (uint16_t)(edid[10] | (edid[11] << 8));
		for (size_t i = 0; i < LEIA_EDID_TABLE_LEN; i++) {
			if (leia_edid_table[i][0] == man && leia_edid_table[i][1] == prod) {
				match = true;
				break;
			}
		}
	}
	free(prop);
	return match;
}

bool
leia_lnx_edid_panel_desktop_position(int32_t *out_left, int32_t *out_top)
{
	int screen_num = 0;
	xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
	if (conn == NULL || xcb_connection_has_error(conn)) {
		U_LOG_I("leia_lnx_edid: no X server — panel desktop position unresolved");
		if (conn != NULL) {
			xcb_disconnect(conn);
		}
		return false;
	}

	bool found = false;
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
	for (int i = 0; i < n_outputs && !found; i++) {
		xcb_randr_get_output_info_reply_t *oi = xcb_randr_get_output_info_reply(
		    conn, xcb_randr_get_output_info(conn, outputs[i], res->config_timestamp), NULL);
		if (oi == NULL) {
			continue;
		}
		// Only connected outputs driven by a CRTC have a desktop position.
		if (oi->connection == XCB_RANDR_CONNECTION_CONNECTED && oi->crtc != XCB_NONE &&
		    randr_output_is_leia_panel(conn, outputs[i], edid_atom)) {
			xcb_randr_get_crtc_info_reply_t *ci = xcb_randr_get_crtc_info_reply(
			    conn, xcb_randr_get_crtc_info(conn, oi->crtc, res->config_timestamp), NULL);
			if (ci != NULL) {
				U_LOG_I("leia_lnx_edid: panel on RandR output %.*s at (%d, %d)",
				        xcb_randr_get_output_info_name_length(oi),
				        (const char *)xcb_randr_get_output_info_name(oi), (int)ci->x, (int)ci->y);
				if (out_left != NULL) {
					*out_left = ci->x;
				}
				if (out_top != NULL) {
					*out_top = ci->y;
				}
				found = true;
				free(ci);
			}
		}
		free(oi);
	}
	if (!found) {
		U_LOG_I("leia_lnx_edid: no active RandR output matches the Leia panel table");
	}

done:
	free(res);
	xcb_disconnect(conn);
	return found;
}
