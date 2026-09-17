// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Opt-in, PURELY OBSERVATIONAL per-weave eye-prediction recorder.
 * @ingroup drv_leia
 *
 * ## What this is for
 *
 * The absolute-target arm (`DXR_LEIA_SR_TARGET_TIME=1`) and the adaptive
 * `setLatency` arm both hand the SR predictor a horizon, and both then weave.
 * Which one predicts the eyes better is not answerable from a log line, and it
 * is not answerable from a blind A/B either -- a two-arm eyeball test on a
 * quantity this small is underpowered by construction. What IS answerable is
 * the OFFLINE prediction error: for every weave, record where the predictor
 * said the eyes would be at photon time, and record the raw tracker samples
 * that later say where they actually were. The join is done afterwards, by
 * `tools/predict_trace_score.py`, against the real measurement.
 *
 * ## The rules this file exists to obey
 *
 * - **Observational.** Nothing here adds, moves or removes a
 *   `srWeaverSetTargetTime` / `srWeaverSetLatency` call, and nothing here
 *   changes what the converged push block decides. The arm under test is the
 *   SHIPPING path, chosen by the existing env var. If enabling the recorder
 *   changed the thing being recorded, the record would be worthless.
 * - **Off the hot path.** The SDK's tracker and system-monitor callbacks fire
 *   on the SDK's own threads and may only push into a lock-free SPSC ring. The
 *   weave thread likewise pushes one fixed-size binary record and returns --
 *   no formatting, no allocation, no lock, no I/O. A low-priority writer
 *   thread drains all three rings and does the text.
 * - **No per-frame logging.** Every U_LOG in this module is a create-time or
 *   teardown-time line, or a one-shot latch. There is no throttled per-frame
 *   WARN, because a throttled per-frame WARN is still a per-frame WARN.
 * - **Never crash.** Every SDK entry point used here is an appended dispatch
 *   slot on some runtimes; the loader trampolines answer
 *   `SR_ERROR_FUNCTION_UNSUPPORTED` rather than dispatching into a NULL. A
 *   missing function logs ONE warning and disables the recorder for that
 *   weaver; it never takes the weave path down with it.
 *
 * ## Lifecycle
 *
 * The recorder owns its OWN `SrInstance`, and the eye tracker and system
 * monitor are created on THAT, never on the weaver's. This is not tidiness: the
 * SDK's eye tracker is a `PredictingEyeTracker` per `SRContext` (one per
 * instance) whose post-stages -- speed limiter, exponential decay, noise
 * rejection -- are stateful PER CALL, so a reference predict issued on the
 * weaver's instance microseconds after the weaver's own getter simply returns
 * most of the weaver's answer back. A separate instance is a separate context,
 * a separate predictor chain, and a reference that never sees target mode. Only
 * `srGetTimeUs` still goes to the weaver's instance, so the clock domain and
 * the file's clock pair are unchanged (the clock is machine-wide
 * QPC-since-boot; the instance is a validity argument, `sr_instance.h:514-553`).
 *
 * The eye tracker and the system monitor are SENSES: the SDK requires them,
 * and their callbacks, to exist BEFORE `srInitialize` runs on the owning
 * instance (`sr_eye_tracker.h:179,258`; the system monitor is created the same
 * way in the Linux arm, `drv_leia_linux/leia_sr_linux_sdk.c:256-276`). The
 * recorder owns both sides of that rule for its own instance and runs
 * `leia_sr_v2_initialize` on it itself. @ref leia_sr_predict_trace_create is
 * still called from inside the arm's creation sequence, between the weaver
 * create and the arm's own `leia_sr_v2_initialize`, and
 * @ref leia_sr_predict_trace_destroy runs before the arm's instance is
 * destroyed.
 *
 * Both are therefore tied to the WEAVER'S life, which on the D3D11 arm means
 * they ride the #144 async create/destroy CAS and the #158 in-place reconnect
 * for free: a reconnect tears the SDK objects down (closing this file with its
 * trailer) and builds them again (opening a fresh one).
 *
 * Compiled only on the v2 path -- both arms under test are v2, and the
 * senses used here have no v1 equivalent worth carrying.
 */

#pragma once

#ifdef DXR_LEIA_HAS_SR_V2

#include <sr/sr_instance.h>
#include <sr/sr_types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Opaque recorder. One per weaver; owns its senses, its rings, its writer
 * thread and its file.
 */
struct leia_sr_predict_trace;

/*!
 * Everything the header rows need that the recorder cannot ask the SDK for
 * itself. Filled by the arm at creation time; nothing here is read again.
 */
struct leia_sr_predict_trace_open_info
{
	//! "target" when leia_sr_target_time_opt_in(), else "legacy". Names the
	//! SHIPPING path this run exercises -- the recorder never selects it.
	const char *arm;
	//! Which weaver arm produced the record: "d3d11" / "d3d12" / "vk".
	const char *weaver_arm;
	//! Plug-in build identity (`DXR_PLUGIN_GIT_DESC`).
	const char *plugin_version;
	//! Compile-time SR v2 SDK pin, e.g. "1.37.0+1584".
	const char *sdk_pin;
	//! leia_sr_api_reason(), so the file says which API family it is about.
	const char *api_reason;

	//! The weaver's window. Used ONLY to name the monitor it sits on
	//! (MonitorFromWindow -- HWND-based, so it needs no coordinate
	//! conversion and cannot be wrong about the host's DPI space).
	void *hwnd;
	//! The live weaver, for a create-time `srWeaverGetLatency` readback.
	SrWeaver weaver;

	//! SR display geometry as the arm already read it. Recorded verbatim.
	uint32_t display_pixel_width;
	uint32_t display_pixel_height;
	int32_t display_screen_left;
	int32_t display_screen_top;
	float display_width_m;
	float display_height_m;
};

/*!
 * One weave's worth of record, built by the arm on the weave thread and
 * pushed straight into the ring. Fixed size, trivially copyable: the push is
 * a struct assignment plus a release store.
 *
 * Field meanings are the CSV's, see the `W` legend written into every file.
 */
struct leia_sr_predict_trace_weave
{
	uint64_t seq;                 //!< Per-weaver weave counter, from 1.
	uint64_t now_us;              //!< The "now" the push used (derived on target, sampled on legacy).
	uint64_t push_us;             //!< Horizon the push block computed this weave, BEFORE any deadband.
	uint64_t scanout_us;          //!< now_us + push_us. The evaluation instant, same definition on both arms.
	uint64_t target_us;           //!< Absolute target set this weave (target arm), else 0.
	uint64_t horizon_us;          //!< Horizon the target was built from (target arm), else push_us.
	uint64_t resolved_us;         //!< srWeaverGetLatency AFTER the weave. Record only, never computed from.
	uint64_t read_now_us;         //!< srGetTimeUs immediately before the predicted-eye read.
	uint64_t last_set_latency_us; //!< The arm's last pushed level, as it stands after this weave.
	double pl[3];                 //!< Predicted LEFT eye, mm, as the getter returned it.
	double pr[3];                 //!< Predicted RIGHT eye, mm, as the getter returned it.
	uint8_t pushed;               //!< 1 if a set_latency/set_target actually happened this weave.
	uint8_t swap_flag;            //!< 1 when pl.x > pr.x (the getter swaps; the raw T rows do not).

	/*!
	 * @name The REFERENCE, filled in by the recorder itself inside
	 * leia_sr_predict_trace_on_weave -- the arm never touches these.
	 *
	 * The tracker callback stream turned out to be an ECHO of predict()
	 * calls (the SDK updates its eye-pair stream only at the end of predict
	 * / predictAt, never on a raw measurement), so on the target arm every
	 * T row is a prediction FOR THE TARGET INSTANT and scoring against it
	 * would be circular. The reference is therefore built explicitly:
	 * `srEyeTrackerPredict(tracker, 0, ...)` on the recorder's OWN tracker,
	 * living on the recorder's OWN instance (so its predictor state is a
	 * different chain from the weaver's, see the file header), which resolves
	 * to the filter's low-lag estimate for
	 * `now + min(1/120 s, maxPredictionScene_s)` (-4.36 ms on this profile)
	 * -- the identical expression in both arms, untouched by target mode.
	 * That chain is called ONCE per weave against the weaver's ~4, so its
	 * per-call filter lag is the same in CALLS and ~4x in SECONDS: the
	 * reference reads smoother and later than the getter by construction,
	 * equally in both arms.
	 * `enablePrediction` on the create info is IGNORED by SDK 1584 (the
	 * handle is always a predicting tracker), so there is no raw sample to
	 * be had from it: the reference is filtered and lagged, by design.
	 * @{
	 */
	uint64_t ref_now_us; //!< srGetTimeUs immediately before the reference predict.
	double rl[3];        //!< Reference LEFT eye, mm.
	double rr[3];        //!< Reference RIGHT eye, mm.
	uint8_t ref_ok;      //!< 1 when the reference predict succeeded; 0 = rl/rr are not data.
	/*! @} */
};

/*!
 * Is `DXR_LEIA_SR_PREDICT_TRACE` set? Read once per process, cached, and
 * announced once -- same shape as leia_sr_target_time_opt_in().
 */
bool
leia_sr_predict_trace_enabled(void);

/*!
 * Create the recorder: own instance + senses + callbacks + `srInitialize` on
 * that instance + file + writer thread.
 *
 * @param instance The WEAVER'S instance. Read ONLY for `srGetTimeUs`, so the
 *                 recorder's timestamps stay in the arm's clock domain; no
 *                 object is created on it. The recorder's senses go on an
 *                 instance it creates for itself (see the file header).
 *
 * Returns NULL when tracing is off, when the runtime is missing a function the
 * recorder needs, when its own instance could not be created or initialised, or
 * when the file could not be opened -- in every case the caller simply carries
 * a NULL and weaves exactly as it would have.
 */
struct leia_sr_predict_trace *
leia_sr_predict_trace_create(SrInstance instance, const struct leia_sr_predict_trace_open_info *info);

/*!
 * Remove the callbacks, destroy the senses, destroy the recorder's own
 * instance, stop the writer, write the trailer, close the file, free.
 *
 * MUST be called before `srDestroyInstance` on the WEAVER'S instance, because
 * the recorder keeps reading that instance's clock until it is torn down. The
 * senses are destroyed explicitly rather than left to the recorder's own
 * instance so the ordering is visible. `*rec_ptr` is NULLed first, so a racing
 * reader sees NULL rather than a dying recorder -- the same discipline the arm
 * already uses for its other SDK objects.
 */
void
leia_sr_predict_trace_destroy(struct leia_sr_predict_trace **rec_ptr);

/*!
 * Push one weave record. Weave thread only. Lock-free; drops (and counts) on
 * a full ring rather than overwriting, because an overwritten record is a
 * silently wrong record and a dropped one is a number in the trailer.
 */
void
leia_sr_predict_trace_on_weave(struct leia_sr_predict_trace *rec, const struct leia_sr_predict_trace_weave *w);

//! Why a weave produced no record. Counted, and reported in the trailer.
enum leia_sr_predict_trace_skip
{
	LEIA_SR_PREDICT_TRACE_SKIP_NO_CLOCK = 0, //!< srGetTimeUs gave us no usable "now".
	LEIA_SR_PREDICT_TRACE_SKIP_NO_EYES = 1,  //!< the predicted-eye read failed.
};

/*!
 * Record that this weave produced no row, and why. Weave thread only; a
 * relaxed atomic increment.
 */
void
leia_sr_predict_trace_skip_weave(struct leia_sr_predict_trace *rec, enum leia_sr_predict_trace_skip why);

#ifdef __cplusplus
}
#endif

#endif // DXR_LEIA_HAS_SR_V2
