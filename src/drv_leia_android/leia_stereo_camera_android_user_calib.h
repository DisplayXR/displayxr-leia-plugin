// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Platform-neutral half of the user stereo calibration: the record the
 *         Leia calibration app publishes for one logical stereo pair (schema 1),
 *         its parse, and the checks that decide whether its rotation replaces
 *         the factory block's R.
 *
 * The record arrives as one JSON string from the calibration app's read-only
 * ContentProvider (content://com.leiainc.calibrationapp.stereo/pairs/<id>,
 * column "json"; no row = use the factory block). Only R_1_from_0 is used:
 * the intrinsics, T and the block's reference size stay the plug-in's own.
 * R_1_from_0 is in the OpenCV camera frame of each sensor's NATIVE (unturned)
 * orientation, with x1 = R x0 + T, lens0 = the block's main lens and lens1 =
 * the sub lens, so it is a drop-in for leia_scam_factory_calib::R and goes in
 * BEFORE any eye turn (leia_scam_stream_calib_from_factory turns last).
 *
 * No Android or runtime dependency: host-tested by
 * tests/test_stereo_camera_android_user_calib.c (Linux CI).
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

//! Largest angle (degrees) a user rotation may differ from the factory R.
#define LEIA_SCAM_USER_CALIB_MAX_DELTA_DEG 5.0

//! The fields of a schema-1 record the plug-in reads; everything else is ignored.
struct leia_scam_user_calib
{
	int schema;              //!< -1 when absent
	char logical_id[16];     //!< pair.logicalCameraId
	char lens_ids[2][16];    //!< pair.lensIds: [0] = lens0 (main), [1] = lens1 (sub)
	bool has_lens_ids;
	char block_sha256[65];   //!< factory.blockSha256, lowercase hex
	double R[3][3];          //!< R_1_from_0, row major
	bool has_R;
	char timestamp[40];      //!< informational
	char method[40];         //!< informational
};

/*!
 * Parse @p len bytes of JSON. Returns false when it is not a JSON object or a
 * field the plug-in reads has the wrong type; missing fields are left empty
 * (schema = -1, has_R = false) and rejected by leia_scam_user_calib_check.
 */
bool
leia_scam_user_calib_parse(const char *json, size_t len, struct leia_scam_user_calib *out);

enum leia_scam_user_calib_verdict
{
	LEIA_SCAM_USER_CALIB_OK = 0,
	LEIA_SCAM_USER_CALIB_BAD_SCHEMA,     //!< schema != 1
	LEIA_SCAM_USER_CALIB_OTHER_PAIR,     //!< pair.logicalCameraId differs
	LEIA_SCAM_USER_CALIB_OTHER_LENSES,   //!< pair.lensIds != [main, sub]
	LEIA_SCAM_USER_CALIB_BLOCK_CHANGED,  //!< factory.blockSha256 != SHA-256 of the block read
	LEIA_SCAM_USER_CALIB_NO_ROTATION,    //!< R_1_from_0 missing
	LEIA_SCAM_USER_CALIB_NOT_ROTATION,   //!< not orthonormal / det != 1 / non-finite
	LEIA_SCAM_USER_CALIB_TOO_FAR,        //!< more than LEIA_SCAM_USER_CALIB_MAX_DELTA_DEG from the factory R
};

//! Short reason string for a log line.
const char *
leia_scam_user_calib_verdict_str(enum leia_scam_user_calib_verdict v);

/*!
 * Decide whether @p u applies to the pair @p logical_id whose factory block
 * (the exact bytes the plug-in read) hashes to @p block_sha256 and whose block
 * rotation is @p R_factory. @p main_id / @p sub_id are checked against
 * pair.lensIds when the record carries them (NULL skips that check).
 * @p out_delta_deg (optional) receives the angle between the user and the
 * factory rotation, when both are rotations.
 */
enum leia_scam_user_calib_verdict
leia_scam_user_calib_check(const struct leia_scam_user_calib *u,
                           const char *logical_id,
                           const char *main_id,
                           const char *sub_id,
                           const uint8_t block_sha256[32],
                           const double R_factory[3][3],
                           double *out_delta_deg);

//! SHA-256 of @p len bytes.
void
leia_scam_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

//! @p digest as 64 lowercase hex chars + NUL.
void
leia_scam_sha256_hex(const uint8_t digest[32], char out[65]);

#ifdef __cplusplus
}
#endif
