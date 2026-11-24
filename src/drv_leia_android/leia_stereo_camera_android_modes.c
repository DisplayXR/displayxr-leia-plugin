// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Spec v8 modes, metering, controls and capture results. See the header.
 * @ingroup drv_leia_android
 */

#include "leia_stereo_camera_android_modes.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void
leia_scam_stream_region(uint32_t array_w,
                        uint32_t array_h,
                        uint32_t stream_w,
                        uint32_t stream_h,
                        struct leia_scam_region *out)
{
	const double aw = (double)array_w, ah = (double)array_h, w = (double)stream_w, h = (double)stream_h;
	if (array_w == 0 || array_h == 0 || stream_w == 0 || stream_h == 0) {
		out->x0 = out->y0 = 0.0;
		out->w = aw;
		out->h = ah;
		return;
	}
	if (w * ah >= h * aw) {
		// At least as wide as the array: every column, the centred rows.
		out->w = aw;
		out->h = aw * h / w;
		out->x0 = 0.0;
		out->y0 = (ah - out->h) / 2.0;
	} else {
		// Narrower: every row, the centred columns.
		out->h = ah;
		out->w = ah * w / h;
		out->y0 = 0.0;
		out->x0 = (aw - out->w) / 2.0;
	}
}

void
leia_scam_eye_size(const struct leia_scam_eye_geom *g, uint32_t *out_w, uint32_t *out_h)
{
	const bool quarter = g->rot == 90 || g->rot == 270;
	*out_w = quarter ? g->crop_h : g->crop_w;
	*out_h = quarter ? g->crop_w : g->crop_h;
}

//! The array columns the published eye of @p g covers (rot 0 / 180 only).
static double
eye_field_width(uint32_t array_w, uint32_t array_h, const struct leia_scam_eye_geom *g)
{
	struct leia_scam_region r;
	leia_scam_stream_region(array_w, array_h, g->cap_w, g->cap_h, &r);
	return r.w * (double)g->crop_w / (double)g->cap_w;
}

//! Candidate modes of one size (rows: all, and optionally the default's aspect).
static size_t
candidates_of(uint32_t array_w,
              uint32_t array_h,
              double field0,
              const struct leia_scam_mode *m0,
              uint32_t w,
              uint32_t h,
              bool default_aspect_too,
              struct leia_scam_mode out[2])
{
	if (w == 0 || h == 0 || (h & 1u) != 0) {
		return 0;
	}
	struct leia_scam_region r;
	leia_scam_stream_region(array_w, array_h, w, h, &r);
	const double s = (double)w / r.w;
	const double ew_exact = field0 * s;
	const double ew_round = floor(ew_exact + 0.5);
	if (fabs(ew_exact - ew_round) > 1e-6 || ew_round < 2.0 || ew_round > (double)w) {
		return 0; // the default's field is not a whole number of this size's columns
	}
	const uint32_t ew = (uint32_t)ew_round;
	if ((ew & 1u) != 0 || ((w - ew) % 4u) != 0 || ew < m0->eye_w) {
		return 0; // NV12: even width, even centred offset; smaller ones the runtime scales itself
	}
	const double rel0 = (double)m0->eye_h / (double)m0->eye_w;
	uint32_t heights[2] = {h, 0};
	size_t nh = 1;
	if (default_aspect_too) {
		// The default's aspect cut from the centre rows: exact, even, even offset.
		const double eh_exact = (double)ew * rel0;
		const double eh_round = floor(eh_exact + 0.5);
		if (fabs(eh_exact - eh_round) < 1e-6 && eh_round >= 2.0 && eh_round < (double)h) {
			const uint32_t eh = (uint32_t)eh_round;
			if ((eh & 1u) == 0 && ((h - eh) % 4u) == 0) {
				heights[nh++] = eh;
			}
		}
	}
	size_t n = 0;
	for (size_t i = 0; i < nh; i++) {
		const uint32_t eh = heights[i];
		const double rel = ((double)eh / (double)ew) / rel0;
		if (rel < 0.5 || rel > 2.0 || (ew == m0->eye_w && eh == m0->eye_h)) {
			continue;
		}
		struct leia_scam_mode m;
		memset(&m, 0, sizeof(m));
		m.eye_w = ew;
		m.eye_h = eh;
		m.geom.cap_w = w;
		m.geom.cap_h = h;
		m.geom.crop_w = ew;
		m.geom.crop_h = eh;
		m.geom.rot = 0;
		out[n++] = m;
	}
	return n;
}

static bool
mode_listed(const struct leia_scam_mode *out, size_t n, uint32_t w, uint32_t h)
{
	for (size_t i = 0; i < n; i++) {
		if (out[i].eye_w == w && out[i].eye_h == h) {
			return true;
		}
	}
	return false;
}

size_t
leia_scam_build_modes(uint32_t array_w,
                      uint32_t array_h,
                      const struct leia_scam_eye_geom *def,
                      const struct leia_scam_size *stream_sizes,
                      size_t n_stream_sizes,
                      const struct leia_scam_size *still_sizes,
                      size_t n_still_sizes,
                      const struct leia_scam_mode_policy *pol,
                      struct leia_scam_mode *out,
                      size_t cap)
{
	if (def == NULL || pol == NULL || out == NULL || cap == 0 || def->cap_w == 0 || def->cap_h == 0 ||
	    def->crop_w == 0 || def->crop_h == 0) {
		return 0;
	}
	struct leia_scam_mode m0;
	memset(&m0, 0, sizeof(m0));
	leia_scam_eye_size(def, &m0.eye_w, &m0.eye_h);
	m0.geom = *def;
	m0.flags = LEIA_SCAM_MODE_STREAM | LEIA_SCAM_MODE_STILL;
	m0.source = LEIA_SCAM_SRC_DEFAULT_CAPTURE;
	m0.max_frame_rate = pol->default_rate;
	out[0] = m0;
	if (def->rot != 0 || array_w == 0 || array_h == 0) {
		return 1;
	}
	const double field0 = eye_field_width(array_w, array_h, def);

	struct leia_scam_mode list[LEIA_SCAM_MODES_MAX * 2];
	size_t nl = 0;
	struct leia_scam_mode c2[2];

	// 1. The default's own capture: every other cut of it is free.
	size_t k = candidates_of(array_w, array_h, field0, &m0, def->cap_w, def->cap_h, true, c2);
	for (size_t i = 0; i < k; i++) {
		c2[i].flags = LEIA_SCAM_MODE_STREAM | LEIA_SCAM_MODE_STILL;
		c2[i].source = LEIA_SCAM_SRC_DEFAULT_CAPTURE;
		c2[i].max_frame_rate = pol->default_rate;
		if (!mode_listed(list, nl, c2[i].eye_w, c2[i].eye_h) && nl < sizeof(list) / sizeof(list[0])) {
			list[nl++] = c2[i];
		}
	}

	// 2. One more stream capture: the largest that fits the cap and runs at the rate.
	if (pol->reconfigure_stream && stream_sizes != NULL) {
		int best = -1;
		for (size_t i = 0; i < n_stream_sizes; i++) {
			const struct leia_scam_size *z = &stream_sizes[i];
			if ((z->w == def->cap_w && z->h == def->cap_h) || z->min_frame_duration_ns <= 0 ||
			    z->min_frame_duration_ns > pol->max_stream_frame_ns ||
			    candidates_of(array_w, array_h, field0, &m0, z->w, z->h, false, c2) == 0 ||
			    c2[0].eye_w > pol->max_stream_w || c2[0].eye_h > pol->max_stream_h) {
				continue;
			}
			if (best < 0 || (double)z->w * z->h > (double)stream_sizes[best].w * stream_sizes[best].h) {
				best = (int)i;
			}
		}
		if (best >= 0) {
			k = candidates_of(array_w, array_h, field0, &m0, stream_sizes[best].w, stream_sizes[best].h, true,
			                  c2);
			for (size_t i = 0; i < k; i++) {
				c2[i].flags = LEIA_SCAM_MODE_STREAM;
				c2[i].source = LEIA_SCAM_SRC_OWN_CAPTURE;
				c2[i].max_frame_rate = pol->own_rate;
				if (!mode_listed(list, nl, c2[i].eye_w, c2[i].eye_h) && nl < sizeof(list) / sizeof(list[0])) {
					list[nl++] = c2[i];
				}
			}
		}
	}

	// 3. The still output: the largest full-rows size within the still cap.
	if (pol->still_output && still_sizes != NULL) {
		struct leia_scam_mode best;
		bool have = false;
		for (size_t i = 0; i < n_still_sizes; i++) {
			if (candidates_of(array_w, array_h, field0, &m0, still_sizes[i].w, still_sizes[i].h, false, c2) == 0) {
				continue;
			}
			const struct leia_scam_mode *m = &c2[0];
			if ((pol->max_still_w != 0 && m->eye_w > pol->max_still_w) ||
			    (pol->max_still_h != 0 && m->eye_h > pol->max_still_h)) {
				continue;
			}
			if (!have || (double)m->eye_w * m->eye_h > (double)best.eye_w * best.eye_h ||
			    ((double)m->eye_w * m->eye_h == (double)best.eye_w * best.eye_h &&
			     (double)m->geom.cap_w * m->geom.cap_h < (double)best.geom.cap_w * best.geom.cap_h)) {
				best = *m;
				have = true;
			}
		}
		if (have && !mode_listed(list, nl, best.eye_w, best.eye_h) && nl < sizeof(list) / sizeof(list[0])) {
			best.flags = LEIA_SCAM_MODE_STILL;
			best.source = LEIA_SCAM_SRC_STILL_OUTPUT;
			best.max_frame_rate = 0.0f;
			list[nl++] = best;
		}
	}

	// Mode 0, then by area.
	size_t n = 1;
	while (n < cap) {
		int pick = -1;
		for (size_t i = 0; i < nl; i++) {
			if (list[i].flags == 0) {
				continue;
			}
			if (pick < 0 || (double)list[i].eye_w * list[i].eye_h < (double)list[pick].eye_w * list[pick].eye_h) {
				pick = (int)i;
			}
		}
		if (pick < 0) {
			break;
		}
		out[n++] = list[pick];
		list[pick].flags = 0;
	}
	return n;
}

int
leia_scam_find_still_mode(const struct leia_scam_mode *modes, size_t n, uint32_t w, uint32_t h)
{
	int best = -1;
	for (size_t i = 0; i < n; i++) {
		if ((modes[i].flags & LEIA_SCAM_MODE_STILL) == 0) {
			continue;
		}
		if (w == 0 && h == 0) {
			if (best < 0 || (double)modes[i].eye_w * modes[i].eye_h >
			                    (double)modes[best].eye_w * modes[best].eye_h) {
				best = (int)i;
			}
		} else if (modes[i].eye_w == w && modes[i].eye_h == h) {
			return (int)i;
		}
	}
	return best;
}

bool
leia_scam_metering_to_array(double u,
                            double v,
                            uint32_t array_w,
                            uint32_t array_h,
                            const struct leia_scam_eye_geom *g,
                            double *out_nx,
                            double *out_ny)
{
	if (g == NULL || array_w == 0 || array_h == 0 || g->cap_w == 0 || g->cap_h == 0 || g->crop_w == 0 ||
	    g->crop_h == 0 || g->crop_w > g->cap_w || g->crop_h > g->cap_h ||
	    (g->rot != 0 && g->rot != 90 && g->rot != 180 && g->rot != 270)) {
		return false;
	}
	uint32_t ew, eh;
	leia_scam_eye_size(g, &ew, &eh);
	u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
	v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
	const double X = u * ew, Y = v * eh, cw = g->crop_w, ch = g->crop_h;
	// Undo the clockwise turn (pack_eye_turned, edge coordinates): one 90 deg
	// step takes crop (x, y) to (ch - y, x).
	double x, y;
	switch (g->rot) {
	case 90:
		x = Y;
		y = ch - X;
		break;
	case 180:
		x = cw - X;
		y = ch - Y;
		break;
	case 270:
		x = cw - Y;
		y = X;
		break;
	default:
		x = X;
		y = Y;
		break;
	}
	// The centred crop's offsets (pack_eye), then the HAL's array -> stream step.
	x += (double)(((g->cap_w - g->crop_w) / 2u) & ~1u);
	y += (double)(((g->cap_h - g->crop_h) / 2u) & ~1u);
	struct leia_scam_region r;
	leia_scam_stream_region(array_w, array_h, g->cap_w, g->cap_h, &r);
	const double ax = r.x0 + x * r.w / (double)g->cap_w;
	const double ay = r.y0 + y * r.h / (double)g->cap_h;
	*out_nx = ax / (double)array_w;
	*out_ny = ay / (double)array_h;
	return true;
}

void
leia_scam_metering_region(double nx, double ny, uint32_t log_w, uint32_t log_h, double frac, int32_t out[5])
{
	const double side = (log_w < log_h ? (double)log_w : (double)log_h) * frac;
	int32_t s = (int32_t)floor(side + 0.5);
	if (s < 2) {
		s = 2;
	}
	if (s > (int32_t)log_w) {
		s = (int32_t)log_w;
	}
	if (s > (int32_t)log_h) {
		s = (int32_t)log_h;
	}
	int32_t x0 = (int32_t)floor(nx * (double)log_w - s / 2.0 + 0.5);
	int32_t y0 = (int32_t)floor(ny * (double)log_h - s / 2.0 + 0.5);
	if (x0 < 0) {
		x0 = 0;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (x0 + s > (int32_t)log_w) {
		x0 = (int32_t)log_w - s;
	}
	if (y0 + s > (int32_t)log_h) {
		y0 = (int32_t)log_h - s;
	}
	out[0] = x0;
	out[1] = y0;
	out[2] = x0 + s;
	out[3] = y0 + s;
	out[4] = 1000;
}

static bool
has_af(const struct leia_scam_hal_caps *hal, uint8_t m)
{
	for (size_t i = 0; i < hal->n_af_modes; i++) {
		if (hal->af_modes[i] == m) {
			return true;
		}
	}
	return false;
}

static double
ev_step(const struct leia_scam_hal_caps *hal)
{
	return hal->ae_comp_step_den > 0 && hal->ae_comp_step_num > 0
	           ? (double)hal->ae_comp_step_num / (double)hal->ae_comp_step_den
	           : 0.0;
}

void
leia_scam_control_caps_from_hal(const struct leia_scam_hal_caps *hal,
                                bool hal_af,
                                bool tracking_shared,
                                bool allow_exposure_on_shared,
                                struct leia_scam_control_caps *out)
{
	memset(out, 0, sizeof(*out));
	if (hal == NULL) {
		return;
	}
	const bool focuses = hal->have_min_focus_distance && hal->min_focus_distance > 0.0f;
	out->min_focus_distance_diopters = focuses ? hal->min_focus_distance : 0.0f;
	out->focus_distance_calibration = hal->focus_distance_calibration <= 2 ? hal->focus_distance_calibration : 0;
	const bool manual = focuses && has_af(hal, LEIA_SCAM_AF_OFF) && hal->manual_sensor;
	if (focuses && hal_af) {
		if (has_af(hal, LEIA_SCAM_AF_CONTINUOUS_PICTURE) || has_af(hal, LEIA_SCAM_AF_CONTINUOUS_VIDEO)) {
			out->supported_focus_modes |= 1u << LEIA_SCAM_FOCUS_CONTINUOUS_AUTO;
		}
		if (has_af(hal, LEIA_SCAM_AF_AUTO)) {
			out->supported_focus_modes |= (1u << LEIA_SCAM_FOCUS_AUTO_ONCE) | (1u << LEIA_SCAM_FOCUS_LOCKED);
		}
	}
	if (manual) {
		// AF off + one logical distance: MANUAL, and LOCKED as "keep this distance".
		out->supported_focus_modes |= (1u << LEIA_SCAM_FOCUS_MANUAL) | (1u << LEIA_SCAM_FOCUS_LOCKED);
		out->supported_controls |= LEIA_SCAM_CTL_FOCUS_DISTANCE;
	}
	if (out->supported_focus_modes != 0) {
		out->supported_controls |= LEIA_SCAM_CTL_FOCUS_MODE;
	}
	const bool exposure_ok = !tracking_shared || allow_exposure_on_shared;
	if (exposure_ok && (hal->max_regions_ae > 0 || (focuses && hal_af && hal->max_regions_af > 0))) {
		out->supported_controls |= LEIA_SCAM_CTL_METERING_POINT;
	}
	const double step = ev_step(hal);
	if (exposure_ok && step > 0.0 && hal->ae_comp_min < hal->ae_comp_max) {
		out->supported_controls |= LEIA_SCAM_CTL_EXPOSURE_COMPENSATION;
		out->exposure_compensation_min_ev = (float)(hal->ae_comp_min * step);
		out->exposure_compensation_max_ev = (float)(hal->ae_comp_max * step);
		out->exposure_compensation_step_ev = (float)step;
	}
}

int32_t
leia_scam_ev_to_index(float ev, const struct leia_scam_hal_caps *hal)
{
	const double step = ev_step(hal);
	if (step <= 0.0 || !isfinite(ev)) {
		return 0;
	}
	double idx = floor((double)ev / step + 0.5);
	if (idx < (double)hal->ae_comp_min) {
		idx = (double)hal->ae_comp_min;
	}
	if (idx > (double)hal->ae_comp_max) {
		idx = (double)hal->ae_comp_max;
	}
	return (int32_t)idx;
}

static float
clamp_d(float d, float max_d)
{
	if (!isfinite(d) || d < 0.0f) {
		return 0.0f;
	}
	return d > max_d ? max_d : d;
}

void
leia_scam_plan_controls(const struct leia_scam_controls *c,
                        const struct leia_scam_control_caps *caps,
                        const struct leia_scam_hal_caps *hal,
                        const struct leia_scam_focus_policy *fp,
                        bool region_ok,
                        const int32_t region[5],
                        uint32_t *trigger_seen,
                        struct leia_scam_request_plan *out)
{
	memset(out, 0, sizeof(*out));
	out->af_mode_default = true;
	out->regions_default = true;
	out->ae_comp_default = true;
	const uint32_t valid = c->valid_controls & caps->supported_controls;
	const bool hal_af = fp == NULL || fp->hal_af;

	const bool trigger_moved = trigger_seen != NULL && c->focus_trigger != *trigger_seen;
	if (trigger_seen != NULL) {
		*trigger_seen = c->focus_trigger;
	}
	const bool mode_ok = (valid & LEIA_SCAM_CTL_FOCUS_MODE) != 0 && c->focus_mode <= 31 &&
	                     (caps->supported_focus_modes & (1u << c->focus_mode)) != 0;
	const float max_d = caps->min_focus_distance_diopters;
	if (!hal_af && (caps->supported_controls & LEIA_SCAM_CTL_FOCUS_MODE) != 0) {
		// AF off, always one logical distance: both lenses follow it.
		out->af_mode_default = false;
		out->af_mode = LEIA_SCAM_AF_OFF;
		out->set_focus_distance = true;
		if (mode_ok && c->focus_mode == LEIA_SCAM_FOCUS_MANUAL && (valid & LEIA_SCAM_CTL_FOCUS_DISTANCE) != 0) {
			out->focus_distance = clamp_d(c->focus_distance_diopters, max_d);
		} else if (mode_ok) {
			out->focus_distance = clamp_d(fp->current_d, max_d); // MANUAL without a distance, LOCKED: stay
		} else {
			out->focus_distance = clamp_d(fp->default_d, max_d); // the source's default: a fixed distance
		}
	} else if (mode_ok) {
		out->af_mode_default = false;
		switch (c->focus_mode) {
		case LEIA_SCAM_FOCUS_CONTINUOUS_AUTO:
			out->af_mode = has_af(hal, LEIA_SCAM_AF_CONTINUOUS_PICTURE) ? LEIA_SCAM_AF_CONTINUOUS_PICTURE
			                                                             : LEIA_SCAM_AF_CONTINUOUS_VIDEO;
			break;
		case LEIA_SCAM_FOCUS_AUTO_ONCE:
			out->af_mode = LEIA_SCAM_AF_AUTO;
			out->af_trigger_start = trigger_moved;
			break;
		case LEIA_SCAM_FOCUS_MANUAL:
			if ((valid & LEIA_SCAM_CTL_FOCUS_DISTANCE) != 0) {
				out->af_mode = LEIA_SCAM_AF_OFF;
				out->set_focus_distance = true;
				out->focus_distance = clamp_d(c->focus_distance_diopters, max_d);
			} else {
				out->af_mode = LEIA_SCAM_AF_AUTO; // no distance given: hold the lens where it is
			}
			break;
		default: // LOCKED: AUTO without a trigger never moves the lens
			out->af_mode = LEIA_SCAM_AF_AUTO;
			break;
		}
	}
	if ((valid & LEIA_SCAM_CTL_METERING_POINT) != 0 && region_ok && region != NULL) {
		const bool focuses = (caps->supported_controls & LEIA_SCAM_CTL_FOCUS_MODE) != 0;
		out->set_af_regions = hal_af && focuses && hal->max_regions_af > 0;
		out->set_ae_regions = hal->max_regions_ae > 0;
		if (out->set_af_regions || out->set_ae_regions) {
			out->regions_default = false;
			memcpy(out->region, region, sizeof(out->region));
		}
	}
	if ((valid & LEIA_SCAM_CTL_EXPOSURE_COMPENSATION) != 0) {
		out->ae_comp_default = false;
		out->ae_comp_index = leia_scam_ev_to_index(c->exposure_compensation_ev, hal);
	}
}

uint32_t
leia_scam_focus_state_from_af(uint8_t af_state, bool holding)
{
	switch (af_state) {
	case 0: return holding ? LEIA_SCAM_FOCUS_STATE_LOCKED : LEIA_SCAM_FOCUS_STATE_UNKNOWN; // INACTIVE
	case 1:                                                                                // PASSIVE_SCAN
	case 3: return LEIA_SCAM_FOCUS_STATE_SEARCHING;                                        // ACTIVE_SCAN
	case 2:                                                                                // PASSIVE_FOCUSED
	case 4: return LEIA_SCAM_FOCUS_STATE_LOCKED;                                           // FOCUSED_LOCKED
	case 5:                                                                                // NOT_FOCUSED_LOCKED
	case 6: return LEIA_SCAM_FOCUS_STATE_FAILED;                                           // PASSIVE_UNFOCUSED
	default: return LEIA_SCAM_FOCUS_STATE_UNKNOWN;
	}
}

void
leia_scam_capture_state_from_hal(const struct leia_scam_hal_result *left,
                                 const struct leia_scam_hal_result *right,
                                 const struct leia_scam_hal_result *logical,
                                 bool fixed_focus,
                                 bool hal_af,
                                 bool holding,
                                 bool eye_times_independent,
                                 int64_t boot_to_mono_ns,
                                 struct leia_scam_capture_state *out)
{
	memset(out, 0, sizeof(*out));
	const struct leia_scam_hal_result *exp_src[2] = {left, logical};
	for (int i = 0; i < 2; i++) {
		const struct leia_scam_hal_result *r = exp_src[i];
		if (r == NULL) {
			continue;
		}
		if ((out->valid_fields & LEIA_SCAM_RES_EXPOSURE_TIME) == 0 && r->have_exposure && r->exposure_ns > 0) {
			out->exposure_time_ns = r->exposure_ns;
			out->valid_fields |= LEIA_SCAM_RES_EXPOSURE_TIME;
		}
		if ((out->valid_fields & LEIA_SCAM_RES_SENSITIVITY) == 0 && r->have_iso && r->iso > 0) {
			out->sensitivity_iso = (uint32_t)r->iso;
			out->valid_fields |= LEIA_SCAM_RES_SENSITIVITY;
		}
	}
	const struct leia_scam_hal_result *dist_src[2] = {logical, left};
	for (int i = 0; i < 2; i++) {
		const struct leia_scam_hal_result *r = dist_src[i];
		if (r != NULL && (out->valid_fields & LEIA_SCAM_RES_FOCUS_DISTANCE) == 0 && r->have_focus_distance &&
		    isfinite(r->focus_distance) && r->focus_distance >= 0.0f) {
			out->focus_distance_diopters = r->focus_distance;
			out->valid_fields |= LEIA_SCAM_RES_FOCUS_DISTANCE;
		}
	}
	if (fixed_focus) {
		out->focus_state = LEIA_SCAM_FOCUS_STATE_FIXED;
		out->valid_fields |= LEIA_SCAM_RES_FOCUS_STATE;
	} else if (hal_af) {
		// The AF state machine is the logical camera's; a physical result
		// stands in only when the logical one lacks it.
		const struct leia_scam_hal_result *a = logical != NULL && logical->have_af_state ? logical
		                                       : left != NULL && left->have_af_state     ? left
		                                                                                 : NULL;
		if (a != NULL) {
			out->focus_state = leia_scam_focus_state_from_af(a->af_state, holding);
			out->valid_fields |= LEIA_SCAM_RES_FOCUS_STATE;
		}
	} else {
		// AF off: the lenses' own state. Both must be known (one lens alone
		// says nothing about the pair).
		const bool l = left != NULL && left->have_lens_state, r = right != NULL && right->have_lens_state;
		if (l && r) {
			out->focus_state = (left->lens_state != 0 || right->lens_state != 0) ? LEIA_SCAM_FOCUS_STATE_SEARCHING
			                                                                      : LEIA_SCAM_FOCUS_STATE_LOCKED;
			out->valid_fields |= LEIA_SCAM_RES_FOCUS_STATE;
		}
	}
	if (eye_times_independent && left != NULL && right != NULL && left->have_timestamp && right->have_timestamp) {
		out->eye_time_ns[0] = left->timestamp - boot_to_mono_ns;
		out->eye_time_ns[1] = right->timestamp - boot_to_mono_ns;
		out->valid_fields |= LEIA_SCAM_RES_EYE_TIMES;
	}
}

void
leia_scam_eye_times_observe(struct leia_scam_eye_times_tracker *t, int64_t left_ts, int64_t right_ts)
{
	if (t == NULL || left_ts == 0 || right_ts == 0) {
		return;
	}
	if (t->both >= 0x7fffffffu) {
		t->both /= 2; // keep the ratio, never overflow
		t->differ /= 2;
	}
	t->both++;
	if (left_ts != right_ts) {
		t->differ++;
	}
}

bool
leia_scam_eye_times_independent(const struct leia_scam_eye_times_tracker *t)
{
	return t != NULL && t->both >= LEIA_SCAM_EYE_TIMES_MIN_FRAMES &&
	       (uint64_t)t->differ * LEIA_SCAM_EYE_TIMES_DIFF_DIV >= t->both;
}

int64_t
leia_scam_pair_tolerance_ns(int64_t configured_ns, int64_t frame_period_ns)
{
	int64_t tol = configured_ns > 0 ? configured_ns : 0;
	if (frame_period_ns > 0) {
		const int64_t cap = frame_period_ns * 3 / 10;
		if (tol > cap) {
			tol = cap;
		}
	}
	return tol;
}

int
leia_scam_pair_pick(const int64_t *ts, size_t n, int64_t t, int64_t tol_ns)
{
	int best = -1;
	int64_t best_d = 0;
	for (size_t i = 0; ts != NULL && i < n; i++) {
		const int64_t d = ts[i] > t ? ts[i] - t : t - ts[i];
		if (d <= tol_ns && (best < 0 || d < best_d)) {
			best = (int)i;
			best_d = d;
		}
	}
	return best;
}

void
leia_scam_skew_reset(struct leia_scam_skew_window *w)
{
	w->n = 0;
	w->next = 0;
}

void
leia_scam_skew_push(struct leia_scam_skew_window *w, int64_t v)
{
	w->v[w->next] = v;
	w->next = (w->next + 1) % LEIA_SCAM_SKEW_WINDOW;
	if (w->n < LEIA_SCAM_SKEW_WINDOW) {
		w->n++;
	}
}

static int
cmp_i64(const void *a, const void *b)
{
	const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

bool
leia_scam_skew_summary(const struct leia_scam_skew_window *w, int64_t *out_min, int64_t *out_median, int64_t *out_max)
{
	if (w == NULL || w->n == 0) {
		return false;
	}
	int64_t tmp[LEIA_SCAM_SKEW_WINDOW];
	memcpy(tmp, w->v, sizeof(int64_t) * w->n);
	qsort(tmp, w->n, sizeof(int64_t), cmp_i64);
	*out_min = tmp[0];
	*out_median = tmp[w->n / 2];
	*out_max = tmp[w->n - 1];
	return true;
}

void
leia_scam_phase_init(struct leia_scam_phase_lock *l, const struct leia_scam_phase_cfg *cfg)
{
	memset(l, 0, sizeof(*l));
	l->cfg = *cfg;
	if (l->cfg.samples == 0) {
		l->cfg.samples = 1;
	}
	if (l->cfg.samples > LEIA_SCAM_PHASE_MAX_SAMPLES) {
		l->cfg.samples = LEIA_SCAM_PHASE_MAX_SAMPLES;
	}
	l->state = LEIA_SCAM_PHASE_MEASURING;
}

void
leia_scam_phase_restart(struct leia_scam_phase_lock *l)
{
	l->state = LEIA_SCAM_PHASE_MEASURING;
	l->n = 0;
	l->pairs = 0;
	l->gap_ns = 0;
	l->shift = 0;
	l->residual_ns = 0;
}

static int64_t
median_i64(int64_t *v, uint32_t n)
{
	for (uint32_t i = 1; i < n; i++) { // insertion sort: n <= 32
		const int64_t x = v[i];
		uint32_t j = i;
		while (j > 0 && v[j - 1] > x) {
			v[j] = v[j - 1];
			j--;
		}
		v[j] = x;
	}
	return v[n / 2];
}

enum leia_scam_phase_action
leia_scam_phase_feed(struct leia_scam_phase_lock *l, int64_t gap_ns, int64_t period_ns)
{
	if (l->state != LEIA_SCAM_PHASE_MEASURING) {
		return l->state;
	}
	l->pairs++;
	l->g[l->n++] = gap_ns;
	if (l->n < l->cfg.samples) {
		return l->state;
	}
	int64_t tmp[LEIA_SCAM_PHASE_MAX_SAMPLES];
	memcpy(tmp, l->g, sizeof(int64_t) * l->n);
	const int64_t G = median_i64(tmp, l->n);
	l->gap_ns = G;
	int shift = 0;
	if (l->cfg.repair && period_ns > 0) {
		const double f = -(double)G / (double)period_ns;
		shift = (int)floor(f + 0.5);
		shift = shift > 1 ? 1 : (shift < -1 ? -1 : shift);
	}
	l->shift = shift;
	l->residual_ns = G + (int64_t)shift * period_ns;
	const int64_t judged = l->cfg.judge_raw_gap ? G : l->residual_ns;
	const int64_t a = judged < 0 ? -judged : judged;
	if (!l->have_best || a < l->best_abs_ns) {
		l->have_best = true;
		l->best_abs_ns = a;
	}
	if (a <= l->cfg.threshold_ns) {
		l->state = LEIA_SCAM_PHASE_LOCKED;
	} else if (l->cfg.reopen && l->reopens < l->cfg.max_reopens) {
		l->reopens++;
		l->state = LEIA_SCAM_PHASE_REOPEN;
	} else {
		l->state = LEIA_SCAM_PHASE_ACCEPTED;
	}
	return l->state;
}

enum leia_scam_phase_action
leia_scam_phase_pair_without_sample(struct leia_scam_phase_lock *l)
{
	if (l->state != LEIA_SCAM_PHASE_MEASURING) {
		return l->state;
	}
	l->pairs++;
	if (l->pairs >= 3 * l->cfg.samples && l->n < l->cfg.samples) {
		l->state = LEIA_SCAM_PHASE_UNKNOWN;
		l->shift = 0;
		l->residual_ns = 0;
	}
	return l->state;
}

bool
leia_scam_phase_holding(const struct leia_scam_phase_lock *l)
{
	return l->state == LEIA_SCAM_PHASE_MEASURING || l->state == LEIA_SCAM_PHASE_REOPEN;
}

int64_t
leia_scam_phase_partner_ts(int64_t t, bool left, int shift, int64_t period_ns)
{
	const int64_t d = (int64_t)shift * period_ns;
	return left ? t + d : t - d;
}

bool
leia_scam_phase_stamps_identical(const struct leia_scam_phase_lock *l)
{
	if (l->n == 0) {
		return false;
	}
	for (uint32_t i = 0; i < l->n; i++) {
		if (l->g[i] != 0) {
			return false;
		}
	}
	return true;
}
