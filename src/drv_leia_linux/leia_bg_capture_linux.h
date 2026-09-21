// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux desktop background capture for Leia DP transparency (runtime#757).
 *
 * The Linux analogue of leia_bg_capture_win (WGC). Captures the desktop behind
 * the app window and exposes the latest frame as a Vulkan-sampleable image the
 * compose-under-bg pass samples as the background under each per-view atlas
 * tile — plus a small CPU preview for the runtime's rear depth budget.
 *
 * Source (GNOME/mutter only): org.gnome.Mutter.ScreenCast RecordArea over the
 * 3D panel's full LOGICAL rectangle → PipeWire (default socket) → shm frames
 * staged into a VkImage (dma-buf import kept for a producer that offers it).
 *
 * SELF-CAPTURE. Windows excludes our window with SetWindowDisplayAffinity
 * (WDA_EXCLUDEFROMCAPTURE). GNOME has no API for it, so the DisplayXR GNOME
 * Shell extension (displayxr-runtime contrib/gnome-shell/
 * window-geometry@displayxr.org, version 2+) provides one:
 * org.displayxr.CaptureExclusion1.Exclude(0) drops every window of this process
 * from off-screen stage paints — which is exactly what RecordArea renders —
 * while it keeps drawing on screen. Without that extension a capture would
 * contain our own previous woven frame, so create() REFUSES to start one and
 * logs, once, that installing the extension is what enables correct
 * transparency. The capture is also distrusted whenever the extension
 * disappears mid-session (GNOME disables extensions on the lock screen).
 *
 * On any failure create() returns NULL and the DP falls back to silhouette
 * intersection — never a hard error.
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include <stdbool.h>
#include <stdint.h>

// Vulkan types come from the shared vk_bundle path (xrt_vulkan_includes.h via
// vk/vk_helpers.h in the translation units that use this).
#include "vk/vk_helpers.h"
#include "xrt/xrt_display_processor.h" // struct xrt_dp_background_preview

#ifdef __cplusplus
extern "C" {
#endif

struct leia_bg_capture_linux;

/*!
 * Exclude this process's windows from capture, find the panel in mutter's
 * layout, and start recording its full logical rectangle.
 *
 * Blocks briefly (DisplayConfig + Exclude + CreateSession/RecordArea/Start and
 * the wait for the PipeWire node, bounded at ~5 s). No dialog: mutter's own
 * ScreenCast API does not prompt. It does register a screen-sharing handle, so
 * GNOME shows its "screen is being shared" indicator while the capture runs.
 *
 * @param vk          Compositor's Vulkan bundle (device/phys/queue). Not owned.
 * @param panel_px_w  The panel's DEVICE resolution as the DP knows it — used to
 * @param panel_px_h  pick the panel if the EDID match fails, and cross-checked.
 * @return session, or NULL (extension absent/outdated, not GNOME, no PipeWire).
 */
struct leia_bg_capture_linux *
leia_bg_capture_linux_create(struct vk_bundle *vk, uint32_t panel_px_w, uint32_t panel_px_h);

/*!
 * True once the panel's layout changed under the recorded area (moved,
 * rescaled, re-moded). The area is fixed at stream creation, so the DP must
 * destroy and re-create the capture; poll() declines until then.
 */
bool
leia_bg_capture_linux_wants_restart(struct leia_bg_capture_linux *c);

struct xrt_dp_background_preview;

/*!
 * The rear-depth-budget background preview (runtime xrt_dp_background_preview):
 * the desktop under the window as of the last poll(), box-filtered by >= 4x to
 * <= 512 px, BGRA8 top-down. Same contract as leia_bg_capture_get_preview on
 * Windows: fields written only within the caller's struct_size; bgra BORROWED
 * until the next process_atlas; false = no source (capture untrusted, no frame,
 * window off the panel, dma-buf frames). Render thread only.
 */
bool
leia_bg_capture_linux_get_preview(struct leia_bg_capture_linux *c, struct xrt_dp_background_preview *out);

/*!
 * The captured-desktop image as a Vulkan view, in SHADER_READ_ONLY_OPTIMAL,
 * sized to @ref leia_bg_capture_linux_get_size. Stable for the session (the
 * PipeWire buffer pool is imported once); poll() refreshes its contents.
 * VK_NULL_HANDLE until the first frame arrives.
 */
VkImageView
leia_bg_capture_linux_get_view(struct leia_bg_capture_linux *c);

/*!
 * Captured stream dimensions (DEVICE px of the panel, up to rounding), for the bg_uv mapping and the imported
 * VkImage extent.
 */
void
leia_bg_capture_linux_get_size(struct leia_bg_capture_linux *c, uint32_t *out_width, uint32_t *out_height);

/*!
 * Per-frame: advance to the newest PipeWire buffer and return the app-window
 * region on the captured monitor as normalized UVs (origin + extent), matching
 * the Windows leia_bg_capture_poll contract so the compose shader's per-tile
 * bg_uv math is identical.
 *
 * @param win_x,win_y  Window top-left relative to the PANEL's top-left, in
 *                     DEVICE pixels (the DP's present_origin — (0,0) when
 *                     display-scoped/fullscreen).
 * @param win_w,win_h  Window (present target) size in DEVICE pixels. 0 ⟹ treat
 *                     the window as covering the whole captured panel.
 *
 * @return true if a TRUSTED frame (recorded while our windows were excluded) is
 *         available and the window is on the panel; false otherwise (caller
 *         skips the compose and must not rely on a background this frame).
 */
bool
leia_bg_capture_linux_poll(struct leia_bg_capture_linux *c,
                           VkCommandBuffer cmd,
                           int32_t win_x,
                           int32_t win_y,
                           uint32_t win_w,
                           uint32_t win_h,
                           float out_bg_uv_origin[2],
                           float out_bg_uv_extent[2]);

void
leia_bg_capture_linux_destroy(struct leia_bg_capture_linux *c);

#ifdef __cplusplus
}
#endif
