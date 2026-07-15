// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux desktop background capture for Leia DP transparency (runtime#757).
 *
 * The Linux analogue of leia_bg_capture_win (WGC). Captures the desktop behind
 * the app window via the xdg-desktop-portal ScreenCast interface → PipeWire, and
 * exposes the latest frame as a Vulkan-sampleable image the compose-under-bg pass
 * samples as the background under each per-view atlas tile.
 *
 * Why portal/PipeWire (not XComposite/XShm): it is the cross-desktop standard
 * (GNOME/KDE/wlroots), hands out dma-buf FDs — zero-copy into Vulkan via
 * VK_EXT_external_memory_dma_buf / VK_EXT_image_drm_format_modifier — and works
 * on both X11 and Wayland, so it survives the display-server transition.
 *
 * The module shares the compositor's VkDevice (via the passed vk_bundle) and owns
 * the dma-buf → VkImage import, so the DP just samples the returned VkImageView.
 *
 * On any failure (no portal, PipeWire unavailable, user denies the ScreenCast
 * permission dialog, unsupported dma-buf modifier), create() returns NULL and the
 * DP stays opaque (or uses the 2D-under backdrop only) — never a hard error.
 *
 * Self-capture note: unlike Windows (SetWindowDisplayAffinity WDA_EXCLUDEFROMCAPTURE)
 * there is no portable X11/PipeWire "exclude this window from capture". The portal
 * restricts capture to the chosen monitor/window; capturing strictly the region
 * behind our surface (and our surface presenting a frame late relative to the
 * ~60 Hz capture) keeps the feedback loop out of the composed result in practice.
 * Documented limitation until a portal window-exclusion hint exists.
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

#ifdef __cplusplus
extern "C" {
#endif

struct leia_bg_capture_linux;

/*!
 * Start a portal ScreenCast session capturing the monitor that contains
 * (@p window_screen_left, @p window_screen_top) — the app window's top-left in
 * root/desktop coordinates, forwarded by the DP from the compositor.
 *
 * Blocks briefly on the portal handshake (Create/SelectSources/Start →
 * OpenPipeWireRemote); the user may see a one-time ScreenCast permission dialog.
 * The imported VkImage lives on @p vk's device and is owned by this module.
 *
 * @param vk                 Compositor's Vulkan bundle (device/phys/queue). Not owned.
 * @param window_screen_left App window X in desktop coords (monitor selection).
 * @param window_screen_top  App window Y in desktop coords.
 * @return session, or NULL on any failure (caller stays opaque / 2D-under only).
 */
struct leia_bg_capture_linux *
leia_bg_capture_linux_create(struct vk_bundle *vk, int32_t window_screen_left, int32_t window_screen_top);

/*!
 * The captured-desktop image as a Vulkan view, in SHADER_READ_ONLY_OPTIMAL,
 * sized to @ref leia_bg_capture_linux_get_size. Stable for the session (the
 * PipeWire buffer pool is imported once); poll() refreshes its contents.
 * VK_NULL_HANDLE until the first frame arrives.
 */
VkImageView
leia_bg_capture_linux_get_view(struct leia_bg_capture_linux *c);

/*!
 * Captured monitor dimensions (px), for the bg_uv mapping and the imported
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
 * @return true if a frame is available and the window is on the captured monitor;
 *         false if no frame yet or the window left the monitor (caller skips the
 *         compose for this frame — passes the raw atlas through).
 */
bool
leia_bg_capture_linux_poll(struct leia_bg_capture_linux *c,
                           VkCommandBuffer cmd,
                           float out_bg_uv_origin[2],
                           float out_bg_uv_extent[2]);

void
leia_bg_capture_linux_destroy(struct leia_bg_capture_linux *c);

#ifdef __cplusplus
}
#endif
