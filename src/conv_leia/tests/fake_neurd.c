// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  A fake libNeurD.so for the conversion provider's function-table guard
 *         (leia_conv_neurd_posix.cpp, neurd_table_has): NeurD_load returns a
 *         version-0.5.0 table whose last byte sits right before a PROT_NONE page.
 *
 * Built in four variants (compile definitions), each its own libNeurD.so:
 *   FAKE_NEURD_SHORT       the table ends BEFORE convert_stream_cpu_ex -- the
 *                          shape of a 0.5.0 runtime built before that entry was
 *                          added. Reading the entry faults on the guard page, so
 *                          a provider that tests it by reading it crashes here.
 *   FAKE_NEURD_EXPORT_SIZE also export NeurD_get_table_size() (the explicit
 *                          query) returning the table's real size.
 *
 *   short          -> no size signal: the provider must not trust the entry
 *   short + size   -> the size says the entry is absent
 *   full           -> no size signal: entry introduced at the runtime's own
 *                     version, so still not trusted (conservative)
 *   full + size    -> the size proves the entry: look-around
 *
 * Only what the provider's activation calls is implemented (properties, init,
 * the CPU stream API); every other entry is NULL. Never converts.
 */

#include <NeurD.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define FAKE_EXPORT __attribute__((visibility("default")))

#ifdef FAKE_NEURD_SHORT
#define FAKE_TABLE_BYTES offsetof(struct NeurD, convert_stream_cpu_ex)
#else
#define FAKE_TABLE_BYTES sizeof(struct NeurD)
#endif

static enum NeurD_status NEURD_CALL
f_set_1f(enum NeurD_prop prop, float value)
{
	(void)prop;
	(void)value;
	return NEURD_SUCCESS;
}

static enum NeurD_status NEURD_CALL
f_set_1i(enum NeurD_prop prop, int32_t value)
{
	(void)prop;
	(void)value;
	return NEURD_SUCCESS;
}

static enum NeurD_status NEURD_CALL
f_init_with_options(NeurD_bool multithreaded, struct NeurD_init_options const *options)
{
	(void)multithreaded;
	(void)options;
	return NEURD_SUCCESS;
}

static enum NeurD_status NEURD_CALL
f_create_stream(struct NeurD_stream **stream)
{
	static int token;
	*stream = (struct NeurD_stream *)&token;
	return NEURD_SUCCESS;
}

static enum NeurD_status NEURD_CALL
f_destroy_stream(struct NeurD_stream *stream)
{
	(void)stream;
	return NEURD_SUCCESS;
}

static enum NeurD_status NEURD_CALL
f_convert_stream_cpu(struct NeurD_stream *stream,
                     struct NeurD_image const *input,
                     enum NeurD_pixel_format output_pix_fmt,
                     struct NeurD_image *output)
{
	(void)stream;
	(void)input;
	(void)output_pix_fmt;
	(void)output;
	return NEURD_GENERIC_ERROR;
}

#ifndef FAKE_NEURD_SHORT
static enum NeurD_status NEURD_CALL
f_convert_stream_cpu_ex(struct NeurD_stream *stream,
                        struct NeurD_image const *input,
                        struct NeurD_convert_params const *params,
                        struct NeurD_convert_outputs *outputs)
{
	(void)stream;
	(void)input;
	(void)params;
	(void)outputs;
	return NEURD_GENERIC_ERROR;
}
#endif

FAKE_EXPORT struct NeurD const *NEURD_CALL
NeurD_load(struct NeurD_load_request const *request)
{
	(void)request;
	static struct NeurD const *table = NULL;
	if (table != NULL) {
		return table;
	}
	struct NeurD full;
	memset(&full, 0, sizeof(full));
	full.version = NEURD_MAKE_VERSION(0, 5, 0);
	full.set_prop_1f = f_set_1f;
	full.set_prop_1i = f_set_1i;
	full.init_with_options = f_init_with_options;
	full.create_stream = f_create_stream;
	full.destroy_stream = f_destroy_stream;
	full.convert_stream_cpu = f_convert_stream_cpu;
#ifndef FAKE_NEURD_SHORT
	full.convert_stream_cpu_ex = f_convert_stream_cpu_ex;
#endif
	const size_t page = (size_t)sysconf(_SC_PAGESIZE);
	uint8_t *base = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED || mprotect(base + page, page, PROT_NONE) != 0) {
		return NULL;
	}
	uint8_t *start = base + page - FAKE_TABLE_BYTES;
	memcpy(start, &full, FAKE_TABLE_BYTES);
	table = (struct NeurD const *)start;
	return table;
}

#ifdef FAKE_NEURD_EXPORT_SIZE
FAKE_EXPORT uint32_t NEURD_CALL
NeurD_get_table_size(void)
{
	return (uint32_t)FAKE_TABLE_BYTES;
}
#endif
