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
 * Block layout. Every value is an IEEE-754 double, little-endian, starting at
 * byte 1. Byte 0 is NOT a version: it reads 1 on a Lume Pad 2 and 128 / 27 on a
 * nubia Pad 3D II (front / rear) with the same layout after it, so it is
 * ignored and the plausibility checks below decide.
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
	if (bytes == NULL || out == NULL || len < LEIA_SCAM_FACTORY_BLOCK_MIN_BYTES) {
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

//! Step 2: the plug-in's centre crop of a cap_w x cap_h image to width x height.
static void
lens_crop(struct leia_scam_lens *l, uint32_t cap_w, uint32_t cap_h, uint32_t width, uint32_t height)
{
	if (cap_w > width) {
		l->cx -= (double)(((cap_w - width) / 2u) & ~1u);
	}
	if (cap_h > height) {
		l->cy -= (double)(((cap_h - height) / 2u) & ~1u);
	}
}

/*!
 * Step 3: one lens of a W x H image (@p h = H) turned 90 degrees clockwise:
 * pixel (x, y) -> (H - 1 - y, x), camera axes X' = -Y, Y' = X. RADTAN's
 * tangential pair turns with them (p1' = p2, p2' = -p1); the radial terms are
 * invariant.
 */
static void
lens_rot90cw(struct leia_scam_lens *l, uint32_t h)
{
	const struct leia_scam_lens o = *l;
	l->fx = o.fy;
	l->fy = o.fx;
	l->cx = (double)h - 1.0 - o.cy;
	l->cy = o.cx;
	l->d[2] = o.d[3];
	l->d[3] = -o.d[2];
}

//! The extrinsics with both cameras turned by Q = [[0,-1,0],[1,0,0],[0,0,1]]: R' = Q R Q^T, T' = Q T.
static void
extr_rot90cw(double R[3][3], double T[3])
{
	static const double Q[3][3] = {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}};
	double QR[3][3], Rn[3][3], Tn[3];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			QR[i][j] = 0.0;
			for (int k = 0; k < 3; k++) {
				QR[i][j] += Q[i][k] * R[k][j];
			}
		}
	}
	for (int i = 0; i < 3; i++) {
		Tn[i] = 0.0;
		for (int j = 0; j < 3; j++) {
			Rn[i][j] = 0.0;
			for (int k = 0; k < 3; k++) {
				Rn[i][j] += QR[i][k] * Q[j][k];
			}
			Tn[i] += Q[i][j] * T[j];
		}
	}
	memcpy(R, Rn, sizeof(Rn));
	memcpy(T, Tn, sizeof(Tn));
}

bool
leia_scam_stream_calib_from_factory(const struct leia_scam_factory_calib *f,
                                    uint32_t array_w,
                                    uint32_t array_h,
                                    uint32_t cap_w,
                                    uint32_t cap_h,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t rot_cw,
                                    struct leia_scam_stream_calib *out)
{
	if (f == NULL || out == NULL || array_w == 0 || array_h == 0 || width == 0 || height == 0 ||
	    (rot_cw != 0 && rot_cw != 90 && rot_cw != 180 && rot_cw != 270)) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	if (cap_w == 0 || cap_h == 0) {
		cap_w = width;
		cap_h = height;
	}
	struct leia_scam_lens main_s, sub_s;
	leia_scam_lens_for_stream(&f->main, array_w, array_h, cap_w, cap_h, &main_s);
	leia_scam_lens_for_stream(&f->sub, array_w, array_h, cap_w, cap_h, &sub_s);
	lens_crop(&main_s, cap_w, cap_h, width, height);
	lens_crop(&sub_s, cap_w, cap_h, width, height);

	// Step 3: turn both eyes (and the extrinsics) with the published image.
	double R[3][3], T[3];
	memcpy(R, f->R, sizeof(R));
	memcpy(T, f->T, sizeof(T));
	uint32_t w = width, h = height;
	for (uint32_t a = 0; a < rot_cw; a += 90) {
		lens_rot90cw(&main_s, h);
		lens_rot90cw(&sub_s, h);
		extr_rot90cw(R, T);
		const uint32_t t = w;
		w = h;
		h = t;
	}
	out->width = w;
	out->height = h;

	// Where the sub lens sits in the main lens's frame: c = -R^T T.
	double c_sub_x = 0.0;
	for (int k = 0; k < 3; k++) {
		c_sub_x -= R[k][0] * T[k];
	}
	out->left_is_sub = c_sub_x < 0.0;
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
				out->R_rl[i][j] = R[j][i];
				t -= R[j][i] * T[j];
			}
			out->T_rl_mm[i] = t;
		}
	} else {
		memcpy(out->R_rl, R, sizeof(out->R_rl));
		memcpy(out->T_rl_mm, T, sizeof(out->T_rl_mm));
	}
	return true;
}

bool
leia_scam_factory_is_vertical(const struct leia_scam_factory_calib *f)
{
	return fabs(f->T[1]) > fabs(f->T[0]);
}

void
leia_scam_factory_swap_convention(struct leia_scam_factory_calib *f)
{
	double Rt[3][3], Tn[3];
	for (int i = 0; i < 3; i++) {
		Tn[i] = 0.0;
		for (int j = 0; j < 3; j++) {
			Rt[i][j] = f->R[j][i];
			Tn[i] -= f->R[j][i] * f->T[j];
		}
	}
	memcpy(f->R, Rt, sizeof(Rt));
	memcpy(f->T, Tn, sizeof(Tn));
}

//! (x, y) turned clockwise by @p deg (a multiple of 90), camera axes as extr_rot90cw: (x, y) -> (-y, x) per step.
static void
vec2_rot_cw(double v[2], int32_t deg)
{
	const int steps = (int)(((deg % 360) + 360) % 360) / 90;
	for (int s = 0; s < steps; s++) {
		const double x = v[0];
		v[0] = -v[1];
		v[1] = x;
	}
}

int
leia_scam_pose_sub_side(const double main_t[3],
                        const double sub_t[3],
                        bool front,
                        int32_t sensor_orientation,
                        uint32_t rot_cw)
{
	if (main_t == NULL || sub_t == NULL || sensor_orientation < 0 || sensor_orientation % 90 != 0 ||
	    rot_cw % 90 != 0) {
		return 0;
	}
	const double dx = sub_t[0] - main_t[0], dy = sub_t[1] - main_t[1];
	if (!isfinite(dx) || !isfinite(dy)) {
		return 0;
	}
	// Upright camera image (y down); a front lens sees the scene mirrored left/right
	// relative to the user facing the screen.
	double v[2] = {front ? -dx : dx, -dy};
	vec2_rot_cw(v, (int32_t)rot_cw - sensor_orientation);
	const double len_mm = 1000.0 * sqrt(v[0] * v[0] + v[1] * v[1]);
	if (!(len_mm >= 1.0 && len_mm <= 200.0) || fabs(v[0]) < fabs(v[1])) {
		return 0;
	}
	return v[0] > 0.0 ? 1 : -1;
}

int
leia_scam_factory_orient_by_pose(struct leia_scam_factory_calib *f,
                                 const double main_t[3],
                                 const double sub_t[3],
                                 bool front,
                                 int32_t sensor_orientation,
                                 uint32_t rot_cw)
{
	const int pose = leia_scam_pose_sub_side(main_t, sub_t, front, sensor_orientation, rot_cw);
	if (f == NULL || pose == 0) {
		return -1;
	}
	// The block's sub centre in the main frame, c = -R^T T, turned with the image.
	double c[3] = {0.0, 0.0, 0.0};
	for (int i = 0; i < 3; i++) {
		for (int k = 0; k < 3; k++) {
			c[i] -= f->R[k][i] * f->T[k];
		}
	}
	vec2_rot_cw(c, (int32_t)rot_cw);
	const int block = c[0] > 0.0 ? 1 : -1;
	if (block == pose) {
		return 0;
	}
	leia_scam_factory_swap_convention(f);
	return 1;
}

int32_t
leia_scam_frame_orientation_deg(bool have_sensor_orientation, int32_t sensor_orientation, uint32_t rot)
{
	if (!have_sensor_orientation || sensor_orientation < 0 || sensor_orientation >= 360 ||
	    sensor_orientation % 90 != 0) {
		return -1;
	}
	if (rot != 0 && rot != 90 && rot != 180 && rot != 270) {
		return -1;
	}
	return (sensor_orientation - (int32_t)rot + 360) % 360;
}

//! Worst relative principal-point offset of both lenses from the centre of w x h.
static double
pp_offset(const struct leia_scam_factory_calib *f, double w, double h)
{
	const struct leia_scam_lens *ls[2] = {&f->main, &f->sub};
	double worst = 0.0;
	for (int i = 0; i < 2; i++) {
		const double ox = fabs(ls[i]->cx - w / 2.0) / w, oy = fabs(ls[i]->cy - h / 2.0) / h;
		worst = fmax(worst, fmax(ox, oy));
	}
	return worst;
}

double
leia_scam_pick_calib_reference(const struct leia_scam_factory_calib *f,
                               uint32_t array_w,
                               uint32_t array_h,
                               const uint32_t *sizes,
                               size_t n,
                               uint32_t *out_w,
                               uint32_t *out_h)
{
	*out_w = array_w;
	*out_h = array_h;
	if (f == NULL || array_w == 0 || array_h == 0) {
		return 0.0;
	}
	const double at_array = pp_offset(f, (double)array_w, (double)array_h);
	if (at_array <= 0.03 || sizes == NULL) {
		return at_array;
	}
	double best = 0.01;
	bool found = false;
	for (size_t i = 0; i < n; i++) {
		const uint32_t w = sizes[2 * i], h = sizes[2 * i + 1];
		if (w == 0 || h == 0 || w > array_w || h > array_h || 2 * w < array_w) {
			continue;
		}
		const double o = pp_offset(f, (double)w, (double)h);
		if (o < best) {
			best = o;
			*out_w = w;
			*out_h = h;
			found = true;
		}
	}
	return found ? best : at_array;
}

void
leia_scam_factory_to_array(const struct leia_scam_factory_calib *f,
                           uint32_t ref_w,
                           uint32_t ref_h,
                           uint32_t array_w,
                           uint32_t array_h,
                           struct leia_scam_factory_calib *out)
{
	*out = *f;
	if (ref_w == 0 || ref_h == 0 || array_w == 0 || array_h == 0 || (ref_w == array_w && ref_h == array_h)) {
		return;
	}
	const double aw = (double)array_w, ah = (double)array_h, w = (double)ref_w, h = (double)ref_h;
	struct leia_scam_lens *ls[2] = {&out->main, &out->sub};
	for (int i = 0; i < 2; i++) {
		struct leia_scam_lens *l = ls[i];
		if (w * ah >= h * aw) {
			// The reference is at least as wide: it was scaled from the full width.
			const double s = w / aw;
			l->fx /= s;
			l->fy /= s;
			l->cx /= s;
			l->cy = (l->cy - h / 2.0) / s + ah / 2.0;
		} else {
			// Narrower: scaled from the full height, columns cropped.
			const double s = h / ah;
			l->fx /= s;
			l->fy /= s;
			l->cx = (l->cx - w / 2.0) / s + aw / 2.0;
			l->cy /= s;
		}
	}
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
