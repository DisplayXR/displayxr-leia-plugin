// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  EDID-based Leia/Dimenco 3D display identification.
 * @ingroup drv_leia
 *
 * Identifies Leia 3D displays using EDID manufacturer+product IDs and checks
 * for SR runtime availability via registry and shared memory. Does NOT depend
 * on the SR SDK — uses only Windows APIs and the generic EDID utility.
 *
 * EDID table extracted from SR SDK's WindowsDisplayUtilities.cpp
 * (generateProductEdidMatcherByProductCodeMap).
 */

#include "leia_interface.h"
#include "os/os_display_edid.h"
#include "util/u_logging.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "leia_edid_table.h"

//! Cached probe result (file-scope)
static struct leia_display_probe_result g_leia_edid_result = {0};
static bool g_leia_edid_probed = false;

bool
leia_edid_probe_display(struct leia_display_probe_result *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	// Step 1: EDID-based hardware identification
	struct os_display_edid_list edid_list;
	bool edid_ok = os_display_edid_enumerate(&edid_list);

	// Always log diagnostics
	U_LOG_W("EDID probe: gdi_monitors=%u setupdi_devices=%u edid_reads=%u correlated=%u diag=%d win32err=%u",
	        edid_list.diag_gdi_count, edid_list.diag_setupdi_count, edid_list.diag_edid_read_count,
	        edid_list.count, (int)edid_list.diag_error, edid_list.diag_win32_error);

	// Log GDI device IDs for debugging correlation issues
	for (uint32_t i = 0; i < edid_list.diag_gdi_count && i < OS_DISPLAY_EDID_MAX_MONITORS; i++) {
		if (edid_list.diag_gdi_device_ids[i][0] != '\0') {
			U_LOG_W("EDID probe:   gdi[%u] DeviceID='%s'", i, edid_list.diag_gdi_device_ids[i]);
		}
	}

	if (!edid_ok) {
		U_LOG_W("EDID probe: enumeration failed (diag=%d)", (int)edid_list.diag_error);
	} else {
		U_LOG_W("EDID probe: enumerated %u monitors", edid_list.count);
		for (uint32_t i = 0; i < edid_list.count; i++) {
			U_LOG_W("EDID probe:   monitor[%u] mfr=0x%04X prod=0x%04X %ux%u @%d,%d %s",
			        i, edid_list.monitors[i].manufacturer_id, edid_list.monitors[i].product_id,
			        edid_list.monitors[i].pixel_width, edid_list.monitors[i].pixel_height,
			        edid_list.monitors[i].screen_left, edid_list.monitors[i].screen_top,
			        edid_list.monitors[i].is_primary ? "(primary)" : "");
		}

		const struct os_display_edid_monitor *match =
		    os_display_edid_find_in_table(&edid_list, leia_edid_table, LEIA_EDID_TABLE_LEN);

		if (match == NULL) {
			U_LOG_W("EDID probe: no known Leia/Dimenco display matched among %u monitors",
			        edid_list.count);
		} else {
			out->hw_found = true;
			out->manufacturer_id = match->manufacturer_id;
			out->product_id = match->product_id;
			out->pixel_w = match->pixel_width;
			out->pixel_h = match->pixel_height;
			out->refresh_hz = (float)match->refresh_hz;
			out->screen_left = match->screen_left;
			out->screen_top = match->screen_top;
			out->hmonitor = match->hmonitor;

			U_LOG_W("EDID probe: Leia display found (mfr=0x%04X prod=0x%04X) at %ux%u @ %d,%d",
			        match->manufacturer_id, match->product_id, match->pixel_width,
			        match->pixel_height, match->screen_left, match->screen_top);
		}
	}

	/*
	 * SR runtime presence — probed regardless of the EDID table match. The
	 * static leia_edid_table[] is a frozen copy of SR's product-code map
	 * (WindowsDisplayUtilities.cpp) and drifts as new panels ship: SR's
	 * ProductCodeInstaller registers a new prototype's EDID IDs in SR's own
	 * registry, invisible to our table. leia_plugin_probe() defers to the SR
	 * runtime on a table miss (the authoritative check), so it needs
	 * sdk_installed/service_running populated even when hw_found is false.
	 */
#ifdef _WIN32
	{
		HKEY hKey;
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Dimenco\\Simulated Reality", 0, KEY_READ, &hKey) ==
		    ERROR_SUCCESS) {
			out->sdk_installed = true;
			RegCloseKey(hKey);
		} else {
			U_LOG_W("EDID probe: SR SDK not installed");
		}
	}

	if (out->sdk_installed) {
		HANDLE hMapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "Global\\sharedDeviceSerialMemory");
		if (hMapping != NULL) {
			out->service_running = true;
			CloseHandle(hMapping);
		} else {
			U_LOG_W("EDID probe: SR SDK installed but SRService not running");
		}
	}
#endif

	// Cache the result
	g_leia_edid_result = *out;
	g_leia_edid_probed = true;
	return out->hw_found;
}

bool
leia_edid_get_cached_result(struct leia_display_probe_result *out)
{
	if (out == NULL || !g_leia_edid_probed) {
		return false;
	}
	*out = g_leia_edid_result;
	return g_leia_edid_result.hw_found;
}

bool
leia_edid_find_3d_display_rect(int32_t *out_left,
                               int32_t *out_top,
                               int32_t *out_width,
                               int32_t *out_height)
{
	struct os_display_edid_list list;
	if (!os_display_edid_enumerate(&list)) {
		return false;
	}

	const struct os_display_edid_monitor *match =
	    os_display_edid_find_in_table(&list, leia_edid_table, LEIA_EDID_TABLE_LEN);
	if (match == NULL) {
		return false;
	}

	if (out_left != NULL) {
		*out_left = match->screen_left;
	}
	if (out_top != NULL) {
		*out_top = match->screen_top;
	}
	if (out_width != NULL) {
		*out_width = (int32_t)match->pixel_width;
	}
	if (out_height != NULL) {
		*out_height = (int32_t)match->pixel_height;
	}
	return true;
}

bool
leia_edid_table_contains(uint16_t manufacturer_id, uint16_t product_id)
{
	for (size_t i = 0; i < LEIA_EDID_TABLE_LEN; i++) {
		if (leia_edid_table[i][0] == manufacturer_id && leia_edid_table[i][1] == product_id) {
			return true;
		}
	}
	return false;
}
