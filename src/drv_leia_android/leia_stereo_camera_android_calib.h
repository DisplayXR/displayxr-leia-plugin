// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Platform-neutral half of the Android stereo camera source: parse the
 *         OEM factory stereo-calibration block a Leia tablet publishes in its
 *         camera characteristics, and convert it to the runtime's RAW
 *         calibration for a given stream size.
 *
 * No Android, CNSDK or runtime dependency, so it is host-tested
 * (tests/test_stereo_camera_android_calib.c, Linux CI) like
 * ../drv_leia/leia_stereo_camera_parse.c. Layout: docs/android-stereo-camera.md.
 *
 * @ingroup drv_leia_android
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! One lens of the factory block, at the sensor's full active array.
struct leia_scam_lens
{
	double fx, fy, cx, cy;
	//! OpenCV order: k1 k2 p1 p2 k3 k4 k5 k6 (RADTAN8).
	double d[8];
};

/*!
 * The factory block as stored. "main" is the lens whose characteristics
 * carry the block (the one the vendor tracking service uses); R and T map
 * main-camera points into the other ("sub") lens: x_sub = R * x_main + T
 * (OpenCV stereoCalibrate with camera 1 = main), T in mm.
 */
struct leia_scam_factory_calib
{
	struct leia_scam_lens main, sub;
	double R[3][3];
	double T[3];
};

//! Smallest block the parser accepts (leading byte + main + sub + R + T).
#define LEIA_SCAM_FACTORY_BLOCK_MIN_BYTES 353u

/*!
 * Parse the block (byte 0 is ignored: it differs per device). Returns false
 * when it is too short or holds values no real calibration can have (non-finite,
 * non-positive focal length, a rotation that is not close to orthonormal, a
 * baseline outside 1..200 mm).
 */
bool
leia_scam_factory_calib_parse(const uint8_t *bytes, size_t len, struct leia_scam_factory_calib *out);

/*!
 * The RAW calibration the runtime wants, for ONE stream size, already in the
 * SBS order the plug-in publishes (left eye = the lens on the camera's own
 * left, spec §4).
 */
struct leia_scam_stream_calib
{
	uint32_t width, height;
	double k[2][4];        //!< fx fy cx cy per eye, pixels at width x height
	double d[2][8];        //!< RADTAN8 per eye
	double R_rl[3][3];     //!< x_right = R_rl * x_left + T_rl
	double T_rl_mm[3];
	//! true = the SBS left eye is the factory "sub" lens, right = "main".
	bool left_is_sub;
};

/*!
 * Rescale the block from the full active array (@p array_w x @p array_h) to a
 * stream of @p width x @p height that the HAL produces by scaling the array to
 * the stream width and centre-cropping the height (the default crop for a
 * wider-aspect output, e.g. 16:9 from a 4:3 sensor) — or, for a narrower
 * aspect, by scaling to the height and centre-cropping the width. Then
 * orient the pair: the lens whose centre lies at -x of the other (the
 * camera's own left) becomes the left eye.
 */
bool
leia_scam_stream_calib_from_factory(const struct leia_scam_factory_calib *f,
                                    uint32_t array_w,
                                    uint32_t array_h,
                                    uint32_t width,
                                    uint32_t height,
                                    struct leia_scam_stream_calib *out);

/*!
 * One lens's intrinsics rescaled to a stream (same crop rule as above): what
 * the vendor's in-app face tracker needs for the lens it is fed.
 */
void
leia_scam_lens_for_stream(const struct leia_scam_lens *l,
                          uint32_t array_w,
                          uint32_t array_h,
                          uint32_t width,
                          uint32_t height,
                          struct leia_scam_lens *out);

//! |T| in mm.
double
leia_scam_factory_baseline_mm(const struct leia_scam_factory_calib *f);

//! FNV-1a 64 of the block: a stable per-device key that exports nothing.
uint64_t
leia_scam_block_hash(const uint8_t *bytes, size_t len);

#ifdef __cplusplus
}
#endif
