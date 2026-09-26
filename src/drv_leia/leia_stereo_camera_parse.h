// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Stereo camera source (runtime ADR-043, L1): the platform-neutral
 *         pieces of the Leia SR provider — no Win32, no SR SDK, no OpenCV — so
 *         they are unit-tested on the host (tests/test_stereo_camera_parse.c,
 *         run by the Linux CI job) instead of only on an SR panel.
 *
 *  - the SR raw-camera shared-memory FRAME HEADER (a reader-defined contract,
 *    copied from the SR eyetracker's MediaFoundationCamera.cpp: four u64
 *    {rawImageFormat, compressedSize, height, width}, then one SBS JPEG at
 *    offset 32; no sequence, no timestamp) — validated field by field;
 *  - the JPEG SOF dimensions (header dims must equal the JPEG's own);
 *  - a new-frame fingerprint (the channel carries no sequence number);
 *  - the device-serial list in `Global\sharedDeviceSerialMemory` (1 count
 *    byte, then 32-byte NUL-terminated serials, primary device FIRST — the
 *    same rule the SR eyetracker's PredictingEyeTracker uses);
 *  - the small OpenCV FileStorage YAML subset of `intrinsics.yml`
 *    (M1 D1 M2 D2) and `extrinsics.yml` (R T), parsed without OpenCV;
 *  - eye order: which SBS half is the camera's LEFT lens, decided from the
 *    sign of T (OpenCV x_2 = R x_1 + T), overridable.
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *
 * Raw-camera frame header.
 *
 */

//! Bytes before the JPEG in the shared-memory block.
#define LEIA_SCAM_HEADER_SIZE 32u
//! Header rawImageFormat value: side-by-side MJPEG (the only one SR writes).
#define LEIA_SCAM_FORMAT_SBS_MJPEG 0u

struct leia_scam_header
{
	uint64_t raw_image_format;
	uint64_t compressed_size;
	uint64_t height;
	uint64_t width; //!< full SBS width
};

enum leia_scam_header_status
{
	//! A plausible frame is present.
	LEIA_SCAM_HEADER_OK = 0,
	//! compressedSize == 0: the tracker zeroes the block when its camera stops.
	LEIA_SCAM_HEADER_EMPTY = 1,
	//! Out of contract (unknown format, size past the region, silly extent).
	LEIA_SCAM_HEADER_BAD = 2,
};

/*!
 * Validate the header at the start of a mapped block of @p region_size bytes.
 * Copies the fields to @p out in every case (for diagnostics).
 */
enum leia_scam_header_status
leia_scam_header_check(const void *block, uint64_t region_size, struct leia_scam_header *out);

/*!
 * Width / height / component count from the first SOF marker of a JPEG.
 * @return false when @p jpg is not a JPEG or has no SOF before the scan.
 */
bool
leia_scam_jpeg_dims(const uint8_t *jpg, size_t size, uint32_t *out_w, uint32_t *out_h, uint32_t *out_components);

/*!
 * Content fingerprint used for new-frame detection: size + FNV-1a over the
 * head, middle and tail of the JPEG (an entropy-coded stream changes
 * everywhere when the image changes, and a sensor never repeats a frame
 * bit-exactly).
 */
uint64_t
leia_scam_fingerprint(const uint8_t *jpg, size_t size);


/*
 *
 * Device serials.
 *
 */

#define LEIA_SCAM_SERIAL_LEN 32u
#define LEIA_SCAM_SERIAL_MEMORY_SIZE (1u + 255u * LEIA_SCAM_SERIAL_LEN)

/*!
 * Parse the `Global\sharedDeviceSerialMemory` block. Writes up to @p cap
 * serials (NUL-terminated, trimmed) and returns how many the block holds.
 * Entry 0 is the PRIMARY device (SRService writes it first).
 */
uint32_t
leia_scam_parse_serials(const uint8_t *block, size_t size, char (*out)[LEIA_SCAM_SERIAL_LEN + 1], uint32_t cap);


/*
 *
 * Calibration (OpenCV FileStorage YAML subset).
 *
 */

/*!
 * Read the `data: [ ... ]` of the `!!opencv-matrix` named @p key. Doubles, up
 * to @p cap values; rows/cols from the node (0 if absent).
 * @return the number of values read, 0 on a missing / malformed node.
 */
uint32_t
leia_scam_yaml_matrix(
    const char *text, size_t len, const char *key, double *out, uint32_t cap, uint32_t *out_rows, uint32_t *out_cols);

//! A pair as the SR files describe it, camera 1 = M1/D1, camera 2 = M2/D2.
struct leia_scam_calibration
{
	double K[2][9];      //!< row-major 3x3
	double D[2][8];      //!< OpenCV order; zero-padded
	uint32_t d_count[2]; //!< coefficients present (4, 5, 8)
	double R[9];         //!< x_2 = R x_1 + T
	double T[3];         //!< mm
};

/*!
 * Parse intrinsics.yml + extrinsics.yml text. Refuses (false + @p why)
 * anything the runtime could not use faithfully: missing nodes, a non-3x3 K,
 * non-positive focal, a distortion vector longer than 8 with non-zero extra
 * terms (thin-prism / tilted models), a non-rotation R, a baseline outside
 * 5..500 mm.
 */
bool
leia_scam_parse_calibration(const char *intr,
                            size_t intr_len,
                            const char *extr,
                            size_t extr_len,
                            struct leia_scam_calibration *out,
                            char *why,
                            size_t why_len);

/*!
 * Eye order. The runtime wants SBS left half = the camera's LEFT lens, with
 * x_R = R x_L + T and T.x < 0. SR files name the cameras 1 and 2 (assumed:
 * camera 1 = the first, left, half of the SBS JPEG). If T.x > 0, camera 2 sits
 * at -x of camera 1, i.e. the left half holds the RIGHT lens: swap.
 * @p override_env: NULL/"auto" = decide from T; "0" = never; "1" = always.
 */
bool
leia_scam_decide_swap(const struct leia_scam_calibration *c, const char *override_env);

/*!
 * Per-eye numbers in the runtime's order (eye 0 = SBS left half AFTER any
 * swap): intrinsics fx fy cx cy, distortion model (1 = RADTAN5, 2 = RADTAN8)
 * + coefficients, and x_R = R x_L + T (mm).
 */
struct leia_scam_eye_calibration
{
	double k[2][4];
	uint32_t model;
	double d[2][8];
	double R[3][3];
	double T[3];
};

void
leia_scam_calibration_for_runtime(const struct leia_scam_calibration *c,
                                  bool swap,
                                  struct leia_scam_eye_calibration *out);

/*!
 * Swap the two halves of a GRAY8 SBS image in place (row by row).
 */
void
leia_scam_swap_halves_gray8(uint8_t *img, uint32_t width, uint32_t height, uint32_t pitch);

#ifdef __cplusplus
}
#endif
