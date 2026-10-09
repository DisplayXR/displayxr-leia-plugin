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
 * every other entry point returns false. Nothing is loaded. Absence is not
 * permanent: discovery re-runs every 30 s from caps and at once on each
 * stream_create, so a NeurD installed later is adopted without a restart; a
 * NeurD refused as too old is re-checked when NeurD.dll changes.
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
	//! LEIA_LIFT_AUX_DEPTH when SBS / NVIEW streams can return the depth of
	//! the same inference (NeurD >= 0.4.9 convert_stream_dx_ex want_depth,
	//! DirectML).
	uint32_t aux_outputs;
	//! LEIA_LIFT_DEPTH_UNITS_* (leia_lift_depth.h) of that depth: METRIC
	//! with the metric video model, RELATIVE otherwise.
	uint32_t aux_depth_semantics;
};

//! Auxiliary output bit (identical value to the runtime's
//! XRT_DP_LIFT_AUX_DEPTH).
#define LEIA_LIFT_AUX_DEPTH 1u

struct leia_lift_neurd_stream_desc
{
	uint32_t mode;         //!< One LEIA_LIFT_MODE_* value.
	uint32_t content_hint; //!< 0 video, 1 photo (advisory; the DX path is video-model only).
	float input_scale;     //!< (0,1] fraction of input height to infer at (1 = native);
	                       //!< outside that, or DXR_LEIA_LIFT_SCALE set = the knob.
	uint32_t aux_outputs;  //!< LEIA_LIFT_AUX_* the runtime asked for (SBS /
	                       //!< NVIEW only).
};

struct leia_lift_neurd_params
{
	float convergence; //!< <0 = NeurD auto-convergence; else relative depth at the display plane
	                   //!< in [0,1], mapped to NeurD units by DXR_LEIA_LIFT_CONV_GAIN.
	float strength;    //!< NeurD gain multiplier: 1 = calibrated budget, 0 = flat; <0 = 1.0; max 10.
	uint32_t inpaint;  //!< 0 stretch fill, non-zero blur fill.
	uint32_t view_count; //!< NVIEW only (2..max_views).

	/*
	 * Viewpoint policy (runtime ADR-048). has_policy = the runtime filled these
	 * (its xrt_dp_lift_params covers them); false = the legacy contract
	 * (panel-centred eyes, x only, fixed 63 mm unit, ±3 clamp). Applied only to
	 * viewpoints the RUNTIME passed — eyes the DP reads from its own tracker
	 * are panel-centred and unprocessed, so they always map the legacy way.
	 */
	bool has_policy;
	float rect_width_m;       //!< Lifted rect size, metres; 0 = unknown (logged only).
	float rect_height_m;      //!< ditto
	float baseline_m;         //!< Eye separation the runtime used, metres; 0 = unknown. Clamp margin + log only.
	uint32_t axis_mode;       //!< LEIA_LIFT_AXIS_* (leia_lift_viewpoint.h).
	float max_offset_m;       //!< The runtime's x/y midpoint clamp, metres; 0 = unclamped.
	uint32_t viewpoint_frame; //!< 0 = panel centre, 1 = rect centre (logged only).
	float ref_z_m;            //!< Reference viewing distance (panel nominal), metres; <= 0 = 0.5.

	/*
	 * App rig (runtime ADR-048 Addendum A). has_app_rig = the runtime
	 * filled these. viewpoint_source DISPLAY_RIG / CAMERA_RIG = the app's
	 * rig eyes: reproduced exactly on the off-axis path (no ViewGain /
	 * YGain).
	 */
	bool has_app_rig;
	uint32_t viewpoint_source; //!< LEIA_LIFT_VIEWPOINTS_* (leia_lift_depth.h).
	float nominal_z_m;         //!< The runtime's reference viewing distance,
	                           //!< metres; 0 = unknown (use ref_z_m).
};

/*!
 * The auxiliary depth of the last conversion on a stream (mirrors the
 * runtime's xrt_dp_lift_depth). resource is owned by the stream and valid
 * until the next convert on it.
 */
struct leia_lift_neurd_depth
{
	void *resource;  //!< ID3D11Texture2D* (R32_FLOAT) on the caller's device.
	uint32_t format; //!< DXGI_FORMAT (R32_FLOAT).
	uint32_t width, height;
	uint32_t units,
	    encoding; //!< LEIA_LIFT_DEPTH_UNITS_* / _ENCODING_* (leia_lift_depth.h)
	float value_scale, value_offset;
	uint32_t source_width, source_height; //!< w x h of the convert call
	float focal_x_px, focal_y_px, principal_x_px, principal_y_px;
	float near_depth, far_depth;
	float convergence_depth;
	uint32_t same_inference;
	uint64_t vendor_frame_id;
};

struct leia_lift_neurd;

/*!
 * Create a per-DP handle. Reads the knobs (env > HKLM > default) once for the
 * process-level ones (enable, backend, versions, video model). The per-convert
 * knobs (ViewGain, YGain, ZGain, ConvGain, DepthGain, Dilate, Scale, OffAxis,
 * ReliefDepthM, MetricReliefMaxM) are re-read and snapshotted per STREAM at
 * stream create. Cheap: no DLL load, no thread. Never fails (a disabled handle
 * reports unavailable).
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
 * @param viewpoints_m   Optional viewpoints from the runtime: (x,y,z) triplets
 *                       in METRES — rect-relative and policy-processed when
 *                       @p params has_policy, else panel-centred display
 *                       space. NULL/0 = use @p eye_left / @p eye_right (the
 *                       DP's own tracker; always mapped the legacy way).
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
 * The depth NeurD retained for the conversion leia_lift_neurd_convert just
 * returned on stream @p id (created with LEIA_LIFT_AUX_DEPTH). Non-blocking:
 * the depth was bridged during that convert. False = none (not requested,
 * NeurD < 0.4.9 / not DirectML, or the last convert returned no depth).
 */
bool
leia_lift_neurd_get_depth(struct leia_lift_neurd *lift, uint64_t id, struct leia_lift_neurd_depth *out);

/*!
 * LEGACY mapping (kept for callers of the old helper): x_n = clamp(gain *
 * x_m / 0.063, ±3), y_n = z_n = 0. A centred viewer at 63 mm IPD maps to
 * x = -0.5 / +0.5 — exactly NeurD's default stereo pattern. The full mapping,
 * including the runtime's viewpoint policy, is leia_lift_vp_map()
 * (leia_lift_viewpoint.h); see docs/lift-neurd.md.
 */
void
leia_lift_neurd_map_viewpoint(const float in_m[3], float gain, float out_n[3]);

#ifdef __cplusplus
}
#endif
