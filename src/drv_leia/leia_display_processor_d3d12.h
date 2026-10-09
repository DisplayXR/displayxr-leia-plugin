// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Leia D3D12 display processor: wraps SR SDK D3D12 weaver
 *         as an @ref xrt_display_processor_d3d12 implementation.
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_display_processor_d3d12.h"
#include "xrt/xrt_plugin.h" // XRT_PLUGIN_IFACE_HAS_CREATE_DP_D3D12_FOR_SCREEN, xrt_screen_binding
#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct leiasr_d3d12;

/*!
 * Factory function for creating a Leia SR D3D12 display processor.
 *
 * Matches the @ref xrt_dp_factory_d3d12_fn_t signature.
 * Creates an SR D3D12 weaver internally and owns it for the lifetime
 * of the display processor.
 */
xrt_result_t
leia_dp_factory_d3d12(void *d3d12_device,
                      void *d3d12_command_queue,
                      void *window_handle,
                      struct xrt_display_processor_d3d12 **out_xdp);

#ifdef XRT_PLUGIN_IFACE_HAS_CREATE_DP_D3D12_FOR_SCREEN
/*!
 * Multi-screen M6: one D3D12 DP per screen a spanning window covers
 * (`xrt_plugin_iface::create_dp_d3d12_for_screen`), the D3D12 twin of
 * leia_dp_factory_d3d11_for_screen. The binding is resolved to an SR display
 * id (the binding's own, else this plug-in's per-monitor claim), the weaver is
 * bound to it with EXTERNAL routing, and — with @p window_handle NULL — phases
 * from the runtime's present origin.
 */
xrt_result_t
leia_dp_factory_d3d12_for_screen(struct xrt_plugin_instance *inst,
                                 void *d3d12_device,
                                 void *d3d12_command_queue,
                                 void *window_handle,
                                 const struct xrt_screen_binding *binding,
                                 struct xrt_display_processor_d3d12 **out_xdp);
#endif

#ifdef __cplusplus
}
#endif
