// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The Leia conversion provider's own D3D11 device and its image
 *         transport: imports runtime-allocated images (NT handle or CPU
 *         memory), never exports.
 *
 * Hosting-agnostic (discovery spec §2.4): the provider never receives a
 * runtime device. It creates its own on the adapter the host names
 * (get_adapter_hint), imports every input and output by NT handle (cached by
 * import_key, dropped when unseen for 64 calls) or reads / writes CPU memory,
 * and honours the image's sync (NONE, KEYED_MUTEX, FENCE_NT).
 *
 * Inputs are copied into a provider-owned flat RGBA8 / BGRA8 texture (the
 * conversion module's input contract). Outputs are drawn — resampled to the
 * target's extent, with the depth-polarity fix where asked — into a
 * provider-owned render target and then copied into the target, so a target
 * needs no particular bind flags.
 *
 * Single-threaded: called only from the provider's serialised [S] slots.
 *
 * @ingroup conv_leia
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct leia_conv_d3d11;

/*!
 * Create the device: on the adapter whose LUID is @p preferred_luid when one
 * matches (0 = none), avoiding @p avoid_luid, else the default hardware
 * adapter. NULL on failure (no usable D3D11.1 device).
 */
struct leia_conv_d3d11 *
leia_conv_d3d11_create(uint64_t preferred_luid, uint64_t avoid_luid);

void
leia_conv_d3d11_destroy(struct leia_conv_d3d11 **d);

//! The adapter LUID the device lives on (the capability's device_luid).
uint64_t
leia_conv_d3d11_luid(const struct leia_conv_d3d11 *d);

//! ID3D11DeviceContext* (multithread-protected) the conversion module records on.
void *
leia_conv_d3d11_context(struct leia_conv_d3d11 *d);

//! The device was removed (DEVICE_LOST class): re-create it on the next warm.
bool
leia_conv_d3d11_lost(struct leia_conv_d3d11 *d);

//! Drop every cached import (stream_destroy).
void
leia_conv_d3d11_clear_cache(struct leia_conv_d3d11 *d);

/*!
 * Make @p img available as a flat single-layer RGBA8 / BGRA8 texture on the
 * provider's device: waits the image's acquire point, copies (NT handle) or
 * uploads (CPU) it, then signals its release point. *out_tex is an
 * ID3D11Texture2D* owned by @p d, valid until the next import.
 */
bool
leia_conv_d3d11_import_input(struct leia_conv_d3d11 *d,
                             const struct xrt_conv_image *img,
                             void **out_tex,
                             uint32_t *out_w,
                             uint32_t *out_h);

/*! @name leia_conv_d3d11_write modes @{ */
#define LEIA_CONV_BLIT_COLOR 0u      //!< RGBA as is (bilinear)
#define LEIA_CONV_BLIT_DEPTH_FLIP 1u //!< 1 - r (point): the module's R8 relative depth -> larger = nearer
#define LEIA_CONV_BLIT_DEPTH 2u      //!< r (point): a float depth as is
/*! @} */

/*!
 * Draw the horizontal band [@p u0, @p u1] of @p src (ID3D11Texture2D* on the
 * provider's device) over the whole of layer @p layer of target @p img,
 * resampled to its extent. NT-handle targets wait their acquire point here;
 * their release is signalled by leia_conv_d3d11_finish. CPU targets are read
 * back and written synchronously.
 */
bool
leia_conv_d3d11_write(struct leia_conv_d3d11 *d,
                      const struct xrt_conv_image *img,
                      uint32_t layer,
                      void *src,
                      float u0,
                      float u1,
                      uint32_t mode);

/*!
 * After the writes of one convert: signal each NT-handle target's release
 * (fence value / keyed-mutex key), and for SYNC_NONE targets drain the device
 * so they are complete before return.
 */
bool
leia_conv_d3d11_finish(struct leia_conv_d3d11 *d);

#ifdef __cplusplus
}
#endif
