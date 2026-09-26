// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Stereo camera source (L1): platform-neutral parsing. See the header.
 * @ingroup drv_leia
 */

#include "leia_stereo_camera_parse.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Frame header + JPEG.
 *
 */

static uint64_t
rd_u64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) {
		v = (v << 8) | p[i]; // little-endian (x64 writer)
	}
	return v;
}

enum leia_scam_header_status
leia_scam_header_check(const void *block, uint64_t region_size, struct leia_scam_header *out)
{
	memset(out, 0, sizeof(*out));
	if (block == NULL || region_size < LEIA_SCAM_HEADER_SIZE) {
		return LEIA_SCAM_HEADER_BAD;
	}
	const uint8_t *p = (const uint8_t *)block;
	out->raw_image_format = rd_u64(p + 0);
	out->compressed_size = rd_u64(p + 8);
	out->height = rd_u64(p + 16);
	out->width = rd_u64(p + 24);
	if (out->compressed_size == 0) {
		return LEIA_SCAM_HEADER_EMPTY;
	}
	if (out->raw_image_format != LEIA_SCAM_FORMAT_SBS_MJPEG) {
		return LEIA_SCAM_HEADER_BAD;
	}
	if (out->compressed_size < 64 || out->compressed_size > region_size - LEIA_SCAM_HEADER_SIZE) {
		return LEIA_SCAM_HEADER_BAD;
	}
	if (out->width < 32 || out->height < 16 || out->width > 8192 || out->height > 4096 || (out->width & 3u) != 0 ||
	    (out->height & 1u) != 0) {
		return LEIA_SCAM_HEADER_BAD; // SBS of two even-width eyes, even height
	}
	return LEIA_SCAM_HEADER_OK;
}

bool
leia_scam_jpeg_dims(const uint8_t *jpg, size_t size, uint32_t *out_w, uint32_t *out_h, uint32_t *out_components)
{
	if (jpg == NULL || size < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) {
		return false;
	}
	size_t i = 2;
	while (i + 4 <= size) {
		if (jpg[i] != 0xFF) {
			return false;
		}
		uint8_t m = jpg[i + 1];
		if (m == 0xFF) { // fill byte
			i++;
			continue;
		}
		if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { // no length
			i += 2;
			continue;
		}
		if (m == 0xDA || m == 0xD9) { // start of scan / end: no SOF seen
			return false;
		}
		size_t len = ((size_t)jpg[i + 2] << 8) | jpg[i + 3];
		if (len < 2 || i + 2 + len > size) {
			return false;
		}
		// SOF0..SOF15 except DHT (C4), JPG (C8), DAC (CC).
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
			if (len < 8) {
				return false;
			}
			*out_h = ((uint32_t)jpg[i + 5] << 8) | jpg[i + 6];
			*out_w = ((uint32_t)jpg[i + 7] << 8) | jpg[i + 8];
			*out_components = jpg[i + 9];
			return true;
		}
		i += 2 + len;
	}
	return false;
}

static uint64_t
fnv1a(uint64_t h, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

uint64_t
leia_scam_fingerprint(const uint8_t *jpg, size_t size)
{
	uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)size;
	if (jpg == NULL || size == 0) {
		return h;
	}
	const size_t span = 256;
	if (size <= 3 * span) {
		return fnv1a(h, jpg, size);
	}
	h = fnv1a(h, jpg, span);
	h = fnv1a(h, jpg + size / 2 - span / 2, span);
	h = fnv1a(h, jpg + size - span, span);
	return h;
}


/*
 *
 * Serials.
 *
 */

uint32_t
leia_scam_parse_serials(const uint8_t *block, size_t size, char (*out)[LEIA_SCAM_SERIAL_LEN + 1], uint32_t cap)
{
	if (block == NULL || size < 1) {
		return 0;
	}
	uint32_t n = block[0];
	uint32_t fit = (uint32_t)((size - 1) / LEIA_SCAM_SERIAL_LEN);
	if (n > fit) {
		n = fit;
	}
	for (uint32_t i = 0; i < n && i < cap; i++) {
		const char *s = (const char *)block + 1 + (size_t)i * LEIA_SCAM_SERIAL_LEN;
		size_t l = 0;
		while (l < LEIA_SCAM_SERIAL_LEN && s[l] != '\0') {
			l++;
		}
		while (l > 0 && (s[l - 1] == ' ' || s[l - 1] == '\r' || s[l - 1] == '\n' || s[l - 1] == '\t')) {
			l--;
		}
		memcpy(out[i], s, l);
		out[i][l] = '\0';
	}
	return n;
}


/*
 *
 * YAML subset.
 *
 */

//! Start of the top-level node "key:" (at column 0), or NULL.
static const char *
find_top_key(const char *text, size_t len, const char *key)
{
	size_t kl = strlen(key);
	const char *end = text + len;
	for (const char *p = text; p + kl < end; p++) {
		if ((p == text || p[-1] == '\n') && strncmp(p, key, kl) == 0 && p[kl] == ':') {
			return p + kl + 1;
		}
	}
	return NULL;
}

//! End of the node that starts at @p p: the next line beginning with a
//! non-space character (a new top-level key), or the text end.
static const char *
node_end(const char *p, const char *end)
{
	for (; p < end; p++) {
		if (*p == '\n' && p + 1 < end && p[1] != ' ' && p[1] != '\t' && p[1] != '\r' && p[1] != '\n') {
			return p + 1;
		}
	}
	return end;
}

static uint32_t
node_uint(const char *p, const char *end, const char *field)
{
	size_t fl = strlen(field);
	for (; p + fl < end; p++) {
		if (strncmp(p, field, fl) == 0 && p[fl] == ':') {
			return (uint32_t)strtoul(p + fl + 1, NULL, 10);
		}
	}
	return 0;
}

uint32_t
leia_scam_yaml_matrix(
    const char *text, size_t len, const char *key, double *out, uint32_t cap, uint32_t *out_rows, uint32_t *out_cols)
{
	*out_rows = *out_cols = 0;
	const char *p = find_top_key(text, len, key);
	if (p == NULL) {
		return 0;
	}
	const char *end = node_end(p, text + len);
	if (strstr(p, "opencv-matrix") == NULL || strstr(p, "opencv-matrix") > end) {
		return 0;
	}
	*out_rows = node_uint(p, end, "rows");
	*out_cols = node_uint(p, end, "cols");
	const char *d = p;
	for (; d + 5 < end; d++) {
		if (strncmp(d, "data:", 5) == 0) {
			break;
		}
	}
	if (d + 5 >= end) {
		return 0;
	}
	const char *b = memchr(d, '[', (size_t)(end - d));
	if (b == NULL) {
		return 0;
	}
	b++;
	uint32_t n = 0;
	while (b < end) {
		while (b < end && (*b == ' ' || *b == ',' || *b == '\n' || *b == '\r' || *b == '\t')) {
			b++;
		}
		if (b >= end) {
			return 0; // no closing bracket in the node
		}
		if (*b == ']') {
			return n;
		}
		char *next = NULL;
		double v = strtod(b, &next);
		if (next == b || next > end) {
			return 0;
		}
		if (n < cap) {
			out[n] = v;
		}
		n++;
		b = next;
	}
	return 0;
}

static void
set_why(char *why, size_t why_len, const char *msg)
{
	if (why != NULL && why_len > 0) {
		snprintf(why, why_len, "%s", msg);
	}
}

bool
leia_scam_parse_calibration(const char *intr,
                            size_t intr_len,
                            const char *extr,
                            size_t extr_len,
                            struct leia_scam_calibration *out,
                            char *why,
                            size_t why_len)
{
	memset(out, 0, sizeof(*out));
	const char *kname[2] = {"M1", "M2"}, *dname[2] = {"D1", "D2"};
	uint32_t r, c;
	for (int e = 0; e < 2; e++) {
		if (leia_scam_yaml_matrix(intr, intr_len, kname[e], out->K[e], 9, &r, &c) != 9 || r != 3 || c != 3) {
			set_why(why, why_len, e == 0 ? "intrinsics.yml: no 3x3 M1" : "intrinsics.yml: no 3x3 M2");
			return false;
		}
		const double *K = out->K[e];
		if (!(K[0] > 0.0) || !(K[4] > 0.0) || K[1] != 0.0 || K[3] != 0.0 || K[6] != 0.0 || K[7] != 0.0 ||
		    K[8] != 1.0) {
			set_why(why, why_len, "intrinsics.yml: K is not a zero-skew pinhole");
			return false;
		}
		double d[14] = {0};
		uint32_t n = leia_scam_yaml_matrix(intr, intr_len, dname[e], d, 14, &r, &c);
		if (n != 4 && n != 5 && n != 8 && n != 12 && n != 14) {
			set_why(why, why_len, "intrinsics.yml: D1/D2 must have 4, 5, 8, 12 or 14 coefficients");
			return false;
		}
		for (uint32_t k = 8; k < n; k++) {
			if (d[k] != 0.0) {
				set_why(why, why_len,
				        "intrinsics.yml: thin-prism / tilted lens terms are not supported");
				return false;
			}
		}
		out->d_count[e] = n > 8 ? 8 : n;
		memcpy(out->D[e], d, sizeof(double) * 8);
	}
	if (leia_scam_yaml_matrix(extr, extr_len, "R", out->R, 9, &r, &c) != 9 || r != 3 || c != 3) {
		set_why(why, why_len, "extrinsics.yml: no 3x3 R");
		return false;
	}
	if (leia_scam_yaml_matrix(extr, extr_len, "T", out->T, 3, &r, &c) != 3) {
		set_why(why, why_len, "extrinsics.yml: no 3-vector T");
		return false;
	}
	// R must be a rotation: R R^T = I, det = +1.
	const double *R = out->R;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			double s = R[i * 3] * R[j * 3] + R[i * 3 + 1] * R[j * 3 + 1] + R[i * 3 + 2] * R[j * 3 + 2];
			if (fabs(s - (i == j ? 1.0 : 0.0)) > 1e-3) {
				set_why(why, why_len, "extrinsics.yml: R is not a rotation");
				return false;
			}
		}
	}
	double det = R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) +
	             R[2] * (R[3] * R[7] - R[4] * R[6]);
	if (det < 0.99) {
		set_why(why, why_len, "extrinsics.yml: R is a reflection");
		return false;
	}
	double b = sqrt(out->T[0] * out->T[0] + out->T[1] * out->T[1] + out->T[2] * out->T[2]);
	if (!(b >= 5.0 && b <= 500.0)) {
		set_why(why, why_len, "extrinsics.yml: |T| outside 5..500 mm (unit is mm)");
		return false;
	}
	if (fabs(out->T[0]) < fabs(out->T[1])) {
		set_why(why, why_len, "extrinsics.yml: vertical pair (|Ty| > |Tx|)");
		return false;
	}
	return true;
}

bool
leia_scam_decide_swap(const struct leia_scam_calibration *c, const char *override_env)
{
	if (override_env != NULL && override_env[0] == '1') {
		return true;
	}
	if (override_env != NULL && override_env[0] == '0') {
		return false;
	}
	return c->T[0] > 0.0;
}

void
leia_scam_calibration_for_runtime(const struct leia_scam_calibration *c,
                                  bool swap,
                                  struct leia_scam_eye_calibration *out)
{
	memset(out, 0, sizeof(*out));
	uint32_t dmax = c->d_count[0] > c->d_count[1] ? c->d_count[0] : c->d_count[1];
	out->model = dmax > 5 ? 2u : 1u; // RADTAN8 : RADTAN5 (a 4-vector is RADTAN5 with k3 = 0)
	for (int e = 0; e < 2; e++) {
		int src = swap ? 1 - e : e;
		const double *K = c->K[src];
		out->k[e][0] = K[0];
		out->k[e][1] = K[4];
		out->k[e][2] = K[2];
		out->k[e][3] = K[5];
		memcpy(out->d[e], c->D[src], sizeof(out->d[e]));
	}
	if (!swap) {
		for (int i = 0; i < 9; i++) {
			out->R[i / 3][i % 3] = c->R[i];
		}
		memcpy(out->T, c->T, sizeof(out->T));
		return;
	}
	// Camera 2 becomes the left eye: x_1 = R^T x_2 - R^T T.
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			out->R[i][j] = c->R[j * 3 + i];
		}
	}
	for (int i = 0; i < 3; i++) {
		out->T[i] = -(out->R[i][0] * c->T[0] + out->R[i][1] * c->T[1] + out->R[i][2] * c->T[2]);
	}
}

void
leia_scam_swap_halves_gray8(uint8_t *img, uint32_t width, uint32_t height, uint32_t pitch)
{
	const uint32_t h = width / 2;
	uint8_t tmp[4096];
	if (h > sizeof(tmp)) {
		return;
	}
	for (uint32_t y = 0; y < height; y++) {
		uint8_t *row = img + (size_t)y * pitch;
		memcpy(tmp, row, h);
		memmove(row, row + h, h);
		memcpy(row + h, tmp, h);
	}
}
