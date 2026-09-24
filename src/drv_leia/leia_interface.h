// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Public interface for the Leia 3D display driver.
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_leia Leia 3D Display Driver
 * @ingroup drv
 *
 * @brief Driver for Leia light field displays using SR SDK (Windows)
 *        and CNSDK (Android).
 */

/*!
 * Cached results from an SR hardware probe.
 *
 * @ingroup drv_leia
 */
struct leiasr_probe_result
{
	bool hw_found;        //!< True if SR hardware was detected
	uint32_t pixel_w;     //!< Native display width in pixels
	uint32_t pixel_h;     //!< Native display height in pixels
	float refresh_hz;     //!< Display refresh rate in Hz
	float display_w_m;    //!< Physical display width in meters
	float display_h_m;    //!< Physical display height in meters
	float nominal_z_m;    //!< Nominal viewer distance in meters
};

/*!
 * The verified display geometry, as ONE record — what the SR platform reports
 * for an IDENTIFIED panel (never the SDK's "default display" placeholders).
 * Resolved and cached process-wide by leia_sr_ready.cpp; consumed by the head
 * device (create + late in-place update), the probe cache and
 * `get_display_info`. Pure C so the Linux arm can share the declaration.
 *
 * @ingroup drv_leia
 */
struct leiasr_geometry
{
	bool valid;           //!< False until a query on an identified panel succeeded
	uint32_t pixel_w;     //!< Native panel width in pixels
	uint32_t pixel_h;     //!< Native panel height in pixels
	uint32_t view_w;      //!< SR-recommended per-view render width in pixels
	uint32_t view_h;      //!< SR-recommended per-view render height in pixels
	float refresh_hz;     //!< Panel refresh rate in Hz (0 = unknown)
	float width_m;        //!< Physical width in meters
	float height_m;       //!< Physical height in meters
	float nominal_x_m;    //!< Nominal viewer position (display-local, meters)
	float nominal_y_m;
	float nominal_z_m;    //!< Nominal viewing distance in meters (0 = unknown)
};

/*!
 * EDID-based probe result for Leia/Dimenco display identification.
 *
 * Three-layer detection:
 * - hw_found: EDID manufacturer+product ID matched a known display panel
 * - sdk_installed: SR SDK registry key exists (HKLM\SOFTWARE\Dimenco\Simulated Reality)
 * - service_running: SRService shared memory is active (Global\sharedDeviceSerialMemory)
 *
 * @ingroup drv_leia
 */
struct leia_display_probe_result
{
	bool hw_found;        //!< EDID matched a known Leia/Dimenco 3D display
	bool sdk_installed;   //!< SR SDK is installed on this machine
	bool service_running; //!< SRService is running with devices connected
	uint16_t manufacturer_id; //!< EDID manufacturer ID of matched display
	uint16_t product_id;      //!< EDID product ID of matched display
	uint32_t pixel_w;     //!< Display width in pixels
	uint32_t pixel_h;     //!< Display height in pixels
	float refresh_hz;     //!< Display refresh rate in Hz
	int32_t screen_left;  //!< Monitor left edge in virtual screen coords
	int32_t screen_top;   //!< Monitor top edge in virtual screen coords
	void *hmonitor;       //!< HMONITOR handle (Windows only, NULL elsewhere)
};

/*!
 * Probe for Leia/Dimenco 3D displays using EDID matching.
 *
 * Does NOT require the SR SDK. Uses Windows SetupAPI to read EDID from
 * the monitor registry and matches against a table of known display panels.
 * Also checks for SR runtime availability (registry + shared memory).
 *
 * Results are cached for later retrieval via leia_edid_get_cached_result().
 *
 * @param[out] out Probe result struct to fill in.
 * @return true if a known Leia/Dimenco 3D display was found.
 *
 * @ingroup drv_leia
 */
bool
leia_edid_probe_display(struct leia_display_probe_result *out);

/*!
 * Retrieve cached EDID probe results from a prior leia_edid_probe_display().
 *
 * @param[out] out Probe result struct to fill in.
 * @return true if cached results are available and a display was found.
 *
 * @ingroup drv_leia
 */
bool
leia_edid_get_cached_result(struct leia_display_probe_result *out);

/*!
 * Perform a fresh EDID probe and return the current screen rectangle of the
 * first Leia/Dimenco 3D display connected to the system.
 *
 * Unlike leia_edid_get_cached_result(), this re-enumerates monitors on every
 * call, so it reflects the current monitor arrangement — useful for placing a
 * compositor window after the user has changed their display layout without
 * restarting the service.
 *
 * @param[out] out_left   Monitor left edge in virtual screen coords.
 * @param[out] out_top    Monitor top edge in virtual screen coords.
 * @param[out] out_width  Monitor width in pixels (optional, may be NULL).
 * @param[out] out_height Monitor height in pixels (optional, may be NULL).
 * @return true if a Leia 3D display was found; false otherwise.
 *
 * @ingroup drv_leia
 */
bool
leia_edid_find_3d_display_rect(int32_t *out_left,
                               int32_t *out_top,
                               int32_t *out_width,
                               int32_t *out_height);

/*!
 * Does the known Leia/Dimenco EDID table contain this (manufacturer, product)
 * pair? Lets `probe_displays()` (issue #69 / ADR-015) match each
 * runtime-supplied display descriptor against the table without
 * re-enumerating monitors.
 *
 * @param manufacturer_id EDID bytes 8-9 (raw).
 * @param product_id      EDID bytes 10-11 (raw).
 * @return true if the pair is a known Leia/Dimenco panel.
 *
 * @ingroup drv_leia
 */
bool
leia_edid_table_contains(uint16_t manufacturer_id, uint16_t product_id);

/*!
 * Probe for SR display hardware.
 *
 * Creates a temporary SR context and checks for an active SR display.
 * Results are cached in statics for later retrieval via
 * leiasr_get_probe_results().
 *
 * On non-SR builds this always returns false.
 *
 * @param timeout_seconds Maximum time to wait for SR context creation.
 * @return true if SR hardware is present and responsive.
 *
 * @ingroup drv_leia
 */
bool
leiasr_probe_display(double timeout_seconds);

/*!
 * Retrieve cached probe results from a prior leiasr_probe_display() call.
 *
 * @param[out] out Probe result struct to fill in.
 * @return true if cached results are available.
 *
 * @ingroup drv_leia
 */
bool
leiasr_get_probe_results(struct leiasr_probe_result *out);

/*!
 * Create a Leia system builder.
 *
 * The builder will detect Leia 3D display hardware and create the
 * appropriate devices and display processors for the platform:
 * - Windows: SR SDK for Vulkan/D3D11 weaving + eye tracking
 * - Android: CNSDK for Vulkan interlacing
 *
 * @return A new xrt_builder, or NULL on failure.
 *
 * @ingroup drv_leia
 */
struct xrt_builder *
t_builder_leia_create(void);

/*!
 * Create a Leia 3D display HMD device.
 *
 * Uses cached probe results from leiasr_probe_display() when available,
 * otherwise falls back to hardcoded defaults.
 *
 * @return A new xrt_device, or NULL on failure.
 *
 * @ingroup drv_leia
 */
struct xrt_device *
leia_hmd_create(void);

/*!
 * Update a LIVE Leia HMD device in place from verified geometry: views/FOV
 * (through the same u_device_setup_split_side_by_side path creation uses),
 * `hmd->screens`, physical size, nominal viewer distance / static pose /
 * eye offsets, and the LeiaSR mode's view scale. The runtime never rebuilds
 * the head device, so this is how geometry that arrives AFTER creation (SR
 * identifying the panel late — see leia_sr_ready.h) reaches apps.
 *
 * Plain field stores; the caller serialises publishes (leia_sr_ready.cpp holds
 * its registry mutex around this call).
 *
 * @param xdev The device returned by leia_hmd_create().
 * @param geom Verified geometry; ignored unless `valid` with sane dimensions.
 * @return true if anything changed.
 *
 * @ingroup drv_leia
 */
bool
leia_hmd_apply_geometry(struct xrt_device *xdev, const struct leiasr_geometry *geom);

/*!
 * Seed the single per-view-scale derivation from a backend's recommended
 * per-view render size and the panel's native size, in pixels.
 *
 * This is the ONLY place the ratio is computed. Ignored (fallback kept) when
 * any dimension is zero, and ignored once a value is latched: FIRST writer
 * wins, because by then the value may already be published in the device's
 * rendering-mode table, and a fresher number that DISAGREES with what the app
 * was sized from is worse than a staler one that agrees.
 *
 * @param view_w    Backend-recommended per-view width in pixels.
 * @param view_h    Backend-recommended per-view height in pixels.
 * @param native_w  Native panel width in pixels.
 * @param native_h  Native panel height in pixels.
 *
 * @ingroup drv_leia
 */
void
leia_view_scale_set_from_dims(uint32_t view_w, uint32_t view_h, uint32_t native_w, uint32_t native_h);

/*!
 * The per-view scale, derived once per process.
 *
 * Both consumers MUST read it from here so they cannot disagree:
 * `xrt_rendering_mode::view_scale_x/y` (which sizes tiles, the worst-case atlas
 * and the compositor's tile grid) and
 * `xrt_plugin_display_info::recommended_view_scale_x/y` (which sizes
 * `XrViewConfigurationView.recommended*`). On first call, and only if nothing
 * has seeded it, this performs the live SR query itself; the result — including
 * the 0.5 x 0.5 fallback — is latched and logged once.
 *
 * @param[out] out_scale_x Horizontal per-view scale; may be NULL.
 * @param[out] out_scale_y Vertical per-view scale; may be NULL.
 *
 * @ingroup drv_leia
 */
void
leia_view_scale_get(float *out_scale_x, float *out_scale_y);

/*!
 * Set an optional external pose source for the Leia HMD.
 *
 * When set, leia_hmd_get_tracked_pose() delegates to this device
 * (e.g. qwerty HMD for WASD/mouse camera control).
 *
 * @param leia_dev  The Leia HMD device returned by leia_hmd_create().
 * @param source    The pose source device, or NULL to disable delegation.
 *
 * @ingroup drv_leia
 */
void
leia_hmd_set_pose_source(struct xrt_device *leia_dev, struct xrt_device *source);

#ifdef __cplusplus
}
#endif
