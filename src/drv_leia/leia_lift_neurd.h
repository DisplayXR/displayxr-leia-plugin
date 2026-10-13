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

//! Values identical to the runtime's XRT_DP_LIFT_MODULE_* (module info for the
//! dashboard / `displayxr-cli info`: which NeurD was found and why it is not usable).
#define LEIA_LIFT_MODULE_UNKNOWN 0u
#define LEIA_LIFT_MODULE_READY 1u
#define LEIA_LIFT_MODULE_ACTIVATING 2u
#define LEIA_LIFT_MODULE_ABSENT 3u
#define LEIA_LIFT_MODULE_TOO_OLD 4u
#define LEIA_LIFT_MODULE_UNLICENSED 5u
#define LEIA_LIFT_MODULE_LOAD_FAILED 6u
#define LEIA_LIFT_MODULE_DISABLED 7u

struct leia_lift_neurd_caps
{
	uint32_t modes;           //!< LEIA_LIFT_MODE_* bits; 0 when unavailable.
	uint32_t max_streams;     //!< Concurrent streams (NeurD limit, process-wide).
	uint32_t max_views;       //!< Max view_count for NVIEW.
	uint32_t depth_semantics; //!< 0 = relative (NeurD: min-max normalised disparity).
	uint32_t state;           //!< LEIA_LIFT_STATE_*.
	uint64_t typical_latency_ns;
	char backend[32]; //!< e.g. "neurd-directml"; "" when unavailable.
	uint32_t module_status;  //!< LEIA_LIFT_MODULE_*.
	char module_version[48]; //!< NeurD's installed version string; "" = none / unknown.
	char module_detail[96];  //!< One line for a human; display only.
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

	/*!
	 * Conversion provider only (src/conv_leia): the zero-parallax plane as a
	 * METRIC depth in metres (> 0), in the units of the metric aux depth this
	 * stream returned. Converted to @ref convergence here, with the stream's own
	 * metric model scale; until that scale is known the convert runs AUTO.
	 * 0 = unused (@ref convergence applies). The display processor leaves it 0.
	 */
	float convergence_metric_m;
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
	//! Off-axis conversions only: z in front of the screen (metres) =
	//! relief_scale / depth + relief_offset (leia_lift_oa_relief).
	uint32_t relief_valid;
	float relief_scale, relief_offset;
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

/*
 *
 * Conversion-provider hosting (src/conv_leia, ADR-053). The display processor
 * never calls these; the module then behaves exactly as described above.
 *
 */

/*!
 * Configuration reader: the conversion provider passes the runtime's
 * host->get_setting here. Once set, every knob is read through it ONLY (no
 * environment, no HKLM\SOFTWARE\DisplayXR\Leia\Lift): keys are the registry
 * value names (Backend, MinVersion, ViewGain, ...), plus Enable (the
 * DXR_LEIA_LIFT switch) and NeurDPath (an absolute NeurD.dll path that, when
 * set, is the ONLY place looked at). Set before the first
 * leia_lift_neurd_create. NULL restores env > HKLM.
 */
typedef bool (*leia_lift_neurd_setting_fn)(const char *key, char *buf, uint32_t buf_size);

void
leia_lift_neurd_set_setting_reader(leia_lift_neurd_setting_fn fn);

/*!
 * Manual activation (conversion provider): the module never starts its
 * background activation thread and never re-probes on its own. caps / stream /
 * convert calls only read the state; leia_lift_neurd_activate() does
 * discovery + load + init on the CALLER's thread. Set before the first
 * leia_lift_neurd_create.
 */
void
leia_lift_neurd_set_manual_activation(bool manual);

//! NeurD.dll as discovery sees it, WITHOUT loading it.
struct leia_lift_neurd_presence
{
	bool enabled;   //!< false: Enable=0 (nothing looked at)
	bool found;     //!< a NeurD.dll was found
	char path[260]; //!< its full path
	char where[48]; //!< which discovery step found it
	//! Its VERSIONINFO file version; false = no usable resource (the floor is then
	//! checked after the load, in activate).
	bool have_version;
	uint32_t ver_major, ver_minor, ver_patch;
	bool too_old; //!< have_version and older than the floor
	uint32_t min_major, min_minor, min_patch;
	char product_name[64]; //!< VERSIONINFO ProductName; "" = none
};

/*!
 * Discovery only (SearchPath, one registry read, the version resource): no
 * LoadLibrary, no thread. Bounded file I/O — call from a control thread.
 */
bool
leia_lift_neurd_probe_presence(struct leia_lift_neurd *lift, struct leia_lift_neurd_presence *out);

//! The process-wide module state, from memory only (no kick, no I/O, no lock
//! held across a NeurD call).
struct leia_lift_neurd_status
{
	uint32_t module_status; //!< LEIA_LIFT_MODULE_*
	//! ACTIVATING because the licence activation needs the network (retry later).
	bool network_retry;
	//! FAILED for a reason a different NeurD.dll can fix (too old, unloadable).
	bool recheckable;
	char version[48];  //!< the loaded NeurD's version, "" when none
	char detail[96];   //!< one line for a human
	char licence[64];  //!< NeurD's own licence line (empty until it printed one)
	char backend[32];  //!< "neurd-<backend>" once READY, else ""
	bool float_depth;  //!< READY: SBS streams can return the depth of the same inference
	bool metric_depth; //!< ...and it is metric (VideoModel=metric)
	bool offaxis;      //!< READY: the metric off-axis camera is available
	uint64_t latency_ns; //!< EMA of a full convert; 0 = none yet
	uint64_t nd_luid;    //!< NeurD's D3D11 adapter LUID (READY), else 0
};

void
leia_lift_neurd_get_status(struct leia_lift_neurd *lift, struct leia_lift_neurd_status *out);

/*!
 * Manual mode: discovery + (re)activation on the caller's thread. BLOCKS for
 * the load and NeurD's init (licence activation, model load — seconds, and
 * not interruptible once NeurD_init is running). Re-probes at once (no 30 s
 * rate limit) and ignores the network-retry timer: the caller owns the retry
 * policy. Returns the resulting LEIA_LIFT_MODULE_* status.
 */
uint32_t
leia_lift_neurd_activate(struct leia_lift_neurd *lift);

/*!
 * Drop the NeurD stream behind @p id (its temporal state); it is re-created
 * lazily on the next convert. For a cut / seek (FRAME_RESET).
 */
void
leia_lift_neurd_stream_reset(struct leia_lift_neurd *lift, uint64_t id);

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
