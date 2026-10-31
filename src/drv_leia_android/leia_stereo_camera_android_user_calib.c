// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  User stereo calibration record: parse + checks. See the header.
 *
 * The JSON reader is a small strict one: it walks the whole document (so a
 * truncated record fails) and keeps only the handful of fields the plug-in
 * reads. Strings are copied byte for byte; a \uXXXX escape becomes '?', which
 * no field the plug-in compares can contain.
 *
 * @ingroup drv_leia_android
 */

#include "leia_stereo_camera_android_user_calib.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 *
 * JSON.
 *
 */

#define JMAX_DEPTH 32

struct jcur
{
	const char *p;
	const char *end;
};

static void
jws(struct jcur *c)
{
	while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
		c->p++;
	}
}

static bool
jeat(struct jcur *c, char ch)
{
	jws(c);
	if (c->p < c->end && *c->p == ch) {
		c->p++;
		return true;
	}
	return false;
}

//! A string at the cursor; copied into @p out (truncated to @p cap - 1) when non-NULL.
static bool
jstring(struct jcur *c, char *out, size_t cap)
{
	jws(c);
	if (c->p >= c->end || *c->p != '"') {
		return false;
	}
	c->p++;
	size_t n = 0;
	while (c->p < c->end && *c->p != '"') {
		char ch = *c->p++;
		if ((unsigned char)ch < 0x20) {
			return false;
		}
		if (ch == '\\') {
			if (c->p >= c->end) {
				return false;
			}
			const char e = *c->p++;
			switch (e) {
			case '"':
			case '\\':
			case '/': ch = e; break;
			case 'b': ch = '\b'; break;
			case 'f': ch = '\f'; break;
			case 'n': ch = '\n'; break;
			case 'r': ch = '\r'; break;
			case 't': ch = '\t'; break;
			case 'u':
				for (int i = 0; i < 4; i++) {
					if (c->p >= c->end || !isxdigit((unsigned char)*c->p)) {
						return false;
					}
					c->p++;
				}
				ch = '?';
				break;
			default: return false;
			}
		}
		if (out != NULL && n + 1 < cap) {
			out[n++] = ch;
		}
	}
	if (c->p >= c->end) {
		return false;
	}
	c->p++; // closing quote
	if (out != NULL && cap > 0) {
		out[n] = '\0';
	}
	return true;
}

static bool
jnumber(struct jcur *c, double *out)
{
	jws(c);
	char buf[64];
	size_t n = 0;
	while (c->p < c->end && n + 1 < sizeof(buf) &&
	       (isdigit((unsigned char)*c->p) || *c->p == '-' || *c->p == '+' || *c->p == '.' || *c->p == 'e' ||
	        *c->p == 'E')) {
		buf[n++] = *c->p++;
	}
	if (n == 0) {
		return false;
	}
	buf[n] = '\0';
	char *e = NULL;
	const double v = strtod(buf, &e);
	if (e != buf + n) {
		return false;
	}
	if (out != NULL) {
		*out = v;
	}
	return true;
}

static bool
jliteral(struct jcur *c, const char *lit)
{
	jws(c);
	const size_t n = strlen(lit);
	if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) {
		return false;
	}
	c->p += n;
	return true;
}

static bool
jskip(struct jcur *c, int depth);

static bool
jskip_container(struct jcur *c, int depth, char close, bool object)
{
	if (depth > JMAX_DEPTH) {
		return false;
	}
	if (jeat(c, close)) {
		return true;
	}
	do {
		if (object && (!jstring(c, NULL, 0) || !jeat(c, ':'))) {
			return false;
		}
		if (!jskip(c, depth + 1)) {
			return false;
		}
	} while (jeat(c, ','));
	return jeat(c, close);
}

static bool
jskip(struct jcur *c, int depth)
{
	jws(c);
	if (c->p >= c->end) {
		return false;
	}
	switch (*c->p) {
	case '"': return jstring(c, NULL, 0);
	case '{': c->p++; return jskip_container(c, depth, '}', true);
	case '[': c->p++; return jskip_container(c, depth, ']', false);
	case 't': return jliteral(c, "true");
	case 'f': return jliteral(c, "false");
	case 'n': return jliteral(c, "null");
	default: return jnumber(c, NULL);
	}
}

static bool
jpeek(struct jcur *c, char ch)
{
	jws(c);
	return c->p < c->end && *c->p == ch;
}

//! "pair": { "logicalCameraId": "...", "lensIds": ["a","b"], ... }
static bool
parse_pair(struct jcur *c, struct leia_scam_user_calib *u)
{
	if (!jeat(c, '{')) {
		return false;
	}
	if (jeat(c, '}')) {
		return true;
	}
	do {
		char key[32];
		if (!jstring(c, key, sizeof(key)) || !jeat(c, ':')) {
			return false;
		}
		if (strcmp(key, "logicalCameraId") == 0) {
			if (!jstring(c, u->logical_id, sizeof(u->logical_id))) {
				return false;
			}
		} else if (strcmp(key, "lensIds") == 0) {
			if (!jeat(c, '[') || !jstring(c, u->lens_ids[0], sizeof(u->lens_ids[0])) || !jeat(c, ',') ||
			    !jstring(c, u->lens_ids[1], sizeof(u->lens_ids[1])) || !jeat(c, ']')) {
				return false;
			}
			u->has_lens_ids = true;
		} else if (!jskip(c, 2)) {
			return false;
		}
	} while (jeat(c, ','));
	return jeat(c, '}');
}

//! "factory": { "blockSha256": "...", ... }
static bool
parse_factory(struct jcur *c, struct leia_scam_user_calib *u)
{
	if (!jeat(c, '{')) {
		return false;
	}
	if (jeat(c, '}')) {
		return true;
	}
	do {
		char key[32];
		if (!jstring(c, key, sizeof(key)) || !jeat(c, ':')) {
			return false;
		}
		if (strcmp(key, "blockSha256") == 0) {
			if (!jstring(c, u->block_sha256, sizeof(u->block_sha256))) {
				return false;
			}
			for (char *s = u->block_sha256; *s != '\0'; s++) {
				*s = (char)tolower((unsigned char)*s);
			}
		} else if (!jskip(c, 2)) {
			return false;
		}
	} while (jeat(c, ','));
	return jeat(c, '}');
}

//! [[a,b,c],[d,e,f],[g,h,i]]
static bool
parse_mat3(struct jcur *c, double R[3][3])
{
	if (!jeat(c, '[')) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		if (i > 0 && !jeat(c, ',')) {
			return false;
		}
		if (!jeat(c, '[')) {
			return false;
		}
		for (int j = 0; j < 3; j++) {
			if ((j > 0 && !jeat(c, ',')) || !jnumber(c, &R[i][j])) {
				return false;
			}
		}
		if (!jeat(c, ']')) {
			return false;
		}
	}
	return jeat(c, ']');
}

bool
leia_scam_user_calib_parse(const char *json, size_t len, struct leia_scam_user_calib *out)
{
	if (json == NULL || out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->schema = -1;
	struct jcur c = {json, json + len};
	if (!jeat(&c, '{')) {
		return false;
	}
	if (!jeat(&c, '}')) {
		do {
			char key[32];
			if (!jstring(&c, key, sizeof(key)) || !jeat(&c, ':')) {
				return false;
			}
			bool ok;
			if (strcmp(key, "schema") == 0) {
				double v = 0.0;
				ok = jnumber(&c, &v) && v == floor(v) && fabs(v) < 1e6;
				out->schema = ok ? (int)v : -1;
			} else if (strcmp(key, "pair") == 0) {
				ok = parse_pair(&c, out);
			} else if (strcmp(key, "factory") == 0) {
				ok = parse_factory(&c, out);
			} else if (strcmp(key, "R_1_from_0") == 0) {
				ok = parse_mat3(&c, out->R);
				out->has_R = ok;
			} else if (strcmp(key, "timestamp") == 0 && jpeek(&c, '"')) {
				ok = jstring(&c, out->timestamp, sizeof(out->timestamp));
			} else if (strcmp(key, "method") == 0 && jpeek(&c, '"')) {
				ok = jstring(&c, out->method, sizeof(out->method));
			} else {
				ok = jskip(&c, 1);
			}
			if (!ok) {
				return false;
			}
		} while (jeat(&c, ','));
		if (!jeat(&c, '}')) {
			return false;
		}
	}
	jws(&c);
	// Trailing NULs are tolerated (a length that counts the terminator).
	while (c.p < c.end && *c.p == '\0') {
		c.p++;
	}
	return c.p == c.end;
}

/*
 *
 * Checks.
 *
 */

const char *
leia_scam_user_calib_verdict_str(enum leia_scam_user_calib_verdict v)
{
	switch (v) {
	case LEIA_SCAM_USER_CALIB_OK: return "ok";
	case LEIA_SCAM_USER_CALIB_BAD_SCHEMA: return "unsupported schema";
	case LEIA_SCAM_USER_CALIB_OTHER_PAIR: return "record is for another logical camera";
	case LEIA_SCAM_USER_CALIB_OTHER_LENSES: return "record's lensIds are not [main, sub]";
	case LEIA_SCAM_USER_CALIB_BLOCK_CHANGED: return "factory block changed since the record was saved (SHA-256 differs)";
	case LEIA_SCAM_USER_CALIB_NO_ROTATION: return "record has no R_1_from_0";
	case LEIA_SCAM_USER_CALIB_NOT_ROTATION: return "R_1_from_0 is not a rotation";
	case LEIA_SCAM_USER_CALIB_TOO_FAR: return "R_1_from_0 is too far from the factory R";
	}
	return "?";
}

static bool
is_rotation(const double R[3][3])
{
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			if (!isfinite(R[i][j])) {
				return false;
			}
		}
	}
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			double d = 0.0;
			for (int k = 0; k < 3; k++) {
				d += R[i][k] * R[j][k];
			}
			if (fabs(d - (i == j ? 1.0 : 0.0)) > 1e-4) {
				return false;
			}
		}
	}
	const double det = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) -
	                   R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
	                   R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
	return fabs(det - 1.0) <= 1e-4;
}

//! Angle (degrees) of A * B^T.
static double
angle_between_deg(const double A[3][3], const double B[3][3])
{
	double tr = 0.0;
	for (int i = 0; i < 3; i++) {
		for (int k = 0; k < 3; k++) {
			tr += A[i][k] * B[i][k];
		}
	}
	double c = (tr - 1.0) / 2.0;
	c = c > 1.0 ? 1.0 : (c < -1.0 ? -1.0 : c);
	return acos(c) * 180.0 / M_PI;
}

enum leia_scam_user_calib_verdict
leia_scam_user_calib_check(const struct leia_scam_user_calib *u,
                           const char *logical_id,
                           const char *main_id,
                           const char *sub_id,
                           const uint8_t block_sha256[32],
                           const double R_factory[3][3],
                           double *out_delta_deg)
{
	if (out_delta_deg != NULL) {
		*out_delta_deg = 0.0;
	}
	if (u->schema != 1) {
		return LEIA_SCAM_USER_CALIB_BAD_SCHEMA;
	}
	if (logical_id == NULL || strcmp(u->logical_id, logical_id) != 0) {
		return LEIA_SCAM_USER_CALIB_OTHER_PAIR;
	}
	if (u->has_lens_ids && main_id != NULL && sub_id != NULL &&
	    (strcmp(u->lens_ids[0], main_id) != 0 || strcmp(u->lens_ids[1], sub_id) != 0)) {
		return LEIA_SCAM_USER_CALIB_OTHER_LENSES;
	}
	char hex[65];
	leia_scam_sha256_hex(block_sha256, hex);
	if (strcmp(u->block_sha256, hex) != 0) {
		return LEIA_SCAM_USER_CALIB_BLOCK_CHANGED;
	}
	if (!u->has_R) {
		return LEIA_SCAM_USER_CALIB_NO_ROTATION;
	}
	if (!is_rotation(u->R)) {
		return LEIA_SCAM_USER_CALIB_NOT_ROTATION;
	}
	const double d = angle_between_deg(u->R, R_factory);
	if (out_delta_deg != NULL) {
		*out_delta_deg = d;
	}
	if (!(d <= LEIA_SCAM_USER_CALIB_MAX_DELTA_DEG)) {
		return LEIA_SCAM_USER_CALIB_TOO_FAR;
	}
	return LEIA_SCAM_USER_CALIB_OK;
}

static bool
consider(const char *json,
         size_t len,
         const char *logical_id,
         const char *main_id,
         const char *sub_id,
         const uint8_t block_sha256[32],
         const double R_factory[3][3],
         struct leia_scam_user_calib_choice *out)
{
	out->considered = true;
	if (!leia_scam_user_calib_parse(json, len, &out->rec)) {
		out->parse_failed = true;
		return false;
	}
	out->verdict = leia_scam_user_calib_check(&out->rec, logical_id, main_id, sub_id, block_sha256, R_factory,
	                                          &out->delta_deg);
	return out->verdict == LEIA_SCAM_USER_CALIB_OK;
}

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
                            struct leia_scam_user_calib_choice *out)
{
	memset(out, 0, sizeof(*out));
	out->source = LEIA_SCAM_USER_CALIB_SRC_FACTORY;
	switch (provider) {
	case LEIA_SCAM_PROVIDER_RECORD:
		if (provider_json != NULL &&
		    consider(provider_json, provider_len, logical_id, main_id, sub_id, block_sha256, R_factory, out)) {
			out->source = LEIA_SCAM_USER_CALIB_SRC_PROVIDER;
			out->cache_write = true;
		} else {
			out->cache_drop = true;
		}
		break;
	case LEIA_SCAM_PROVIDER_NO_RECORD: out->cache_drop = true; break;
	case LEIA_SCAM_PROVIDER_UNAVAILABLE:
		if (cache_json != NULL && cache_len > 0 &&
		    consider(cache_json, cache_len, logical_id, main_id, sub_id, block_sha256, R_factory, out)) {
			out->source = LEIA_SCAM_USER_CALIB_SRC_CACHE;
		}
		break;
	}
}

/*
 *
 * SHA-256 (FIPS 180-4).
 *
 */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
sha256_block(uint32_t h[8], const uint8_t *b)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)b[4 * i] << 24) | ((uint32_t)b[4 * i + 1] << 16) | ((uint32_t)b[4 * i + 2] << 8) |
		       (uint32_t)b[4 * i + 3];
	}
	for (int i = 16; i < 64; i++) {
		const uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
	for (int i = 0; i < 64; i++) {
		const uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
		const uint32_t ch = (e & f) ^ (~e & g);
		const uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
		const uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
		const uint32_t mj = (a & bb) ^ (a & c) ^ (bb & c);
		const uint32_t t2 = S0 + mj;
		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = bb;
		bb = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += bb;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += hh;
}

void
leia_scam_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	size_t i = 0;
	for (; i + 64 <= len; i += 64) {
		sha256_block(h, data + i);
	}
	uint8_t tail[128];
	memset(tail, 0, sizeof(tail));
	const size_t rem = len - i;
	if (rem > 0) {
		memcpy(tail, data + i, rem);
	}
	tail[rem] = 0x80;
	const size_t tlen = rem + 1 + 8 <= 64 ? 64 : 128;
	const uint64_t bits = (uint64_t)len * 8u;
	for (int k = 0; k < 8; k++) {
		tail[tlen - 1 - (size_t)k] = (uint8_t)(bits >> (8 * k));
	}
	sha256_block(h, tail);
	if (tlen == 128) {
		sha256_block(h, tail + 64);
	}
	for (int k = 0; k < 8; k++) {
		out[4 * k] = (uint8_t)(h[k] >> 24);
		out[4 * k + 1] = (uint8_t)(h[k] >> 16);
		out[4 * k + 2] = (uint8_t)(h[k] >> 8);
		out[4 * k + 3] = (uint8_t)h[k];
	}
}

void
leia_scam_sha256_hex(const uint8_t digest[32], char out[65])
{
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++) {
		out[2 * i] = hx[digest[i] >> 4];
		out[2 * i + 1] = hx[digest[i] & 15];
	}
	out[64] = '\0';
}
