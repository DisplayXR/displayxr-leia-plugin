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
	uint32_t width, height; //!< per published eye, after the rotation
	double k[2][4];        //!< fx fy cx cy per eye, pixels at width x height
	double d[2][8];        //!< RADTAN8 per eye
	double R_rl[3][3];     //!< x_right = R_rl * x_left + T_rl
	double T_rl_mm[3];
	//! true = the SBS left eye is the factory "sub" lens, right = "main".
	bool left_is_sub;
};

/*!
 * Rescale the block from the full active array (@p array_w x @p array_h) to
 * the per-eye output the plug-in publishes, in two steps that mirror the
 * pipeline:
 *  1. array -> capture (@p cap_w x @p cap_h): the HAL produces a stream of a
 *     different aspect from the largest centred region of the array with the
 *     stream's aspect (scale to the width and crop rows when the stream is
 *     wider, scale to the height and crop columns when it is narrower);
 *  2. capture -> output (@p width x @p height): the plug-in's own centre crop,
 *     at even offsets (pack_eye).
 * @p cap_w / @p cap_h 0 = the output is captured directly (step 2 is a no-op).
 * Then orient the pair: the lens whose centre lies at -x of the other (the
 * camera's own left) becomes the left eye.
 *
 * The two steps matter on a sensor that is not 4:3: a phone-class panel's
 * 4576x3168 array captured at 1280x960 and cropped to 1280x720 differs from
 * "1280x720 straight from the array" by 8 % in focal length and ~53 px in the
 * principal point.
 *
 * Last, @p rot_cw (0, 90, 180 or 270) turns each eye clockwise by that angle
 * AFTER the crop, as pack_eye does: the published eye is then @p height x
 * @p width for 90 / 270, and the intrinsics, the tangential distortion, R and T
 * are turned with it (x' = Q x for both lenses, R' = Q R Q^T, T' = Q T). The
 * eye order is decided on the turned geometry. A lens pair whose baseline lies
 * along the sensor's y axis (a phone held upright) is published this way, so
 * that the runtime's rectifier sees a side-by-side pair.
 *
 * The rotation is the last step, so it applies to whatever R / T the caller
 * put in @p f (the factory block, or a later recalibration of R).
 */
bool
leia_scam_stream_calib_from_factory(const struct leia_scam_factory_calib *f,
                                    uint32_t array_w,
                                    uint32_t array_h,
                                    uint32_t cap_w,
                                    uint32_t cap_h,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t rot_cw,
                                    struct leia_scam_stream_calib *out);

/*!
 * true when the block's baseline lies along the sensor's y axis (|T.y| >
 * |T.x|): the pair is stacked vertically in the sensor frame and must be
 * published turned by 90 or 270 degrees.
 */
bool
leia_scam_factory_is_vertical(const struct leia_scam_factory_calib *f);

/*!
 * The size the factory block's intrinsics are expressed at. Usually the
 * active array; on some modules a listed output size instead (a phone-class
 * module's block centres on its largest 4:3 output 4192x3144, not on its
 * 4576x3168 array). Rule: keep the array unless both lenses' principal points
 * sit more than 3 % (of the width / height) off its centre AND one of the
 * @p n candidate sizes (@p sizes = w0 h0 w1 h1 ..., no larger than the array,
 * at least half its width) has them within 1 %; then the closest candidate
 * wins. Writes the array size when nothing better fits. Returns the
 * worst-lens relative offset of the choice.
 */
double
leia_scam_pick_calib_reference(const struct leia_scam_factory_calib *f,
                               uint32_t array_w,
                               uint32_t array_h,
                               const uint32_t *sizes,
                               size_t n,
                               uint32_t *out_w,
                               uint32_t *out_h);

/*!
 * Re-express the block's intrinsics, given at @p ref_w x @p ref_h, at the
 * active array: the inverse of the HAL's array -> stream step (largest centred
 * region of the stream's aspect, scaled). A no-op when the reference is the
 * array. R, T and distortion are unchanged.
 */
void
leia_scam_factory_to_array(const struct leia_scam_factory_calib *f,
                           uint32_t ref_w,
                           uint32_t ref_h,
                           uint32_t array_w,
                           uint32_t array_h,
                           struct leia_scam_factory_calib *out);

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
