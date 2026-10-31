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

/*
 *
 * Provider vs cache.
 *
 * The provider can be unreachable while the calibration app's process is not
 * running (an OEM auto-start policy refuses to start it for a provider query).
 * The plug-in therefore keeps a copy of the last record it accepted, in its
 * host process's own files, and falls back to it. The copy passes the SAME
 * gate as a fresh record (schema, pair, lenses, block SHA-256, rotation), so a
 * re-burned block or another pair's record never revives through it.
 *
 */

//! What the provider answered.
enum leia_scam_user_calib_provider
{
	LEIA_SCAM_PROVIDER_RECORD,      //!< a row: @p provider_json holds it
	LEIA_SCAM_PROVIDER_NO_RECORD,   //!< authoritative "nothing saved" (no row, or the app is not installed)
	LEIA_SCAM_PROVIDER_UNAVAILABLE, //!< no answer (unreachable, query failed): only then is the cache used
};

enum leia_scam_user_calib_source
{
	LEIA_SCAM_USER_CALIB_SRC_FACTORY = 0,
	LEIA_SCAM_USER_CALIB_SRC_PROVIDER,
	LEIA_SCAM_USER_CALIB_SRC_CACHE,
};

struct leia_scam_user_calib_choice
{
	enum leia_scam_user_calib_source source;
	//! Gate result of the record considered (provider's, else the cache's);
	//! LEIA_SCAM_USER_CALIB_OK when one is used.
	enum leia_scam_user_calib_verdict verdict;
	bool considered;   //!< a record was looked at (else verdict is meaningless)
	bool parse_failed; //!< the record looked at does not parse
	double delta_deg;
	//! The record considered (used when source != FACTORY; else only for the log).
	struct leia_scam_user_calib rec;
	bool cache_write; //!< store @p provider_json as the new cache copy
	bool cache_drop;  //!< the provider says the cache is no longer valid: delete it
};

/*!
 * Pick the rotation source for one pair:
 *  - provider RECORD that passes the gate -> PROVIDER, and refresh the cache;
 *  - provider RECORD that fails it, or NO_RECORD -> FACTORY, and drop the cache
 *    (the provider is authoritative when it answers);
 *  - provider UNAVAILABLE -> CACHE when @p cache_json passes the gate, else FACTORY.
 * @p cache_json NULL / @p cache_len 0 = no cache.
 */
void
leia_scam_user_calib_choose(enum leia_scam_user_calib_provider provider,
                            const char *provider_json,
                            size_t provider_len,
                            const char *cache_json,
                            size_t cache_len,
                            const char *logical_id,
                            const char *main_id,
                            const char *sub_id,
                            const uint8_t block_sha256[32],
                            const double R_factory[3][3],
                            struct leia_scam_user_calib_choice *out);

//! SHA-256 of @p len bytes.
void
leia_scam_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

//! @p digest as 64 lowercase hex chars + NUL.
void
leia_scam_sha256_hex(const uint8_t digest[32], char out[65]);

#ifdef __cplusplus
}
#endif
