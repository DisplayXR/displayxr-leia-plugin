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
#include <string.h>

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
