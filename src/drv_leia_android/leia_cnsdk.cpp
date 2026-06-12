// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CNSDK wrapper implementation — isolates CNSDK headers
 *         from the rest of the compositor.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_cnsdk.h"

#include "util/u_logging.h"

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

	float camera_center_x_m{0.0f};
	float camera_center_y_m{0.0f};
	float camera_center_z_m{0.0f};

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
};

static void
force_backlight_2d(struct leia_cnsdk *cnsdk, const char *reason);


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
	g_calib_loaded.store(true, std::memory_order_release);
	U_LOG_W("HW_DBG_CNSDK calibration: flip_uv=%d face_flip_xyz=%d%d%d face_swap_xy=%d",
	        (int)g_calib.flip_uv,
	        (int)g_calib.face_flip_x, (int)g_calib.face_flip_y, (int)g_calib.face_flip_z,
	        (int)g_calib.face_swap_xy);
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

// Frame listener callback — invoked on a CNSDK background thread for every
// head-tracking frame the service delivers (the HTS-Binder onMessage(Frame)
// stream). Cache the primary face's Kalman-filtered point so the render thread
// can read it. Must release the frame (ownership transferred in).
void
on_headtracking_frame(struct leia_headtracking_frame *frame, void *userData)
{
	struct leia_cnsdk *cnsdk = static_cast<struct leia_cnsdk *>(userData);

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
	// getters instead of struct fields. Camera-center (used only to translate
	// our get_primary_face output, which feeds the app's per-eye cameras, NOT
	// the in-service weave steering) moved into a leia_camera struct; defer it
	// (left 0) — the weave is steered by CNSDK's own in-service face data.
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
	android_load.context = (jobject)activity;  // Activity is-a android.content.Context
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
	// Panel back to flat 2D FIRST, while the core is healthiest — the
	// switchable backlight is system-global and outlives this process
	// (stuck-3D-after-close). Even if the worker join below times out,
	// this call has already gone out to the display service.
	force_backlight_2d(cnsdk, "destroy");

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
	// Drop the panel to flat 2D before pausing — the backlight is
	// system-global and the home screen / picker behind us is 2D content.
	// The weave loop re-enables 3D on the first frame after resume.
	force_backlight_2d(cnsdk, "on_pause");
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
	const bool lst_ok = cnsdk->listener_face_valid.load(std::memory_order_acquire);
	float lst_pos[3] = {
	    cnsdk->listener_face_x_mm.load(std::memory_order_relaxed),
	    cnsdk->listener_face_y_mm.load(std::memory_order_relaxed),
	    cnsdk->listener_face_z_mm.load(std::memory_order_relaxed),
	};

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
	float position[3];
	bool used_nonpred = false;
	if (np_ok) {
		position[0] = np_pos[0]; position[1] = np_pos[1]; position[2] = np_pos[2];
		used_nonpred = true;
	} else if (pred_ok) {
		position[0] = pred_pos[0]; position[1] = pred_pos[1]; position[2] = pred_pos[2];
	} else if (lst_ok) {
		position[0] = lst_pos[0]; position[1] = lst_pos[1]; position[2] = lst_pos[2];
	} else {
		return false;
	}

	// CNSDK returns millimeters relative to the camera. xrt_eye_position
	// wants meters relative to the display center, so divide by 1000 then
	// subtract the cached camera center (also already in meters).
	float pos_x_m = position[0] / 1000.0f - cnsdk->camera_center_x_m;
	float pos_y_m = position[1] / 1000.0f - cnsdk->camera_center_y_m;
	float pos_z_m = position[2] / 1000.0f - cnsdk->camera_center_z_m;

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

	// Compare against the instance's APPLIED state: pause/destroy force it
	// to 2D out-of-band, so the first weave after a resume re-enables 3D.
	if (want != cnsdk->backlight_applied.load(std::memory_order_acquire)) {
		leia_core_enable_3d(cnsdk->core, want != 0);
		cnsdk->backlight_applied.store(want, std::memory_order_release);
		U_LOG_W("HW_DBG_CNSDK: backlight -> %s (weave)", want ? "3D ON" : "2D OFF");
	}
}

/*!
 * Force the panel back to flat 2D. The switchable backlight is system-global
 * state held by the Leia display service — it stays 3D forever if the last
 * weaving client goes away without this (stuck-3D-after-close). Called from
 * on_pause (session end / backgrounding) and destroy (client teardown).
 */
static void
force_backlight_2d(struct leia_cnsdk *cnsdk, const char *reason)
{
	if (cnsdk == NULL || cnsdk->core == NULL || !leia_core_is_initialized(cnsdk->core)) {
		return;
	}
	if (cnsdk->backlight_applied.load(std::memory_order_acquire) == 0) {
		return; // already 2D
	}
	leia_core_enable_3d(cnsdk->core, false);
	cnsdk->backlight_applied.store(0, std::memory_order_release);
	U_LOG_W("HW_DBG_CNSDK: backlight -> 2D OFF (%s)", reason);
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
                 VkImage targetImage)
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

	DXR_HW_DBG_ONCE("weave: first do_post_process atlas=%ux%u target=%ux%u",
	                atlas_width, atlas_height, w, h);
	leia_interlacer_vulkan_do_post_process(
	    cnsdk->interlacer, w, h, false, fb, targetImage, NULL,
	    NULL, NULL, 0);
}
