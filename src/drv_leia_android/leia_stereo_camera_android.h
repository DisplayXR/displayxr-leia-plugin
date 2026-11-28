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
 *   debug.dxr.leia.scam_user_calib=0        ignore the calibration app's user rotation
 *                                           (factory R only; re-read on enumerate)
 *   spec v8 (modes, controls, stills): debug.dxr.leia.scam_modes, _max_stream, _max_still,
 *   _still, _still_template, _still_quality, _hal_af, _default_focus_d, _front_controls
 *   (docs/android-stereo-camera.md, "Knobs")
 *
 * The user rotation's last accepted record is cached in the host process's
 * files dir (leia_scam_user_calib_<pair>_<block sha>.json) and used, through
 * the same gate, while the calibration app's provider is unreachable.
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

/*
 * Spec v8 (runtime XR_DXR_stereo_camera §5.5-§5.7, §9b): each group only when
 * the runtime headers announce it. docs/android-stereo-camera.md, "Capture
 * modes, controls and stills".
 */
#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_MODES
uint32_t
leia_stereo_camera_android_enumerate_modes(struct xrt_plugin_instance *inst,
                                           uint32_t index,
                                           uint32_t capacity,
                                           struct xrt_plugin_stereo_camera_mode *out);

xrt_result_t
leia_stereo_camera_android_set_mode(struct xrt_plugin_stereo_camera *cam, uint32_t mode_index);
#endif

#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_CONTROLS
xrt_result_t
leia_stereo_camera_android_get_control_caps(struct xrt_plugin_instance *inst,
                                            uint32_t index,
                                            struct xrt_plugin_stereo_camera_control_caps *out);

xrt_result_t
leia_stereo_camera_android_set_controls(struct xrt_plugin_stereo_camera *cam,
                                        const struct xrt_plugin_stereo_camera_controls *controls);

xrt_result_t
leia_stereo_camera_android_get_capture_result(struct xrt_plugin_stereo_camera *cam,
                                              struct xrt_plugin_stereo_camera_capture_result *out);
#endif

#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_STILL
xrt_result_t
leia_stereo_camera_android_request_still(struct xrt_plugin_stereo_camera *cam,
                                         const struct xrt_plugin_stereo_camera_still_request *req);

uint32_t
leia_stereo_camera_android_poll_still(struct xrt_plugin_stereo_camera *cam,
                                      struct xrt_plugin_stereo_camera_still *out,
                                      struct xrt_plugin_stereo_camera_capture_result *out_result);

void
leia_stereo_camera_android_release_still(struct xrt_plugin_stereo_camera *cam);
#endif

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
