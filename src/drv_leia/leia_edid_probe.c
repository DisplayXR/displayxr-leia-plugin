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

/*
 * The enumeration the cached result was derived from. Kept only so the WARN
 * diagnostics block in leia_edid_probe_display() is re-emitted when the
 * enumeration itself changes (monitor count, per-monitor identity / mode /
 * position, the SetupDi/EDID diag counters) and stays silent otherwise. The
 * runtime re-runs the DP registry probe at ~1 Hz while the panel is
 * unidentified (displayxr-runtime#1722), and WARN is reserved for one-off
 * lifecycle events — a service started with the panel asleep must not emit
 * the same five lines every second until the panel wakes.
 *
 * Unguarded, like the two statics above: the probe is called from the
 * plug-in's probe() and probe_displays() entry points, which the runtime
 * serialises today. A concurrent pair of probes could at worst log the block
 * twice or skip one repeat — never corrupt the returned result, which is
 * written to the caller's own buffer.
 */
static struct os_display_edid_list g_leia_edid_last_list;
static bool g_leia_edid_last_ok = false;

/*!
 * Compare the fields of a probe result that the diagnostics report or that a
 * caller acts on. `hmonitor` is deliberately excluded: an opaque handle that
 * is never logged and that the per-monitor position/mode fields already
 * cover.
 */
static bool
probe_result_equal(const struct leia_display_probe_result *a, const struct leia_display_probe_result *b)
{
	return a->hw_found == b->hw_found &&               //
	       a->sdk_installed == b->sdk_installed &&     //
	       a->service_running == b->service_running && //
	       a->manufacturer_id == b->manufacturer_id && //
	       a->product_id == b->product_id &&           //
	       a->pixel_w == b->pixel_w &&                 //
	       a->pixel_h == b->pixel_h &&                 //
	       a->refresh_hz == b->refresh_hz &&           //
	       a->screen_left == b->screen_left &&         //
	       a->screen_top == b->screen_top;
}

/*!
 * Compare the parts of an enumeration that the diagnostics block prints:
 * the diag counters, the GDI device IDs and each monitor's identity, mode and
 * position. A change in any of them is a real enumeration change worth a
 * fresh block.
 */
static bool
edid_list_equal(const struct os_display_edid_list *a, const struct os_display_edid_list *b)
{
	if (a->count != b->count ||                               //
	    a->diag_gdi_count != b->diag_gdi_count ||             //
	    a->diag_setupdi_count != b->diag_setupdi_count ||     //
	    a->diag_edid_read_count != b->diag_edid_read_count || //
	    a->diag_error != b->diag_error ||                     //
	    a->diag_win32_error != b->diag_win32_error) {
		return false;
	}
	for (uint32_t i = 0; i < a->diag_gdi_count && i < OS_DISPLAY_EDID_MAX_MONITORS; i++) {
		if (strncmp(a->diag_gdi_device_ids[i], b->diag_gdi_device_ids[i], sizeof(a->diag_gdi_device_ids[i])) !=
		    0) {
			return false;
		}
	}
	for (uint32_t i = 0; i < a->count && i < OS_DISPLAY_EDID_MAX_MONITORS; i++) {
		const struct os_display_edid_monitor *ma = &a->monitors[i];
		const struct os_display_edid_monitor *mb = &b->monitors[i];
		if (ma->manufacturer_id != mb->manufacturer_id || //
		    ma->product_id != mb->product_id ||           //
		    ma->pixel_width != mb->pixel_width ||         //
		    ma->pixel_height != mb->pixel_height ||       //
		    ma->screen_left != mb->screen_left ||         //
		    ma->screen_top != mb->screen_top ||           //
		    ma->is_primary != mb->is_primary) {
			return false;
		}
	}
	return true;
}

bool
leia_edid_probe_display(struct leia_display_probe_result *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	// Step 1: EDID-based hardware identification. Silent here — the
	// diagnostics are emitted below, once the outcome is known, and only
	// when it differs from the previous call.
	struct os_display_edid_list edid_list;
	memset(&edid_list, 0, sizeof(edid_list));
	bool edid_ok = os_display_edid_enumerate(&edid_list);

	const struct os_display_edid_monitor *match = NULL;
	if (edid_ok) {
		match = os_display_edid_find_in_table(&edid_list, leia_edid_table, LEIA_EDID_TABLE_LEN);
		if (match != NULL) {
			out->hw_found = true;
			out->manufacturer_id = match->manufacturer_id;
			out->product_id = match->product_id;
			out->pixel_w = match->pixel_width;
			out->pixel_h = match->pixel_height;
			out->refresh_hz = (float)match->refresh_hz;
			out->screen_left = match->screen_left;
			out->screen_top = match->screen_top;
			out->hmonitor = match->hmonitor;
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
		}
	}

	if (out->sdk_installed) {
		HANDLE hMapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "Global\\sharedDeviceSerialMemory");
		if (hMapping != NULL) {
			out->service_running = true;
			CloseHandle(hMapping);
		}
	}
#endif

	/*
	 * Step 2: diagnostics. The first probe always logs; a later probe logs
	 * only when the enumeration or the derived result differs from the
	 * cached one, so a steady state (panel asleep, nothing plugged in,
	 * SRService down) costs one block, not one per refresh.
	 */
	const bool changed = !g_leia_edid_probed ||                                //
	                     edid_ok != g_leia_edid_last_ok ||                     //
	                     !edid_list_equal(&edid_list, &g_leia_edid_last_list) || //
	                     !probe_result_equal(out, &g_leia_edid_result);

	if (changed) {
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
				U_LOG_W("EDID probe:   monitor[%u] mfr=0x%04X prod=0x%04X %ux%u @%d,%d %s", i,
				        edid_list.monitors[i].manufacturer_id, edid_list.monitors[i].product_id,
				        edid_list.monitors[i].pixel_width, edid_list.monitors[i].pixel_height,
				        edid_list.monitors[i].screen_left, edid_list.monitors[i].screen_top,
				        edid_list.monitors[i].is_primary ? "(primary)" : "");
			}

			if (match == NULL) {
				U_LOG_W("EDID probe: no known Leia/Dimenco display matched among %u monitors",
				        edid_list.count);
			} else {
				U_LOG_W("EDID probe: Leia display found (mfr=0x%04X prod=0x%04X) at %ux%u @ %d,%d",
				        match->manufacturer_id, match->product_id, match->pixel_width,
				        match->pixel_height, match->screen_left, match->screen_top);
			}
		}

#ifdef _WIN32
		if (!out->sdk_installed) {
			U_LOG_W("EDID probe: SR SDK not installed");
		} else if (!out->service_running) {
			U_LOG_W("EDID probe: SR SDK installed but SRService not running");
		}
#endif
	} else {
		U_LOG_D("EDID probe: unchanged since last probe (hw=%d sdk=%d service=%d monitors=%u) -- not re-logging",
		        out->hw_found, out->sdk_installed, out->service_running, edid_list.count);
	}

	// Cache the result (and the enumeration it came from, for the next diff)
	g_leia_edid_result = *out;
	g_leia_edid_last_list = edid_list;
	g_leia_edid_last_ok = edid_ok;
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
