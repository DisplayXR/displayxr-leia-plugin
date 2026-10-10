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
 *  - ONE process-wide ENUMERATION instance (CLIENT mode, kept alive): it
 *    answers which monitors are SR displays (srEnumerateDisplays), reads the
 *    active display's geometry, and carries an SrSystemMonitor whose
 *    SR_EVENT_TYPE_DISPLAY_TOPOLOGY_CHANGED marks everything below stale.
 *    The next query re-enumerates; CGDirectDisplayIDs (SR's platformHandle)
 *    are never kept across that event.
 *  - ONE instance PER DISPLAY PROCESSOR (leia_display_processor_macos.m),
 *    created only once the compositor hands over its MTLDevice + queue. On the
 *    macOS SR line the weaver (and the eye-tracker callback) MUST exist before
 *    srInitialize: srInitialize -> startAllSenses() is what starts the
 *    weaver's PredictingWeaverTracker, so a weaver created on an
 *    already-initialised instance never receives eye positions.
 *
 * Panel identity: srEnumerateDisplays when the SR runtime has it (FPC verified
 * or EDID only, platformHandle = CGDirectDisplayID). An SR runtime that
 * predates it (SR_ERROR_FUNCTION_UNSUPPORTED) falls back to the old path:
 * srCreateDisplay on the active display, matched to a CGDisplay through the
 * frozen EDID table. srEnumerateDisplays returning 0 = no SR panel attached:
 * the plug-in declines (srDisplayIsValid alone is NOT evidence — the SR
 * runtime reports the active display valid with no panel connected).
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
	uint32_t rec_view_width;  //!< per view (srDisplayGetRecommendedTextureSize, halved when it reports side-by-side)
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
	//! CGDisplayCreateUUIDFromDisplayID as a string — what the runtime's macOS
	//! monitor enumeration puts in xrt_screen_binding::device_name.
	char uuid[40];

	//! srEnumerateDisplays identity (0 / empty / false on the legacy path).
	uint64_t sr_display_id;   //!< SrDisplayDescriptor::displayId (0 = the active display)
	char fpc_serial[32];      //!< SR device serial when FPC verified
	bool fpc_verified;        //!< SR_DISPLAY_CONFIDENCE_FPC_VERIFIED
	bool from_enumeration;    //!< identity came from srEnumerateDisplays (not the EDID table)
};

//! Most SR displays tracked (the macOS line binds 1 today).
#define LEIA_MAC_MAX_SR_DISPLAYS 4

//! What the SR runtime can do for a per-screen weaver (srGetRuntimeCapabilities).
struct leia_mac_sr_caps
{
	bool external_routing;      //!< SrWeaverRoutingCapabilities::externalRouting
	bool display_binding;       //!< SrDisplayBindingCapabilities::displayBinding
	uint32_t max_bound_displays;
	bool lens_per_device;       //!< SrLensBindingCapabilities::lensPerDevice
	bool eye_tracker_per_device; //!< SrEyeTrackerBindingCapabilities::eyeTrackerPerDevice
};

/*!
 * Bring up (once) the probe SR instance and cache the panel geometry. Returns
 * true when SR reports a valid display. Thread-safe; later calls return the
 * cache. A failed probe is retried at most once a second.
 */
bool
leia_mac_sr_probe(void);

//! The active SR panel (false when there is none). Re-enumerates first if a
//! topology change arrived since the last enumeration.
bool
leia_mac_get_display_info(struct leia_mac_display_info *out);

/*!
 * Every SR display srEnumerateDisplays reports, CoreGraphics side resolved
 * (cg_display_id, bounds in points, UUID). Index 0 is the active panel (the
 * one leia_mac_get_display_info describes). Only the active panel carries the
 * SR geometry (recommended view size, nominal viewer); the others carry the
 * descriptor's. Returns the count (0 = no SR panel). Legacy SR runtimes
 * report the one EDID-table panel.
 */
uint32_t
leia_mac_get_sr_displays(struct leia_mac_display_info *out, uint32_t max);

//! Capability snapshot of the enumeration instance (false if no SR runtime).
bool
leia_mac_get_sr_caps(struct leia_mac_sr_caps *out);

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
