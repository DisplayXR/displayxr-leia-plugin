// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Stereo camera source (displayxr-runtime ADR-043, XR_DXR_stereo_camera,
 *         phase L1): the SR eye tracker's camera, read from the SR raw-camera
 *         shared memory WITHOUT taking the device from the tracker.
 *
 * Fills the `stereo_camera_*` slots of `xrt_plugin_iface`. Everything here is
 * compiled only when the runtime headers announce the slots
 * (`XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA`), so the plug-in still builds against
 * a runtime pin that predates them.
 *
 * Knobs (read by the process that loads the plug-in — the runtime SERVICE):
 *   DXR_LEIA_STEREO_CAMERA=0             advertise no camera
 *   DXR_LEIA_STEREO_CAMERA_KEEPALIVE=0   do not hold the tracker up while open
 *   DXR_LEIA_STEREO_CAMERA_SWAP=auto|0|1 which SBS half is the LEFT lens
 *                                        (auto = from the sign of T)
 *   DXR_LEIA_STEREO_CAMERA_SERIAL=<s>    calibration folder override (debug;
 *                                        default = the ACTIVE device's serial)
 *
 * @ingroup drv_leia
 */

#pragma once

#include "xrt/xrt_plugin.h"

#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA

#ifdef __cplusplus
extern "C" {
#endif

uint32_t
leia_stereo_camera_enumerate(struct xrt_plugin_instance *inst,
                             uint32_t capacity,
                             struct xrt_plugin_stereo_camera_info *out);

xrt_result_t
leia_stereo_camera_get_calibration(struct xrt_plugin_instance *inst,
                                   uint32_t index,
                                   struct xrt_plugin_stereo_camera_calibration *out);

xrt_result_t
leia_stereo_camera_open(struct xrt_plugin_instance *inst, uint32_t index, struct xrt_plugin_stereo_camera **out_cam);

uint32_t
leia_stereo_camera_wait_frame(struct xrt_plugin_stereo_camera *cam,
                              int64_t timeout_ns,
                              struct xrt_plugin_stereo_camera_frame *out);

void
leia_stereo_camera_release_frame(struct xrt_plugin_stereo_camera *cam);

void
leia_stereo_camera_close(struct xrt_plugin_stereo_camera *cam);

#ifdef __cplusplus
}
#endif

#endif // XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA
