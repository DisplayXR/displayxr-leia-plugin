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

#ifdef __cplusplus
}
#endif
