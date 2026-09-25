// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Minimal binary-interface declarations for the NeurD runtime DLL
 *         (Leia's 2D->3D conversion library), used by leia_lift_neurd.cpp.
 *
 * NeurD ships as a DLL whose only exported symbol is `NeurD_load`; it returns a
 * pointer to a versioned table of function pointers. This header declares just
 * enough of that contract to drive the D3D11 stream path — the enums and
 * structs the plug-in passes by value, and the table layout. It is written
 * against NeurD 0.4.6 and contains no NeurD code.
 *
 * ABI rules this relies on (NeurD's own compatibility policy):
 *  - table entries are append-only, never reordered or re-signatured;
 *  - `version` in the table is the LOADED runtime's version, so an entry may
 *    only be read when `version >= the entry's introduction version` (an older
 *    runtime's table is physically shorter — reading past it is UB). Use
 *    LEIA_NEURD_HAS() for every call.
 *
 * If NeurD ever breaks this (it would be a NeurD major), the version check in
 * leia_lift_neurd.cpp rejects a major other than 0 and lift reports unavailable.
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define LEIA_NEURD_CALL __stdcall
#else
#define LEIA_NEURD_CALL
#endif

#define LEIA_NEURD_MAKE_VERSION(major, minor, patch)                                                          \
	((((uint64_t)(major)&0xffffULL) << 48) | (((uint64_t)(minor)&0xffffULL) << 32) |                        \
	 ((uint64_t)(patch)&0xffffffffULL))
#define LEIA_NEURD_VERSION_MAJOR(v) ((uint32_t)(((uint64_t)(v) >> 48) & 0xffffULL))
#define LEIA_NEURD_VERSION_MINOR(v) ((uint32_t)(((uint64_t)(v) >> 32) & 0xffffULL))
#define LEIA_NEURD_VERSION_PATCH(v) ((uint32_t)((uint64_t)(v)&0xffffffffULL))

//! The table revision this header describes (requested in NeurD_load).
#define LEIA_NEURD_BUILT_AGAINST LEIA_NEURD_MAKE_VERSION(0, 4, 6)

enum leia_neurd_status
{
	LEIA_NEURD_SUCCESS = 0,
	LEIA_NEURD_IN_PROGRESS = 1,
	LEIA_NEURD_GENERIC_ERROR = -1,
	LEIA_NEURD_INVALID_ARG = -2,
	LEIA_NEURD_INVALID_LICENSE = -3,
	LEIA_NEURD_UNAVAILABLE_OUTDATED_RUNTIME = -4,
	LEIA_NEURD_NOT_INITIALIZED = -5,
	LEIA_NEURD_LICENSE_NETWORK_ERROR = -6,
	LEIA_NEURD_INVALID_MODEL = -7,
	LEIA_NEURD_FILESYSTEM_ERROR = -8,
	LEIA_NEURD_NETWORK_ERROR = -9,
	LEIA_NEURD_ABORTED = -10,
	LEIA_NEURD_STATUS_MAKE_32BIT = 0x7FFFFFFF
};

enum leia_neurd_pixel_format
{
	LEIA_NEURD_PIXEL_FORMAT_NV12 = 0,
	LEIA_NEURD_PIXEL_FORMAT_BGR8 = 1,
	LEIA_NEURD_PIXEL_FORMAT_BGR32 = 2,
	LEIA_NEURD_PIXEL_FORMAT_RGBA8 = 3,
	LEIA_NEURD_PIXEL_FORMAT_RGB8 = 4,
	LEIA_NEURD_PIXEL_FORMAT_MAKE_32BIT = 0x7FFFFFFF
};

//! For the DX entry points `data` is an ID3D11Buffer* (input) and an
//! ID3D11Buffer* or ID3D11Texture2D* (output, owned by NeurD).
struct leia_neurd_image
{
	void *data;
	int32_t width;
	int32_t height;
	int32_t stride;
	enum leia_neurd_pixel_format pix_fmt;
};

enum leia_neurd_output_type
{
	LEIA_NEURD_OUTPUT_SBS = 0,
	LEIA_NEURD_OUTPUT_TB = 1,
	LEIA_NEURD_OUTPUT_DEPTH = 2,
};

enum leia_neurd_autoscaling
{
	LEIA_NEURD_AUTOSCALING_NONE = 0,
	LEIA_NEURD_AUTOSCALING_720P = 1,
	LEIA_NEURD_AUTOSCALING_1080P = 2,
	LEIA_NEURD_AUTOSCALING_1440P = 3,
};

enum leia_neurd_inpaint
{
	LEIA_NEURD_INPAINT_V1_STRETCH = 0,
	LEIA_NEURD_INPAINT_V1_BLUR = 1,
};

enum leia_neurd_model_id
{
	LEIA_NEURD_MODEL_PHOTO_RELATIVE_QUALITY = 0,
	LEIA_NEURD_MODEL_VIDEO_RELATIVE_FAST = 1,
	LEIA_NEURD_MODEL_VIDEO_METRIC_QUALITY = 2,
};

struct leia_neurd_init_options
{
	uint32_t struct_size;
	int32_t photo_model_id;
	int32_t video_model_id;
};

//! Property ids (NeurD_prop). All are process-global inside NeurD.
enum leia_neurd_prop
{
	LEIA_NEURD_PROP_OUTPUT_TYPE = 0,        //!< int, leia_neurd_output_type
	LEIA_NEURD_PROP_GAIN_MULTIPLIER = 2,    //!< float, default 1
	LEIA_NEURD_PROP_CONVERGENCE = 3,        //!< float, must be in [-0.2, 0.2]
	LEIA_NEURD_PROP_INPUT_AUTOSCALING = 7,  //!< int, leia_neurd_autoscaling (default 1440p)
	LEIA_NEURD_PROP_INPAINT_TYPE = 8,       //!< int, leia_neurd_inpaint
	LEIA_NEURD_PROP_OUTPUT_TILES_W = 9,     //!< int, default 1
	LEIA_NEURD_PROP_OUTPUT_TILES_H = 10,    //!< int, default 1
	LEIA_NEURD_PROP_AUTO_CONVERGENCE = 11,  //!< int bool, default true
	LEIA_NEURD_PROP_MAKE_32BIT = 0x7FFFFFFF
};

enum leia_neurd_backend
{
	LEIA_NEURD_BACKEND_CUDA = 0,
	LEIA_NEURD_BACKEND_DIRECTML = 1,
	LEIA_NEURD_BACKEND_OPENVINO = 2,
	LEIA_NEURD_BACKEND_MAKE_32BIT = 0x7FFFFFFF
};

struct leia_neurd_load_request
{
	uint64_t version;
};

struct leia_neurd_stream; // opaque

typedef void *leia_neurd_fn_unused;

/*!
 * The NeurD function table, in NeurD's table order (append-only). Entries the
 * plug-in never calls are typed as leia_neurd_fn_unused — they only hold their
 * slot. The trailing comment is the version that introduced each entry.
 */
struct leia_neurd_table
{
	uint64_t version;

	enum leia_neurd_status(LEIA_NEURD_CALL *set_prop_1f)(enum leia_neurd_prop prop, float value); // 0.2.1
	enum leia_neurd_status(LEIA_NEURD_CALL *set_prop_1i)(enum leia_neurd_prop prop, int32_t value); // 0.2.1
	leia_neurd_fn_unused deprecated_init_license;                                                    // 0.2.1
	void(LEIA_NEURD_CALL *deinit)(void);                                                             // 0.2.1
	leia_neurd_fn_unused convert_cuda;                                                               // 0.2.1
	enum leia_neurd_status(LEIA_NEURD_CALL *init)(int32_t multithreaded);                            // 0.2.2
	leia_neurd_fn_unused get_prop_1f;                                                                // 0.2.2
	leia_neurd_fn_unused get_prop_1i;                                                                // 0.2.2
	enum leia_neurd_status(LEIA_NEURD_CALL *set_logger_callback)(void (*cb)(const char *msg, int size)); // 0.2.5
	leia_neurd_fn_unused get_cuda_runtime_version;                                                   // 0.2.7
	leia_neurd_fn_unused read_multiview;                                                             // 0.3.2
	leia_neurd_fn_unused get_multiview_attributes;                                                   // 0.3.2
	leia_neurd_fn_unused render_multiview;                                                           // 0.3.2
	leia_neurd_fn_unused render_multiview_interactive_cpu;                                           // 0.3.2
	leia_neurd_fn_unused render_multiview_interactive_cuda;                                          // 0.3.2
	leia_neurd_fn_unused free_multiview;                                                             // 0.3.2
	enum leia_neurd_status(LEIA_NEURD_CALL *set_backend)(enum leia_neurd_backend backend);           // 0.3.4
	enum leia_neurd_status(LEIA_NEURD_CALL *get_backend)(enum leia_neurd_backend *backend);          // 0.3.4
	leia_neurd_fn_unused convert_cpu;                                                                // 0.3.5
	leia_neurd_fn_unused prepare_multiview;                                                          // 0.3.7
	leia_neurd_fn_unused convert_dx;                                                                 // 0.3.8
	void(LEIA_NEURD_CALL *shrink_memory_pool)(void);                                                 // 0.3.10
	enum leia_neurd_status(LEIA_NEURD_CALL *create_stream)(struct leia_neurd_stream **stream);       // 0.3.11
	enum leia_neurd_status(LEIA_NEURD_CALL *destroy_stream)(struct leia_neurd_stream *stream);       // 0.3.11
	leia_neurd_fn_unused convert_stream_cpu;                                                         // 0.3.11
	leia_neurd_fn_unused convert_stream_cuda;                                                        // 0.3.11
	enum leia_neurd_status(LEIA_NEURD_CALL *convert_stream_dx)(struct leia_neurd_stream *stream,
	                                                           const struct leia_neurd_image *input,
	                                                           enum leia_neurd_pixel_format output_pix_fmt,
	                                                           struct leia_neurd_image *output); // 0.3.11
	leia_neurd_fn_unused convert_batch_cpu;                                                          // 0.4.3
	leia_neurd_fn_unused convert_batch_cuda;                                                         // 0.4.3
	leia_neurd_fn_unused convert_batch_dx;                                                           // 0.4.3
	void *(LEIA_NEURD_CALL *get_dx_device)(void);                                                    // 0.4.3
	leia_neurd_fn_unused convert_stream_cuda_interactive;                                            // 0.4.5
	enum leia_neurd_status(LEIA_NEURD_CALL *convert_stream_dx_interactive)(
	    struct leia_neurd_stream *stream,
	    const struct leia_neurd_image *input,
	    const float *viewpoint_xyz,
	    int size,
	    enum leia_neurd_pixel_format output_pix_fmt,
	    struct leia_neurd_image *output);                                                            // 0.4.5
	leia_neurd_fn_unused convert_batch_cuda_interactive;                                             // 0.4.5
	leia_neurd_fn_unused convert_batch_dx_interactive;                                               // 0.4.5
	enum leia_neurd_status(LEIA_NEURD_CALL *init_with_options)(int32_t multithreaded,
	                                                           const struct leia_neurd_init_options *opts); // 0.4.6
};

typedef const struct leia_neurd_table *(LEIA_NEURD_CALL *leia_neurd_load_fn)(const struct leia_neurd_load_request *req);

/*!
 * True when the loaded table carries @p field. The version test comes FIRST and
 * short-circuits, so a shorter (older) table is never read past its end.
 */
#define LEIA_NEURD_HAS(tbl, field, maj, min, pat)                                                             \
	((tbl) != NULL && (tbl)->version >= LEIA_NEURD_MAKE_VERSION(maj, min, pat) && (tbl)->field != NULL)

#ifdef __cplusplus
}
#endif
