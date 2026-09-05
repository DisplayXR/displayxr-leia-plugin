// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Windows Graphics Capture (WGC) helper for Leia DP transparency.
 *
 * Captures the desktop region behind the app's HWND and exposes the
 * latest frame as a cross-API shared D3D11 texture (SHARED_NTHANDLE +
 * KEYEDMUTEX). The D3D11 and D3D12 Leia DPs import that handle and
 * sample the desktop content into the per-tile compose-under-bg pass —
 * replacing the older chroma-key trick.
 *
 * On any failure path (Windows < 10 2004, WGC unavailable, DRM, env
 * override), create() returns NULL and the DP falls back to chroma-key.
 *
 * Self-capture defense: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
 * is applied to the HWND so WGC does not recursively capture our own
 * woven output back into the background.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#ifdef _WIN32

#include "xrt/xrt_display_processor.h"

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * leia-plugin#224 / runtime#1363 — rear depth budget, DP-side background
 * source.
 *
 * The preview producer below compiles ONLY when the pinned runtime headers
 * carry `struct xrt_dp_background_preview` (announced by XRT_DP_BG_PREVIEW_STALE,
 * the same coupled-ABI-addition pattern as XRT_DP_D3D11_HAS_FRAME_TIMING). With
 * an older pin the producer disappears entirely — zero cost — and every per-API
 * `get_background_preview` slot stays NULL, which the runtime reads as "no
 * source" and degrades to today's clip-at-the-display-plane behaviour.
 */
#ifdef XRT_DP_BG_PREVIEW_STALE
#define LEIA_BG_CAPTURE_HAS_PREVIEW 1
#endif

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11Fence;
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12Fence;

#ifdef __cplusplus
extern "C" {
#endif

struct leia_bg_capture;

/*!
 * Create a WGC capture session targeting the monitor containing @p hwnd.
 * Returns NULL on any failure — caller falls back to chroma-key.
 *
 * @p adapter_luid selects the DXGI adapter for the internal producer D3D11
 * device (packed LUID, HighPart<<32 | LowPart; 0 = system default). Pass the
 * LUID of the adapter the CONSUMER device lives on: a D3D11 shared texture
 * cannot be opened across adapters, and on some drivers (Intel UHD
 * 30.0.100.x) a cross-adapter Vulkan import crashes inside the ICD instead
 * of failing (#819). With 0 the device follows the process GpuPreference,
 * which is unrelated to where the consumer lives.
 *
 * Side-effect on success: SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE).
 */
struct leia_bg_capture *leia_bg_capture_create(HWND hwnd, uint64_t adapter_luid);

/*!
 * Packed LUID of the adapter the producer D3D11 device actually landed on
 * (authoritative — queried from the created device). 0 if unknown.
 */
uint64_t leia_bg_capture_get_adapter_luid(struct leia_bg_capture *c);

/*!
 * Open the shared staging texture on the caller's D3D11 device + create an SRV.
 * Call once at DP init. *out_tex and *out_srv are owned by the caller.
 */
long leia_bg_capture_open_d3d11(struct leia_bg_capture *c,
                                struct ID3D11Device *dev,
                                struct ID3D11Texture2D **out_tex,
                                struct ID3D11ShaderResourceView **out_srv);

/*!
 * Open the shared staging texture on the caller's D3D12 device.
 * Caller creates the SRV in its own descriptor heap.
 */
long leia_bg_capture_open_d3d12(struct leia_bg_capture *c,
                                struct ID3D12Device *dev,
                                struct ID3D12Resource **out_res);

/*!
 * Open the producer's shared fence on the caller's D3D11 device. Consumer
 * waits on this fence (via @c ID3D11DeviceContext4::Wait) before sampling
 * the shared staging texture so it sees the producer's copy result.
 */
long leia_bg_capture_open_fence_d3d11(struct leia_bg_capture *c,
                                      struct ID3D11Device *dev,
                                      struct ID3D11Fence **out_fence);

/*!
 * Open the producer's shared fence on the caller's D3D12 device. Consumer
 * waits via @c ID3D12CommandQueue::Wait before sampling.
 */
long leia_bg_capture_open_fence_d3d12(struct leia_bg_capture *c,
                                      struct ID3D12Device *dev,
                                      struct ID3D12Fence **out_fence);

/*!
 * Expose the shared NT handle of the staging texture so the caller can
 * import it into Vulkan via @c VK_KHR_external_memory_win32 (handle type
 * @c VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT). The handle is owned
 * by the capture module; do NOT @c CloseHandle on it.
 */
HANDLE leia_bg_capture_get_shared_handle(struct leia_bg_capture *c);

/*!
 * Monitor dimensions used to size the staging texture (BGRA8). Use these
 * for the imported VkImage extent.
 */
void leia_bg_capture_get_size(struct leia_bg_capture *c,
                              uint32_t *out_width,
                              uint32_t *out_height);

/*!
 * Per-frame: pull the latest WGC frame into the shared staging tex, return
 * window-on-monitor region as normalized UVs and the fence value the caller
 * must Wait on before sampling.
 *
 * @return  true if a captured frame is available; false if no frame yet
 *          or the window has crossed monitors (caller should skip compose
 *          for this frame and either fall back or pass-through).
 */
bool leia_bg_capture_poll(struct leia_bg_capture *c,
                          float out_bg_uv_origin[2],
                          float out_bg_uv_extent[2],
                          uint64_t *out_fence_wait_value);

#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
/*!
 * Hand back the latest downsampled CPU preview of the desktop under the app's
 * window — the background source for the runtime's rear depth budget (#224).
 *
 * PIXELS ONLY. This module decides nothing perceptual: it reports the bytes,
 * the generation they came from, and whether they are stale. The runtime owns
 * the neutrality analysis and the policy.
 *
 * The preview is produced inside @ref leia_bg_capture_poll, once per capture
 * generation (<= 15 Hz), never per weave. This call is a cache read.
 *
 * Threading: produced and read on the compositor render thread only —
 * `process_atlas` → `leia_bg_capture_poll` produces, and the runtime calls the
 * DP slot on that same thread right after `process_atlas`. No lock is taken and
 * none is needed; adding one would misdescribe the model.
 *
 * @p out must be pre-set by the caller (the runtime uses
 * @ref xrt_dp_background_preview_init); only fields inside its `struct_size`
 * are written. @ref xrt_dp_background_preview::bgra is BORROWED — owned by this
 * module and valid only until the next `process_atlas()` on the same DP.
 *
 * @return false when there is no source right now: capture disabled (env kill
 *         switch or a producer failure), no captured frame yet, the last poll
 *         failed (cross-monitor drag / client-present mode), or the runtime's
 *         `struct_size` is too small to describe a buffer.
 */
bool leia_bg_capture_get_preview(struct leia_bg_capture *c, struct xrt_dp_background_preview *out);
#endif // LEIA_BG_CAPTURE_HAS_PREVIEW

void leia_bg_capture_destroy(struct leia_bg_capture *c);

#ifdef __cplusplus
}
#endif

#endif // _WIN32
