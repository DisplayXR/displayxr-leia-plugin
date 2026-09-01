// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  CNSDK wrapper implementation — isolates CNSDK headers
 *         from the rest of the compositor.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_cnsdk.h"

#include "util/u_logging.h"
#include "os/os_time.h"

// CNSDK 0.10.x headers (relocated from leia/sdk/ → leia/core/ vs 0.7.28).
#include <leia/core/core.h>
#include <leia/core/experimental.h>
#include <leia/core/faceTracking.h>
// #206: we use `leia_interlacer_set_predicted_scanout_ns` and its _VERSION
// guard from here. interlacer.vulkan.h does include it transitively, so this is
// include-what-you-use rather than a fix — the feature guard resolves either
// way (verified by preprocessing without it).
#include <leia/core/interlacer.h>
#include <leia/core/interlacer.vulkan.h>
#include <leia/core/library.h>
#include <leia/core/deviceConfig.h>
#include <leia/headTracking/common/types.h>
#include <leia/common/version.h>

#ifdef XRT_OS_ANDROID
#include "android/android_globals.h"
#include <sys/system_properties.h>
#ifdef XRT_DEBUG_ANDROID_VERBOSE
#include <android/trace.h>
#endif
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>


// Hardware-bring-up debug logging. Gated by XRT_DEBUG_ANDROID_VERBOSE
// which is passed via cppFlags from the Android Debug build variant
// (src/xrt/targets/openxr_android/build.gradle::debug). Compiles to
// nothing in release. Tag "HW_DBG_CNSDK:" is greppable in logcat.
#ifdef XRT_DEBUG_ANDROID_VERBOSE
#define DXR_HW_DBG(...)       U_LOG_I("HW_DBG_CNSDK: " __VA_ARGS__)
#define DXR_HW_DBG_ONCE(...)  do {                                                                 \
		static bool _logged = false;                                                                \
		if (!_logged) { U_LOG_I("HW_DBG_CNSDK[once]: " __VA_ARGS__); _logged = true; }              \
	} while (0)

// ATrace RAII scope — captures show up in Perfetto / Studio Profiler.
// Same gate as DXR_HW_DBG; release builds compile to nothing.
struct AtraceScopeCnsdk {
	AtraceScopeCnsdk(const char *name) { ATrace_beginSection(name); }
	~AtraceScopeCnsdk() { ATrace_endSection(); }
};
#define DXR_ATRACE(name) AtraceScopeCnsdk _atrace_##__LINE__(name)
#else
#define DXR_HW_DBG(...)       ((void)0)
#define DXR_HW_DBG_ONCE(...)  ((void)0)
#define DXR_ATRACE(name)      ((void)0)
#endif


/*
 *
 * Internal struct.
 *
 */

struct leia_cnsdk
{
	// CNSDK 0.10.x loader handle — loads the core impl (which talks to the
	// on-device Leia service). Held for the lifetime of the core; released
	// after the core in destroy.
	struct leia_core_library *lib{nullptr};
	struct leia_core *core{nullptr};
	struct leia_interlacer *interlacer{nullptr};

	// Face-tracking startup is offloaded to a worker thread because
	// leia_core_enable_face_tracking is heavy (CNSDK docs explicitly warn
	// against the main thread). Worker pattern:
	//   - Spawn in leia_cnsdk_create.
	//   - Worker polls leia_core_is_initialized until ready, then snapshots
	//     the camera center from leia_device_config (needed to convert
	//     CNSDK's camera-relative face positions into display-relative),
	//     calls enable + start face tracking, then sets
	//     face_tracking_started and exits.
	//   - Destroy sets shutting_down to ask the worker to bail if it's
	//     still in the polling phase, then joins.
	//
	// camera_center_{x,y,z}_m: cached at worker init. The `_m` suffix
	// reminds the reader they're already mm→m converted before storage.
	// These are read by leia_cnsdk_get_primary_face on the render thread
	// only after face_tracking_started.load(acquire) returns true — the
	// happens-before ordering of the atomic gives the read visibility on
	// the worker's writes.
	std::atomic<bool> face_tracking_started{false};
	std::atomic<bool> shutting_down{false};
	std::thread worker;

	// Eye-tracking control mode (#522): 0 = MANAGED (CNSDK owns the
	// tracking-loss lifecycle — grace + auto-2D via NoFaceMode), 1 = MANUAL
	// (CNSDK stands down; the app drives 2D⇄3D). Set by the runtime via
	// leia_cnsdk_set_eye_tracking_mode; applied by apply_eye_tracking_mode once
	// the core is initialized and the licensing/availability of face tracking is
	// known. Default MANAGED, matching the plug-in's advertised default.
	std::atomic<uint32_t> eye_tracking_mode{0};

	// Hardware 2D/3D backlight state actually APPLIED to the panel (-1 =
	// unknown, 0 = 2D, 1 = 3D). Per-instance (NOT a function-local static):
	// pause/destroy force it to 2D from the IPC/teardown threads while the
	// weave loop re-applies the wanted state on the render thread, and the
	// panel's switchable backlight is system-global — it stays however we
	// leave it (stuck-3D-after-close bug). throttle is render-thread-only.
	std::atomic<int> backlight_applied{-1};
	int backlight_throttle{0};

	// True when the host context handed to CNSDK is a real Activity
	// (in-process). leia_core_on_pause/on_resume internally call
	// Activity-typed Java helpers (FaceTrackingHelper.checkPermission) —
	// out-of-process the context is the runtime SERVICE, and the call
	// trips CheckJNI and aborts the service. Same gate as the
	// limit_orientations registration below.
	bool host_is_activity{false};

	// Camera extrinsics snapshot (leia_camera::translation_mm, mm->m at
	// storage time) — the camera's position relative to the DISPLAY CENTER.
	// Read once by the worker from leia_device_config_get_camera_data, then
	// read on the render thread only after face_tracking_started acquires.
	// Used to lift the frame-listener's CAMERA-space face into display-center
	// space; the core's predicted / non-predicted faces already carry the
	// extrinsics and must NOT be translated again (#152 L-c).
	float camera_center_x_m{0.0f};
	float camera_center_y_m{0.0f};
	float camera_center_z_m{0.0f};
	bool camera_extrinsics_ok{false};

	// In-service face readout. With LEIA_FACE_TRACKING_RUNTIME_IN_SERVICE the
	// detection runs in the system head-tracking service and steers the weave
	// directly — leia_core_get_primary_face stays empty in our process
	// (confirmed on the nubia NP02J: sdk_started=1 but pred/nonpred both 0).
	// To drive the app's per-eye cameras (scene parallax) we register a frame
	// listener and cache the latest head point (mm, camera-relative) from each
	// frame the service delivers. Written on the listener's background thread,
	// read on the render thread — atomics give cross-thread visibility. The
	// listener is OWNED by leia_core once set, so we never release it ourselves.
	struct leia_headtracking_frame_listener *frame_listener{nullptr};
	// #201 watchdog state: tracking_cycling gates the weave while the
	// watchdog cycles core pause/resume (CNSDK throws if the weave runs
	// mid-teardown — observed as terminate()/emutls SIGSEGV on the repaint
	// thread); lifecycle_paused stops the watchdog once the app itself
	// pauses (its own resume will redo the handshake anyway).
	std::atomic<bool> tracking_cycling{false};
	std::atomic<bool> lifecycle_paused{false};

	std::atomic<bool> listener_face_valid{false};
	std::atomic<int> listener_miss_count{0};
	// Monotonic timestamp of the LAST frame-listener invocation — stamped on
	// every callback, hit or miss. listener_miss_count only advances while the
	// callback keeps firing, so on its own it can never expire a latched face
	// when the callback STOPS (service hiccup, client eviction, camera loss):
	// the last tuple would stay valid forever and the DP would keep weaving a
	// stale eye. This wall clock is what bounds that (#152 L-c).
	std::atomic<int64_t> listener_frame_ns{0};
	std::atomic<float> listener_face_x_mm{0.0f};
	std::atomic<float> listener_face_y_mm{0.0f};
	std::atomic<float> listener_face_z_mm{0.0f};

	// #ROLL: per-EYE data from the same listener frame, cached alongside the
	// face point above and written under the same listener_face_valid release.
	//
	// Two independent roll carriers, because on this hardware the core's own
	// face accessors come back empty (see the pred=0 note above) and the
	// listener is the only live source:
	//   - listener_eyes_*: leia_headtracking_raw_face::eyePoints[2], the
	//     DEPROJECTED per-eye camera-space points. Real eye geometry, so roll
	//     is carried exactly. listener_eyes_valid says whether this frame had
	//     them (raw faces can be absent while detected faces are present).
	//   - listener_roll_rad: leia_headtracking_detected_face::poseAngle.z, the
	//     head roll. Enough to orient a synthetic IPD vector when eyePoints
	//     are unavailable. listener_roll_valid gates it.
	// Both are CAMERA-space quantities (image Y down), lifted/flipped where
	// they are consumed, exactly like the face point.
	std::atomic<bool> listener_eyes_valid{false};
	std::atomic<float> listener_eye_l_x_mm{0.0f};
	std::atomic<float> listener_eye_l_y_mm{0.0f};
	std::atomic<float> listener_eye_l_z_mm{0.0f};
	std::atomic<float> listener_eye_r_x_mm{0.0f};
	std::atomic<float> listener_eye_r_y_mm{0.0f};
	std::atomic<float> listener_eye_r_z_mm{0.0f};
	std::atomic<bool> listener_roll_valid{false};
	std::atomic<float> listener_roll_rad{0.0f};

	// #ROLL: CNSDK experimental per-eye accessors, resolved once at library
	// load (leia_get_experimental_api is a plain library-level lookup, so it
	// needs no initialized core). Either may stay null on an SDK that dropped
	// them — every consumer null-checks. These are what the Unity/LeiaViewer
	// path consumes (libleiaSDK-jni exports getLookaroundEyes over the same
	// entry point), and they are the ONLY sources that carry the eye vector
	// rather than a bare face point.
	leia_core_get_lookaround_eyes fn_lookaround_eyes{nullptr};
	// #206: experimental registry entry that takes the ABSOLUTE predicted
	// scanout time. Guarded on CNSDK's own VERSION macro so this file still
	// builds against a CNSDK that predates the API — same shape as the
	// runtime's XRT_DP_VK_HAS_* convention. nullptr at runtime (API present at
	// compile time, absent in the loaded library) is also fine: we simply never
	// publish and CNSDK keeps facePredictLatencyMs.
#if defined(leia_interlacer_set_predicted_scanout_ns_VERSION)
	leia_interlacer_set_predicted_scanout_ns fn_set_predicted_scanout{nullptr};
#endif
	leia_core_get_non_predicted_eyes fn_nonpred_eyes{nullptr};

	// #ROLL: unit auto-detect for the experimental eye accessors. core.h does
	// not document whether they return mm (like get_primary_face) or meters,
	// and guessing wrong is a 1000x IPD error, so the first plausible pair
	// decides it from the measured separation. 0 = undecided, 1 = mm,
	// 2 = meters. eyes_unit_candidate/_run accumulate the consecutive-agreement
	// run that must be reached before latching — the reported separation is
	// noisy during acquisition, and a single stray sample must not decide the
	// scale for the session. Render-thread only.
	int eyes_unit{0};
	int eyes_unit_candidate{0};
	int eyes_unit_run{0};

	// #211: the last GOOD tier-1/2 pair (post unit-scale, millimetres,
	// pre-orientation) and when it was served. A single absent/rejected
	// lookaround frame otherwise falls through to a lower tier for that one
	// frame — a visible stereo-geometry pop (field-measured storm on NP02J
	// minutes after v2.6.7). Holding the last pair for up to the freshness
	// window smooths one-frame dropouts; a genuinely stale pair still falls
	// through, which stays the honest state. Same 100 ms freshness convention
	// as the #206 forward-horizon work (two staleness policies must agree).
	float eyes_held_l[3]{0, 0, 0};
	float eyes_held_r[3]{0, 0, 0};
	int64_t eyes_held_ns{0};

	// Cached display metrics. Populated by the worker thread alongside
	// the camera-center snapshot; the atomic flag gives the render
	// thread happens-before visibility on the float/int writes. Once
	// set, leia_cnsdk_get_display_metrics returns the cached values
	// instead of calling get_device_config / release_device_config per
	// frame — eliminates a per-frame allocation churn AND the
	// concurrent-device-config-access concern (audit B9).
	std::atomic<bool> display_metrics_cached{false};
	float display_width_m_cached{0.0f};
	float display_height_m_cached{0.0f};
	uint32_t display_pixel_w_cached{0};
	uint32_t display_pixel_h_cached{0};
	// Per-view (tile) resolution in pixels, in the device NATURAL orientation —
	// CNSDK VIEW_RESOLUTION_PX. The 3D view_scale is tile ÷ panel (#518). Cached
	// alongside the panel res under the same display_metrics_cached release flag.
	uint32_t view_res_w_cached{0};
	uint32_t view_res_h_cached{0};

	// Device's natural orientation (LANDSCAPE=0 / PORTRAIT=1 / …), cached from
	// the device config in the worker. The predicted/non-predicted faces arrive
	// in this natural-orientation display frame (weave-ready); the look-around
	// face we return must be rotated into the CURRENT held orientation. -1 =
	// unspecified (no rotation until known). Read on the render thread.
	std::atomic<int> natural_orientation{-1};

	// One-shot flag: once leia_interlacer_vulkan_initialize fails, give
	// up rather than retrying every frame. Read + written only by the
	// render thread (no concurrent access; no atomic needed).
	bool interlacer_init_failed{false};

	// Window rect on the panel (runtime#1033 / #150, ADR-036 D6). The runtime's
	// per-window compositor instance reports where THIS window sits on screen —
	// in CURRENT-orientation screen pixels, exactly as Android's
	// View.getLocationOnScreen returns them (CNSDK rotates into the natural
	// orientation itself: interlacer.cpp "the user provides data in the current
	// orientation space, we convert it to the natural one"). It is the BASE the
	// per-frame zone/canvas offset is added to, so a window that is not at the
	// panel origin weaves at its own interlace phase instead of the panel's.
	// Written from the runtime's DP call (render thread), read in the weave on
	// the same thread; atomic anyway because a future compositor could report
	// off-thread. have_window_rect stays false until the first report ⟹ the
	// pre-#1033 display-scoped behaviour is bit-identical.
	std::atomic<bool> have_window_rect{false};
	std::atomic<int32_t> window_screen_x{0};
	std::atomic<int32_t> window_screen_y{0};
	std::atomic<int32_t> window_screen_w{0};
	std::atomic<int32_t> window_screen_h{0};
	// Panel size in the CURRENT orientation, from the runtime (0 = not reported).
	std::atomic<uint32_t> panel_now_w{0};
	std::atomic<uint32_t> panel_now_h{0};
	// #206: measured weave->scanout residual in ns (0 = unknown/untrusted).
	std::atomic<uint64_t> weave_to_scanout_ns{0};
	std::atomic<int32_t> window_display_id{-1};
};

static void
release_lens_preference(struct leia_cnsdk *cnsdk, const char *reason);
static void
assert_lens_preference(struct leia_cnsdk *cnsdk, const char *reason);


/*
 *
 * Calibration knobs (runtime-tunable via `setprop`).
 *
 * Three CNSDK conventions are ambiguous from the SDK headers and can
 * only be validated on real Lume Pad hardware. Each lives behind a
 * `debug.dxr.leia.*` system property so we can flip the right knob
 * without rebuilding the plug-in. Cached once at first read (CNSDK
 * init time) because `__system_property_get` is not free, and
 * because changing these mid-session would race against in-flight
 * frames. To re-read after a flip, force-stop the app.
 *
 * See `displayxr-leia-plugin/docs/cnsdk-android-calibration.md` for
 * the symptom→knob table.
 */
struct calibration_knobs {
	bool flip_uv;        // debug.dxr.leia.flip_uv         default 0 (VK Y-down apps)
	bool face_flip_x;    // debug.dxr.leia.face_flip_x     default 0
	bool face_flip_y;    // debug.dxr.leia.face_flip_y     default 0
	bool face_flip_z;    // debug.dxr.leia.face_flip_z     default 0
	bool face_swap_xy;   // debug.dxr.leia.face_swap_xy    default 0
	// debug.dxr.leia.capture_lux    default 0. Requests CNSDK's lux capture on
	// this core BEFORE face tracking is enabled, so the value travels in the
	// head-tracking ClientHello rather than as a later global write. That makes
	// it a per-client input to the service's engine-config aggregation
	// (LeiaInc/CNSDK#698) — the only knob we have that varies a Hello field,
	// which is what makes the aggregation testable from logcat
	// (LeiaInc/CNSDK#709). Read once per satellite process, so setting it
	// between two app launches gives the two clients different values.
	bool capture_lux;
};

static struct calibration_knobs g_calib = {};
static std::atomic<bool> g_calib_loaded{false};

#ifdef XRT_OS_ANDROID
static bool
get_prop_bool(const char *name, bool default_val)
{
	char buf[PROP_VALUE_MAX] = {0};
	int n = __system_property_get(name, buf);
	if (n <= 0) {
		return default_val;
	}
	/* Accept "1", "true", "yes", "on" (case-insensitive) as true.
	 * Anything else, including empty string, is false. */
	if (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' || buf[0] == 'y' || buf[0] == 'Y') {
		return true;
	}
	return false;
}
#else
static bool
get_prop_bool(const char *name, bool default_val)
{
	(void)name;
	return default_val;
}
#endif

// #558 per-app: overlay mode is decided per-session by the runtime (from the
// connecting app's manifest) and published into android_globals (service
// process), so read it there instead of a global sysprop. debug.dxr.overlay
// stays as a dev force-all override.
static bool
overlay_mode_active(void)
{
	return android_globals_get_overlay_mode() || get_prop_bool("debug.dxr.overlay", false);
}

/*!
 * DEPRECATED A/B override (runtime #1039; was the #151 opt-in).
 *
 * `debug.dxr.multiapp=1` restores the old behaviour where `leia_cnsdk_on_pause`
 * HOLDS this process's backlight bind instead of releasing it. Keep it only to
 * A/B the two behaviours on a device; nothing should depend on it.
 *
 * It exists because #151 assumed `leia_core_enable_3d(false)` "drops the global
 * backlight AND unbinds". Traced through CNSDK, that is not what happens on the
 * multi-client tier — see @ref release_lens_preference. Releasing is exactly the
 * per-window "I no longer want the lens" vote the runtime contract asks for, so
 * the hold is now wrong in BOTH directions (it takes a lens vote for a window
 * nobody can see, which re-opens runtime #563) and the default is to release.
 *
 * Read once via `__system_property_get` and cached. Default OFF.
 */
static bool
multiapp_mode_active(void)
{
	static std::atomic<int> cached{-1};
	int v = cached.load(std::memory_order_acquire);
	if (v < 0) {
		v = get_prop_bool("debug.dxr.multiapp", false) ? 1 : 0;
		cached.store(v, std::memory_order_release);
	}
	return v != 0;
}

#ifdef XRT_OS_ANDROID
// Tri-state property override: returns `derived` when the property is unset,
// otherwise the property's boolean value. Lets orientation-derived axis
// defaults be overridden per-axis via setprop for calibration, no rebuild.
static bool
prop_override(const char *name, bool derived)
{
	char buf[PROP_VALUE_MAX] = {0};
	if (__system_property_get(name, buf) <= 0) {
		return derived;
	}
	const char c = buf[0];
	if (c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y') { return true; }
	if (c == '0' || c == 'f' || c == 'F' || c == 'n' || c == 'N') { return false; }
	return derived;
}
#else
static bool
prop_override(const char *name, bool derived)
{
	(void)name;
	return derived;
}
#endif

static void
ensure_calibration_loaded(void)
{
	if (g_calib_loaded.load(std::memory_order_acquire)) {
		return;
	}
	// Default FALSE: the Android DP is VK-only, and DisplayXR VK apps render
	// their atlas in Vulkan's Y-down convention (the projection bakes the Y
	// flip), so the interlacer must NOT flip the input again — flipping turned
	// the cube upside-down on the nubia NP02J (#499). Set to 1 to re-flip for a
	// GL-Y-up source.
	g_calib.flip_uv       = get_prop_bool("debug.dxr.leia.flip_uv",       false);
	g_calib.face_flip_x   = get_prop_bool("debug.dxr.leia.face_flip_x",   false);
	g_calib.face_flip_y   = get_prop_bool("debug.dxr.leia.face_flip_y",   false);
	g_calib.face_flip_z   = get_prop_bool("debug.dxr.leia.face_flip_z",   false);
	g_calib.face_swap_xy  = get_prop_bool("debug.dxr.leia.face_swap_xy",  false);
	g_calib.capture_lux   = get_prop_bool("debug.dxr.leia.capture_lux",   false);
	g_calib_loaded.store(true, std::memory_order_release);
	U_LOG_W("HW_DBG_CNSDK calibration: flip_uv=%d face_flip_xyz=%d%d%d face_swap_xy=%d capture_lux=%d",
	        (int)g_calib.flip_uv,
	        (int)g_calib.face_flip_x, (int)g_calib.face_flip_y, (int)g_calib.face_flip_z,
	        (int)g_calib.face_swap_xy, (int)g_calib.capture_lux);
}

extern "C" void
leia_cnsdk_log_calibration_knobs(void)
{
	/* Exposed via leia_cnsdk.h so the plug-in's probe() can force the
	 * knob log line to land in logcat at xrCreateInstance time, even
	 * when CNSDK init never runs (e.g. emulator hits
	 * VK_ERROR_EXTENSION_NOT_PRESENT before reaching the DP). */
	ensure_calibration_loaded();
}


/*
 *
 * Private helpers.
 *
 */

namespace {

// Wall-clock lifetime of a latched frame-listener face, measured from the last
// callback invocation (not the last HIT — see listener_frame_ns). The system
// head-tracking service delivers frames at >=30 Hz whenever it is streaming,
// and keeps delivering them with numFaces==0 while it sees nobody, so 500 ms is
// >=15 consecutive missed DELIVERIES: far beyond scheduler jitter, yet short
// enough that a wedged/evicted service can't steer the weave off a stale eye
// for a human-noticeable stretch. Deliberately looser than the 90-miss
// (~1 s at the service rate) hold in on_headtracking_frame, which stays the
// authority while the callback is alive: this timer only fires when the
// callback itself has stopped. (#152 L-c)
constexpr int64_t kListenerFrameStaleNs = 500 * 1000 * 1000LL;

// Frame listener callback — invoked on a CNSDK background thread for every
// head-tracking frame the service delivers (the HTS-Binder onMessage(Frame)
// stream). Cache the primary face's Kalman-filtered point so the render thread
// can read it. Must release the frame (ownership transferred in).
void
on_headtracking_frame(struct leia_headtracking_frame *frame, void *userData)
{
	struct leia_cnsdk *cnsdk = static_cast<struct leia_cnsdk *>(userData);

	// Stamp the liveness clock on EVERY invocation, hit or miss — this measures
	// "is the listener still being called at all", which is exactly the
	// condition the miss counter below cannot observe (#152 L-c).
	if (cnsdk != nullptr) {
		cnsdk->listener_frame_ns.store(os_monotonic_get_ns(), std::memory_order_relaxed);
	}

	// Read the head position. On this device the Kalman tracking_result comes
	// back empty, but the raw DETECTED faces (posePosition, mm, camera origin)
	// are populated every frame — confirmed in the service log
	// ([FaceTracker] detected face: ...posePosition={...}). Prefer detected
	// faces; fall back to the tracking result for other devices/modes.
	bool got = false;
	float px = 0.0f, py = 0.0f, pz = 0.0f;
	// #ROLL: head roll, carried alongside the face point. The detected face's
	// poseAngle is the only rotation the service publishes on this hardware, and
	// without it a synthesized eye pair can only ever be horizontal.
	bool got_roll = false;
	float roll_rad = 0.0f;
	struct leia_headtracking_detected_faces detected = {};
	if (cnsdk != nullptr &&
	    leia_headtracking_frame_get_detected_faces(frame, &detected) ==
	        kLeiaHeadTrackingStatusSuccess &&
	    detected.numFaces > 0) {
		px = detected.faces[0].posePosition.x;
		py = detected.faces[0].posePosition.y;
		pz = detected.faces[0].posePosition.z;
		// poseAngle is "head rotation in radians, left handed" (CNSDK
		// headTracking/common/types.h). z is the roll about the view axis.
		roll_rad = detected.faces[0].poseAngle.z;
		got_roll = std::isfinite(roll_rad);
		got = true;
	} else if (cnsdk != nullptr) {
		struct leia_headtracking_tracking_result tracked = {};
		if (leia_headtracking_frame_get_tracking_result(frame, &tracked) ==
		        kLeiaHeadTrackingStatusSuccess &&
		    tracked.num_faces > 0) {
			px = tracked.faces[0].point.pos.x;
			py = tracked.faces[0].point.pos.y;
			pz = tracked.faces[0].point.pos.z;
			roll_rad = tracked.faces[0].angle.z;
			got_roll = std::isfinite(roll_rad);
			got = true;
		}
	}

	// #ROLL: the deprojected per-EYE points, when the service publishes them.
	// This is the best roll carrier available on the listener path — real eye
	// geometry rather than a point plus an angle — so it is preferred over the
	// poseAngle above wherever both are present. Raw faces are an independent
	// getter from detected faces and can be absent while detected faces are
	// populated, hence its own validity flag.
	bool got_eyes = false;
	float lx = 0.0f, ly = 0.0f, lz = 0.0f, rx = 0.0f, ry = 0.0f, rz = 0.0f;
	struct leia_headtracking_raw_faces raw = {};
	if (cnsdk != nullptr &&
	    leia_headtracking_frame_get_raw_faces(frame, &raw) == kLeiaHeadTrackingStatusSuccess &&
	    raw.numFaces > 0) {
		lx = raw.faces[0].eyePoints[0].x;
		ly = raw.faces[0].eyePoints[0].y;
		lz = raw.faces[0].eyePoints[0].z;
		rx = raw.faces[0].eyePoints[1].x;
		ry = raw.faces[0].eyePoints[1].y;
		rz = raw.faces[0].eyePoints[1].z;
		// Same plausibility band as the face gate, plus a separation sanity
		// check: a degenerate pair (both eyes at one point, or metres apart)
		// is worse than the synthetic fallback, so reject rather than serve it.
		const float sep = sqrtf((lx - rx) * (lx - rx) + (ly - ry) * (ly - ry) + (lz - rz) * (lz - rz));
		got_eyes = std::isfinite(sep) && sep > 30.0f && sep < 100.0f && lz > 150.0f && lz < 2000.0f &&
		           rz > 150.0f && rz < 2000.0f;
		if (!got_eyes) {
			static int eyerej = 0;
			if ((eyerej++ % 120) == 0) {
				U_LOG_W("HW_EYES: rejected implausible eyePoints L=(%.0f,%.0f,%.0f) "
				        "R=(%.0f,%.0f,%.0f) sep=%.1f mm",
				        lx, ly, lz, rx, ry, rz, sep);
			}
		}
	}

	// Plausibility gate: reject garbage detections (the service occasionally
	// emits a bogus face — e.g. z=66 mm, ~6.6 cm — which puts the eye camera
	// almost on the screen and wildly distorts/stretches the cube). A real face
	// sits ~150–2000 mm deep and within ~800 mm laterally; anything outside that
	// is dropped (treated as a miss, so the last good position holds).
	if (got && (pz < 150.0f || pz > 2000.0f ||
	            px < -800.0f || px > 800.0f ||
	            py < -800.0f || py > 800.0f)) {
		static int rej = 0;
		if ((rej++ % 60) == 0) {
			U_LOG_W("HW_FACE: rejected implausible face (%.0f,%.0f,%.0f) mm", px, py, pz);
		}
		got = false;
	}

	if (cnsdk != nullptr && got) {
		cnsdk->listener_face_x_mm.store(px, std::memory_order_relaxed);
		cnsdk->listener_face_y_mm.store(py, std::memory_order_relaxed);
		cnsdk->listener_face_z_mm.store(pz, std::memory_order_relaxed);
		// #ROLL: publish the eye pair / roll under the SAME release as the face
		// point, so a reader that sees a fresh face never pairs it with stale
		// eye geometry. Each carries its own validity flag because either can be
		// absent on a frame whose face point is fine.
		cnsdk->listener_eye_l_x_mm.store(lx, std::memory_order_relaxed);
		cnsdk->listener_eye_l_y_mm.store(ly, std::memory_order_relaxed);
		cnsdk->listener_eye_l_z_mm.store(lz, std::memory_order_relaxed);
		cnsdk->listener_eye_r_x_mm.store(rx, std::memory_order_relaxed);
		cnsdk->listener_eye_r_y_mm.store(ry, std::memory_order_relaxed);
		cnsdk->listener_eye_r_z_mm.store(rz, std::memory_order_relaxed);
		cnsdk->listener_eyes_valid.store(got_eyes, std::memory_order_relaxed);
		cnsdk->listener_roll_rad.store(roll_rad, std::memory_order_relaxed);
		cnsdk->listener_roll_valid.store(got_roll, std::memory_order_relaxed);
		cnsdk->listener_face_valid.store(true, std::memory_order_release);
		cnsdk->listener_miss_count.store(0, std::memory_order_relaxed);
	} else if (cnsdk != nullptr) {
		// The service's detector drops to num_faces==0 intermittently (steep
		// camera angle / brief loss). Don't snap the view back to the default
		// on a single miss — hold the last position and only invalidate after a
		// sustained loss (~1 s at the service's frame rate), so head tracking
		// stays smooth instead of flickering to the static fallback.
		const int misses = cnsdk->listener_miss_count.fetch_add(1, std::memory_order_relaxed) + 1;
		if (misses > 90) {
			cnsdk->listener_eyes_valid.store(false, std::memory_order_relaxed);
			cnsdk->listener_roll_valid.store(false, std::memory_order_relaxed);
			cnsdk->listener_face_valid.store(false, std::memory_order_release);
		}
	}
	leia_headtracking_frame_release(frame);
}

// Apply the current eye-tracking control mode (#522) to the CNSDK core. Safe to
// call only once the core is initialized. The licensing/availability guard keys
// off face_tracking_started: if in-app face tracking never licensed (no eye
// position will ever arrive), MANAGED's auto-2D would collapse to PERMANENT flat
// 2D — so we only engage CNSDK NoFaceMode (vendor-managed auto-2D) when MANAGED
// is selected AND tracking is actually available; otherwise we force the 3D
// light-field on (NoFaceMode off), which is also the correct MANUAL behavior
// (the vendor stands down; the app drives 2D⇄3D).
void
apply_eye_tracking_mode(struct leia_cnsdk *cnsdk)
{
	if (cnsdk == nullptr || cnsdk->core == nullptr) {
		return;
	}
	uint32_t mode = cnsdk->eye_tracking_mode.load(std::memory_order_acquire);
	bool tracking_available = cnsdk->face_tracking_started.load(std::memory_order_acquire);
	bool managed_auto_2d = (mode == 0u /*MANAGED*/) && tracking_available;

	// NoFaceMode ON => CNSDK owns grace + auto-drop-to-2D on tracking loss
	// (MANAGED). NoFaceMode OFF => force the 3D light-field regardless of face
	// (MANUAL, or MANAGED-without-license POC guard).
	leia_core_enable_no_face_mode(cnsdk->core, managed_auto_2d);
	leia_core_enable_3d(cnsdk->core, true);
	DXR_HW_DBG("apply_eye_tracking_mode: mode=%u tracking_available=%d -> no_face_mode=%d",
	           mode, (int)tracking_available, (int)managed_auto_2d);
}

void
face_tracking_worker(struct leia_cnsdk *cnsdk)
{
	using namespace std::chrono_literals;

	DXR_HW_DBG("worker: entered, waiting for leia_core_is_initialized");

	// Phase 1: wait for the async core init to complete. Poll every 50 ms;
	// honor shutdown promptly.
	int poll_count = 0;
	while (!cnsdk->shutting_down.load(std::memory_order_acquire)) {
		if (cnsdk->core != nullptr && leia_core_is_initialized(cnsdk->core)) {
			break;
		}
		if ((++poll_count % 20) == 0) {
			DXR_HW_DBG("worker: still polling for core init (~%d s elapsed)",
			           poll_count / 20);
		}
		std::this_thread::sleep_for(50ms);
	}
	if (cnsdk->shutting_down.load(std::memory_order_acquire)) {
		DXR_HW_DBG("worker: shutdown requested before core ready, exiting");
		return;
	}
	DXR_HW_DBG("worker: core initialized after %d polls", poll_count);

	// BOOTSTRAP (pre-tracking): force the 3D light-field ON until we know whether
	// face tracking licenses. NoFaceMode would otherwise auto-drop to flat 2D
	// before any face arrives. The FINAL eye-tracking-mode policy (#522) is
	// applied by apply_eye_tracking_mode() below, once face_tracking_started is
	// known: MANAGED + licensed -> NoFaceMode on; MANUAL or unlicensed -> stays
	// force-3D. A live 2D/3D A-B toggle is available via debug.dxr.leia.backlight.
	leia_core_enable_no_face_mode(cnsdk->core, false);
	leia_core_enable_3d(cnsdk->core, true);
	DXR_HW_DBG("worker: bootstrap force-3D (no-face mode OFF) until tracking status known");

	// Phase 2a: snapshot all device-config values we need on the render
	// thread (camera center for face-position translation; display
	// metrics for Kooima projection). CNSDK doesn't annotate device
	// config thread safety, so we keep it on this one worker thread and
	// expose only cached values to the render thread via atomics. mm→m
	// conversion happens at storage time so render-thread reads are
	// branch-free.
	// CNSDK 0.10.x: leia_device_config is opaque — read via typed property
	// getters instead of struct fields. Camera-center moved into a leia_camera
	// struct (leia_device_config_get_camera_data); the 0.10.x port deferred it
	// and left the cached value 0, which silently made the camera->display
	// translation a no-op. That was harmless for the core's predicted /
	// non-predicted faces (already display-center) but wrong for the
	// frame-listener fallback, which is raw camera-space — #152 L-c. Read it
	// for real here; it is applied ONLY to the listener value.
	struct leia_device_config *cfg = leia_core_get_device_config(cnsdk->core);
	if (cfg != NULL) {
		float display_size_mm[2] = {0.0f, 0.0f};
		int32_t panel_res_px[2] = {0, 0};
		bool got_size = leia_device_config_get_f32(
		    cfg, LEIA_DEVICE_CONFIG_PROPERTY_DISPLAY_SIZE_MM, 2, display_size_mm);
		bool got_res = leia_device_config_get_i32(
		    cfg, LEIA_DEVICE_CONFIG_PROPERTY_PANEL_RESOLUTION_PX, 2, panel_res_px);
		// #518: per-view (tile) resolution → the 3D view_scale = tile ÷ panel.
		// Both are in the device NATURAL orientation. VIEW_RESOLUTION_PX is
		// Read-Write (an app could override it) — log it so we can confirm it
		// reads the device default (e.g. 1200x1920 on the nubia NP02J).
		int32_t view_res_px[2] = {0, 0};
		bool got_view = leia_device_config_get_i32(
		    cfg, LEIA_DEVICE_CONFIG_PROPERTY_VIEW_RESOLUTION_PX, 2, view_res_px);
		if (got_size) {
			cnsdk->display_width_m_cached = display_size_mm[0] / 1000.0f;
			cnsdk->display_height_m_cached = display_size_mm[1] / 1000.0f;
		}
		if (got_res) {
			cnsdk->display_pixel_w_cached = (uint32_t)panel_res_px[0];
			cnsdk->display_pixel_h_cached = (uint32_t)panel_res_px[1];
		}
		if (got_view) {
			cnsdk->view_res_w_cached = (uint32_t)view_res_px[0];
			cnsdk->view_res_h_cached = (uint32_t)view_res_px[1];
		}
		int32_t natural_ori = -1;
		if (leia_device_config_get_i32(
		        cfg, LEIA_DEVICE_CONFIG_PROPERTY_DEVICE_NATURAL_ORIENTATION, 1, &natural_ori)) {
			cnsdk->natural_orientation.store(natural_ori, std::memory_order_relaxed);
		}

		// Camera 0 = the face-tracking camera. translation_mm is its offset from
		// the display center; rotation_deg is logged (not applied) so a device
		// whose camera is NOT display-plane-aligned shows up in triage instead
		// of silently skewing the fallback face.
		struct leia_camera cam = {};
		if (leia_device_config_get_camera_data(cfg, &cam, 0)) {
			cnsdk->camera_center_x_m = cam.translation_mm.x / 1000.0f;
			cnsdk->camera_center_y_m = cam.translation_mm.y / 1000.0f;
			cnsdk->camera_center_z_m = cam.translation_mm.z / 1000.0f;
			cnsdk->camera_extrinsics_ok = true;
			U_LOG_W("CNSDK camera extrinsics: translation=(%.1f,%.1f,%.1f) mm "
			        "rotation=(%.1f,%.1f,%.1f) deg sensorOri=%d frontFacing=%d",
			        cam.translation_mm.x, cam.translation_mm.y, cam.translation_mm.z,
			        cam.rotation_deg.x, cam.rotation_deg.y, cam.rotation_deg.z,
			        (int)cam.sensorOrientation, (int)cam.frontFacing);
		} else {
			U_LOG_W("leia_device_config_get_camera_data(0) failed; frame-listener "
			        "face fallback keeps a zero camera offset (axis flip still applied)");
		}
		leia_device_config_release(cfg);
		cnsdk->display_metrics_cached.store(true, std::memory_order_release);
		DXR_HW_DBG("worker: natural_orientation=%d", natural_ori);
		DXR_HW_DBG("worker: cached metrics: panel=%ux%u px, view(tile)=%ux%u px, "
		           "%.3fx%.3f m (size_ok=%d res_ok=%d view_ok=%d)",
		           cnsdk->display_pixel_w_cached, cnsdk->display_pixel_h_cached,
		           cnsdk->view_res_w_cached, cnsdk->view_res_h_cached,
		           cnsdk->display_width_m_cached, cnsdk->display_height_m_cached,
		           (int)got_size, (int)got_res, (int)got_view);
	} else {
		U_LOG_W("leia_core_get_device_config failed in worker; metrics stay default");
	}

	// Phase 2b: heavy enable + start. Single call, can't be interrupted —
	// destroy will block on the join until this returns.
	if (!leia_core_enable_face_tracking(cnsdk->core, true)) {
		U_LOG_W("leia_core_enable_face_tracking failed (worker)");
		return;
	}
	leia_core_start_face_tracking(cnsdk->core, true);

	// Register the frame listener for the in-service face readout. leia_core
	// takes ownership of the listener, so we don't release it ourselves.
	cnsdk->frame_listener = leia_headtracking_frame_listener_alloc(
	    cnsdk->lib, on_headtracking_frame, cnsdk, nullptr);
	if (cnsdk->frame_listener != nullptr) {
		leia_core_set_face_tracking_frame_listener(cnsdk->core, cnsdk->frame_listener);
		U_LOG_W("CNSDK frame listener registered (in-service face readout)");
	} else {
		U_LOG_W("leia_headtracking_frame_listener_alloc failed");
	}

	cnsdk->face_tracking_started.store(true, std::memory_order_release);
	U_LOG_W("CNSDK face tracking started (worker)");

	// Tracking is now available (licensed): apply the eye-tracking-mode policy
	// (#522). If the runtime already selected MANUAL, or selected MANAGED, this
	// reconciles NoFaceMode accordingly (replacing the bootstrap force-3D). If no
	// explicit request arrived, the default MANAGED now engages vendor auto-2D.
	apply_eye_tracking_mode(cnsdk);

	// Phase 3 (#201): tracking watchdog. The CNSDK HTS client DROPS messages
	// sent before its binder connection to the head-tracking service is up
	// ("[HTS-Client] Failed to send message ...: not connected") and never
	// replays them on connect. When the dropped message is the frame-
	// subscription handshake, this session runs face-less for its entire
	// life — the engine tracks, other clients get frames, this one gets
	// none → viewer (0,0,0) → zero-IPD Kooima → woven MONO ("stuck in 2D").
	// 100%-reproducible on swipe-close→relaunch (the fresh process's
	// handshake always races its own connect). Detection is trivial thanks
	// to the liveness clock (#152 L-c): listener_frame_ns is stamped on
	// EVERY delivered frame, so "still 0 well after start" == subscribed
	// nothing. Remedy: bounce start_face_tracking — by then the connection
	// is up, so the re-sent handshake lands. Bounded attempts; exits the
	// moment a frame arrives or shutdown is requested.
	// Wait ~2 s for the first frame; if none, cycle; after each cycle allow
	// ~5 s for the replayed handshake + service round-trip (measured: frames
	// can arrive several seconds after the cycle).
	auto wait_for_frames = [&](int ticks_50ms) -> bool {
		for (int i = 0; i < ticks_50ms; i++) {
			if (cnsdk->shutting_down.load(std::memory_order_acquire) ||
			    cnsdk->lifecycle_paused.load(std::memory_order_relaxed)) {
				return true; // stop the watchdog either way
			}
			if (cnsdk->listener_frame_ns.load(std::memory_order_relaxed) != 0) {
				return true;
			}
			std::this_thread::sleep_for(50ms);
		}
		return cnsdk->listener_frame_ns.load(std::memory_order_relaxed) != 0;
	};
	if (wait_for_frames(40)) { // ~2 s: the healthy path
		return;
	}
	for (int attempt = 1; attempt <= 3; attempt++) {
		if (cnsdk->shutting_down.load(std::memory_order_acquire) ||
		    cnsdk->lifecycle_paused.load(std::memory_order_relaxed)) {
			return; // app paused/quitting; its own resume redoes the handshake
		}
		// start_face_tracking(false/true) does NOT replay the dropped
		// subscription handshake (measured — 3 bounces, still 0 frames).
		// The core on_pause/on_resume cycle is the path the user-visible
		// minimize/refocus workaround takes, and it reliably restores the
		// frame stream. Call the CNSDK core functions directly (NOT the
		// plug-in's leia_cnsdk_on_pause wrapper) so the lens-preference
		// refcount is untouched — no visible 2D blink. Activity-lifecycle
		// API is only safe with a real Activity (in-process); never in a
		// service context. tracking_cycling gates process_atlas_weave for
		// the duration: CNSDK throws if the weave runs mid-teardown
		// (observed: terminate()/__emutls SIGSEGV on the repaint thread).
		if (!cnsdk->host_is_activity) {
			U_LOG_W("HW_DBG_CNSDK: tracking watchdog: no frames and no Activity "
			        "(service context) — cannot cycle, session stays untracked");
			return;
		}
		U_LOG_W("HW_DBG_CNSDK: tracking watchdog: no frames after start — frame "
		        "subscription likely lost pre-connect (leia-plugin#201); cycling "
		        "core pause/resume to replay the handshake (attempt %d/3)", attempt);
		cnsdk->tracking_cycling.store(true, std::memory_order_release);
		std::this_thread::sleep_for(100ms); // let an in-flight weave drain
		leia_core_on_pause(cnsdk->core);
		std::this_thread::sleep_for(150ms);
		leia_core_on_resume(cnsdk->core);
		cnsdk->tracking_cycling.store(false, std::memory_order_release);
		if (wait_for_frames(100)) { // ~5 s post-cycle
			if (cnsdk->listener_frame_ns.load(std::memory_order_relaxed) != 0) {
				U_LOG_W("HW_DBG_CNSDK: tracking watchdog: frames flowing after "
				        "cycle %d — recovered", attempt);
			}
			return;
		}
	}
	U_LOG_W("HW_DBG_CNSDK: tracking watchdog: still no frames after 3 cycles — "
	        "giving up (session weaves untracked/mono until next app resume)");
}

} // namespace


/*
 *
 * Public API.
 *
 */

// Host-iface Android accessors captured at xrtPluginNegotiate. The plug-in's
// own statically-linked android_globals copy is never populated by the
// runtime, so we obtain the JavaVM/Activity through these host callbacks.
// NULL until set (older runtime / non-Android) -> legacy fallback below.
static void *(*g_host_get_android_vm)(void) = nullptr;
static void *(*g_host_get_android_activity)(void) = nullptr;

extern "C" void
leia_cnsdk_set_host_android_accessors(void *(*get_vm)(void), void *(*get_activity)(void))
{
	g_host_get_android_vm = get_vm;
	g_host_get_android_activity = get_activity;
}

// Optional third accessor (runtime #1037 / ADR-036 D2): a Context whose
// getClassLoader() is the RUNTIME APK's. Used ONLY as the core loader's
// Context — CNSDK derives its DexClassLoader parent from it — which is why it
// is separate from the Activity accessor above; everything Activity-typed
// (limit_orientations, permission dialogs) keeps using the Activity.
// NULL on a runtime without the slot -> previous behaviour.
static void *(*g_host_get_class_host_context)(void) = nullptr;

extern "C" void
leia_cnsdk_set_host_class_context_accessor(void *(*get_class_host_context)(void))
{
	g_host_get_class_host_context = get_class_host_context;
}

extern "C" xrt_result_t
leia_cnsdk_create(struct leia_cnsdk **out_cnsdk)
{
	DXR_HW_DBG("leia_cnsdk_create: entering");

#ifdef XRT_OS_ANDROID
	// Prefer the host-iface accessors (the runtime's populated VM globals);
	// fall back to our own android_globals only if the host didn't supply
	// them (older runtime that predates the host-iface getters).
	void *vm = g_host_get_android_vm != nullptr ? g_host_get_android_vm() : (void *)android_globals_get_vm();
	void *activity = g_host_get_android_activity != nullptr ? g_host_get_android_activity()
	                                                        : android_globals_get_activity();
	DXR_HW_DBG("leia_cnsdk_create: android vm=%p activity=%p (host_accessors=%p/%p)", vm, activity,
	           (void *)g_host_get_android_vm, (void *)g_host_get_android_activity);
#endif

	// CNSDK 0.10.x loader: load the core library first. The JavaVM + a
	// Context go into the loader request now (not the init configuration),
	// and the loader brings up the core impl that talks to the on-device
	// Leia service. (0.7.28 had no loader — leia_core_init_configuration_alloc
	// took just the version string.)
	struct leia_core_library_load_request load_req = {};
	load_req.apiVersion = CNSDK_VERSION_U64;
	load_req.loaderVersion = LEIA_CORE_LOADER_API_VERSION;
#ifdef XRT_OS_ANDROID
	struct leia_core_library_load_android android_load = {};
	android_load.vm = (JavaVM *)vm;
	// #1037: prefer the runtime's class-host Context. CNSDK builds the core
	// loader's DexClassLoader with `context.getClassLoader()` as the parent, so
	// THIS is what decides whether com.leia.sdk.internal.Plugin resolves out of
	// the RUNTIME APK (Architecture A: the plug-in runs in the app's process and
	// the app bundles no vendor AAR, ADR-025 + ADR-036 D2) or out of the host
	// app's dex (the previous coupling). NULL — an older runtime, or one that
	// could not build the Context — falls back to the Activity / Service Context,
	// which is what every shipping configuration used until now. Either way it is
	// an android.content.Context, which is all the loader needs.
	void *class_ctx = g_host_get_class_host_context != nullptr ? g_host_get_class_host_context() : nullptr;
	android_load.context = (jobject)(class_ctx != nullptr ? class_ctx : activity);
	DXR_HW_DBG("leia_cnsdk_create: core loader context=%p (%s)", (void *)android_load.context,
	           class_ctx != nullptr ? "runtime class-host" : "app activity/service context");
	load_req.android = &android_load;
#endif
	struct leia_core_library *lib = leia_core_library_load(&load_req);
	if (lib == NULL) {
		U_LOG_E("leia_core_library_load failed");
		*out_cnsdk = NULL;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// #ROLL: resolve the experimental per-EYE accessors up front. These are the
	// only CNSDK entry points that report an eye VECTOR rather than a bare face
	// point, and without one the DP has to synthesize a fixed horizontal pair —
	// which silently discards head ROLL and is exactly the look-around defect
	// this addresses. leia_get_experimental_api is a library-level lookup, so it
	// works here, before any core exists. A null result is not an error: every
	// consumer falls back, and the resolution is logged once so a bug report
	// says which sources this build actually had.
	leia_core_get_lookaround_eyes fn_lookaround =
	    LEIA_GET_EXPERIMENTAL_API(lib, leia_core_get_lookaround_eyes);
	leia_core_get_non_predicted_eyes fn_nonpred_eyes =
	    LEIA_GET_EXPERIMENTAL_API(lib, leia_core_get_non_predicted_eyes);
	// #206: the per-frame prediction horizon sink. Absent on older CNSDK.
#if defined(leia_interlacer_set_predicted_scanout_ns_VERSION)
	leia_interlacer_set_predicted_scanout_ns fn_scanout =
	    LEIA_GET_EXPERIMENTAL_API(lib, leia_interlacer_set_predicted_scanout_ns);
#else
	void *fn_scanout = nullptr; // API predates this CNSDK
#endif
	U_LOG_W("HW_EYES: experimental API — lookaround=%s non_predicted=%s predicted_scanout=%s",
	        fn_lookaround != nullptr ? "OK" : "MISSING",
	        fn_nonpred_eyes != nullptr ? "OK" : "MISSING",
	        fn_scanout != nullptr ? "OK" : "MISSING");

#ifdef XRT_OS_ANDROID
	// LOXR-730/733: register the host Activity for orientation tracking so the
	// core's orientation auto-detect follows the real device orientation across
	// rotations. The native (NativeActivity) path otherwise never does this;
	// Leia's own Java sample calls LeiaSDK.limitOrientations(activity,
	// legalOrientations) at startup (com.leia.sdk.test MainActivity.kt:102).
	// This matters for the weave because leia_interlacer_vulkan_do_post_process
	// takes NO orientation param — the interlacing shader reads orientation from
	// the core. If the core's orientation is stale in landscape, the landscape
	// weave ghosts (clean portrait, double-image landscape). All four
	// orientations are legal for us.
	// OrientationLimiter.configure() is a Java method typed to android.app.Activity
	// and calls setRequestedOrientation — Activity-only. Out-of-process (#510) the
	// host hands us the runtime SERVICE's Context (no Activity exists; the app owns
	// its own orientation), so calling it would trip CheckJNI ("bad arguments
	// passed to OrientationLimiter.configure(Activity,...)") and abort the service.
	// Only register the limiter when the handle is really an Activity (in-process);
	// the core's orientation auto-detect still follows rotations otherwise.
	if (activity != NULL && android_globals_is_instance_of_activity((struct _JavaVM *)vm, activity)) {
		struct leia_legal_orientations legal = {};
		legal.portrait = 1;
		legal.landscape = 1;
		legal.reversePortrait = 1;
		legal.reverseLandscape = 1;
		leia_core_limit_orientations(lib, (jobject)activity, &legal);
		DXR_HW_DBG("leia_cnsdk_create: limit_orientations(all) registered activity=%p", activity);
	} else {
		DXR_HW_DBG("leia_cnsdk_create: skipping limit_orientations (no Activity; "
		           "out-of-process service Context=%p)", activity);
	}
#endif

	struct leia_core_init_configuration *config = leia_core_init_configuration_alloc(lib, CNSDK_VERSION);

#ifdef XRT_OS_ANDROID
	// Activity handle still goes through the init configuration (permission
	// dialogs etc.); the JavaVM no longer does — it's in the loader request.
	leia_core_init_configuration_set_platform_android_handle(config, LEIA_CORE_ANDROID_HANDLE_ACTIVITY,
	                                                         (jobject)activity);
#endif

	leia_core_init_configuration_set_platform_log_level(config, kLeiaLogLevelTrace);
	leia_core_init_configuration_set_enable_validation(config, true);

	// HW-2c: use the in-SERVICE face-tracking runtime — it delegates tracking
	// to the device's licensed Leia system service. The in-app runtime failed
	// to license on the Lume Pad ("Invalid Device"). IN_SERVICE is the enum
	// default (0), set explicitly so it's unambiguous.
	leia_core_init_configuration_set_face_tracking_runtime(
	    config, LEIA_FACE_TRACKING_RUNTIME_IN_SERVICE);

	// NOTE: tried config-time auto enable/start + check_permission(false) to
	// "start in 3D", but it regressed the device's 3D activation (lost the
	// system Present3D / mode3D=true engagement that the worker-driven path
	// triggers). Reverted — the worker enables face tracking + 3D after the
	// async core init, which is what actually flips the panel into 3D. The
	// brief 2D→3D warmup is inherent to the async service bring-up.

	struct leia_core *core = leia_core_init_async(config);
	leia_core_init_configuration_free(config);

	if (core == NULL) {
		U_LOG_E("leia_core_init_async failed");
		leia_core_library_release(lib);
		*out_cnsdk = NULL;
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// debug.dxr.leia.capture_lux — set BEFORE the worker enables face tracking, so
	// the value rides in the head-tracking ClientHello (a per-client input to the
	// service's engine-config aggregation, LeiaInc/CNSDK#698) instead of becoming a
	// later global write. Off by default; the only effect when on is that the
	// service's engine captures ambient lux, which we do not consume.
	ensure_calibration_loaded();
	if (g_calib.capture_lux) {
		leia_core_set_face_tracking_capture_lux(core, 1);
		U_LOG_W("HW_DBG_CNSDK: debug.dxr.leia.capture_lux=1 — requested lux capture "
		        "pre-ClientHello for this core");
	}

	// Do NOT touch the core here — leia_core_init_async() only *kicks off*
	// asynchronous initialization (core.h: "while (!leia_core_is_initialized)"
	// before any use). Calling leia_core_enable_3d() now, before the core is
	// initialized, stalls the calling thread. Out-of-process (#510) this create
	// runs marshaled on the service MAIN thread so init_async can post to the
	// main Looper; blocking here would freeze that Looper, so the async init can
	// never complete (core tears down: "Release refCount=0 / Shutdown"). The
	// worker (face_tracking_worker) already polls leia_core_is_initialized and
	// then calls enable_no_face_mode + enable_3d in the correct order — let it.
	auto *cnsdk = new struct leia_cnsdk();
	cnsdk->lib = lib;
	cnsdk->core = core;
	cnsdk->fn_lookaround_eyes = fn_lookaround;
#if defined(leia_interlacer_set_predicted_scanout_ns_VERSION)
	cnsdk->fn_set_predicted_scanout = fn_scanout;
#endif
	cnsdk->fn_nonpred_eyes = fn_nonpred_eyes;
#ifdef XRT_OS_ANDROID
	// Same gate as limit_orientations above: Activity-typed CNSDK calls
	// (leia_core_on_pause/on_resume) are only safe with a real Activity.
	cnsdk->host_is_activity =
	    activity != NULL && android_globals_is_instance_of_activity((struct _JavaVM *)vm, activity);
#endif
	cnsdk->worker = std::thread(face_tracking_worker, cnsdk);

	DXR_HW_DBG("leia_cnsdk_create: core=%p, worker thread spawned", (void *)core);
	*out_cnsdk = cnsdk;
	return XRT_SUCCESS;
}

extern "C" void
leia_cnsdk_destroy(struct leia_cnsdk **cnsdk_ptr)
{
	if (cnsdk_ptr == NULL || *cnsdk_ptr == NULL) {
		return;
	}

	struct leia_cnsdk *cnsdk = *cnsdk_ptr;
	DXR_HW_DBG("leia_cnsdk_destroy: entering, core=%p", (void *)cnsdk->core);

	// Signal the worker, then join with a watchdog: if it doesn't finish
	// within kWorkerJoinTimeoutMs, detach instead so destroy can return.
	// The worker might be mid-leia_core_enable_face_tracking with no
	// interruption hook — without the timeout, destroy can hang
	// indefinitely on a CNSDK deadlock (audit B10).
	//
	// Detaching leaks the std::thread but is the only option short of
	// CNSDK exposing a cancel API.
	// Release the lens preference FIRST, while the core is healthiest — the
	// arbiter's refcount outlives this process, so a vote we never give back
	// leaves the panel in 3D forever (stuck-3D-after-close, runtime #563).
	// DP destroy implies on_pause per the runtime contract (#1039). Even if
	// the worker join below times out, this has already gone out.
	release_lens_preference(cnsdk, "destroy");

	cnsdk->shutting_down.store(true, std::memory_order_release);
	if (cnsdk->worker.joinable()) {
		constexpr auto kWorkerJoinTimeoutMs = std::chrono::milliseconds(2000);
		// std::thread::join doesn't take a timeout, so use a side thread
		// that does the join and a condition variable to wait on with a
		// deadline. Cheap on the happy path (the worker is usually
		// already finished by destroy time, so join returns instantly).
		std::atomic<bool> joined{false};
		std::thread joiner([&]() {
			cnsdk->worker.join();
			joined.store(true, std::memory_order_release);
		});
		const auto deadline = std::chrono::steady_clock::now() + kWorkerJoinTimeoutMs;
		while (!joined.load(std::memory_order_acquire) &&
		       std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (joined.load(std::memory_order_acquire)) {
			joiner.join();
		} else {
			U_LOG_W("CNSDK worker did not exit within %lld ms; detaching",
			        (long long)kWorkerJoinTimeoutMs.count());
			cnsdk->worker.detach();
			joiner.detach();
		}
	}

	// Stop face tracking BEFORE releasing the core (#39). The worker enabled +
	// started tracking and registered a frame listener; leia_core_release joins
	// leiaCore's internal tracking/service threads, and with tracking still
	// started that join never returns — the service then hangs inside
	// multi_compositor_end_session, the IPC session_end reply is never sent and
	// the client deadlocks in xrEndSession (black screen on resume,
	// runtime#528's picker scenario). Tear down in the reverse order of the
	// worker's bring-up: listener → started → enabled.
	if (cnsdk->core != NULL && cnsdk->face_tracking_started.load(std::memory_order_acquire)) {
		leia_core_set_face_tracking_frame_listener(cnsdk->core, NULL);
		cnsdk->frame_listener = NULL; // core owned it; cleared with the listener slot
		leia_core_start_face_tracking(cnsdk->core, false);
		leia_core_enable_face_tracking(cnsdk->core, false);
		cnsdk->face_tracking_started.store(false, std::memory_order_release);
		DXR_HW_DBG("leia_cnsdk_destroy: face tracking stopped + disabled (#39)");
	}

	if (cnsdk->interlacer != NULL) {
		// CNSDK 0.10.x: single-arg interlacer release.
		leia_interlacer_release(cnsdk->interlacer);
		cnsdk->interlacer = NULL;
	}

	if (cnsdk->core != NULL) {
		leia_core_release(cnsdk->core);
		cnsdk->core = NULL;
	}

	// CNSDK 0.10.x: release the loader library last (replaces 0.7.28's
	// leia_platform_on_library_unload()).
	if (cnsdk->lib != NULL) {
		leia_core_library_release(cnsdk->lib);
		cnsdk->lib = NULL;
	}

	delete cnsdk;
	*cnsdk_ptr = NULL;
}

extern "C" bool
leia_cnsdk_is_initialized(struct leia_cnsdk *cnsdk)
{
	if (cnsdk == NULL || cnsdk->core == NULL) {
		return false;
	}
	return leia_core_is_initialized(cnsdk->core);
}

extern "C" void
leia_cnsdk_on_pause(struct leia_cnsdk *cnsdk)
{
	if (cnsdk != NULL) {
		cnsdk->lifecycle_paused.store(true, std::memory_order_relaxed);
	}
	if (cnsdk == NULL || cnsdk->core == NULL) {
		return;
	}
	if (!leia_core_is_initialized(cnsdk->core)) {
		DXR_HW_DBG("on_pause: skipped (core not initialized yet)");
		return;
	}
	// The runtime contract (runtime #1039, xrt_display_processor.h): on_pause
	// means "this session's window is not visible — stop weaving and RELEASE
	// your lens preference". It does NOT mean "force the panel to 2D": with a
	// sibling window still weaving on the same panel, commanding 2D would
	// flatten it under the sibling. Release is refcount-correct in both
	// directions — see release_lens_preference for why, and why that also keeps
	// stuck-3D-after-close (runtime #563) fixed.
	// #558 overlay mode is the exception: the avatar intentionally backgrounds
	// while still weaving the tiger on top of the launcher, so it keeps its
	// vote (the weave loop keeps forcing 3D each frame).
	if (overlay_mode_active()) {
		DXR_HW_DBG("on_pause: overlay mode — still weaving, keeping the lens preference");
		return;
	}
	// Deprecated A/B override (runtime #1039): the #151 behaviour, kept only
	// so the two can be compared on a device. Holding the bind takes a lens
	// vote for a window nobody can see, which re-opens runtime #563.
	if (multiapp_mode_active()) {
		U_LOG_W("HW_DBG_CNSDK: on_pause: debug.dxr.multiapp=1 (DEPRECATED) — holding the lens "
		        "preference instead of releasing it; release happens at destroy");
		return;
	}
	release_lens_preference(cnsdk, "on_pause");
	if (!cnsdk->host_is_activity) {
		// Out-of-process: leia_core_on_pause/on_resume are Activity-
		// lifecycle APIs (on_resume calls FaceTrackingHelper.
		// checkPermission(Activity,...) → CheckJNI abort with a Service
		// context). The release above is the part that matters.
		DXR_HW_DBG("on_pause: lens preference released (service context, no Activity)");
		return;
	}
	DXR_HW_DBG("on_pause: forwarding to leia_core_on_pause");
	leia_core_on_pause(cnsdk->core);
}

extern "C" void
leia_cnsdk_on_resume(struct leia_cnsdk *cnsdk)
{
	if (cnsdk != NULL) {
		cnsdk->lifecycle_paused.store(false, std::memory_order_relaxed);
	}
	if (cnsdk == NULL || cnsdk->core == NULL) {
		return;
	}
	if (!leia_core_is_initialized(cnsdk->core)) {
		DXR_HW_DBG("on_resume: skipped (core not initialized yet)");
		return;
	}
	// Counterpart of the release in on_pause (runtime #1039): this session's
	// window is visible again, so re-assert the lens preference NOW rather
	// than waiting for the weave loop's first frame — the vendor arbiter is a
	// refcount and the panel should come back the moment the window does.
	assert_lens_preference(cnsdk, "on_resume");
	if (!cnsdk->host_is_activity) {
		// Out-of-process: see on_pause — the Activity-lifecycle API would
		// abort the service. The re-assert above is the part that matters.
		DXR_HW_DBG("on_resume: lens preference re-asserted (service context, no Activity)");
		return;
	}
	DXR_HW_DBG("on_resume: forwarding to leia_core_on_resume");
	leia_core_on_resume(cnsdk->core);
}

extern "C" bool
leia_cnsdk_is_tracking_cycling(struct leia_cnsdk *cnsdk)
{
	return cnsdk != NULL && cnsdk->tracking_cycling.load(std::memory_order_acquire);
}

extern "C" void
leia_cnsdk_set_eye_tracking_mode(struct leia_cnsdk *cnsdk, uint32_t mode)
{
	if (cnsdk == NULL) {
		return;
	}
	cnsdk->eye_tracking_mode.store(mode, std::memory_order_release);
	DXR_HW_DBG("set_eye_tracking_mode: mode=%u (0=MANAGED,1=MANUAL)", mode);

	// Apply immediately if the core is up; otherwise the face-tracking worker
	// applies the stored mode once it knows the licensing/availability status.
	if (cnsdk->core != NULL && leia_core_is_initialized(cnsdk->core)) {
		apply_eye_tracking_mode(cnsdk);
	}
}

extern "C" bool
leia_cnsdk_get_display_metrics(struct leia_cnsdk *cnsdk,
                               float *out_width_m,
                               float *out_height_m,
                               uint32_t *out_pixel_w,
                               uint32_t *out_pixel_h)
{
	// Worker thread snapshots all four values from the device config
	// once, then sets the atomic. Render thread polls the atomic and
	// reads the cached float/int fields. No per-frame get/release.
	if (cnsdk == NULL ||
	    !cnsdk->display_metrics_cached.load(std::memory_order_acquire)) {
		return false;
	}

	if (out_width_m != NULL) {
		*out_width_m = cnsdk->display_width_m_cached;
	}
	if (out_height_m != NULL) {
		*out_height_m = cnsdk->display_height_m_cached;
	}
	if (out_pixel_w != NULL) {
		*out_pixel_w = cnsdk->display_pixel_w_cached;
	}
	if (out_pixel_h != NULL) {
		*out_pixel_h = cnsdk->display_pixel_h_cached;
	}
	return true;
}

extern "C" bool
leia_cnsdk_get_view_resolution(struct leia_cnsdk *cnsdk,
                               uint32_t *out_view_w,
                               uint32_t *out_view_h,
                               int32_t *out_natural_orientation)
{
	// Per-view (tile) resolution in NATURAL orientation, cached by the worker
	// under display_metrics_cached. Used to derive the 3D view_scale (#518).
	if (cnsdk == NULL ||
	    !cnsdk->display_metrics_cached.load(std::memory_order_acquire)) {
		return false;
	}
	if (cnsdk->view_res_w_cached == 0 || cnsdk->view_res_h_cached == 0) {
		return false;
	}
	if (out_view_w != NULL) {
		*out_view_w = cnsdk->view_res_w_cached;
	}
	if (out_view_h != NULL) {
		*out_view_h = cnsdk->view_res_h_cached;
	}
	if (out_natural_orientation != NULL) {
		*out_natural_orientation = cnsdk->natural_orientation.load(std::memory_order_relaxed);
	}
	return true;
}

extern "C" bool
leia_cnsdk_ensure_face_tracking_started(struct leia_cnsdk *cnsdk)
{
	// Worker thread handles enable + start; this is now a non-blocking
	// status check.
	if (cnsdk == NULL) {
		return false;
	}
	return cnsdk->face_tracking_started.load(std::memory_order_acquire);
}

extern "C" bool
leia_cnsdk_ensure_interlacer(struct leia_cnsdk *cnsdk,
                              VkDevice device,
                              VkPhysicalDevice physDev,
                              VkFormat targetFmt)
{
	if (cnsdk == NULL || cnsdk->core == NULL) {
		return false;
	}
	if (cnsdk->interlacer != NULL) {
		return true;
	}
	// One-shot give-up: once leia_interlacer_vulkan_initialize fails
	// (typically permanently — wrong VkDevice format, no GPU memory,
	// CNSDK lib mismatch), don't keep retrying every frame.
	if (cnsdk->interlacer_init_failed) {
		return false;
	}
	if (!leia_core_is_initialized(cnsdk->core)) {
		return false;
	}

	struct leia_interlacer_init_configuration *ic = leia_interlacer_init_configuration_alloc();
	// Atlas mode: CNSDK accepts the SBS atlas VkImage+View directly per
	// frame via set_interlace_view_texture_atlas, and splits internally.
	// No per-view image management on our side; the DP shrinks
	// substantially. See feature/android-cnsdk-ci for the prior art
	// (CNSDK 0.10.56 used a different API but same architectural idea).
	leia_interlacer_init_configuration_set_use_atlas_for_views(ic, true);
	// Views format = atlas format. Atlas is rendered to UNORM by
	// comp_vk_native_renderer.c, so use UNORM here (audit B2).
	// CNSDK 0.10.x dropped the separate textureFormat param (now just
	// renderTargetFormat + depthStencilFormat).
	cnsdk->interlacer = leia_interlacer_vulkan_initialize(
	    cnsdk->core, ic, device, physDev,
	    targetFmt, VK_FORMAT_D32_SFLOAT, 3);
	leia_interlacer_init_configuration_free(ic);

	if (cnsdk->interlacer == NULL) {
		U_LOG_W("leia_interlacer_vulkan_initialize returned NULL; giving up (no retries)");
		cnsdk->interlacer_init_failed = true;
		return false;
	}

	// Tell CNSDK the atlas is laid out 2x1 SBS horizontal. This is the
	// default but we set it explicitly so future layout changes
	// (multi-view modes) only have to touch one place.
	leia_interlacer_set_num_tiles(cnsdk->interlacer, 2, 1);
	DXR_HW_DBG("ensure_interlacer: created interlacer=%p (atlas mode, 2x1, targetFmt=%d)",
	           (void *)cnsdk->interlacer, (int)targetFmt);
	return true;
}

#ifdef XRT_OS_ANDROID
/*!
 * Orientation tripwire (runtime#1079).
 *
 * CNSDK decides the DEVICE orientation in
 * `leia/core/android/java/com/leia/sdk/OrientationHelper.java`
 * (`getRealOrientation()`): it reads
 * `context.getSystemService(WINDOW_SERVICE).getDefaultDisplay().getMetrics()`
 * and compares width vs height against `Display.getRotation()`.
 *
 * `Display.getMetrics()` is **window-adjusted** — inside an app's own process it
 * reports the app's window, not the panel. So an app in a portrait-SHAPED
 * multi-window (freeform / split-screen) window on a landscape panel — e.g.
 * 1000x1500 on a 2560x1600 landscape display — makes CNSDK conclude PORTRAIT
 * while the device is physically LANDSCAPE. The core then pushes
 * `SetDeviceOrientation(Portrait)` to the head-tracking service, whose face
 * detector runs the wrong camera/rotation and logs "could not find a face"
 * forever; with no face the weave has no viewer to steer to and the output
 * reads FLAT. The interlacer's own rotation compensation is wrong too.
 *
 * Out-of-process the runtime SERVICE's Context has no window, so its metrics are
 * the panel's — which is exactly why the OOP route was never affected and the
 * in-process route was.
 *
 * There is no client-side fix: the adjustment is applied per PROCESS (a
 * `Context.createDisplayContext()` Context was measured to make no difference on
 * an NP02J), and CNSDK exposes no orientation setter — only
 * `leia_core_get_orientation()`. The real fix belongs in CNSDK:
 * `getRealOrientation()` must use `Display.getRealMetrics()` (or
 * `WindowManager.getMaximumWindowMetrics()`), which is window-independent,
 * and/or CNSDK should expose `leia_core_set_device_orientation()`.
 *
 * Until then this tripwire turns a silent, baffling failure ("tracking just
 * doesn't work in a floating window") into one greppable WARN. It computes the
 * TRUE orientation the same way CNSDK does but from `getRealMetrics()`, and logs
 * only when the two disagree, and only when the disagreement changes.
 */
static int
true_device_orientation_android(JavaVM *vm, jobject ctx)
{
	// Returns a leia_orientation value, or -1 when it cannot be determined.
	if (vm == nullptr || ctx == nullptr) {
		return -1;
	}
	JNIEnv *env = nullptr;
	if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
		return -1;
	}

	int result = -1;
	jclass ctx_cls = env->FindClass("android/content/Context");
	jmethodID get_service =
	    ctx_cls != nullptr
	        ? env->GetMethodID(ctx_cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;")
	        : nullptr;
	if (get_service == nullptr) {
		env->ExceptionClear();
		return -1;
	}

	jstring svc = env->NewStringUTF("window"); // Context.WINDOW_SERVICE
	jobject wm = env->CallObjectMethod(ctx, get_service, svc);
	env->DeleteLocalRef(svc);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		wm = nullptr;
	}
	if (wm != nullptr) {
		jclass wm_cls = env->GetObjectClass(wm);
		jmethodID get_default =
		    wm_cls != nullptr ? env->GetMethodID(wm_cls, "getDefaultDisplay", "()Landroid/view/Display;")
		                      : nullptr;
		jobject disp = get_default != nullptr ? env->CallObjectMethod(wm, get_default) : nullptr;
		if (env->ExceptionCheck()) {
			env->ExceptionClear();
			disp = nullptr;
		}
		if (disp != nullptr) {
			jclass disp_cls = env->GetObjectClass(disp);
			jclass dm_cls = env->FindClass("android/util/DisplayMetrics");
			jmethodID get_real =
			    (disp_cls != nullptr && dm_cls != nullptr)
			        ? env->GetMethodID(disp_cls, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V")
			        : nullptr;
			jmethodID get_rot =
			    disp_cls != nullptr ? env->GetMethodID(disp_cls, "getRotation", "()I") : nullptr;
			jmethodID dm_ctor = dm_cls != nullptr ? env->GetMethodID(dm_cls, "<init>", "()V") : nullptr;
			if (get_real != nullptr && get_rot != nullptr && dm_ctor != nullptr) {
				jobject dm = env->NewObject(dm_cls, dm_ctor);
				env->CallVoidMethod(disp, get_real, dm);
				jfieldID w_fid = env->GetFieldID(dm_cls, "widthPixels", "I");
				jfieldID h_fid = env->GetFieldID(dm_cls, "heightPixels", "I");
				jint rot = env->CallIntMethod(disp, get_rot);
				if (!env->ExceptionCheck() && w_fid != nullptr && h_fid != nullptr) {
					jint w = env->GetIntField(dm, w_fid);
					jint h = env->GetIntField(dm, h_fid);
					// Same decision table as CNSDK's OrientationHelper, but on
					// REAL (window-independent) metrics. Surface.ROTATION_* are
					// 0/1/2/3 == 0/90/180/270 degrees.
					const bool natural_portrait = ((rot == 0 || rot == 2) && h > w) ||
					                              ((rot == 1 || rot == 3) && w > h);
					static const int kPortraitTable[4] = {
					    LEIA_ORIENTATION_PORTRAIT, LEIA_ORIENTATION_LANDSCAPE,
					    LEIA_ORIENTATION_REVERSE_PORTRAIT, LEIA_ORIENTATION_REVERSE_LANDSCAPE};
					static const int kLandscapeTable[4] = {
					    LEIA_ORIENTATION_LANDSCAPE, LEIA_ORIENTATION_PORTRAIT,
					    LEIA_ORIENTATION_REVERSE_LANDSCAPE, LEIA_ORIENTATION_REVERSE_PORTRAIT};
					if (rot >= 0 && rot < 4) {
						result = natural_portrait ? kPortraitTable[rot] : kLandscapeTable[rot];
					}
				} else {
					env->ExceptionClear();
				}
				if (dm != nullptr) {
					env->DeleteLocalRef(dm);
				}
			} else {
				env->ExceptionClear();
			}
			env->DeleteLocalRef(disp);
		}
		env->DeleteLocalRef(wm);
	}
	return result;
}

/*!
 * Panel size in pixels from `Display.getRealMetrics()`, in the device's NATURAL
 * orientation (long edge = height when the device is naturally portrait).
 *
 * Why this exists: the display-processor is asked for panel geometry during
 * `xrt_instance_create_system`, which happens ~80 ms BEFORE the CNSDK worker
 * caches the real device metrics — so the CNSDK path always missed and the DP
 * fell back to compiled-in Lume Pad 2 constants (2560x1600). On any other panel
 * the compositor then sized its atlas for a display that isn't there and
 * presented into a smaller surface, so nothing was visible: a black screen with
 * working audio. Measured on a 1080x2400 phone, 2026-08-26.
 *
 * `getRealMetrics()` is window-independent (unlike the Context's own metrics,
 * see true_device_orientation_android above), so it is correct for a floating
 * window too, and it needs no CNSDK — which is the point.
 *
 * @return true and fills the outputs, or false when they cannot be determined.
 */
static bool
android_panel_px(JavaVM *vm, jobject ctx, uint32_t *out_w, uint32_t *out_h, float *out_w_m, float *out_h_m)
{
	if (vm == nullptr || ctx == nullptr || out_w == nullptr || out_h == nullptr) {
		return false;
	}
	JNIEnv *env = nullptr;
	if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
		return false;
	}
	bool ok = false;

	jclass ctx_cls = env->FindClass("android/content/Context");
	jmethodID get_service =
	    ctx_cls != nullptr
	        ? env->GetMethodID(ctx_cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;")
	        : nullptr;
	if (get_service != nullptr) {
		jstring name = env->NewStringUTF("window");
		jobject wm = env->CallObjectMethod(ctx, get_service, name);
		env->DeleteLocalRef(name);
		if (wm != nullptr && !env->ExceptionCheck()) {
			jclass wm_cls = env->FindClass("android/view/WindowManager");
			jmethodID get_disp =
			    wm_cls != nullptr ? env->GetMethodID(wm_cls, "getDefaultDisplay", "()Landroid/view/Display;")
			                      : nullptr;
			jobject disp = get_disp != nullptr ? env->CallObjectMethod(wm, get_disp) : nullptr;
			if (disp != nullptr && !env->ExceptionCheck()) {
				jclass disp_cls = env->GetObjectClass(disp);
				jclass dm_cls = env->FindClass("android/util/DisplayMetrics");
				jmethodID get_real =
				    disp_cls != nullptr && dm_cls != nullptr
				        ? env->GetMethodID(disp_cls, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V")
				        : nullptr;
				jmethodID get_rot =
				    disp_cls != nullptr ? env->GetMethodID(disp_cls, "getRotation", "()I") : nullptr;
				jmethodID dm_ctor = dm_cls != nullptr ? env->GetMethodID(dm_cls, "<init>", "()V") : nullptr;
				if (get_real != nullptr && get_rot != nullptr && dm_ctor != nullptr) {
					jobject dm = env->NewObject(dm_cls, dm_ctor);
					env->CallVoidMethod(disp, get_real, dm);
					jfieldID w_fid = env->GetFieldID(dm_cls, "widthPixels", "I");
					jfieldID h_fid = env->GetFieldID(dm_cls, "heightPixels", "I");
					jint rot = env->CallIntMethod(disp, get_rot);
					if (!env->ExceptionCheck() && w_fid != nullptr && h_fid != nullptr) {
						jint w = env->GetIntField(dm, w_fid);
						jint h = env->GetIntField(dm, h_fid);
						// getRealMetrics() reports the CURRENT rotation. Undo it so the
						// caller always sees the natural-orientation panel, which is what
						// CNSDK's PANEL_RESOLUTION_PX reports and what the atlas is sized
						// against.
						if (rot == 1 || rot == 3) {
							jint t = w;
							w = h;
							h = t;
						}
						if (w > 0 && h > 0) {
							*out_w = (uint32_t)w;
							*out_h = (uint32_t)h;
							// Physical size from the real DPI, so Kooima gets the panel
							// it is actually drawing on rather than a Lume Pad's.
							if (out_w_m != nullptr && out_h_m != nullptr) {
								jfieldID xdpi_fid = env->GetFieldID(dm_cls, "xdpi", "F");
								jfieldID ydpi_fid = env->GetFieldID(dm_cls, "ydpi", "F");
								if (xdpi_fid != nullptr && ydpi_fid != nullptr) {
									float xdpi = env->GetFloatField(dm, xdpi_fid);
									float ydpi = env->GetFloatField(dm, ydpi_fid);
									if (xdpi > 1.0f && ydpi > 1.0f) {
										*out_w_m = (float)w / xdpi * 0.0254f;
										*out_h_m = (float)h / ydpi * 0.0254f;
									}
								} else {
									env->ExceptionClear();
								}
							}
							ok = true;
						}
					} else {
						env->ExceptionClear();
					}
					if (dm != nullptr) {
						env->DeleteLocalRef(dm);
					}
				} else {
					env->ExceptionClear();
				}
				env->DeleteLocalRef(disp);
			}
			env->DeleteLocalRef(wm);
		} else {
			env->ExceptionClear();
		}
	}
	return ok;
}

extern "C" bool
leia_cnsdk_get_android_panel_px(uint32_t *out_w, uint32_t *out_h, float *out_w_m, float *out_h_m)
{
	void *vm = g_host_get_android_vm != nullptr ? g_host_get_android_vm() : (void *)android_globals_get_vm();
	void *ctx = g_host_get_android_activity != nullptr ? g_host_get_android_activity()
	                                                   : android_globals_get_activity();
	return android_panel_px((JavaVM *)vm, (jobject)ctx, out_w, out_h, out_w_m, out_h_m);
}

/*!
 * Re-express a display-center point (meters) from the display's NATURAL
 * orientation frame into the CURRENT held orientation, then apply the residual
 * per-device calibration overrides.
 *
 * CNSDK's faces and eye points arrive in the natural-orientation frame (that's
 * the frame it weaves in); our look-around output must be in the held one.
 *
 * GetRelativeClockwiseAngle(natural, current) is how far the DEVICE FRAME
 * actively turned from natural to current. But we're re-expressing a fixed
 * point's COORDINATES from the natural frame into the current frame, which is
 * the INVERSE (passive) rotation: when a frame turns clockwise by θ, a fixed
 * point's coords in it turn counter-clockwise by θ. So we rotate by
 * GetRelativeClockwiseAngle(current, natural) = (natural - current) steps, each
 * step 90°. z (depth) is orientation-invariant.
 *
 * #ROLL: factored out of leia_cnsdk_get_primary_face so the per-eye path runs
 * the IDENTICAL transform. Applying it to BOTH eye points (rather than to a
 * centre plus a separately-rotated offset) is what keeps the eye VECTOR correct
 * through a device rotation — the vector is just the difference of two points
 * that both went through the same map.
 *
 * @param[in,out] pos    xyz in meters, rewritten in place.
 * @param[out]    steps  Optional: the 90° step count applied, for logging.
 */
static void
orient_display_point(struct leia_cnsdk *cnsdk, float pos[3], int *out_steps)
{
	const int natural = cnsdk->natural_orientation.load(std::memory_order_relaxed);
	const enum leia_orientation cur = leia_core_get_orientation(cnsdk->core);
	int steps = 0;
	if (natural >= 0 && (int)cur >= 0) {
		steps = (((natural - (int)cur) % 4) + 4) % 4; // inverse: (natural - current), × 90°
	}
	switch (steps) {
	case 1: { float t = pos[0]; pos[0] = -pos[1]; pos[1] = t; break; }  //  90°: (-y,  x)
	case 2: { pos[0] = -pos[0]; pos[1] = -pos[1]; break; }              // 180°: (-x, -y)
	case 3: { float t = pos[0]; pos[0] = pos[1]; pos[1] = -t; break; }  // 270°: ( y, -x)
	default: break;                                                     //   0°: ( x,  y)
	}

	// Residual per-device calibration overrides (all default OFF — the rotation
	// above is the principled mapping). Applied after the rotation.
	if (prop_override("debug.dxr.leia.face_swap_xy", false)) { float t = pos[0]; pos[0] = pos[1]; pos[1] = t; }
	if (prop_override("debug.dxr.leia.face_flip_x", false))  { pos[0] = -pos[0]; }
	if (prop_override("debug.dxr.leia.face_flip_y", false))  { pos[1] = -pos[1]; }
	if (prop_override("debug.dxr.leia.face_flip_z", false))  { pos[2] = -pos[2]; }

	if (out_steps != NULL) {
		*out_steps = steps;
	}
}

//! Log once per distinct disagreement. See true_device_orientation_android().
static void
check_orientation_tripwire(struct leia_cnsdk *cnsdk)
{
	if (cnsdk == nullptr || cnsdk->core == nullptr) {
		return;
	}
	void *vm = g_host_get_android_vm != nullptr ? g_host_get_android_vm() : (void *)android_globals_get_vm();
	void *ctx = g_host_get_class_host_context != nullptr ? g_host_get_class_host_context() : nullptr;
	if (ctx == nullptr) {
		ctx = g_host_get_android_activity != nullptr ? g_host_get_android_activity()
		                                             : android_globals_get_activity();
	}
	const int truth = true_device_orientation_android((JavaVM *)vm, (jobject)ctx);
	if (truth < 0) {
		return;
	}
	const int core_says = (int)leia_core_get_orientation(cnsdk->core);

	static int last_truth = -2, last_core = -2;
	if (truth == last_truth && core_says == last_core) {
		return;
	}
	last_truth = truth;
	last_core = core_says;
	if (truth == core_says) {
		return;
	}
	U_LOG_W("ORIENTATION MISMATCH (#1079): CNSDK core reports %d but the PANEL is %d. "
	        "CNSDK derives this from Display.getMetrics(), which is window-adjusted in an "
	        "app's own process, so a portrait-shaped freeform/split window on a landscape "
	        "panel (or vice-versa) reads wrong. Consequence: the head-tracking service is "
	        "told the wrong orientation, its face detector logs 'could not find a face', and "
	        "with no viewer the weave reads FLAT. Out-of-process (debug.dxr.force_ipc 1) is "
	        "unaffected. Vendor fix: OrientationHelper.getRealOrientation() must use "
	        "Display.getRealMetrics().",
	        core_says, truth);
}
#endif // XRT_OS_ANDROID

extern "C" bool
leia_cnsdk_get_primary_face(struct leia_cnsdk *cnsdk,
                            float *out_x,
                            float *out_y,
                            float *out_z)
{
	if (cnsdk == NULL || cnsdk->core == NULL ||
	    !cnsdk->face_tracking_started.load(std::memory_order_acquire)) {
		return false;
	}

	// Query all three face sources. The core's predicted / non-predicted faces
	// are latency-compensated and, crucially, already in DISPLAY-CENTER mm (camera
	// extrinsics applied). The frame-listener detection is raw CAMERA-space
	// posePosition (a fallback for when the core face is empty). The diagnostic is
	// U_LOG_W (WARN) on purpose — aux INFO is dropped from the Android hot path.
#ifdef XRT_OS_ANDROID
	// #1079 tripwire: a wrong core orientation is THE reason face detection dies
	// in a floating window, so check it right where we read the face.
	check_orientation_tripwire(cnsdk);
#endif

	bool lst_ok = cnsdk->listener_face_valid.load(std::memory_order_acquire);
	float lst_pos[3] = {
	    cnsdk->listener_face_x_mm.load(std::memory_order_relaxed),
	    cnsdk->listener_face_y_mm.load(std::memory_order_relaxed),
	    cnsdk->listener_face_z_mm.load(std::memory_order_relaxed),
	};

	// Expire the latch if the frame listener has gone quiet. Only the callback
	// can clear listener_face_valid, so without this the last tuple is latched
	// valid FOREVER once the callback stops firing (#152 L-c) and the DP keeps
	// weaving for an eye that left minutes ago. Invalidating drops us to the
	// no-face path (this function returns false unless the core has a face),
	// and the DP then uses the nominal viewer with is_tracking = false —
	// which is the honest answer, not the stale one.
	if (lst_ok) {
		const int64_t last_ns = cnsdk->listener_frame_ns.load(std::memory_order_relaxed);
		const int64_t age_ns = os_monotonic_get_ns() - last_ns;
		if (last_ns == 0 || age_ns > kListenerFrameStaleNs) {
			cnsdk->listener_face_valid.store(false, std::memory_order_release);
			lst_ok = false;
			// One-shot WARN so the first expiry is unmissable in a bug report;
			// recurrences are throttled INFO (this is a per-frame call site, so
			// a repeating WARN would flood the log). NOTE: aux INFO is dropped
			// from the Android hot path, so treat the WARN as the signal and
			// the INFO as a bonus on verbose builds.
			static bool warned_once = false;
			if (!warned_once) {
				warned_once = true;
				U_LOG_W("HW_FACE: frame listener stale (%lld ms, limit %lld ms) — "
				        "invalidating latched face, falling back to the nominal viewer",
				        (long long)(age_ns / 1000000), (long long)(kListenerFrameStaleNs / 1000000));
			} else {
				static int stale_dbg = 0;
				if ((stale_dbg++ % 300) == 0) {
					U_LOG_I("HW_FACE: frame listener stale again (%lld ms) — "
					        "latched face invalidated",
					        (long long)(age_ns / 1000000));
				}
			}
		}
	}

	float pred_pos[3] = {0, 0, 0};
	float np_pos[3] = {0, 0, 0};
	struct leia_float_slice pred_slice = {pred_pos, 3};
	struct leia_float_slice np_slice = {np_pos, 3};
	const bool pred_ok = leia_core_get_primary_face(cnsdk->core, pred_slice);
	const bool np_ok = leia_core_get_non_predicted_primary_face(cnsdk->core, np_slice);

	static int dbg = 0;
	if ((dbg++ % 60) == 0) {
		U_LOG_W("HW_FACE: listener=%d(%.0f,%.0f,%.0f) pred=%d(%.0f,%.0f,%.0f) nonpred=%d(%.0f,%.0f,%.0f)",
		        (int)lst_ok, lst_pos[0], lst_pos[1], lst_pos[2],
		        (int)pred_ok, pred_pos[0], pred_pos[1], pred_pos[2],
		        (int)np_ok, np_pos[0], np_pos[1], np_pos[2]);
	}

	// Source preference. This face drives the app's LOOK-AROUND (the runtime's
	// Kooima off-axis projection / rendered parallax), which must follow the
	// user's ACTUAL head — so use the NON-PREDICTED face. The PREDICTED face is
	// latency-compensated for the lenticular and is consumed INTERNALLY by CNSDK
	// for weaving (interlacer.cpp:527 GetPrimaryFace()); using it for rendering
	// would over-compensate the geometry. Both the non-predicted and predicted
	// faces are returned in DISPLAY-CENTER millimeters with the camera extrinsics
	// already applied (same frame as showFacePosition()'s dot). The raw
	// frame-listener detection is the head-tracking service's `posePosition`,
	// which is CAMERA-space (origin at the camera, sensor axes, Y image-down);
	// using it directly skips the extrinsics (the offset + inverted-Y we saw), so
	// it is only a last resort when the core face is genuinely empty.
	// Everything below this point works in DISPLAY-CENTER millimeters. The
	// core's faces already are; the listener's is not, so it gets lifted here
	// rather than downstream (#152 L-c) — mixing the two frames further down
	// was the coordinate-frame bug: the listener value used to flow straight
	// into the shared conversion, which subtracted a camera center that the
	// 0.10.x port had left permanently 0, so the extrinsics were simply never
	// applied and the fallback face carried the camera's offset and an
	// upside-down Y.
	float position[3];
	bool used_nonpred = false;
	if (np_ok) {
		position[0] = np_pos[0]; position[1] = np_pos[1]; position[2] = np_pos[2];
		used_nonpred = true;
	} else if (pred_ok) {
		position[0] = pred_pos[0]; position[1] = pred_pos[1]; position[2] = pred_pos[2];
	} else if (lst_ok) {
		// Camera space -> display-center space. posePosition has its origin at
		// the camera with image axes (Y points DOWN the image), so: flip Y into
		// the display's Y-up convention, then translate by the camera's own
		// offset from the display center. camera_center_*_m is 0 (and
		// camera_extrinsics_ok false) if the device config had no camera 0 — the
		// axis flip is a fixed convention and still applies, the translation
		// just degrades to the pre-fix behaviour. leia_camera::rotation_deg is
		// NOT applied: for a display-plane-mounted front camera it is ~0, and it
		// is logged once at snapshot time so a device where it isn't shows up.
		//
		// We do NOT drop this fallback: under
		// LEIA_FACE_TRACKING_RUNTIME_IN_SERVICE the core's own faces stay empty
		// on this hardware (see the pred=0 note at the top of this file), so the
		// listener is the ONLY face source there — refusing it would kill head
		// tracking outright on the NP02J.
		position[0] = lst_pos[0] + cnsdk->camera_center_x_m * 1000.0f;
		position[1] = -lst_pos[1] + cnsdk->camera_center_y_m * 1000.0f;
		position[2] = lst_pos[2] + cnsdk->camera_center_z_m * 1000.0f;

		static bool listener_fallback_warned = false;
		if (!listener_fallback_warned) {
			listener_fallback_warned = true;
			U_LOG_W("HW_FACE: core face empty — using the frame-listener fallback "
			        "(camera-space -> display-center, extrinsics_ok=%d, "
			        "offset=(%.1f,%.1f,%.1f) mm)",
			        (int)cnsdk->camera_extrinsics_ok,
			        cnsdk->camera_center_x_m * 1000.0f,
			        cnsdk->camera_center_y_m * 1000.0f,
			        cnsdk->camera_center_z_m * 1000.0f);
		}
	} else {
		return false;
	}

	// Display-center millimeters -> meters. NO camera-center subtraction here:
	// every source above is already display-center by construction (#152 L-c).
	float pos_x_m = position[0] / 1000.0f;
	float pos_y_m = position[1] / 1000.0f;
	float pos_z_m = position[2] / 1000.0f;

	// Re-express into the CURRENT held orientation + apply the calibration
	// overrides. Shared with the per-eye path so an eye pair and the face point
	// can never disagree about the frame they are in (#ROLL).
	int steps = 0;
	float pos[3] = {pos_x_m, pos_y_m, pos_z_m};
	orient_display_point(cnsdk, pos, &steps);
	pos_x_m = pos[0];
	pos_y_m = pos[1];
	pos_z_m = pos[2];

	static int oridbg = 0;
	if ((oridbg++ % 120) == 0) {
		U_LOG_W("HW_ORI: natural=%d current=%d steps=%d -> face=(%.3f,%.3f,%.3f)m",
		        cnsdk->natural_orientation.load(std::memory_order_relaxed),
		        (int)leia_core_get_orientation(cnsdk->core), steps, pos_x_m, pos_y_m, pos_z_m);
	}

	(void)used_nonpred;

	if (out_x != NULL) { *out_x = pos_x_m; }
	if (out_y != NULL) { *out_y = pos_y_m; }
	if (out_z != NULL) { *out_z = pos_z_m; }
	return true;
}

/*
 * #ROLL: per-EYE readout.
 *
 * leia_cnsdk_get_primary_face returns a POINT, so the DP had to synthesize the
 * eye pair as point ± (IPD/2, 0, 0) — a constant horizontal vector. That is
 * correct only for a perfectly upright head: roll the head and the true eyes
 * rotate about the face centre, so the rendered pair keeps the full horizontal
 * disparity (should be d·cosθ) and drops the vertical disparity entirely
 * (should be d·sinθ). The runtime's Kooima builds a genuine per-eye off-axis
 * frustum from whatever points it is handed, so the whole defect is here.
 *
 * Sources, in preference order — each returns the eye pair in display-center
 * millimetres, in the NATURAL orientation frame (the same contract as the face
 * accessors), and the caller orients both points together:
 *   1. leia_core_get_lookaround_eyes    — experimental; the pair the Unity /
 *      LeiaViewer path consumes (libleiaSDK-jni exports getLookaroundEyes over
 *      this entry point), i.e. the reference implementation of "correct".
 *   2. leia_core_get_non_predicted_eyes — experimental; the per-eye twin of the
 *      non-predicted face this file already prefers for look-around.
 *   3. listener eyePoints               — deprojected per-eye points from the
 *      frame listener, camera-space (lifted here exactly as the face is).
 *   4. listener face + poseAngle roll   — a point plus a roll angle: synthesize
 *      the pair, but ORIENT the IPD vector instead of forcing it horizontal.
 * Falls through to `false` if none is available, and the DP then keeps today's
 * fixed-horizontal behaviour.
 *
 * Sources 1–2 matter on hardware where the core's own accessors are live;
 * 3–4 matter on the NP02J, where LEIA_FACE_TRACKING_RUNTIME_IN_SERVICE leaves
 * the core's faces empty and the listener is the only source at all (see the
 * pred=0 note earlier in this file). Without 3–4 this fix would be a no-op on
 * exactly the device it was reported against.
 */
extern "C" bool
leia_cnsdk_use_lookaround_eyes(void)
{
	// Re-read periodically rather than latching at first use: this is the A/B
	// switch for the roll fix, and the whole point is to flip it against a LIVE
	// scene and watch the same head movement change. Latching it would force a
	// service restart per flip, which loses the side-by-side. Throttled to every
	// 30th call (~0.5 s at 60 Hz) so the property read stays off the hot path,
	// and only a CHANGE is logged.
	static std::atomic<int> cached{-1};
	static std::atomic<int> throttle{0};
	int v = cached.load(std::memory_order_acquire);
	if (v < 0 || (throttle.fetch_add(1, std::memory_order_relaxed) % 30) == 0) {
		const int fresh = get_prop_bool("debug.dxr.leia.lookaround_eyes", true) ? 1 : 0;
		if (fresh != v) {
			U_LOG_W("HW_EYES: per-eye look-around %s (debug.dxr.leia.lookaround_eyes)",
			        fresh ? "ENABLED" : "DISABLED — using the legacy fixed-horizontal pair");
			cached.store(fresh, std::memory_order_release);
		}
		v = fresh;
	}
	return v != 0;
}

extern "C" bool
leia_cnsdk_get_primary_eyes(struct leia_cnsdk *cnsdk, float out_left[3], float out_right[3])
{
	if (cnsdk == NULL || cnsdk->core == NULL || out_left == NULL || out_right == NULL ||
	    !cnsdk->face_tracking_started.load(std::memory_order_acquire)) {
		return false;
	}

	float l[3] = {0, 0, 0};
	float r[3] = {0, 0, 0};
	const char *src = NULL;
	// Tier 3's raw eyePoints[2] carry no documented L/R order (field-measured
	// mirrored on NP02J: roll=-176.6 deg with a level head = the pair swapped),
	// so that tier is re-ordered by display X after orientation. Not applied to
	// the measured lookaround pair, whose labeling is the SDK's contract — and
	// which a >90-degree roll would legitimately "invert".
	bool tier3_order_by_x = false;

	// --- 1 + 2: the experimental core accessors --------------------------------
	struct leia_float_slice ls = {l, 3};
	struct leia_float_slice rs = {r, 3};
	if (cnsdk->fn_lookaround_eyes != nullptr && cnsdk->fn_lookaround_eyes(cnsdk->core, ls, rs)) {
		src = "lookaround";
	} else if (cnsdk->fn_nonpred_eyes != nullptr && cnsdk->fn_nonpred_eyes(cnsdk->core, ls, rs)) {
		src = "non_predicted";
	}

	// Unit auto-detect. core.h documents neither mm nor metres for these two,
	// and guessing wrong is a 1000x IPD error — instantly visible but easy to
	// misdiagnose. Decide from the measured separation (a human IPD is ~50-75,
	// so mm reads ~65 and metres reads ~0.065). Everything below is millimetres.
	//
	// NOT decided from one frame. Measured on the NP02J, the reported separation
	// is noisy during acquisition — 6.5 and 30.5 were both observed on single
	// frames whose steady state is 61 — and a transient landing in the METRES
	// band (two momentarily-coincident eye points) would latch a 1000x error for
	// the whole session. So require several CONSECUTIVE frames to agree on the
	// same band before latching; a disagreeing sample resets the run. Until the
	// unit is settled the pair is refused and the caller falls through to the
	// next source, which is the honest state rather than a guessed scale.
	if (src != NULL) {
		const float sep = sqrtf((l[0] - r[0]) * (l[0] - r[0]) + (l[1] - r[1]) * (l[1] - r[1]) +
		                        (l[2] - r[2]) * (l[2] - r[2]));
		if (cnsdk->eyes_unit == 0 && std::isfinite(sep)) {
			const int band = (sep > 30.0f && sep < 100.0f)     ? 1   // mm
			                 : (sep > 0.030f && sep < 0.100f) ? 2   // meters
			                                                  : 0;
			if (band != 0 && band == cnsdk->eyes_unit_candidate) {
				if (++cnsdk->eyes_unit_run >= 10) {
					cnsdk->eyes_unit = band;
					U_LOG_W("HW_EYES: %s eyes report in %s (separation %.4f, "
					        "%d consecutive agreeing samples) — latched",
					        src, band == 1 ? "MILLIMETRES" : "METRES", sep,
					        cnsdk->eyes_unit_run);
				}
			} else {
				cnsdk->eyes_unit_candidate = band;
				cnsdk->eyes_unit_run = band != 0 ? 1 : 0;
			}
		}
		if (cnsdk->eyes_unit == 0) {
			static int unitrej = 0;
			if ((unitrej++ % 120) == 0) {
				U_LOG_W("HW_EYES: %s eyes separation %.4f — units not settled yet, "
				        "falling through to the next source",
				        src, sep);
			}
			src = NULL;
		} else {
			if (cnsdk->eyes_unit == 2) {
				for (int i = 0; i < 3; i++) { l[i] *= 1000.0f; r[i] *= 1000.0f; }
			}
			// Per-frame gate, applied AFTER the unit is known. The same
			// acquisition noise that made single-sample latching unsafe can
			// still emit an occasional degenerate pair mid-session; serving one
			// collapses the stereo for a frame. Reject it and let the next
			// source answer instead.
			const float sep_mm = cnsdk->eyes_unit == 2 ? sep * 1000.0f : sep;
			if (!(sep_mm > 30.0f && sep_mm < 100.0f)) {
				static int seprej = 0;
				if ((seprej++ % 120) == 0) {
					U_LOG_W("HW_EYES: %s eyes transient separation %.1f mm — rejecting "
					        "this frame",
					        src, sep_mm);
				}
				src = NULL;
			} else {
				// Good frame: remember it for the holdover below.
				for (int i = 0; i < 3; i++) {
					cnsdk->eyes_held_l[i] = l[i];
					cnsdk->eyes_held_r[i] = r[i];
				}
				cnsdk->eyes_held_ns = os_monotonic_get_ns();
			}
		}
	}

	// --- 2b: holdover (#211). A bad or absent tier-1/2 frame within the
	// freshness window repeats the last good pair instead of demoting the
	// tier — a one-frame-old correct geometry beats a per-frame pop to the
	// horizontal-synthesized pair and back. Only active once the unit has
	// latched (there is no held pair before that, and the settling phase
	// falling through to the lower tiers is the designed behaviour).
	if (src == NULL && cnsdk->eyes_unit != 0 && cnsdk->eyes_held_ns != 0 &&
	    (os_monotonic_get_ns() - cnsdk->eyes_held_ns) <= 100 * 1000 * 1000) {
		for (int i = 0; i < 3; i++) {
			l[i] = cnsdk->eyes_held_l[i];
			r[i] = cnsdk->eyes_held_r[i];
		}
		src = "held";
	}

	// --- 3: the frame listener's deprojected eye points ------------------------
	// Camera space -> display-center, the SAME lift the face fallback applies
	// (flip the image-down Y, then translate by the camera's own offset). Kept
	// bit-identical to that path on purpose: any divergence would show up as the
	// eye pair and the face centre disagreeing, which is worse than either.
	if (src == NULL && cnsdk->listener_face_valid.load(std::memory_order_acquire) &&
	    cnsdk->listener_eyes_valid.load(std::memory_order_relaxed)) {
		l[0] = cnsdk->listener_eye_l_x_mm.load(std::memory_order_relaxed) +
		       cnsdk->camera_center_x_m * 1000.0f;
		l[1] = -cnsdk->listener_eye_l_y_mm.load(std::memory_order_relaxed) +
		       cnsdk->camera_center_y_m * 1000.0f;
		l[2] = cnsdk->listener_eye_l_z_mm.load(std::memory_order_relaxed) +
		       cnsdk->camera_center_z_m * 1000.0f;
		r[0] = cnsdk->listener_eye_r_x_mm.load(std::memory_order_relaxed) +
		       cnsdk->camera_center_x_m * 1000.0f;
		r[1] = -cnsdk->listener_eye_r_y_mm.load(std::memory_order_relaxed) +
		       cnsdk->camera_center_y_m * 1000.0f;
		r[2] = cnsdk->listener_eye_r_z_mm.load(std::memory_order_relaxed) +
		       cnsdk->camera_center_z_m * 1000.0f;
		src = "listener_eyepoints";
		tier3_order_by_x = true;
	}

	// --- 4: listener face point + head roll ------------------------------------
	// Not a real eye measurement, but it carries the one thing the current code
	// throws away. Orient a nominal IPD vector by the reported roll instead of
	// pinning it to +X.
	//
	// Sign: poseAngle is left-handed and camera-space, and we flip the camera's
	// image-down Y into display Y-up above — a handedness flip about the view
	// axis, which negates the roll. debug.dxr.leia.roll_sign inverts it again if
	// a device disagrees, and the applied angle is logged so it can be read off
	// rather than guessed at.
	// The synthetic OFFSET must NOT be rotated by orient_display_point: the
	// detector's poseAngle.z is already display-relative (measured on NP02J:
	// a level head in LANDSCAPE reports poseAngle.z ~ 0, while the camera
	// frame is portrait-natural), so constructing the offset in camera space
	// and then orienting the pair applied the display rotation TWICE —
	// field-measured as roll=105.5 deg (= 15.5 applied + 90 landscape) on a
	// level head. Only the face CENTRE is a camera-space point; the pair is
	// therefore assembled AFTER orientation: centre oriented like every other
	// camera point, offset applied directly in display space.
	float tier4_dx_mm = 0.0f, tier4_dy_mm = 0.0f;
	bool tier4_display_offset = false;
	if (src == NULL && cnsdk->listener_face_valid.load(std::memory_order_acquire) &&
	    cnsdk->listener_roll_valid.load(std::memory_order_relaxed)) {
		const float fx = cnsdk->listener_face_x_mm.load(std::memory_order_relaxed) +
		                 cnsdk->camera_center_x_m * 1000.0f;
		const float fy = -cnsdk->listener_face_y_mm.load(std::memory_order_relaxed) +
		                 cnsdk->camera_center_y_m * 1000.0f;
		const float fz = cnsdk->listener_face_z_mm.load(std::memory_order_relaxed) +
		                 cnsdk->camera_center_z_m * 1000.0f;
		const float sign = prop_override("debug.dxr.leia.roll_sign", false) ? 1.0f : -1.0f;
		const float roll = sign * cnsdk->listener_roll_rad.load(std::memory_order_relaxed);
		const float half_ipd_mm = 32.5f;
		tier4_dx_mm = half_ipd_mm * cosf(roll);
		tier4_dy_mm = half_ipd_mm * sinf(roll);
		tier4_display_offset = true;
		// Both slots carry the CENTRE; the offset lands after orientation.
		l[0] = fx; l[1] = fy; l[2] = fz;
		r[0] = fx; r[1] = fy; r[2] = fz;
		src = "listener_roll";

		static int rolldbg = 0;
		if ((rolldbg++ % 120) == 0) {
			U_LOG_W("HW_EYES: synthesized from face + roll %.1f deg (sign %+.0f, display-space offset)",
			        roll * 57.2958f, sign);
		}
	}

	if (src == NULL) {
		return false;
	}

	// mm -> m, then orient BOTH points through the identical map. Rotating the
	// two points (rather than a centre plus an offset) is what carries the eye
	// vector correctly through a device rotation.
	for (int i = 0; i < 3; i++) {
		out_left[i] = l[i] / 1000.0f;
		out_right[i] = r[i] / 1000.0f;
	}
	orient_display_point(cnsdk, out_left, NULL);
	orient_display_point(cnsdk, out_right, NULL);
	if (tier3_order_by_x && out_right[0] < out_left[0]) {
		for (int i = 0; i < 3; i++) {
			const float t = out_left[i];
			out_left[i] = out_right[i];
			out_right[i] = t;
		}
	}
	if (tier4_display_offset) {
		out_left[0] -= tier4_dx_mm / 1000.0f;
		out_left[1] -= tier4_dy_mm / 1000.0f;
		out_right[0] += tier4_dx_mm / 1000.0f;
		out_right[1] += tier4_dy_mm / 1000.0f;
	}

	// L/R assignment is CNSDK's, and "left" could mean the viewer's left eye or
	// the left one as the camera sees it — a mirror flip that would invert the
	// stereo. The runtime's convention is eyes[0] = viewer's left = more negative
	// display X, so assert that here and let a device that disagrees be fixed
	// without a rebuild.
	if (prop_override("debug.dxr.leia.eyes_swap_lr", false)) {
		for (int i = 0; i < 3; i++) {
			const float t = out_left[i];
			out_left[i] = out_right[i];
			out_right[i] = t;
		}
	}

	static int eyedbg = 0;
	if ((eyedbg++ % 120) == 0) {
		const float dx = out_right[0] - out_left[0];
		const float dy = out_right[1] - out_left[1];
		U_LOG_W("HW_EYES: src=%s L=(%.3f,%.3f,%.3f) R=(%.3f,%.3f,%.3f) roll=%.1f deg", src,
		        out_left[0], out_left[1], out_left[2], out_right[0], out_right[1], out_right[2],
		        atan2f(dy, dx) * 57.2958f);
	}
	return true;
}

// Live 2D/3D A-B toggle: `adb shell setprop debug.dxr.leia.backlight 0|1`.
//   1 (default) → 3D light-field backlight ON.
//   0           → flat 2D (backlight off; the SAME weaved image is shown, so
//                 the user can compare 2D vs 3D and confirm the weave works).
// Re-read every ~30 frames; only call CNSDK when the value actually changes.
static void
apply_backlight_toggle(struct leia_cnsdk *cnsdk)
{
	if (cnsdk == NULL || cnsdk->core == NULL) {
		return;
	}
	if ((cnsdk->backlight_throttle++ % 30) != 0) {
		return;
	}
	int want = get_prop_bool("debug.dxr.leia.backlight", true) ? 1 : 0;

	// #558 overlay mode: the avatar runs as a backgrounded system overlay over the
	// 2D launcher, so face tracking is lost — MANAGED NoFaceMode would drop the
	// WEAVE to flat 2D (lens may be on but the tiger looks flat) and on_pause would
	// release the lens preference. Force the light-field 3D + NoFaceMode OFF every
	// weave so the tiger stays 3D regardless of face/foreground state. Gated on
	// overlay mode — per-session, from the runtime (the app's manifest flag).
	if (overlay_mode_active()) {
		want = 1;
		leia_core_enable_no_face_mode(cnsdk->core, false);
	}

	// Compare against the instance's APPLIED state: pause/destroy release the
	// preference out-of-band, so a weave after a resume re-asserts it.
	if (want != cnsdk->backlight_applied.load(std::memory_order_acquire)) {
		leia_core_enable_3d(cnsdk->core, want != 0);
		cnsdk->backlight_applied.store(want, std::memory_order_release);
		U_LOG_W("HW_DBG_CNSDK: backlight -> %s (weave)", want ? "3D ON" : "2D OFF");
	}
}

/*!
 * RELEASE this instance's lens preference (runtime #1039). Called from on_pause
 * (window not visible / session end) and destroy (client teardown).
 *
 * ## Why this is a release and not a "force the panel to 2D"
 *
 * The switchable 3D lens is display-global, but on this hardware it is arbitrated
 * by `BacklightMultiClientControlService` as a binder BIND-REFCOUNT: `onBind` /
 * `onRebind` request `MODE_3D`, `onUnbind` — which Android delivers only when the
 * LAST client has disconnected — requests `MODE_2D`. So the service is the OR of
 * every client's vote.
 *
 * `leia_core_enable_3d(false)` on that tier is a pure UNBIND. CNSDK
 * `device::SetBacklightMode` (androidDevice.cpp) takes the multi-client branch
 * first and RETURNS from it — `BaseServiceConnection::Connect(env, false)` →
 * Java `connect(false)` → `context.unbindService(this)`. It never reaches the
 * legacy `BacklightControlService::requestBacklightMode("MODE_2D")` nor
 * `LeiaManagerUtility::SetBacklightMode(false)`, both of which WOULD command the
 * panel globally. (#151's premise that enable_3d(false) "does both at once" was
 * wrong; the `setBacklightMode:false` seen in its trace came from the SERVICE
 * reacting to its refcount hitting zero, not from this process.)
 *
 * That makes one call correct in both directions:
 *   - multi-window: a hidden window drops to refcount N-1, the panel stays 3D
 *     for the sibling that is still weaving;
 *   - last app out: refcount 0, the service itself flattens the panel — which is
 *     stuck-3D-after-close (runtime #563), fixed for free.
 *
 * VENDOR GAP: on a device with no multi-client service, CNSDK falls back to the
 * legacy service / `LeiaManagerUtility`, and there the same call DOES force a
 * global `MODE_2D`. There is no "unbind only" on those tiers, so a release
 * degrades to a global command. Tracked as limitations L2/L3 in the runtime's
 * `docs/roadmap/android-concurrent-multi-app.md` (runtime #1038); multi-window
 * weaving is only claimed on the multi-client tier.
 */
static void
release_lens_preference(struct leia_cnsdk *cnsdk, const char *reason)
{
	if (cnsdk == NULL || cnsdk->core == NULL || !leia_core_is_initialized(cnsdk->core)) {
		return;
	}
	if (cnsdk->backlight_applied.load(std::memory_order_acquire) == 0) {
		return; // already released
	}
	leia_core_enable_3d(cnsdk->core, false);
	cnsdk->backlight_applied.store(0, std::memory_order_release);
	U_LOG_W("HW_DBG_CNSDK: lens preference RELEASED (unbind, refcount--) (%s)", reason);
}

/*!
 * Re-ASSERT this instance's lens preference — the counterpart of
 * @ref release_lens_preference. On the multi-client tier this is a re-bind
 * (refcount++), and the service switches the panel to 3D on the first vote.
 */
static void
assert_lens_preference(struct leia_cnsdk *cnsdk, const char *reason)
{
	if (cnsdk == NULL || cnsdk->core == NULL || !leia_core_is_initialized(cnsdk->core)) {
		return;
	}
	if (cnsdk->backlight_applied.load(std::memory_order_acquire) == 1) {
		return; // already asserted
	}
	leia_core_enable_3d(cnsdk->core, true);
	cnsdk->backlight_applied.store(1, std::memory_order_release);
	U_LOG_W("HW_DBG_CNSDK: lens preference ASSERTED (bind, refcount++) (%s)", reason);
}

extern "C" void
leia_cnsdk_set_window_screen_rect(
    struct leia_cnsdk *cnsdk, int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t display_id)
{
	if (cnsdk == NULL) {
		return;
	}
	const bool had = cnsdk->have_window_rect.load(std::memory_order_acquire);
	if (had && cnsdk->window_screen_x.load(std::memory_order_relaxed) == x &&
	    cnsdk->window_screen_y.load(std::memory_order_relaxed) == y &&
	    cnsdk->window_screen_w.load(std::memory_order_relaxed) == (int32_t)w &&
	    cnsdk->window_screen_h.load(std::memory_order_relaxed) == (int32_t)h &&
	    cnsdk->window_display_id.load(std::memory_order_relaxed) == display_id) {
		return; // unchanged — skip the vendor call entirely
	}
	cnsdk->window_screen_x.store(x, std::memory_order_relaxed);
	cnsdk->window_screen_y.store(y, std::memory_order_relaxed);
	cnsdk->window_screen_w.store((int32_t)w, std::memory_order_relaxed);
	cnsdk->window_screen_h.store((int32_t)h, std::memory_order_relaxed);
	cnsdk->window_display_id.store(display_id, std::memory_order_relaxed);
	cnsdk->have_window_rect.store(true, std::memory_order_release);
	// Lifecycle event (a window moved or resized), never per frame.
	U_LOG_W("HW_DBG_CNSDK: window screen rect %d,%d %ux%u display %d (#150/#1033)", x, y, w, h, display_id);
}

extern "C" void
leia_cnsdk_set_panel_size(struct leia_cnsdk *cnsdk, uint32_t panel_w, uint32_t panel_h, int32_t display_id)
{
	if (cnsdk == NULL || panel_w == 0 || panel_h == 0) {
		return;
	}
	(void)display_id;
	const uint32_t pw_old = cnsdk->panel_now_w.exchange(panel_w, std::memory_order_relaxed);
	const uint32_t ph_old = cnsdk->panel_now_h.exchange(panel_h, std::memory_order_relaxed);
	if (pw_old != panel_w || ph_old != panel_h) {
		// Lifecycle event (first report, or a rotation), never per frame.
		U_LOG_W("HW_DBG_CNSDK: panel size (current orientation) %ux%u", panel_w, panel_h);
	}
}

extern "C" void
leia_cnsdk_set_predicted_scanout(struct leia_cnsdk *cnsdk, uint64_t weave_to_scanout_ns)
{
	if (cnsdk == NULL) {
		return;
	}
	// Stored raw. The conversion to CNSDK's absolute monotonic target happens
	// at the weave (see leia_cnsdk_weave), not here.
	cnsdk->weave_to_scanout_ns.store(weave_to_scanout_ns, std::memory_order_relaxed);
}

extern "C" bool
leia_cnsdk_weave(struct leia_cnsdk *cnsdk,
                 VkDevice device,
                 VkPhysicalDevice physDev,
                 VkImage atlas_image,
                 VkImageView atlas_view,
                 uint32_t atlas_width,
                 uint32_t atlas_height,
                 VkFormat targetFmt,
                 uint32_t w,
                 uint32_t h,
                 VkFramebuffer fb,
                 VkImage targetImage,
                 int32_t vp_x,
                 int32_t vp_y,
                 uint32_t vp_w,
                 uint32_t vp_h,
                 VkSemaphore wait_sem,
                 VkSemaphore signal_sem)
{
	(void)device; (void)physDev; (void)targetFmt;

	if (cnsdk == NULL || cnsdk->interlacer == NULL) {
		return false;
	}

	// Live 2D/3D A-B toggle for verifying the weave on the panel.
	apply_backlight_toggle(cnsdk);

	DXR_ATRACE("dxr_cnsdk:weave");
	// Calibration knob (B18): flip_input_uv_vertical defaults to true
	// because we assume CNSDK uses GL convention (Y-up) and Vulkan NDC
	// is Y-down. If Lume Pad shows text upside-down, flip via
	// `adb shell setprop debug.dxr.leia.flip_uv 0`.
	ensure_calibration_loaded();
	leia_interlacer_set_flip_input_uv_vertical(cnsdk->interlacer, g_calib.flip_uv);

	// Atlas mode: hand CNSDK the SBS atlas VkImage+View each frame; it
	// splits internally per the 2x1 layout set in ensure_interlacer.
	// CNSDK 0.10.x renamed set_interlace_view_texture_atlas →
	// set_source_views(texture, view, viewIndex, layer); with atlas-for-views
	// enabled, viewIndex 0 / layer 0 designates the whole atlas.
	// (set_shader_debug_mode was removed in 0.10.x.)
	leia_interlacer_vulkan_set_source_views(
	    cnsdk->interlacer, atlas_image, atlas_view, /*viewIndex=*/0, /*layer=*/0);
	leia_interlacer_set_source_views_size(
	    cnsdk->interlacer, (int32_t)atlas_width, (int32_t)atlas_height,
	    /*isHorizontalViews=*/true);

	// XR_DXR_display_zones (#568): confine the interlace to the canvas sub-rect
	// (e.g. the avatar's bottom-75% band). CNSDK splits placement and phase into
	// TWO decoupled knobs:
	//   * set_viewport(posX,posY,w,h)         — where the output lands in the
	//                                            render-target (pixel placement).
	//   * set_viewport_screen_position(x,y)   — the origin ON THE SCREEN that the
	//                                            lenticular interlace PHASE is
	//                                            referenced to (default 0,0).
	// The render-target viewport alone does NOT re-reference the phase, so a band
	// drawn 25% down weaves at the full-panel phase → the 3D registers shifted
	// (#53). Set BOTH: the viewport for placement, the screen position so the
	// phase tracks the band's panel origin. This is the CNSDK analog of the
	// desktop SR SDK auto-folding vpX/vpY from the single D3D viewport. Set both
	// explicitly each frame (band OR full target) so a prior frame's offset never
	// leaks. A/B: `setprop debug.dxr.leia.zonephase 0` reverts to the (shifted)
	// pre-#53 phase for on-device comparison; 1 (default) applies the fix.
	const bool zonephase = prop_override("debug.dxr.leia.zonephase", true);
	// Per-window base origin (#150 / runtime#1033): where THIS window sits on the
	// panel, in current-orientation screen pixels. Zone rects are relative to the
	// render target, i.e. to the window — so the screen position the phase is
	// referenced to is window origin + zone offset. Until the runtime reports a
	// rect (older runtime, single full-screen window) the base is (0,0) and every
	// line below is bit-identical to the pre-#150 behaviour.
	//
	// NOTE this is why the old "reset to (0,0) on a full-target frame" is gone: a
	// full-target frame in a MOVED window is exactly the case that needs a real
	// origin, and zeroing it there is what collapsed the 3D in the right-hand
	// window of a side-by-side pair.
	const bool have_win = cnsdk->have_window_rect.load(std::memory_order_acquire);
	const int32_t win_x = have_win ? cnsdk->window_screen_x.load(std::memory_order_relaxed) : 0;
	const int32_t win_y = have_win ? cnsdk->window_screen_y.load(std::memory_order_relaxed) : 0;
	if (vp_w > 0u && vp_h > 0u) {
		leia_interlacer_set_viewport(cnsdk->interlacer, vp_x, vp_y,
		                             (int32_t)vp_w, (int32_t)vp_h);
		const int32_t sp_x = win_x + (zonephase ? vp_x : 0);
		const int32_t sp_y = win_y + (zonephase ? vp_y : 0);
		leia_interlacer_set_viewport_screen_position(cnsdk->interlacer, sp_x, sp_y);
		/*
		 * #206: publish the ABSOLUTE predicted scanout time for THIS weave.
		 *
		 * The runtime gives us a DURATION (the measured weave->scanout
		 * residual). CNSDK wants an absolute CLOCK_MONOTONIC target, and
		 * computes `delay = target - now` itself at the moment it fetches the
		 * face. So the conversion must happen HERE, at the weave — not when
		 * the runtime handed the duration over. Any latency between the two
		 * would be subtracted from the horizon, and a shortened horizon looks
		 * like a BETTER measurement while making prediction worse: the same
		 * failure shape as predicting 40 ms ahead of a 29 ms reality.
		 *
		 * Delta form also means we need not reconcile clock domains: CNSDK's
		 * face timestamps are CLOCK_BOOTTIME (~2h49m from CLOCK_MONOTONIC on
		 * this pad), but the offset cancels in its own subtraction, so we
		 * publish plain monotonic.
		 *
		 * 0 = the runtime has no trusted measurement; publish nothing and let
		 * CNSDK keep facePredictLatencyMs.
		 */
#if defined(leia_interlacer_set_predicted_scanout_ns_VERSION)
		if (cnsdk->fn_set_predicted_scanout != nullptr) {
			const uint64_t residual_ns =
			    cnsdk->weave_to_scanout_ns.load(std::memory_order_relaxed);
			if (residual_ns != 0) {
				const int64_t target_ns =
				    (int64_t)os_monotonic_get_ns() + (int64_t)residual_ns;
				cnsdk->fn_set_predicted_scanout(cnsdk->interlacer, target_ns);
				// One WARN per distinct horizon in ms — a lifecycle-rate
				// signal that the chain is live, never per frame.
				static int64_t s_last_ms = INT64_MIN;
				const int64_t ms = (int64_t)(residual_ns / 1000000ULL);
				if (ms != s_last_ms) {
					s_last_ms = ms;
					U_LOG_W("#206: publishing predicted scanout horizon %lld ms to CNSDK",
					        (long long)ms);
				}
			}
		}
#endif
		// #53 diagnostic: re-log whenever the band/phase changes (covers the live
		// `debug.dxr.leia.zonephase` A/B toggle) — one WARN per distinct state.
		static int32_t last_vpx = INT32_MIN, last_vpy = INT32_MIN;
		static int32_t last_spx = INT32_MIN, last_spy = INT32_MIN;
		if (vp_x != last_vpx || vp_y != last_vpy || sp_x != last_spx || sp_y != last_spy) {
			last_vpx = vp_x; last_vpy = vp_y; last_spx = sp_x; last_spy = sp_y;
			U_LOG_W("HW_DBG_CNSDK: weave band %d,%d %ux%u screen-pos %d,%d (zonephase=%d) target %ux%u (#53)",
			        vp_x, vp_y, vp_w, vp_h, sp_x, sp_y, (int)zonephase, w, h);
		}
	} else {
		// browser#165: CNSDK converts the viewport screen position to GL
		// bottom-origin panel coords (interlacer.cpp: origin_gl_y = panel_h -
		// vp_h - y), but this API is documented to take Android top-left screen
		// pixels. For a NATURAL-orientation (portrait) window that does not span
		// the panel, passing the top-anchored y therefore lands the interlace
		// pattern with a constant (panel_h - vp_h)-row phase offset — a uniform,
		// head-position-independent double image (LPD-20W) or inverted views
		// (NP02J), and no error at fullscreen where the offset is zero.
		// Pre-apply the inverse so the pattern anchors where the calibration
		// did. Landscape (rotated-from-natural) windows go through CNSDK's
		// rotation compensation and weave correctly with the top-anchored y —
		// verified on device — so only portrait is corrected.
		// browser#173: the conversion is NOT portrait-specific. CNSDK derives
		// origin_gl_y = panel_h - vp_h - y in whatever orientation is current, so
		// ANY window shorter than the panel's height IN THE CURRENT ORIENTATION
		// needs the inverse pre-applied — landscape included. The old
		// `if (h > w)` guard only ran in portrait, and its "landscape verified on
		// device" note was misleading: every landscape case tested was FULLSCREEN,
		// where the offset is identically zero, so the guard was never exercised.
		// On a landscape tablet with a 60px status bar (NP02J: 2560x1540 surface
		// on a 2560x1600 panel) the phase landed 60 rows off — soft, crosstalky
		// 3D that looked "almost right". Use the panel dimension along the
		// window's height axis: portrait -> long side, landscape -> short side.
		int32_t sp_y = win_y;
		// browser#128: a NONZERO reported y is a truthful origin from a client
		// that measures its own surface (the browser's Java geometry feed,
		// View.getLocationOnScreen on the compositor SurfaceView). CNSDK's
		// bottom-origin conversion then needs NO pre-applied inverse — the
		// top-anchored y is exactly what the API documents, so sp_y = win_y.
		// The inverse below exists ONLY to compensate clients that report 0
		// while actually sitting below a top inset (#165/#173 — two bugs
		// cancelling; measured on NP02J: truthful y=60 with the inverse still
		// applied anchors 60 rows off, bottom-origin 0 where 60 is correct).
		// A truthful y=0 window whose height fills the panel is unaffected
		// either way (the h < panel guard below), and a truthful y=0 window
		// with only a BOTTOM inset keeps the old compensation — wrong there,
		// but that case was equally wrong before this change, and
		// debug.dxr.leia.origin_compat=0 force-trusts the report for a stack
		// known to send truthful origins.
		const bool trust_origin =
		    (win_y != 0) || !prop_override("debug.dxr.leia.origin_compat", true);
		if (trust_origin) {
			static int32_t trust_last = INT32_MIN;
			if (win_y != trust_last) {
				trust_last = win_y;
				U_LOG_W("HW_DBG_CNSDK: #128 trusting reported origin y=%d (no inverse)", win_y);
			}
		} else {
			uint32_t pw = 0, ph = 0;
			if (leia_cnsdk_get_display_metrics(cnsdk, NULL, NULL, &pw, &ph) && pw != 0 && ph != 0) {
				const uint32_t panel_long = pw > ph ? pw : ph;
				const uint32_t panel_short = pw > ph ? ph : pw;
				/*
				 * #173 follow-up: "which panel dimension is the height" is a
				 * property of the PANEL's current orientation, NOT of the
				 * window's aspect. Testing `h > w` asks the window, so a
				 * portrait-SHAPED window on a landscape panel (NP02J: a
				 * 1200x1600 window on a 2560x1600 panel) picked panel_long
				 * 2560 and produced sp_y = 2560-1600-0 = 960 where the correct
				 * value is 1600-1600-0 = 0. A 960-row vertical offset on a
				 * SLANTED lenticular is a horizontal phase error, and it landed
				 * on half a period: exactly inverted eyes. Fullscreen apps
				 * (2560x1600, w > h) classified correctly and were unaffected,
				 * which is why only windowed apps showed it.
				 */
				/*
				 * CNSDK's metrics are ORIENTATION-INDEPENDENT (it reports the
				 * native portrait panel, 1600x2560 on NP02J, even when the
				 * device is in landscape), so neither pw/ph nor the window's
				 * aspect can tell us which dimension is "height" right now.
				 *
				 * The window's own screen rect can: it must FIT the panel. A
				 * window whose right edge exceeds panel_short cannot be on a
				 * portrait panel, so the panel is landscape (and vice versa).
				 * Avatar on NP02J is the case that exposed this: x=680 w=1200
				 * -> right edge 1880 > 1600, so landscape, panel_h_now = 1600,
				 * sp_y = 1600-1600-0 = 0. The window-aspect test said
				 * "portrait" (1600 > 1200) and produced 960 instead.
				 */
				const uint32_t rt_panel_h = cnsdk->panel_now_h.load(std::memory_order_relaxed);
				uint32_t panel_h_now;
				if (rt_panel_h != 0) {
					// Ground truth: the runtime reports the panel in the
					// display's CURRENT orientation. Always prefer it.
					panel_h_now = rt_panel_h;
				} else if (win_x + (int32_t)w > (int32_t)panel_short) {
					panel_h_now = panel_short; // too wide to be a portrait panel
				} else if (win_y + (int32_t)h > (int32_t)panel_short) {
					panel_h_now = panel_long; // too tall to be a landscape panel
				} else {
					// Older runtime with no panel-size slot AND a window small
					// enough to fit either orientation. Nothing here can
					// disambiguate, so keep the historical behaviour rather
					// than invent a new guess.
					panel_h_now = (h > w) ? panel_long : panel_short;
				}
				static bool pm_logged = false;
				if (!pm_logged) {
					pm_logged = true;
					U_LOG_W("HW_DBG_CNSDK: #205fix cnsdk panel %ux%u (orientation-blind) win %ux%u at "
					        "%d,%d -> panel_h_now %u (source: %s)",
					        pw, ph, w, h, win_x, win_y, panel_h_now,
					        rt_panel_h != 0 ? "runtime panel size" : "window-fit inference");
				}
				if (h < panel_h_now) {
					sp_y = (int32_t)panel_h_now - (int32_t)h - win_y;
					static int32_t sp_last = INT32_MIN;
					if (sp_y != sp_last) {
						sp_last = sp_y;
						U_LOG_W("HW_DBG_CNSDK: #165/#173 %s window y %d -> bottom-origin %d "
						        "(panel_h_now %u, vp_h %u)",
						        (h > w) ? "portrait" : "landscape", win_y, sp_y,
						        panel_h_now, h);
					}
				}
			}
		}
		leia_interlacer_set_viewport(cnsdk->interlacer, 0, 0, (int32_t)w, (int32_t)h);
		leia_interlacer_set_viewport_screen_position(cnsdk->interlacer, win_x, sp_y);
		// One WARN per distinct window phase — the line the side-by-side PoC greps
		// to prove each satellite weaves at its own origin. Never per frame.
		static int32_t last_wx = INT32_MIN, last_wy = INT32_MIN;
		if (win_x != last_wx || win_y != last_wy) {
			last_wx = win_x; last_wy = win_y;
			U_LOG_W("HW_DBG_CNSDK: weave full-target screen-pos %d,%d target %ux%u (#150)",
			        win_x, win_y, w, h);
		}
	}

	DXR_HW_DBG_ONCE("weave: first do_post_process atlas=%ux%u target=%ux%u",
	                atlas_width, atlas_height, w, h);
	// runtime#1073 L11 — hand CNSDK the caller's GPU dependencies instead of
	// pre-synchronising on the CPU.
	//
	//   wait_sem   → `imageAvailableSemaphore`. CNSDK stores it and attaches it
	//                to the submit whose command buffer samples the source views
	//                (interlacer.cpp DoPostProcess → ApplyInterlacing →
	//                CRendererVulkan::Apply2DEffect, applied at `currentPass ==
	//                0`). Our single-layer atlas setup takes exactly that path:
	//                `mLayerCount == 1` skips layer compositing, so the interlace
	//                pass IS pass 0 and the wait is honoured on the submit that
	//                reads the atlas. Verified against CNSDK main.
	//   signal_sem → `renderFinishedSemaphore`, signalled by the final pass
	//                (interlace when the debug GUI is hidden, which is always
	//                here). Lets the post-weave alpha gate order itself after the
	//                weave without a device-wide idle.
	//
	// Both are optional: NULL keeps the pre-L11 "caller already synchronised"
	// behaviour, which is what the fallback paths still use.
	leia_interlacer_vulkan_do_post_process(
	    cnsdk->interlacer, w, h, false, fb, targetImage, NULL,
	    wait_sem, signal_sem, 0);
	return true;
}
