// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Factory stereo-calibration block: parse + convert. See the header.
 * @ingroup drv_leia_android
 */

#include "leia_stereo_camera_android_calib.h"

#include <math.h>
#include <string.h>

/*
 * Block layout (version 1). Every value is an IEEE-754 double, little-endian,
 * starting at byte 1 (byte 0 is the version):
 *
 *   1   + 8*i, i = 0..15   main lens: fx fy cx cy k1 k2 p1 p2 k3 k4 k5 k6 s1..s4
 *   129 + 8*i, i = 0..15   sub lens, same order
 *   257 + 8*i, i = 0..8    R, row-major
 *   329 + 8*i, i = 0..2    T (mm)
 *
 * Anything after byte 352 (a precomputed rectification on some devices) is
 * ignored: the runtime rectifies from the RAW calibration itself.
 */
#define OFF_MAIN 1u
#define OFF_SUB 129u
#define OFF_R 257u
#define OFF_T 329u

static double
rd(const uint8_t *b, size_t off)
{
	uint64_t u = 0;
	for (int i = 7; i >= 0; i--) {
		u = (u << 8) | b[off + (size_t)i];
	}
	double v;
	memcpy(&v, &u, sizeof(v));
	return v;
}

static bool
lens_parse(const uint8_t *b, size_t off, struct leia_scam_lens *l)
{
	l->fx = rd(b, off + 0);
	l->fy = rd(b, off + 8);
	l->cx = rd(b, off + 16);
	l->cy = rd(b, off + 24);
	for (int i = 0; i < 8; i++) {
		l->d[i] = rd(b, off + 32 + 8 * (size_t)i);
	}
	if (!isfinite(l->fx) || !isfinite(l->fy) || !isfinite(l->cx) || !isfinite(l->cy) || l->fx <= 1.0 ||
	    l->fy <= 1.0 || l->cx <= 0.0 || l->cy <= 0.0) {
		return false;
	}
	for (int i = 0; i < 8; i++) {
		if (!isfinite(l->d[i]) || fabs(l->d[i]) > 1e4) {
			return false;
		}
	}
	return true;
}

bool
leia_scam_factory_calib_parse(const uint8_t *bytes, size_t len, struct leia_scam_factory_calib *out)
{
	if (bytes == NULL || out == NULL || len < LEIA_SCAM_FACTORY_BLOCK_MIN_BYTES || bytes[0] != 1) {
		return false;
	}
	struct leia_scam_factory_calib c;
	memset(&c, 0, sizeof(c));
	if (!lens_parse(bytes, OFF_MAIN, &c.main) || !lens_parse(bytes, OFF_SUB, &c.sub)) {
		return false;
	}
	for (int i = 0; i < 9; i++) {
		c.R[i / 3][i % 3] = rd(bytes, OFF_R + 8 * (size_t)i);
	}
	for (int i = 0; i < 3; i++) {
		c.T[i] = rd(bytes, OFF_T + 8 * (size_t)i);
	}
	// R must be a rotation: R * R^T ~ I.
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			double s = 0.0;
			for (int k = 0; k < 3; k++) {
				s += c.R[i][k] * c.R[j][k];
			}
			if (!isfinite(s) || fabs(s - (i == j ? 1.0 : 0.0)) > 1e-3) {
				return false;
			}
		}
	}
	const double b = leia_scam_factory_baseline_mm(&c);
	if (!isfinite(b) || b < 1.0 || b > 200.0) {
		return false;
	}
	*out = c;
	return true;
}

double
leia_scam_factory_baseline_mm(const struct leia_scam_factory_calib *f)
{
	return sqrt(f->T[0] * f->T[0] + f->T[1] * f->T[1] + f->T[2] * f->T[2]);
}

void
leia_scam_lens_for_stream(const struct leia_scam_lens *l,
                          uint32_t array_w,
                          uint32_t array_h,
                          uint32_t width,
                          uint32_t height,
                          struct leia_scam_lens *out)
{
	*out = *l;
	if (array_w == 0 || array_h == 0 || width == 0 || height == 0) {
		return;
	}
	const double aw = (double)array_w, ah = (double)array_h, w = (double)width, h = (double)height;
	if (w * ah >= h * aw) {
		// Stream at least as wide as the array: scale to the width, centre-crop the height.
		const double s = w / aw;
		out->fx = l->fx * s;
		out->fy = l->fy * s;
		out->cx = l->cx * s;
		out->cy = (l->cy - ah / 2.0) * s + h / 2.0;
	} else {
		// Narrower: scale to the height, centre-crop the width.
		const double s = h / ah;
		out->fx = l->fx * s;
		out->fy = l->fy * s;
		out->cx = (l->cx - aw / 2.0) * s + w / 2.0;
		out->cy = l->cy * s;
	}
}

bool
leia_scam_stream_calib_from_factory(const struct leia_scam_factory_calib *f,
                                    uint32_t array_w,
                                    uint32_t array_h,
                                    uint32_t width,
                                    uint32_t height,
                                    struct leia_scam_stream_calib *out)
{
	if (f == NULL || out == NULL || array_w == 0 || array_h == 0 || width == 0 || height == 0) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->width = width;
	out->height = height;

	// Where the sub lens sits in the main lens's frame: c = -R^T T.
	double c_sub_x = 0.0;
	for (int k = 0; k < 3; k++) {
		c_sub_x -= f->R[k][0] * f->T[k];
	}
	out->left_is_sub = c_sub_x < 0.0;

	struct leia_scam_lens main_s, sub_s;
	leia_scam_lens_for_stream(&f->main, array_w, array_h, width, height, &main_s);
	leia_scam_lens_for_stream(&f->sub, array_w, array_h, width, height, &sub_s);
	const struct leia_scam_lens *L = out->left_is_sub ? &sub_s : &main_s;
	const struct leia_scam_lens *Rt = out->left_is_sub ? &main_s : &sub_s;
	const struct leia_scam_lens *eyes[2] = {L, Rt};
	for (int e = 0; e < 2; e++) {
		out->k[e][0] = eyes[e]->fx;
		out->k[e][1] = eyes[e]->fy;
		out->k[e][2] = eyes[e]->cx;
		out->k[e][3] = eyes[e]->cy;
		memcpy(out->d[e], eyes[e]->d, sizeof(out->d[e]));
	}

	if (out->left_is_sub) {
		// right = main, left = sub: x_main = R^T x_sub - R^T T.
		for (int i = 0; i < 3; i++) {
			double t = 0.0;
			for (int j = 0; j < 3; j++) {
				out->R_rl[i][j] = f->R[j][i];
				t -= f->R[j][i] * f->T[j];
			}
			out->T_rl_mm[i] = t;
		}
	} else {
		memcpy(out->R_rl, f->R, sizeof(out->R_rl));
		memcpy(out->T_rl_mm, f->T, sizeof(out->T_rl_mm));
	}
	return true;
}

uint64_t
leia_scam_block_hash(const uint8_t *bytes, size_t len)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < len; i++) {
		h ^= bytes[i];
		h *= 1099511628211ull;
	}
	return h;
}
