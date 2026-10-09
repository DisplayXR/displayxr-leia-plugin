// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  macOS arm: process-wide SR state shared by the plug-in iface and the
 *         Metal display processor — the probed panel geometry, the panel's
 *         CGDirectDisplayID, and the SR session helpers.
 *
 * Two kinds of SR instance exist on this arm, and why matters:
 *
 *  - ONE short-lived PROBE instance (leia_mac_sr_probe): CLIENT mode,
 *    srInitialize, srCreateDisplay, read the geometry, destroy. It answers
 *    probe() and get_display_info() and is cached for the process.
 *  - ONE instance PER DISPLAY PROCESSOR (leia_display_processor_macos.m),
 *    created only once the compositor hands over its MTLDevice + queue. On the
 *    macOS SR line the weaver (and the eye-tracker callback) MUST exist before
 *    srInitialize: srInitialize -> startAllSenses() is what starts the
 *    weaver's PredictingWeaverTracker, so a weaver created on an
 *    already-initialised instance never receives eye positions. A shared
 *    pre-initialised context (the Linux arm's shape) therefore cannot host the
 *    weaver here.
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <sr/sr.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The SR panel as this process sees it (probe-time snapshot).
struct leia_mac_display_info
{
	bool valid;
	float width_m;            //!< physical size (srDisplayGetPhysicalSize, cm -> m)
	float height_m;
	uint32_t pixel_width;     //!< native panel pixels (srDisplayGetPhysicalResolution)
	uint32_t pixel_height;
	uint32_t rec_view_width;  //!< srDisplayGetRecommendedTextureSize (per view)
	uint32_t rec_view_height;
	float nominal_x_m;        //!< srDisplayGetDefaultViewingPosition, mm -> m
	float nominal_y_m;
	float nominal_z_m;
	uint32_t refresh_mhz;     //!< from the panel's current CGDisplayMode (60 Hz fallback)

	//! CGDirectDisplayID of the panel, 0 = not found among the online displays.
	uint32_t cg_display_id;
	//! Panel CGDisplayBounds origin in global display POINTS (top-left origin,
	//! y down — CoreGraphics' desktop space). 0,0 when the panel is not found.
	int32_t screen_left_pt;
	int32_t screen_top_pt;
	//! Panel CGDisplayBounds size in points, and its backing scale.
	uint32_t screen_width_pt;
	uint32_t screen_height_pt;
	float backing_scale;
	uint16_t edid_vendor;     //!< CGDisplayVendorNumber (EDID manufacturer id)
	uint16_t edid_product;    //!< CGDisplayModelNumber  (EDID product id)
};

/*!
 * Bring up (once) the probe SR instance and cache the panel geometry. Returns
 * true when SR reports a valid display. Thread-safe; later calls return the
 * cache. A failed probe is retried at most once a second.
 */
bool
leia_mac_sr_probe(void);

//! Copy of the cached probe result (false if the probe never succeeded).
bool
leia_mac_get_display_info(struct leia_mac_display_info *out);

/*!
 * Find the SR panel among the online displays: an EDID (vendor, product) pair
 * from the frozen Leia table first, then a localizedName match (the same
 * keywords LeiaSR's mtl_weaver_window uses), then the only display whose
 * native mode matches @p want_px_w x @p want_px_h (0 = skip that step).
 * Returns the CGDirectDisplayID, or 0.
 */
uint32_t
leia_mac_find_panel_display(uint32_t want_px_w, uint32_t want_px_h);

//! The EDID table lookup (leia_edid_table.h).
bool
leia_mac_edid_is_leia_panel(uint16_t vendor, uint16_t product);

//! srResultToString, NULL-safe.
const char *
leia_mac_sr_result_str(SrResult res);

//! Route SR SDK log lines into the DisplayXR log (INFO; errors as WARN).
void
leia_mac_sr_log_cb(SrLogLevel level, const char *message, void *user_data);

#ifdef __cplusplus
}
#endif
