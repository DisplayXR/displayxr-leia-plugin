// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  GNOME/mutter side of the Linux background capture: find the 3D panel
 *         in mutter's LOGICAL layout, exclude our windows from off-screen
 *         paints, and record the panel's area — all over libdbus, no Vulkan.
 *
 * Three D-Bus services, each doing one job:
 *
 *  - org.gnome.Mutter.DisplayConfig — GetCurrentState. The ONLY honest source
 *    of the panel's rectangle in the coordinate space RecordArea takes (mutter
 *    stage = LOGICAL coordinates under the default logical layout mode). X11 /
 *    XWayland positions are no substitute: XWayland's root space is itself
 *    scaled, so it is neither logical nor device.
 *
 *  - org.displayxr.WindowGeometry → org.displayxr.CaptureExclusion1 — the
 *    DisplayXR GNOME Shell extension (displayxr-runtime
 *    contrib/gnome-shell/window-geometry@displayxr.org, version 2+). Exclude(0)
 *    removes every window of the calling process from off-screen stage paints —
 *    the GNOME equivalent of WDA_EXCLUDEFROMCAPTURE — for as long as the
 *    calling bus connection lives.
 *
 *  - org.gnome.Mutter.ScreenCast — CreateSession → RecordArea → Start →
 *    Stream::PipeWireStreamAdded(node). RecordArea re-renders the area
 *    OFF-SCREEN (that is what the extension's effect keys on); RecordMonitor
 *    may blit the on-screen view and would contain our window. The PipeWire
 *    node is then reachable over the default PipeWire socket — no portal, no
 *    dialog, no fd hand-off.
 *
 * UNITS, stated once:
 *   logical_*  mutter stage coordinates (what RecordArea takes)
 *   device_*   the panel's own pixels (its current mode, after rotation) — the
 *              space the runtime's present origin / window extent are in, and
 *              the space the recorded stream comes back in (area × the highest
 *              overlapping monitor scale; for an area that IS the panel, the
 *              panel's scale, so stream == device up to rounding).
 *
 * Split out of leia_bg_capture_linux.c so the D-Bus/units logic is exercisable
 * without a GPU (and so the pure parsers below can be unit-tested).
 *
 * @ingroup drv_leia_linux
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Bus name the DisplayXR GNOME Shell extension owns (shared with WindowGeometry1).
#define LEIA_MUTTER_EXT_BUS "org.displayxr.WindowGeometry"
#define LEIA_MUTTER_EXT_EXCLUDE_PATH "/org/displayxr/CaptureExclusion"
#define LEIA_MUTTER_EXT_EXCLUDE_IFACE "org.displayxr.CaptureExclusion1"

/*!
 * Identity of the 3D panel, derived from its EDID — the key used to find it
 * among mutter's monitors. Mutter names a monitor by (connector, vendor,
 * product, serial) where vendor is the EDID PNP id, product is the EDID
 * monitor-name descriptor (else the product code as "0x%04x") and serial is
 * the serial-string descriptor (else the serial number as "0x%08x").
 */
struct leia_panel_identity
{
	bool valid;
	char vendor[4];        //!< EDID PNP id, e.g. "ACR"
	char name[16];         //!< monitor-name descriptor (0xFC), trimmed; "" if absent
	char serial_str[16];   //!< serial-string descriptor (0xFF), trimmed; "" if absent
	uint16_t product_code; //!< EDID bytes 10-11, little-endian
	uint32_t serial_number; //!< EDID bytes 12-15, little-endian
	char drm_connector[64]; //!< sysfs connector, e.g. "HDMI-A-1" (card prefix stripped)
};

//! The panel as mutter lays it out. See the UNITS note above.
struct leia_mutter_panel
{
	char connector[64];   //!< mutter's connector name, e.g. "HDMI-1"
	int32_t logical_x;    //!< stage (logical) origin — what RecordArea takes
	int32_t logical_y;
	uint32_t logical_w;   //!< stage (logical) extent
	uint32_t logical_h;
	uint32_t device_w;    //!< panel pixels: current mode, rotated into layout orientation
	uint32_t device_h;
	double scale;         //!< logical-monitor scale (fractional, e.g. 1.6667 or 2.0)
	uint32_t transform;   //!< 0 normal, 1 90°, 2 180°, 3 270°, 4-7 flipped variants
	uint32_t layout_mode; //!< 1 = logical (stage in logical px), 2 = physical
};

/*!
 * Parse a 128-byte EDID base block into an identity (pure). Returns false if
 * the header magic is wrong. @p drm_connector may be NULL.
 */
bool
leia_edid_identity_parse(const uint8_t edid[128], const char *drm_connector, struct leia_panel_identity *out);

/*!
 * Does mutter's monitor spec name this panel? (pure) Vendor must match; then
 * the product must equal the monitor name (or the "0x%04x" code when the EDID
 * has no name). The serial is compared only when @p use_serial — the caller
 * uses it to break a tie between identical panels.
 */
bool
leia_mutter_spec_matches(const struct leia_panel_identity *id,
                         const char *vendor,
                         const char *product,
                         const char *serial,
                         bool use_serial);

/*!
 * Map a DRM sysfs connector name onto mutter's ("HDMI-A-1" → "HDMI-1",
 * "DP-2" → "DP-2", "eDP-1" → "eDP-1"). Pure; @p out gets "" on overflow.
 */
void
leia_mutter_connector_from_drm(const char *drm, char *out, size_t out_len);

/*!
 * The logical-monitor extent in stage coordinates for a monitor whose current
 * mode is @p mode_w × @p mode_h (pure). Rotates for transform 1/3/5/7; divides
 * by @p scale (rounded) only in logical layout mode — in physical layout mode
 * the stage is in device pixels already. Also returns the rotated device size.
 */
void
leia_mutter_logical_extent(uint32_t mode_w,
                           uint32_t mode_h,
                           double scale,
                           uint32_t transform,
                           uint32_t layout_mode,
                           uint32_t *out_logical_w,
                           uint32_t *out_logical_h,
                           uint32_t *out_device_w,
                           uint32_t *out_device_h);

/*!
 * Read the panel identity from sysfs: the connected connector whose EDID is in
 * the frozen Leia table, else the first connected external (non-eDP/LVDS)
 * connector — the same policy as leia_lnx_edid_panel_physical_size().
 */
bool
leia_mutter_panel_identity_from_sysfs(struct leia_panel_identity *out);

struct DBusConnection;

/*!
 * Locate the panel in mutter's current layout (DisplayConfig.GetCurrentState).
 *
 * Selection, first hit wins:
 *   1. DXR_LEIA_PANEL_CONNECTOR=<mutter connector> (dev / nested-shell testing)
 *   2. EDID identity match (vendor + product; serial breaks ties)
 *   3. the DRM connector name mapped onto mutter's naming
 *   4. the only non-builtin monitor whose current mode is @p expect_w × @p expect_h
 * @p id may be NULL (skips 2 and 3); @p expect_w/h may be 0 (skips 4).
 */
bool
leia_mutter_find_panel(struct DBusConnection *conn,
                       const struct leia_panel_identity *id,
                       uint32_t expect_w,
                       uint32_t expect_h,
                       struct leia_mutter_panel *out);

//! Outcome of asking the extension to exclude our windows.
enum leia_mutter_exclude_status
{
	LEIA_MUTTER_EXCLUDE_OK = 0,   //!< registered; our windows are skipped in off-screen paints
	LEIA_MUTTER_EXCLUDE_ABSENT,   //!< nobody owns org.displayxr.WindowGeometry
	LEIA_MUTTER_EXCLUDE_OUTDATED, //!< extension present but predates CaptureExclusion1 (v1)
	LEIA_MUTTER_EXCLUDE_ERROR,    //!< any other failure (denied, timeout, ...)
};

/*!
 * CaptureExclusion1.Exclude(0) — exclude every window of THIS process, for the
 * lifetime of @p conn. @p out_windows (may be NULL) = windows excluded right now
 * (0 is normal before our window maps; the extension covers later ones).
 */
enum leia_mutter_exclude_status
leia_mutter_capture_exclude(struct DBusConnection *conn, int timeout_ms, uint32_t *out_windows);

const char *
leia_mutter_exclude_status_str(enum leia_mutter_exclude_status s);

struct DBusMessage;

/*
 * ASYNC halves, for the render thread after create(): send without waiting,
 * keep the serial, and hand the matching reply (a METHOD_RETURN or ERROR whose
 * reply_serial equals it, popped by the caller's own non-blocking pump) to the
 * parser. Nothing here blocks.
 */

//! Send CaptureExclusion1.Exclude(0); false if it could not be queued.
bool
leia_mutter_capture_exclude_send(struct DBusConnection *conn, uint32_t *out_serial);

//! Outcome of an Exclude reply (METHOD_RETURN or ERROR).
enum leia_mutter_exclude_status
leia_mutter_capture_exclude_from_reply(struct DBusMessage *reply, uint32_t *out_windows);

//! Send DisplayConfig.GetCurrentState; false if it could not be queued.
bool
leia_mutter_request_layout(struct DBusConnection *conn, uint32_t *out_serial);

//! leia_mutter_find_panel's selection, applied to a GetCurrentState reply.
bool
leia_mutter_panel_from_reply(struct DBusMessage *reply,
                             const struct leia_panel_identity *id,
                             uint32_t expect_w,
                             uint32_t expect_h,
                             struct leia_mutter_panel *out);

/*!
 * Start a Mutter ScreenCast session recording @p panel's LOGICAL rectangle,
 * cursor hidden. Blocks until the stream's PipeWireStreamAdded (bounded by
 * @p timeout_ms). On success @p out_session / @p out_stream are malloc'd object
 * paths (caller frees) and @p out_node is the PipeWire node id. Installs the
 * signal matches the caller's pump needs (Session::Closed).
 */
bool
leia_mutter_screencast_start(struct DBusConnection *conn,
                             const struct leia_mutter_panel *panel,
                             int timeout_ms,
                             char **out_session,
                             char **out_stream,
                             uint32_t *out_node);

//! Session.Stop — best effort, non-blocking (mutter also tears down on disconnect).
void
leia_mutter_screencast_stop(struct DBusConnection *conn, const char *session);

#ifdef __cplusplus
}
#endif
