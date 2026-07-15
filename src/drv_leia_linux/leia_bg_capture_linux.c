// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux desktop background capture (portal/PipeWire → dma-buf) — runtime#757.
 *
 * @ingroup drv_leia_linux
 */

#include "leia_bg_capture_linux.h"

#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

/*
 * Implementation plan (portal ScreenCast → PipeWire → dma-buf → VkImage).
 * Guarded behind DXR_LEIA_HAVE_PIPEWIRE (set by CMake only when libpipewire-0.3
 * and dbus-1 are present) so CI images without those -dev packages still compile
 * the graceful-decline path and stay green. The real body is brought up on the
 * Intel-Arc / Ubuntu 24.04 demo box (no portal or PipeWire in CI to run against).
 *
 * Sequence:
 *   1. org.freedesktop.portal.ScreenCast over the session D-Bus:
 *        CreateSession → SelectSources(types=MONITOR, cursor=hidden,
 *        persist_mode=1) → Start.  Start's Response signal carries the selected
 *        stream node id + the monitor's position/size, used to pick the monitor
 *        containing (window_screen_left, window_screen_top) and to build the
 *        window→monitor bg_uv mapping in poll().
 *   2. OpenPipeWireRemote → an fd; pw_context_connect_fd() → pw_core.
 *   3. pw_stream_connect(DIRECTION_INPUT) on the node, requesting a
 *        SPA_DATA_DmaBuf format (SPA_VIDEO_FORMAT_BGRA/RGBA) with the device's
 *        supported DRM modifiers (queried from Vulkan via
 *        VK_EXT_image_drm_format_modifier — vkGetPhysicalDeviceImageFormatProperties2
 *        with VkPhysicalDeviceImageDrmFormatModifierInfoEXT).
 *   4. On the stream `add_buffer` callback, for each PipeWire buffer carrying a
 *        dma-buf FD (spa_data.type == SPA_DATA_DmaBuf, .fd, .chunk->stride,
 *        .chunk->offset + the modifier from SPA_META), import it once as a
 *        VkImage:  VkImportMemoryFdInfoKHR{handleType =
 *        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT} +
 *        VkImageDrmFormatModifierExplicitCreateInfoEXT.  Cache VkImage/view keyed
 *        by the PipeWire buffer id (the pool is fixed-size, imported once).
 *   5. poll(): pull the newest dequeued buffer (pw_stream_dequeue_buffer in the
 *        `process` callback stashes the current buffer id), point get_view() at
 *        that buffer's cached view, insert an acquire barrier on `cmd`
 *        (EXTERNAL/UNDEFINED → SHADER_READ_ONLY_OPTIMAL, dma-buf is written by
 *        the compositor out-of-band), and return the window-on-monitor bg_uv.
 *
 * PipeWire runs its own thread loop (pw_thread_loop); poll() only reads the
 * atomically-published "current buffer id" — no blocking on the render thread
 * (contract-style, cf. R-T6).
 */

#ifdef DXR_LEIA_HAVE_PIPEWIRE

// TODO(#757, hardware bring-up on the Arc/24.04 box): implement the sequence
// above. Kept out of the CI-compiled path until it can be exercised against a
// live portal + PipeWire; see the header for the module contract. Until this
// lands, the block below deliberately declines so behavior is identical to a
// build without the libs (opaque / 2D-under-only), never a half-working capture.

struct leia_bg_capture_linux *
leia_bg_capture_linux_create(struct vk_bundle *vk, int32_t window_screen_left, int32_t window_screen_top)
{
	(void)vk;
	(void)window_screen_left;
	(void)window_screen_top;
	U_LOG_W("leia_bg_capture_linux: portal/PipeWire producer not yet implemented — "
	        "transparency uses the 2D-under backdrop only (runtime#757 WS1 hardware bring-up)");
	return NULL;
}

#else // !DXR_LEIA_HAVE_PIPEWIRE

struct leia_bg_capture_linux *
leia_bg_capture_linux_create(struct vk_bundle *vk, int32_t window_screen_left, int32_t window_screen_top)
{
	(void)vk;
	(void)window_screen_left;
	(void)window_screen_top;
	U_LOG_W("leia_bg_capture_linux: built without libpipewire-0.3 / dbus-1 — desktop "
	        "capture unavailable; transparency uses the 2D-under backdrop only (runtime#757)");
	return NULL;
}

#endif // DXR_LEIA_HAVE_PIPEWIRE

// --- Interface tail shared by both build configs (all no-ops until create()
//     returns non-NULL) --------------------------------------------------------

VkImageView
leia_bg_capture_linux_get_view(struct leia_bg_capture_linux *c)
{
	(void)c;
	return VK_NULL_HANDLE;
}

void
leia_bg_capture_linux_get_size(struct leia_bg_capture_linux *c, uint32_t *out_width, uint32_t *out_height)
{
	(void)c;
	if (out_width != NULL) {
		*out_width = 0;
	}
	if (out_height != NULL) {
		*out_height = 0;
	}
}

bool
leia_bg_capture_linux_poll(struct leia_bg_capture_linux *c,
                           VkCommandBuffer cmd,
                           float out_bg_uv_origin[2],
                           float out_bg_uv_extent[2])
{
	(void)c;
	(void)cmd;
	(void)out_bg_uv_origin;
	(void)out_bg_uv_extent;
	return false;
}

void
leia_bg_capture_linux_destroy(struct leia_bg_capture_linux *c)
{
	(void)c;
}
