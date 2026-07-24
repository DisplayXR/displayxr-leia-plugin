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
