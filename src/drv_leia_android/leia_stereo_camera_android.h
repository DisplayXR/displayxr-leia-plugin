// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Android stereo camera source (displayxr-runtime ADR-043,
 *         XR_DXR_stereo_camera): the tablet's logical stereo camera pair read
 *         with NDK Camera2, packed side by side as NV12.
 *
 * Fills the `stereo_camera_*` slots of `xrt_plugin_iface`. Compiled only when
 * the runtime headers announce them (`XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA`).
 *
 * The FRONT pair is also the head tracker's camera. Opening it evicts the
 * vendor tracking service's camera, so while it is open this process's CNSDK
 * cores track IN-APP from 640x480 frames of the same lens the service uses
 * (leia_cnsdk_ext_tracking_*). Closing it hands tracking back to the service.
 * Design + device results: docs/android-stereo-camera.md.
 *
 * Knobs (Android system properties, read by the process that loads the
 * plug-in, i.e. the runtime satellite):
 *   debug.dxr.leia.scam=0                   advertise no camera
 *   debug.dxr.leia.scam_rear=0              advertise the front pair only
 *   debug.dxr.leia.scam_size=WxH            per-eye size (default 1280x720)
 *   debug.dxr.leia.scam_track=0             do not hand tracking over (front)
 *   debug.dxr.leia.scam_swap=auto|0|1       which lens is the SBS left eye
 *   debug.dxr.leia.scam_calib_key_prefix=…  vendor key prefix of the factory block
 *   debug.dxr.leia.scam_rotate=auto|0|90|180|270  clockwise turn of each eye
 *   debug.dxr.leia.scam_calib_ref=auto|array|WxH  size the factory block is expressed at
 *
 * @ingroup drv_leia_android
 */

#pragma once

#include "xrt/xrt_plugin.h"

#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA

#ifdef __cplusplus
extern "C" {
#endif

uint32_t
leia_stereo_camera_android_enumerate(struct xrt_plugin_instance *inst,
                                     uint32_t capacity,
                                     struct xrt_plugin_stereo_camera_info *out);

xrt_result_t
leia_stereo_camera_android_get_calibration(struct xrt_plugin_instance *inst,
                                           uint32_t index,
                                           struct xrt_plugin_stereo_camera_calibration *out);

xrt_result_t
leia_stereo_camera_android_open(struct xrt_plugin_instance *inst,
                                uint32_t index,
                                struct xrt_plugin_stereo_camera **out_cam);

uint32_t
leia_stereo_camera_android_wait_frame(struct xrt_plugin_stereo_camera *cam,
                                      int64_t timeout_ns,
                                      struct xrt_plugin_stereo_camera_frame *out);

void
leia_stereo_camera_android_release_frame(struct xrt_plugin_stereo_camera *cam);

void
leia_stereo_camera_android_close(struct xrt_plugin_stereo_camera *cam);

/*!
 * The physical id of the FRONT pair's factory "main" lens (the one carrying
 * the factory stereo block), scanning once if needed. It stands in for the
 * head tracker's camera: CNSDK uses the display config's trackingCameras[0]
 * (or debug.cnsdk.cam), which its public C API does not expose, and
 * leia_device_config_get_camera_data indexes cameras[], whose order differs
 * on some devices. False when there is no front pair.
 */
bool
leia_stereo_camera_android_tracking_lens(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif // XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA
