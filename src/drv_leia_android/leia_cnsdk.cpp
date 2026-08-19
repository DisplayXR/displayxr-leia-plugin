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
#include <leia/core/faceTracking.h>
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
	struct leia_headtracking_detected_faces detected = {};
	if (cnsdk != nullptr &&
	    leia_headtracking_frame_get_detected_faces(frame, &detected) ==
	        kLeiaHeadTrackingStatusSuccess &&
	    detected.numFaces > 0) {
		px = detected.faces[0].posePosition.x;
		py = detected.faces[0].posePosition.y;
		pz = detected.faces[0].posePosition.z;
		got = true;
	} else if (cnsdk != nullptr) {
		struct leia_headtracking_tracking_result tracked = {};
		if (leia_headtracking_frame_get_tracking_result(frame, &tracked) ==
		        kLeiaHeadTrackingStatusSuccess &&
		    tracked.num_faces > 0) {
			px = tracked.faces[0].point.pos.x;
			py = tracked.faces[0].point.pos.y;
			pz = tracked.faces[0].point.pos.z;
			got = true;
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

	// The predicted/non-predicted faces arrive in the display's NATURAL
	// orientation frame (that's the frame CNSDK weaves in). Our look-around face
	// must be expressed in the CURRENT held orientation.
	//
	// GetRelativeClockwiseAngle(natural, current) is how far the DEVICE FRAME
	// actively turned from natural to current. But we're re-expressing a fixed
	// point's COORDINATES from the natural frame into the current frame, which is
	// the INVERSE (passive) rotation: when a frame turns clockwise by θ, a fixed
	// point's coords in it turn counter-clockwise by θ. So we rotate by
	// GetRelativeClockwiseAngle(current, natural) = (natural - current) steps,
	// each step 90°. z (depth) is orientation-invariant.
	const int natural = cnsdk->natural_orientation.load(std::memory_order_relaxed);
	const enum leia_orientation cur = leia_core_get_orientation(cnsdk->core);
	int steps = 0;
	if (natural >= 0 && (int)cur >= 0) {
		steps = (((natural - (int)cur) % 4) + 4) % 4; // inverse: (natural - current), × 90°
	}
	switch (steps) {
	case 1: { float t = pos_x_m; pos_x_m = -pos_y_m; pos_y_m = t; break; }  //  90°: (-y,  x)
	case 2: { pos_x_m = -pos_x_m; pos_y_m = -pos_y_m; break; }              // 180°: (-x, -y)
	case 3: { float t = pos_x_m; pos_x_m = pos_y_m; pos_y_m = -t; break; }  // 270°: ( y, -x)
	default: break;                                                        //   0°: ( x,  y)
	}

	// Residual per-device calibration overrides (all default OFF — the rotation
	// above is the principled mapping). Applied after the rotation.
	if (prop_override("debug.dxr.leia.face_swap_xy", false)) { float t = pos_x_m; pos_x_m = pos_y_m; pos_y_m = t; }
	if (prop_override("debug.dxr.leia.face_flip_x", false))  { pos_x_m = -pos_x_m; }
	if (prop_override("debug.dxr.leia.face_flip_y", false))  { pos_y_m = -pos_y_m; }
	if (prop_override("debug.dxr.leia.face_flip_z", false))  { pos_z_m = -pos_z_m; }

	static int oridbg = 0;
	if ((oridbg++ % 120) == 0) {
		U_LOG_W("HW_ORI: natural=%d current=%d steps=%d -> face=(%.3f,%.3f,%.3f)m",
		        natural, (int)cur, steps, pos_x_m, pos_y_m, pos_z_m);
	}

	(void)used_nonpred;

	if (out_x != NULL) { *out_x = pos_x_m; }
	if (out_y != NULL) { *out_y = pos_y_m; }
	if (out_z != NULL) { *out_z = pos_z_m; }
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
                 uint32_t vp_h)
{
	(void)device; (void)physDev; (void)targetFmt;

	if (cnsdk == NULL || cnsdk->interlacer == NULL) {
		return;
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
		leia_interlacer_set_viewport(cnsdk->interlacer, 0, 0, (int32_t)w, (int32_t)h);
		leia_interlacer_set_viewport_screen_position(cnsdk->interlacer, win_x, win_y);
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
	leia_interlacer_vulkan_do_post_process(
	    cnsdk->interlacer, w, h, false, fb, targetImage, NULL,
	    NULL, NULL, 0);
}
