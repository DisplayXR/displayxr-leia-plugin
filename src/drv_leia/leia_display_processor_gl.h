// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Leia GL display processor: wraps SR SDK GL weaver
 *         as an @ref xrt_display_processor_gl implementation.
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_display_processor_gl.h"
#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct leiasr_gl;

/*!
 * Factory function for creating a Leia SR GL display processor.
 *
 * Matches the @ref xrt_dp_factory_gl_fn_t signature.
 * Creates an SR GL weaver internally and owns it for the lifetime
 * of the display processor.
 *
 * Set this as dp_factory_gl in xrt_system_compositor_info from
 * the Leia target builder.
 */
xrt_result_t
leia_dp_factory_gl(void *window_handle,
                    struct xrt_display_processor_gl **out_xdp);

struct xrt_plugin_instance;
struct xrt_screen_binding;

/*!
 * Multi-screen M6 (GL): the per-screen factory
 * (`xrt_plugin_iface::create_dp_gl_for_screen`). The binding is resolved to an
 * SR display id (the runtime's, or this plug-in's own per-monitor claim), the
 * weaver is bound to it with EXTERNAL routing, and — with @p window_handle
 * NULL — phases from set_present_origin. The weave stays inside the canvas
 * (viewport + scissor). Compiled only against a runtime header announcing
 * XRT_PLUGIN_IFACE_HAS_CREATE_DP_GL_FOR_SCREEN.
 */
xrt_result_t
leia_dp_factory_gl_for_screen(struct xrt_plugin_instance *inst,
                              void *window_handle,
                              const struct xrt_screen_binding *binding,
                              struct xrt_display_processor_gl **out_xdp);

#ifdef __cplusplus
}
#endif
