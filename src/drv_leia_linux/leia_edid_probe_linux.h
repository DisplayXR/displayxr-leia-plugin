// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux panel detection: match connected DRM monitors' EDID against
 *         the frozen Leia panel table (drv_leia/leia_edid_table.h) — the
 *         Linux mirror of the Windows EDID fast path (leia_edid_probe.c).
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Scan /sys/class/drm/<connector>/edid for a connected monitor whose EDID
 * manufacturer+product IDs match the known Leia panel table.
 *
 * @param[out] out_manufacturer_id  Matched EDID manufacturer ID — may be NULL.
 * @param[out] out_product_id       Matched EDID product ID — may be NULL.
 * @return true when a known Leia panel is connected.
 */
bool
leia_lnx_edid_panel_present(uint16_t *out_manufacturer_id, uint16_t *out_product_id);

/*!
 * Resolve the Leia panel's desktop position (top-left, root-window pixels) by
 * matching the frozen EDID table against the X server's RandR outputs' EDID
 * property and reading the active CRTC's x/y (runtime#715, #91). Matching by
 * EDID rather than connector name sidesteps DRM-vs-RandR naming drift
 * ("HDMI-A-1" vs "HDMI-1" vs NVIDIA's "HDMI-0").
 *
 * Transient query — connects to the X server, resolves, disconnects. Callers
 * should cache the result. Fails gracefully headless (no DISPLAY / no RandR /
 * no match), e.g. the CI selftest.
 *
 * @param[out] out_left  Panel left edge in root-window pixels — may be NULL.
 * @param[out] out_top   Panel top edge in root-window pixels — may be NULL.
 * @return true when a known Leia panel was found on an active RandR output.
 */
bool
leia_lnx_edid_panel_desktop_position(int32_t *out_left, int32_t *out_top);

#ifdef __cplusplus
}
#endif
