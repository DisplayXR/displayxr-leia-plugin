// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  macOS Metal display processor over the srSDK Metal weaver.
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_display_processor_metal;

/*!
 * xrt_dp_factory_metal_fn_t: @p metal_device = id<MTLDevice>,
 * @p command_queue = id<MTLCommandQueue>, @p window_handle = the app's NSView
 * (handle/texture apps) or NULL (hosted apps: the runtime makes its own window
 * and does not pass it).
 */
xrt_result_t
leia_mac_dp_factory_metal(void *metal_device,
                          void *command_queue,
                          void *window_handle,
                          struct xrt_display_processor_metal **out_xdp);

/*!
 * Windowless DP for ONE display (create_dp_metal_for_screen). The caller has
 * already checked the display is the Leia panel. Loads the target (never
 * clears) and confines every write to the canvas; phase via set_present_origin.
 */
xrt_result_t
leia_mac_dp_factory_metal_for_screen(void *metal_device,
                                     void *command_queue,
                                     struct xrt_display_processor_metal **out_xdp);

#ifdef __cplusplus
}
#endif
