// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  NeurD-backed 2D->3D conversion ("lift") for the Leia D3D11 display
 *         processor. See docs/lift-neurd.md.
 *
 * Plug-in-owned types only — the D3D11 DP translates the runtime's
 * xrt_dp_lift_* contract (behind XRT_DP_D3D11_HAS_LIFT) onto this, so the
 * module builds against any runtime pin.
 *
 * Process model: ONE NeurD instance per process, loaded + initialised lazily on
 * a background thread the first time a DP asks for caps or a stream (never at
 * DP create, never on the caller's thread — first activation runs a licence
 * check over the network). Each DP owns a leia_lift_neurd handle holding its
 * streams; handles ref-count the instance.
 *
 * Absent NeurD.dll (or DXR_LEIA_LIFT=0): caps report modes=0 / state=0 and
 * every other entry point returns false. Nothing is loaded.
 *
 * Threading: every entry point is callable from any thread. convert() is
 * synchronous and blocking (tens of ms) — call it only from a worker thread,
 * never a render/IPC thread. Calls are serialised process-wide (NeurD's
 * properties are global).
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Mode bits (identical values to the runtime's lift contract).
#define LEIA_LIFT_MODE_DEPTH 1u
#define LEIA_LIFT_MODE_SBS 2u
#define LEIA_LIFT_MODE_NVIEW 4u

//! Caps state (identical values to the runtime's lift contract).
#define LEIA_LIFT_STATE_UNAVAILABLE 0u
#define LEIA_LIFT_STATE_ACTIVATING 1u
#define LEIA_LIFT_STATE_READY 2u

struct leia_lift_neurd_caps
{
	uint32_t modes;           //!< LEIA_LIFT_MODE_* bits; 0 when unavailable.
	uint32_t max_streams;     //!< Concurrent streams (NeurD limit, process-wide).
	uint32_t max_views;       //!< Max view_count for NVIEW.
	uint32_t depth_semantics; //!< 0 = relative (NeurD: min-max normalised disparity).
	uint32_t state;           //!< LEIA_LIFT_STATE_*.
	uint64_t typical_latency_ns;
	char backend[32]; //!< e.g. "neurd-directml"; "" when unavailable.
};

struct leia_lift_neurd_stream_desc
{
	uint32_t mode;         //!< One LEIA_LIFT_MODE_* value.
	uint32_t content_hint; //!< 0 video, 1 photo (advisory; the DX path is video-model only).
	float input_scale;     //!< (0,1] fraction of input height to infer at (1 = native);
	                       //!< outside that, or DXR_LEIA_LIFT_SCALE set = the knob.
};

struct leia_lift_neurd_params
{
	float convergence; //!< <0 = NeurD auto-convergence; else relative depth at the display plane
	                   //!< in [0,1], mapped to NeurD units by DXR_LEIA_LIFT_CONV_GAIN.
	float strength;    //!< NeurD gain multiplier: 1 = calibrated budget, 0 = flat; <0 = 1.0; max 10.
	uint32_t inpaint;  //!< 0 stretch fill, non-zero blur fill.
	uint32_t view_count; //!< NVIEW only (2..max_views).
};

struct leia_lift_neurd;

/*!
 * Create a per-DP handle. Reads the env knobs (DXR_LEIA_LIFT,
 * DXR_LEIA_LIFT_BACKEND, DXR_LEIA_LIFT_SCALE, DXR_LEIA_LIFT_VIEW_GAIN) once.
 * Cheap: no DLL load, no thread. Never fails (a disabled handle reports
 * unavailable).
 */
struct leia_lift_neurd *
leia_lift_neurd_create(void);

void
leia_lift_neurd_destroy(struct leia_lift_neurd **lift);

//! Non-blocking. May kick the background load/activation (or a licence retry).
bool
leia_lift_neurd_get_caps(struct leia_lift_neurd *lift, struct leia_lift_neurd_caps *out);

//! Non-blocking. Succeeds while NeurD is still activating (the NeurD stream is
//! created lazily on the first convert); fails when unavailable / out of slots.
bool
leia_lift_neurd_stream_create(struct leia_lift_neurd *lift,
                              const struct leia_lift_neurd_stream_desc *desc,
                              uint64_t *out_id);

void
leia_lift_neurd_stream_destroy(struct leia_lift_neurd *lift, uint64_t id);

/*!
 * Convert one frame. SYNCHRONOUS and blocking.
 *
 * @param d3d11_context  Caller's immediate context (the device that owns
 *                       @p input). Must be ID3D11Multithread-protected — it is
 *                       enabled here if it is not.
 * @param input          ID3D11Texture2D* (as ID3D11Resource*), RGBA8 or BGRA8
 *                       family, single-sampled; the top-left w x h is used.
 * @param viewpoints_m   Optional explicit viewpoints: (x,y,z) triplets in
 *                       METRES, display space (same space as tracked eyes).
 *                       NULL/0 = use @p eye_left / @p eye_right.
 * @param eyes_valid     False (or NULL eyes) = no tracked viewer: NeurD's
 *                       default pattern is used.
 * @param out_resource   ID3D11Texture2D* on the caller's device, owned by the
 *                       stream, valid until the next convert on this stream.
 * @param out_format     DXGI_FORMAT: R8G8B8A8_UNORM (SBS / NVIEW, N views in one
 *                       row, view 0 leftmost) or R8_UNORM (DEPTH).
 */
bool
leia_lift_neurd_convert(struct leia_lift_neurd *lift,
                        uint64_t id,
                        void *d3d11_context,
                        void *input,
                        uint32_t w,
                        uint32_t h,
                        const struct leia_lift_neurd_params *params,
                        const float *viewpoints_m,
                        uint32_t viewpoint_floats,
                        const float *eye_left,
                        const float *eye_right,
                        bool eyes_valid,
                        void **out_resource,
                        uint32_t *out_w,
                        uint32_t *out_h,
                        uint32_t *out_format);

/*!
 * Pure helper, exposed for tests + docs: map display-space positions (metres)
 * to NeurD's dimensionless viewpoint units. See docs/lift-neurd.md.
 *
 * x_n = clamp(gain * x_m / 0.063), y_n = clamp(gain * y_m / 0.063), z_n = 0.
 * A centred viewer at 63 mm IPD maps to x = -0.5 / +0.5 — exactly NeurD's
 * default stereo pattern — so gain 1.0 reproduces the untracked output and
 * head motion becomes look-around.
 */
void
leia_lift_neurd_map_viewpoint(const float in_m[3], float gain, float out_n[3]);

#ifdef __cplusplus
}
#endif
