// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Platform-neutral half of the Android stereo camera source's spec v8
 *         slots (runtime XR_DXR_stereo_camera §5.5-§5.7, §9b): the capture-mode
 *         list, the metering-point mapping, the control caps and the
 *         control -> Camera2 request plan, and the capture-result conversion.
 *
 * No Android, CNSDK or runtime dependency (the runtime's bit values are
 * mirrored here and pinned by the static asserts in the .cpp), so it is
 * host-tested (tests/test_stereo_camera_android_modes.c, Linux CI) like
 * leia_stereo_camera_android_calib.c. Design: docs/android-stereo-camera.md,
 * "Capture modes, controls and stills".
 *
 * @ingroup drv_leia_android
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *
 * Geometry.
 *
 */

/*!
 * The part of the active array (@p array_w x @p array_h) a @p stream_w x
 * @p stream_h stream sees: the largest centred region of the stream's aspect
 * (Camera2's rule for a stream whose aspect differs from the array's). Edge
 * coordinates, array pixels.
 */
struct leia_scam_region
{
	double x0, y0, w, h;
};

void
leia_scam_stream_region(uint32_t array_w,
                        uint32_t array_h,
                        uint32_t stream_w,
                        uint32_t stream_h,
                        struct leia_scam_region *out);

/*!
 * What one published eye is made of: each lens streams @p cap_w x @p cap_h,
 * the plug-in keeps its centred @p crop_w x @p crop_h (offsets
 * ((cap - crop) / 2) & ~1, pack_eye) and turns it @p rot clockwise. The
 * published eye is crop_w x crop_h, or crop_h x crop_w for 90 / 270.
 */
struct leia_scam_eye_geom
{
	uint32_t cap_w, cap_h;
	uint32_t crop_w, crop_h;
	uint32_t rot;
};

//! Published per-eye size of @p g.
void
leia_scam_eye_size(const struct leia_scam_eye_geom *g, uint32_t *out_w, uint32_t *out_h);

/*
 *
 * Capture modes (spec v8 §5.5).
 *
 */

//! Same values as the runtime's XRT_PLUGIN_STEREO_CAMERA_MODE_* (pinned in the .cpp).
#define LEIA_SCAM_MODE_STREAM (1u << 1)
#define LEIA_SCAM_MODE_STILL (1u << 2)

//! Modes one camera lists at most (the runtime asks for up to 16).
#define LEIA_SCAM_MODES_MAX 8u

//! How the source produces a mode (leia_scam_mode::source).
enum leia_scam_mode_source
{
	//! The default's own capture, cropped differently: a free switch, a free
	//! still (the next pair), and no change for the tracking feed.
	LEIA_SCAM_SRC_DEFAULT_CAPTURE = 0,
	//! Its own stream capture: a set_mode to it is a session reconfigure.
	LEIA_SCAM_SRC_OWN_CAPTURE = 1,
	//! A still from the JPEG output each lens has in the running session
	//! (stage 0: one YUV stream + one JPEG still output per lens is accepted,
	//! a still request on it costs no stream frame). Still only.
	LEIA_SCAM_SRC_STILL_OUTPUT = 2,
};

/*!
 * One listed mode and how the source runs it. @p geom.cap is the per-lens
 * capture (for LEIA_SCAM_SRC_STILL_OUTPUT the JPEG output's size), @p geom.crop
 * the centred part that is published.
 */
struct leia_scam_mode
{
	uint32_t eye_w, eye_h; //!< published per-eye size
	struct leia_scam_eye_geom geom;
	uint32_t flags;       //!< LEIA_SCAM_MODE_*
	uint32_t source;      //!< leia_scam_mode_source
	float max_frame_rate; //!< Hz when known, else 0
};

//! One output size listed on BOTH lenses of the pair.
struct leia_scam_size
{
	uint32_t w, h;
	//! ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS of the size, ns; 0 = not listed.
	int64_t min_frame_duration_ns;
};

/*!
 * What the source may list for one camera.
 *
 * The defaults the .cpp uses (each overridable by a system property):
 * - stream modes no larger than @p max_stream (debug.dxr.leia.scam_max_stream,
 *   default 1920x1440 on the rear pair: stage 0 measured YUV holding the
 *   sensor's 29.80 fps up to 3264x2448 per lens with the lowest latency at and
 *   below 1920x1440, and 4000x3000 / 4208x3120 dropping to ~27.4 fps);
 * - the still output no wider than @p max_still_w (debug.dxr.leia.scam_max_still,
 *   default 4096 wide per eye);
 * - @p reconfigure_stream / @p still_output false on a tracking-shared (front)
 *   pair: its tracker is fed from the same capture, and a session change (a
 *   reconfigure, or a JPEG output added on the tracking lens) is not something
 *   to do under it without a measurement.
 */
struct leia_scam_mode_policy
{
	uint32_t max_stream_w, max_stream_h; //!< own-capture STREAM modes no larger than this
	uint32_t max_still_w, max_still_h;   //!< still-output modes no larger than this (0 = no cap)
	bool reconfigure_stream;             //!< may a STREAM mode need its own capture session?
	bool still_output;                   //!< may the session carry a JPEG still output per lens?
	float default_rate;                  //!< the default capture's measured rate, 0 = not yet
	float own_rate;                      //!< the own stream capture's measured rate, 0 = not yet
	//! The longest frame a STREAM mode on its own capture may need, ns (1/30 s).
	int64_t max_stream_frame_ns;
};

/*!
 * Build the mode list of one camera. Mode 0 is the default @p def (STREAM |
 * STILL). Every further mode has the default's HORIZONTAL field of view about
 * the same optical axis with square pixels, the rule the runtime derives
 * their calibration by (§5.5):
 *
 * - from each listed size (w, h) the HAL serves the centred region of its
 *   aspect (leia_scam_stream_region), scaled by s = w / region.w on both axes;
 * - the eye keeps the columns that cover exactly the default's field,
 *   eye_w = F0 * s (F0 = the default eye's field in array columns), which must
 *   be a whole, even number of pixels with a centred crop at an even offset
 *   ((w - eye_w) % 4 == 0);
 * - rows: every row of the capture, and (for STREAM captures) also the
 *   default's own aspect cut from its centre (h - eye_h a multiple of 4);
 * - eye_w is at least the default's (the runtime scales smaller streams
 *   itself) and the relative height is within [1/2, 2] of the default's.
 *
 * A turned pair (@p def->rot != 0, a phone held upright) lists mode 0 only:
 * its published horizontal is the sensor's vertical.
 *
 * STREAM + STILL: modes on the default's capture (no session change at all).
 * STREAM: modes of ONE further capture, the largest @p stream_sizes entry
 * that fits @p max_stream and whose minimum frame duration is listed and at
 * most @p max_stream_frame_ns, when @p pol allows a reconfigure. The runtime
 * serves anything in between by downscaling, so one more capture is enough.
 * STILL: the single largest mode from @p still_sizes (the JPEG sizes) within
 * @p max_still, when @p pol allows a still output.
 * max_frame_rate: the measured rate of the mode's capture when known, else 0.
 *
 * Returns the number written (at most @p cap), mode 0 first, then by area.
 */
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
                      size_t cap);

/*!
 * Find the listed mode that a still request of @p w x @p h per eye names: an
 * exact STILL mode, or (0, 0) for the largest STILL mode. -1 when none.
 */
int
leia_scam_find_still_mode(const struct leia_scam_mode *modes, size_t n, uint32_t w, uint32_t h);

/*
 *
 * Metering point (spec v8 §5.6).
 *
 */

/*!
 * The metering point (@p u, @p v), normalised in the published left eye of a
 * frame made by @p g (0, 0 = top-left), as a normalised position of the
 * lens's active array (@p out_nx, @p out_ny in [0, 1]): undo the turn, add the
 * centred crop's offsets, then the HAL's array -> stream step. The source
 * mirrors nothing (a front frame is the sensor image, not a selfie mirror).
 * False on a degenerate geometry.
 */
bool
leia_scam_metering_to_array(double u,
                            double v,
                            uint32_t array_w,
                            uint32_t array_h,
                            const struct leia_scam_eye_geom *g,
                            double *out_nx,
                            double *out_ny);

/*!
 * A Camera2 metering rectangle (NDK layout xmin, ymin, xmax, ymax, weight;
 * max exclusive) of the logical camera's active array (@p log_w x @p log_h,
 * coordinates relative to it) centred on the normalised point (@p nx, @p ny),
 * @p frac of the shorter side wide, shifted (not shrunk) to stay inside.
 */
void
leia_scam_metering_region(double nx, double ny, uint32_t log_w, uint32_t log_h, double frac, int32_t out[5]);

/*
 *
 * Controls (spec v8 §5.6).
 *
 */

//! Same values as XRT_PLUGIN_STEREO_CAMERA_CONTROL_* (pinned in the .cpp).
#define LEIA_SCAM_CTL_FOCUS_MODE (1u << 0)
#define LEIA_SCAM_CTL_FOCUS_DISTANCE (1u << 1)
#define LEIA_SCAM_CTL_METERING_POINT (1u << 2)
#define LEIA_SCAM_CTL_EXPOSURE_COMPENSATION (1u << 3)

//! Same values as xrt_plugin_stereo_camera_focus_mode.
enum leia_scam_focus_mode
{
	LEIA_SCAM_FOCUS_CONTINUOUS_AUTO = 1,
	LEIA_SCAM_FOCUS_AUTO_ONCE = 2,
	LEIA_SCAM_FOCUS_MANUAL = 3,
	LEIA_SCAM_FOCUS_LOCKED = 4,
};

//! Camera2 values used below (acamera_metadata_enum_android_control_af_mode_t).
enum leia_scam_af_mode
{
	LEIA_SCAM_AF_OFF = 0,
	LEIA_SCAM_AF_AUTO = 1,
	LEIA_SCAM_AF_MACRO = 2,
	LEIA_SCAM_AF_CONTINUOUS_VIDEO = 3,
	LEIA_SCAM_AF_CONTINUOUS_PICTURE = 4,
	LEIA_SCAM_AF_EDOF = 5,
};

//! What the HAL says about one logical camera (its characteristics).
struct leia_scam_hal_caps
{
	const uint8_t *af_modes; //!< ACAMERA_CONTROL_AF_AVAILABLE_MODES
	size_t n_af_modes;
	float min_focus_distance; //!< ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE (dioptres, or HAL units if uncalibrated)
	bool have_min_focus_distance;
	uint8_t focus_distance_calibration; //!< ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION (0/1/2 = the runtime's enum)
	bool manual_sensor;                 //!< REQUEST_AVAILABLE_CAPABILITIES has MANUAL_SENSOR
	int32_t max_regions_ae, max_regions_af; //!< ACAMERA_CONTROL_MAX_REGIONS [0] and [2]
	int32_t ae_comp_min, ae_comp_max;       //!< ACAMERA_CONTROL_AE_COMPENSATION_RANGE
	int32_t ae_comp_step_num, ae_comp_step_den; //!< ACAMERA_CONTROL_AE_COMPENSATION_STEP
};

//! Mirror of xrt_plugin_stereo_camera_control_caps (without struct_size).
struct leia_scam_control_caps
{
	uint32_t supported_controls;
	uint32_t supported_focus_modes; //!< 1 << leia_scam_focus_mode
	float min_focus_distance_diopters;
	uint32_t focus_distance_calibration;
	float exposure_compensation_min_ev, exposure_compensation_max_ev, exposure_compensation_step_ev;
};

/*!
 * The controls the source advertises for one camera, from the HAL's caps.
 *
 * Focus (only for a lens that focuses, min focus distance > 0):
 * - @p hal_af false (the default for a logical multi-lens pair): MANUAL and
 *   LOCKED only, both with the AF state machine OFF and one logical
 *   LENS_FOCUS_DISTANCE, which needs AF mode OFF and MANUAL_SENSOR. Stage 0 on
 *   the tablet's rear pair: one logical distance moves BOTH lenses (within
 *   ~0.03 D, settled ~260 ms later), while continuous AF and an AF trigger
 *   move lens 0 only and the HAL offers no per-lens request keys, so the
 *   HAL's AF would leave the two eyes at different focus. The app focuses
 *   from the stereo pair itself.
 * - @p hal_af true (debug.dxr.leia.scam_hal_af=1): also CONTINUOUS_AUTO
 *   (CONTINUOUS_PICTURE / _VIDEO) and AUTO_ONCE (AUTO), LOCKED via AUTO.
 * METERING_POINT when the HAL takes AE regions (and, with @p hal_af, AF
 * regions); EXPOSURE_COMPENSATION when the range is not [0, 0] and step > 0.
 *
 * @p tracking_shared (the front pair, whose lens also feeds head tracking):
 * exposure compensation and the metering point move the exposure the
 * tracker sees, so both stay off unless @p allow_exposure_on_shared
 * (debug.dxr.leia.scam_front_controls=1). The tablets' front lenses are fixed
 * focus, so the front pair then advertises no controls at all.
 */
void
leia_scam_control_caps_from_hal(const struct leia_scam_hal_caps *hal,
                                bool hal_af,
                                bool tracking_shared,
                                bool allow_exposure_on_shared,
                                struct leia_scam_control_caps *out);

//! Mirror of xrt_plugin_stereo_camera_controls (without struct_size).
struct leia_scam_controls
{
	uint32_t valid_controls;
	uint32_t focus_mode;
	uint32_t focus_trigger;
	float focus_distance_diopters;
	float metering_x, metering_y;
	float exposure_compensation_ev;
};

/*!
 * What to write into the Camera2 request for a control state. "default"
 * means: put back what the request template had (the source's own default).
 */
struct leia_scam_request_plan
{
	bool af_mode_default;
	uint8_t af_mode; //!< leia_scam_af_mode, when !af_mode_default
	bool set_focus_distance;
	float focus_distance; //!< LENS_FOCUS_DISTANCE, clamped to [0, min focus distance]
	//! Send one capture with AF_TRIGGER_START (AUTO_ONCE and the trigger moved).
	bool af_trigger_start;
	bool regions_default; //!< no metering point: the template's (usually no) regions
	bool set_af_regions, set_ae_regions;
	int32_t region[5];
	bool ae_comp_default;
	int32_t ae_comp_index; //!< CONTROL_AE_EXPOSURE_COMPENSATION
};

/*!
 * How the source focuses a lens that can (see leia_scam_control_caps_from_hal).
 */
struct leia_scam_focus_policy
{
	bool hal_af;           //!< the HAL's AF state machine may be used
	float default_d;       //!< without hal_af: the distance a cleared focus bit means, dioptres
	float current_d;       //!< without hal_af: the distance commanded now (LOCKED keeps it)
};

/*!
 * The request plan for the WHOLE wanted state @p c (a clear bit = the
 * source's default; valid_controls == 0 = every default), given the caps the
 * camera advertised (@p caps, @p hal), the focus policy @p fp and the trigger
 * last acted on (@p *trigger_seen, updated). @p region_ok says whether the
 * metering point could be mapped (@p region then holds its rectangle).
 * Out-of-range values the runtime should already have refused are clamped,
 * never trusted.
 *
 * Without hal_af, for a lens that focuses, the plan ALWAYS has AF OFF and a
 * distance: MANUAL = the asked distance (or the current one when none was
 * given), LOCKED = the current one, focus bit clear = fp->default_d. So the
 * source's own default focus is a fixed distance, never the HAL's AF.
 */
void
leia_scam_plan_controls(const struct leia_scam_controls *c,
                        const struct leia_scam_control_caps *caps,
                        const struct leia_scam_hal_caps *hal,
                        const struct leia_scam_focus_policy *fp,
                        bool region_ok,
                        const int32_t region[5],
                        uint32_t *trigger_seen,
                        struct leia_scam_request_plan *out);

//! EV -> the HAL's compensation index (rounded to the step, clamped to the range).
int32_t
leia_scam_ev_to_index(float ev, const struct leia_scam_hal_caps *hal);

/*
 *
 * Capture result (spec v8 §5.6).
 *
 */

//! Same values as XRT_PLUGIN_STEREO_CAMERA_RESULT_* (pinned in the .cpp).
#define LEIA_SCAM_RES_FOCUS_DISTANCE (1u << 0)
#define LEIA_SCAM_RES_FOCUS_STATE (1u << 1)
#define LEIA_SCAM_RES_EXPOSURE_TIME (1u << 2)
#define LEIA_SCAM_RES_SENSITIVITY (1u << 3)
#define LEIA_SCAM_RES_EYE_TIMES (1u << 4)

//! Same values as xrt_plugin_stereo_camera_focus_state.
enum leia_scam_focus_state
{
	LEIA_SCAM_FOCUS_STATE_UNKNOWN = 0,
	LEIA_SCAM_FOCUS_STATE_SEARCHING = 1,
	LEIA_SCAM_FOCUS_STATE_LOCKED = 2,
	LEIA_SCAM_FOCUS_STATE_FIXED = 3,
	LEIA_SCAM_FOCUS_STATE_FAILED = 4,
};

//! What one capture's metadata said (each field only when the HAL put it there).
struct leia_scam_hal_result
{
	bool have_af_state;
	uint8_t af_state; //!< ACAMERA_CONTROL_AF_STATE
	bool have_focus_distance;
	float focus_distance; //!< ACAMERA_LENS_FOCUS_DISTANCE
	bool have_exposure;
	int64_t exposure_ns; //!< ACAMERA_SENSOR_EXPOSURE_TIME
	bool have_iso;
	int32_t iso; //!< ACAMERA_SENSOR_SENSITIVITY
	bool have_lens_state;
	uint8_t lens_state; //!< ACAMERA_LENS_STATE (0 STATIONARY, 1 MOVING)
	bool have_timestamp;
	int64_t timestamp; //!< ACAMERA_SENSOR_TIMESTAMP
};

//! Mirror of xrt_plugin_stereo_camera_capture_result (without struct_size / reserved).
struct leia_scam_capture_state
{
	uint32_t valid_fields;
	float focus_distance_diopters;
	uint32_t focus_state;
	int64_t exposure_time_ns;
	uint32_t sensitivity_iso;
	int64_t eye_time_ns[2];
};

/*!
 * Camera2 AF state -> the runtime's focus state. PASSIVE/ACTIVE_SCAN =
 * SEARCHING, PASSIVE_FOCUSED / FOCUSED_LOCKED = LOCKED, NOT_FOCUSED_LOCKED /
 * PASSIVE_UNFOCUSED = FAILED; INACTIVE = LOCKED when the source holds the lens
 * itself (@p holding: MANUAL or LOCKED, AF off or AUTO without a trigger),
 * else UNKNOWN (no search yet).
 */
uint32_t
leia_scam_focus_state_from_af(uint8_t af_state, bool holding);

/*!
 * One frame's capture state, only what the HAL reported:
 * - exposure / ISO: the left eye lens's physical result (@p left), else the
 *   logical one;
 * - focus distance: the logical result (it commands both lenses; with
 *   APPROXIMATE calibration it may be the commanded value, not a sensed one),
 *   else the left lens's;
 * - focus state: FIXED for a fixed-focus lens (@p fixed_focus, a HAL
 *   characteristic); with @p hal_af the logical AF state
 *   (leia_scam_focus_state_from_af, @p holding); without it the lenses' own
 *   LENS_STATE: MOVING on either = SEARCHING, STATIONARY on both = LOCKED;
 * - per-eye times: only when @p eye_times_independent (see
 *   leia_scam_eye_times_tracker): the two physical results' SENSOR_TIMESTAMP,
 *   converted by @p boot_to_mono_ns (subtracted).
 */
void
leia_scam_capture_state_from_hal(const struct leia_scam_hal_result *left,
                                 const struct leia_scam_hal_result *right,
                                 const struct leia_scam_hal_result *logical,
                                 bool fixed_focus,
                                 bool hal_af,
                                 bool holding,
                                 bool eye_times_independent,
                                 int64_t boot_to_mono_ns,
                                 struct leia_scam_capture_state *out);

/*!
 * Do the two lenses' physical results carry INDEPENDENT start-of-exposure
 * times? Stage 0: the tablet's HAL stamps both lenses with one synthesized
 * time on 99.7 % of frames although lens 2's buffers arrive 12-16 ms later,
 * so equal stamps are not evidence of sync and must not be reported as two
 * measured eye times. The source reports per-eye times only once at least
 * LEIA_SCAM_EYE_TIMES_MIN_FRAMES frames had both physical stamps and at
 * least 1 in LEIA_SCAM_EYE_TIMES_DIFF_DIV of them differed.
 */
#define LEIA_SCAM_EYE_TIMES_MIN_FRAMES 60u
#define LEIA_SCAM_EYE_TIMES_DIFF_DIV 10u

struct leia_scam_eye_times_tracker
{
	uint32_t both;   //!< frames with both physical stamps
	uint32_t differ; //!< of which the two stamps differed
};

void
leia_scam_eye_times_observe(struct leia_scam_eye_times_tracker *t, int64_t left_ts, int64_t right_ts);

bool
leia_scam_eye_times_independent(const struct leia_scam_eye_times_tracker *t);

#ifdef __cplusplus
}
#endif
