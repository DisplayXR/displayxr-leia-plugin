// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux Vulkan display processor for the Leia plug-in (Track A).
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_display_processor;

/*!
 * Vulkan display-processor factory, matching `xrt_dp_factory_vk_fn_t`
 * (xrt/xrt_display_processor.h). Wired into
 * `xrt_plugin_iface::create_dp_vk` by leia_plugin_linux.c.
 */
xrt_result_t
leia_lnx_dp_factory_vk(void *vk_bundle,
                       void *vk_cmd_pool,
                       void *window_handle,
                       int32_t target_format,
                       struct xrt_display_processor **out_xdp);

struct leia_lnx_screen_binding; // leia_screen_linux.h

/*!
 * Per-screen variant (multi-screen M4; the runtime's
 * `xrt_plugin_iface::create_dp_vk_for_screen`, M2): same as
 * leia_lnx_dp_factory_vk, but the DP describes the screen @p binding names —
 * its own size, pixels and desktop origin, resolved once at creation into
 * per-instance state — and, being a segment DP, confines every write to the
 * canvas it is handed. @p binding is never NULL.
 */
xrt_result_t
leia_lnx_dp_factory_vk_for_screen(void *vk_bundle,
                                  void *vk_cmd_pool,
                                  void *window_handle,
                                  int32_t target_format,
                                  const struct leia_lnx_screen_binding *binding,
                                  struct xrt_display_processor **out_xdp);

#ifdef __cplusplus
}
#endif
