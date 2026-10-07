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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! Upper bound on panels one scan reports (a box with more Leia panels than
 *  this keeps the first ones in connector order). */
#define LEIA_LNX_EDID_MAX_PANELS 8

/*!
 * One connected monitor whose EDID matched the frozen Leia panel table, as one
 * scan of /sys/class/drm (+ optionally RandR) sees it. Multi-screen M0: the
 * probe used to stop at the first match and forget which connector it was on;
 * this keeps every match and its identity so per-monitor claims
 * (probe_displays) and per-connector caches can tell two panels apart.
 */
struct leia_lnx_edid_panel
{
	//! DRM connector without the "cardN-" prefix, e.g. "HDMI-A-1" (the same
	//! spelling SrDisplayDescriptor::connector uses). Empty for a panel seen
	//! only through RandR (no sysfs entry, e.g. inside a container).
	char connector[32];
	//! RandR output name ("HDMI-1", NVIDIA "HDMI-0"), when an X server was
	//! queried and this panel joined one of its outputs. Empty otherwise.
	char randr_output[32];

	uint16_t manufacturer_id; //!< EDID bytes 8-9, little-endian word (table convention)
	uint16_t product_id;      //!< EDID bytes 10-11, little-endian word
	uint32_t serial;          //!< EDID bytes 12-15, little-endian; 0 = none (tie-breaker only)

	uint32_t width_mm;  //!< image size: DTD #1 mm, else bytes 21/22 cm*10; 0 = unknown
	uint32_t height_mm; //!< see @ref width_mm
	uint32_t native_w;  //!< DTD #1 active pixels; 0 = unknown
	uint32_t native_h;  //!< see @ref native_w

	bool internal; //!< eDP / LVDS / DSI connector

	//! RandR CRTC placement — valid only when @ref has_position.
	bool has_position;
	int32_t left, top;
	uint32_t crtc_w, crtc_h;
};

/*!
 * Parse identity, serial, image size and native resolution out of one EDID
 * base block. Pure — no I/O — so the unit test feeds it fixture blobs. Leaves
 * the connector / RandR fields zeroed.
 * @return false when @p len < 128 or the header magic is wrong.
 */
bool
leia_lnx_edid_parse(const uint8_t *edid, size_t len, struct leia_lnx_edid_panel *out);

//! True when (manufacturer, product) is in the frozen Leia panel table.
bool
leia_lnx_edid_table_contains(uint16_t manufacturer_id, uint16_t product_id);

/*!
 * One pass over /sys/class/drm: every connected connector whose EDID matches
 * the Leia table, sorted by connector name (readdir order is arbitrary, and
 * "first panel" must mean the same panel on every call). With
 * @p with_positions, also connects to the X server once and joins each panel
 * to its RandR output by EDID (manufacturer, product, serial) to fill the CRTC
 * origin/size; a table-matching RandR output that joins no sysfs entry is
 * appended with an empty connector. Headless-tolerant (no X / no RandR just
 * leaves has_position false).
 * @return the number of entries written to @p out (<= @p cap).
 */
uint32_t
leia_lnx_edid_enumerate_panels(struct leia_lnx_edid_panel *out, uint32_t cap, bool with_positions);

/*!
 * The panel list (with RandR positions) from the shared cache, rescanned when
 * it is older than @p max_age_ns or was invalidated (UINT64_MAX = only on
 * invalidation or first use). probe_displays reads it with a short TTL, so a
 * burst of registry rebuilds (the runtime rebuilds per client connect) costs
 * one sysfs scan + one X connection, while a hot-plugged panel is picked up by
 * the next rebuild after the TTL. Thread-safe.
 * @return entries copied to @p out (<= @p cap).
 */
uint32_t
leia_lnx_edid_panels_snapshot(struct leia_lnx_edid_panel *out, uint32_t cap, uint64_t max_age_ns);

//! Mark the shared panel cache stale (SR display topology/connect events).
void
leia_lnx_edid_cache_invalidate(void);

/*!
 * Desktop position of one panel, from the shared panel cache (resolved on
 * first use; refreshed by leia_lnx_edid_panels_snapshot() / invalidation —
 * never by this hot-path accessor itself, which is called per frame). @p connector NULL = the first panel in connector
 * order — the one a single-panel session binds today; M4/M5 pass the DP's own connector. Thread-safe.
 * @return true when that panel sits on an active RandR output.
 */
bool
leia_lnx_edid_panel_desktop_position_cached(const char *connector, int32_t *out_left, int32_t *out_top);

/*!
 * The shared panel cache as it is now (no rescan of its own: probe_displays'
 * TTL and SR topology events keep it fresh). What a per-screen DP resolves
 * its screen against (multi-screen M4). Thread-safe.
 * @return entries copied to @p out (<= @p cap).
 */
uint32_t
leia_lnx_edid_panels_cached(struct leia_lnx_edid_panel *out, uint32_t cap);

/*!
 * Scan /sys/class/drm/<connector>/edid for a connected monitor whose EDID
 * manufacturer+product IDs match the known Leia panel table. Single-panel
 * accessor: the first entry of leia_lnx_edid_enumerate_panels().
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
 * Single-panel accessor: the first panel (connector order) of
 * leia_lnx_edid_enumerate_panels(.., true) that has a desktop position.
 * Transient query — connects to the X server, resolves, disconnects. Callers
 * should use leia_lnx_edid_panel_desktop_position_cached() instead. Fails gracefully headless (no DISPLAY / no RandR /
 * no match), e.g. the CI selftest.
 *
 * @param[out] out_left  Panel left edge in root-window pixels — may be NULL.
 * @param[out] out_top   Panel top edge in root-window pixels — may be NULL.
 * @return true when a known Leia panel was found on an active RandR output.
 */
bool
leia_lnx_edid_panel_desktop_position(int32_t *out_left, int32_t *out_top);

/*!
 * Read the panel's physical size from its EDID (metres). Preferred source:
 * detailed timing descriptor #1's mm image-size fields (bytes 66-68); fallback:
 * the cm max-image-size fields (bytes 21/22). Connector choice: a frozen-table
 * match wins; otherwise the first connected non-internal (non-eDP/LVDS)
 * connector — covers panels whose EDID IDs are not (yet) in the table, e.g. the
 * Samsung Odyssey 3D. Used to override the SR runtime's display geometry when
 * it reports its built-in default (344.2x193.6 mm) instead of the real panel —
 * on Linux SRService display geometry is not yet populated per-panel, and a
 * wrong physical size skews the runtime's whole projection (wrong convergence).
 * @param[out] out_width_m   Panel width in metres — may be NULL.
 * @param[out] out_height_m  Panel height in metres — may be NULL.
 * @return true when a plausible size (> 0.05 m each axis) was read.
 */
bool
leia_lnx_edid_panel_physical_size(float *out_width_m, float *out_height_m);

#ifdef __cplusplus
}
#endif
