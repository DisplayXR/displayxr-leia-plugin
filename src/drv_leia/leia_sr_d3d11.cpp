// Copyright 2024-2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Leia SR D3D11 weaver wrapper implementation.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_sr_d3d11.h"
#include "leia_sr_api_select.h"
#include "leia_sr_v2_common.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/weaver/dx11weaver.h>
#include <sr/world/display/display.h>
#include <sr/sense/display/switchablehint.h>
#include <sr/utility/exception.h>

#ifdef DXR_LEIA_HAS_SR_V2
#include <sr/sr_dx11.h>
#include <sr/sr_weaver.h>
#endif

#include <d3d11.h>

#include <windows.h>
#include <sysinfoapi.h>

#include <cmath>
#include <cstdlib>

/*!
 * D3D11 SR weaver instance.
 */
struct leiasr_d3d11
{
	// SR SDK objects, v1 (legacy C++). NULL on the v2 path.
	SR::SRContext *context = nullptr;
	SR::IDX11Weaver1 *weaver = nullptr;
	SR::SwitchableLensHint *lens_hint = nullptr;

#ifdef DXR_LEIA_HAS_SR_V2
	// SR SDK objects, v2 (C99). NULL on the v1 path.
	//
	// `weaver_v2 != nullptr` IS the discriminant used at every call site below.
	// The selector is consulted exactly once, in create(); after that "which
	// path am I on?" is a property of the object, not a re-query. That matters
	// because a selector that could be re-evaluated mid-session is a selector
	// that could answer differently mid-session, and a half-v1/half-v2 object
	// is the exact silent-divergence failure this migration exists to remove.
	SrInstance instance_v2 = nullptr;
	SrWeaver weaver_v2 = nullptr;
	SrLens lens_v2 = nullptr;
#endif

	// D3D11 resources (references, not owned)
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *d3d11_context = nullptr;

	// Current input texture info
	ID3D11ShaderResourceView *input_srv = nullptr;
	uint32_t view_width = 0;
	uint32_t view_height = 0;
	DXGI_FORMAT input_format = DXGI_FORMAT_R8G8B8A8_UNORM;

	// Display dimensions in meters (for Kooima FOV calculation)
	float display_width_m = 0.0f;
	float display_height_m = 0.0f;
	bool display_dims_valid = false;

	// Display pixel resolution and screen position (for window metrics)
	uint32_t display_pixel_width = 0;
	uint32_t display_pixel_height = 0;
	int32_t display_screen_left = 0;
	int32_t display_screen_top = 0;
	bool display_pixel_dims_valid = false;

	// Recommended view texture dimensions from SR display
	uint32_t recommended_view_width = 0;
	uint32_t recommended_view_height = 0;
	bool recommended_dims_valid = false;

	// Configuration
	bool srgb_read = false;
	bool srgb_write = false;

	// --- #625 window-drag phase-snap probe ---------------------------------
	// A present-owner (CEF host) that drags its OWN window across processes can't
	// get the SR weaver's WndProc phase-snap, because that subclass installs
	// in-process via SetWindowLongPtr and the real weaver runs here in the
	// service against the host's CROSS-process HWND (SetWindowLongPtr fails
	// cross-process). To still snap, we drive the SDK's REAL SnapToPhase (no
	// reimplementation, ADR-019) through a service-owned hidden probe window
	// bound to its own SR weaver: that weaver's WndProc subclass installs
	// in-process, and SnapToPhase reads the DLL-global slant/pitch/user-distance
	// that the MAIN weaver already populates every frame in weave(). Created
	// lazily on the first snap; the probe weaver never weaves (it only carries
	// the WndProc). All driving happens on the snap-calling (IPC) thread —
	// SetWindowPos/SendMessage dispatch WM_WINDOWPOSCHANGING synchronously to the
	// WndProc only on the owning thread.
	HWND snap_probe_hwnd = nullptr;
	SR::IDX11Weaver1 *snap_probe_weaver = nullptr;
	DWORD snap_probe_thread = 0;    //!< Thread that created/owns snap_probe_hwnd.
	bool snap_probe_failed = false; //!< Lazy-init tried and failed — don't retry.

	// --- Adaptive weave-latency estimation (D3D11), microseconds ------------
	// Same additive motion-to-photon model as the VK arm (leia_sr.cpp):
	//     horizon = N_buffered * frame_interval + T_display
	// pushed via setLatency() each frame. The SDK's own frames-based heuristic
	// (setLatencyInFrames) uses the identical shape with N pinned by the app —
	// the point of the per-frame push is the N_BUFFERED KNOB: under the
	// runtime's late-weave scheduling (DXR_LATE_WEAVE=1) the weave runs ~one
	// refresh before scanout, so LEIA_D3D11_LATENCY_FRAMES=0 aligns the
	// predictor with the measured horizon instead of over-predicting ~2x.
	// Defaults reproduce today's numbers (N=1 + one refresh).
	bool     adaptive_latency_enabled = true;
	float    latency_frames_factor = 1.0f;   // N_buffered (frames in flight)
	uint64_t display_term_us = 16667;        // T_display, constant (~1 panel refresh @60Hz)
	uint64_t latency_min_us = 5000;          // clamp floor
	uint64_t latency_max_us = 60000;         // clamp ceiling
	uint64_t latency_fixed_us = 0;           // >0 => bypass adaptive, set once
	double   latency_ema_alpha = 0.15;       // EMA smoothing for frame interval
	uint64_t prev_weave_ns = 0;              // timestamp of previous weave()
	double   ema_interval_ns = 0.0;          // smoothed weave interval (ns)
	uint64_t last_set_latency_us = 0;        // last value pushed to setLatency()

	// Measured weave→scanout residual from the runtime's timing feedback
	// loop (xrt_display_processor set_frame_timing). When fresh, it
	// REPLACES the heuristic: exact per-path, per-panel-Hz horizon.
	uint64_t measured_r_us = 0;
	uint64_t measured_seen_ns = 0;
	double   measured_ema_us = 0.0;
};

namespace {

/*!
 * Create and wait for the SR context to become ready.
 */
bool
create_sr_context(double max_time, leiasr_d3d11 &sr)
{
	const double start_time = (double)GetTickCount64() / 1000.0;

	// Create SR context.
	while (sr.context == nullptr) {
		try {
			sr.context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException &e) {
			// Ignore errors because SR may be starting-up.
			(void)e;
		}

		U_LOG_D("Waiting for SR context...");

		// Wait a bit.
		Sleep(100);

		// Abort if we exceed the maximum allowed time.
		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (sr.context == nullptr) {
		U_LOG_E("Failed to create SR context within %.1f seconds", max_time);
		return false;
	}

	// Get display manager (modern API) and wait for display to be ready.
	SR::IDisplayManager *displayManager = nullptr;
	SR::IDisplay *display = nullptr;
	bool display_ready = false;

	try {
		displayManager = SR::GetDisplayManagerInstance(*sr.context);
		if (displayManager == nullptr) {
			U_LOG_E("Failed to get SR DisplayManager instance");
			return false;
		}
	} catch (...) {
		U_LOG_E("Exception getting SR DisplayManager - requires runtime version 1.34.8-RC1 or later");
		return false;
	}

	while (!display_ready) {
		display = displayManager->getPrimaryActiveSRDisplay();
		if (display != nullptr && display->isValid()) {
			SR_recti display_location = display->getLocation();
			int64_t width = display_location.right - display_location.left;
			int64_t height = display_location.bottom - display_location.top;
			if ((width != 0) && (height != 0)) {
				display_ready = true;

				// Cache display dimensions in meters for Kooima FOV calculation
				// Use SR SDK's physical size API (returns cm, convert to meters)
				float raw_width_cm = display->getPhysicalSizeWidth();
				float raw_height_cm = display->getPhysicalSizeHeight();
				sr.display_width_m = raw_width_cm / 100.0f;
				sr.display_height_m = raw_height_cm / 100.0f;
				sr.display_dims_valid = true;

				// Cache display pixel resolution and screen position
				sr.display_pixel_width = static_cast<uint32_t>(width);
				sr.display_pixel_height = static_cast<uint32_t>(height);
				sr.display_screen_left = static_cast<int32_t>(display_location.left);
				sr.display_screen_top = static_cast<int32_t>(display_location.top);
				sr.display_pixel_dims_valid = true;

				// Cache recommended view texture dimensions from SR display
				sr.recommended_view_width = display->getRecommendedViewsTextureWidth();
				sr.recommended_view_height = display->getRecommendedViewsTextureHeight();
				sr.recommended_dims_valid = (sr.recommended_view_width > 0 && sr.recommended_view_height > 0);

				U_LOG_W("SR D3D11 display (modern API): %ldx%ld px, physical %.2fcm x %.2fcm = %.4fm x %.4fm",
				        (long)width, (long)height,
				        raw_width_cm, raw_height_cm,
				        sr.display_width_m, sr.display_height_m);
				U_LOG_W("SR recommended view texture: %ux%u per eye",
				        sr.recommended_view_width, sr.recommended_view_height);

				break;
			}
		}

		U_LOG_D("Waiting for SR display...");

		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (!display_ready) {
		U_LOG_E("SR display not ready within %.1f seconds", max_time);
		return false;
	}

	// Create SwitchableLensHint for 2D/3D mode switching
	try {
		sr.lens_hint = SR::SwitchableLensHint::create(*sr.context);
		U_LOG_W("SR D3D11 SwitchableLensHint created successfully");
	} catch (...) {
		sr.lens_hint = nullptr;
		U_LOG_W("SR D3D11 SwitchableLensHint not available on this display");
	}

	return true;
}

#ifdef DXR_LEIA_HAS_SR_V2
/*!
 * The whole v2 creation sequence: instance, display, weaver, senses, lens.
 *
 * Mirrors the v1 ordering exactly, including the one piece of it that is not
 * obvious — `srInitialize` runs *after* the weaver exists, because
 * initialisation starts the senses and the weaver must be registered before
 * they come up. (v1 has the same constraint and the same comment.)
 */
bool
create_v2(double max_time, void *hwnd, leiasr_d3d11 &sr)
{
	if (!leia_sr_v2_create_instance(max_time, &sr.instance_v2)) {
		return false;
	}

	leia_sr_v2_display_info info{};
	if (!leia_sr_v2_query_display(sr.instance_v2, hwnd, max_time, &info)) {
		srDestroyInstance(sr.instance_v2);
		sr.instance_v2 = nullptr;
		return false;
	}

	sr.display_width_m = info.width_m;
	sr.display_height_m = info.height_m;
	sr.display_dims_valid = true;
	sr.display_pixel_width = info.pixel_width;
	sr.display_pixel_height = info.pixel_height;
	sr.display_screen_left = info.screen_left;
	sr.display_screen_top = info.screen_top;
	sr.display_pixel_dims_valid = true;
	sr.recommended_view_width = info.recommended_view_width;
	sr.recommended_view_height = info.recommended_view_height;
	sr.recommended_dims_valid = info.recommended_valid;

	// Same DPI dance as v1: the SDK reads the HWND's geometry during creation
	// and must see physical pixels. See LeiaInc/LeiaSR@a8a9fb9.
	DPI_AWARENESS_CONTEXT oldDpiCtx = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	SrWeaverCreateInfoDX11 ci{};
	ci.sType = SR_TYPE_WEAVER_CREATE_INFO_DX11;
	ci.pNext = nullptr;
	ci.d3d11Context = sr.d3d11_context;
	ci.window = (SrNativeWindowHandle)hwnd;

	const SrResult wr = srCreateWeaverDX11(sr.instance_v2, &ci, &sr.weaver_v2);

	if (oldDpiCtx != NULL) {
		SetThreadDpiAwarenessContext(oldDpiCtx);
	}

	if (!SR_SUCCEEDED(wr) || sr.weaver_v2 == nullptr) {
		U_LOG_E("srCreateWeaverDX11 failed: %s (%d)", leia_sr_v2_result_str(wr), (int)wr);
		sr.weaver_v2 = nullptr;
		srDestroyInstance(sr.instance_v2);
		sr.instance_v2 = nullptr;
		return false;
	}

	if (!leia_sr_v2_initialize(sr.instance_v2)) {
		srDestroyWeaver(sr.weaver_v2);
		sr.weaver_v2 = nullptr;
		srDestroyInstance(sr.instance_v2);
		sr.instance_v2 = nullptr;
		return false;
	}

	// Default latency, overridden per-frame by the adaptive path below exactly
	// as on v1 (setLatency disables the SDK's frames mode).
	srWeaverSetLatencyInFrames(sr.weaver_v2, 1);

	leia_sr_v2_create_lens(sr.instance_v2, &sr.lens_v2);

	U_LOG_W("SR D3D11 weaver created via the v2 C API");
	return true;
}
#endif // DXR_LEIA_HAS_SR_V2

/* ------------------------------------------------------------------ *
 * v1/v2 dispatch
 *
 * One helper per weaver operation, so the branch lives in exactly one place
 * per operation instead of being sprinkled through the call sites. The
 * discriminant is always `weaver_v2 != nullptr` — see the struct comment.
 *
 * When v1 is retired these bodies collapse to their v2 half and the call
 * sites do not change at all.
 * ------------------------------------------------------------------ */

void
w_set_latency(leiasr_d3d11 *sr, uint64_t latency_us)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetLatency(sr->weaver_v2, latency_us);
		return;
	}
#endif
	sr->weaver->setLatency(latency_us);
}

void
w_set_latency_in_frames(leiasr_d3d11 *sr, uint64_t frames)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetLatencyInFrames(sr->weaver_v2, frames);
		return;
	}
#endif
	sr->weaver->setLatencyInFrames(frames);
}

void
w_set_input_texture(leiasr_d3d11 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetInputTextureDX11(sr->weaver_v2, sr->input_srv, (int32_t)sr->view_width,
		                            (int32_t)sr->view_height, (int32_t)sr->input_format);
		return;
	}
#endif
	sr->weaver->setInputViewTexture(sr->input_srv, static_cast<int>(sr->view_width),
	                                static_cast<int>(sr->view_height), sr->input_format);
}

void
w_set_srgb(leiasr_d3d11 *sr, bool read_srgb, bool write_srgb)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetShaderSRGBConversion(sr->weaver_v2, read_srgb ? SR_TRUE : SR_FALSE,
		                                write_srgb ? SR_TRUE : SR_FALSE);
		return;
	}
#endif
	sr->weaver->setShaderSRGBConversion(read_srgb, write_srgb);
}

void
w_weave(leiasr_d3d11 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverWeave(sr->weaver_v2);
		return;
	}
#endif
	sr->weaver->weave();
}

/*!
 * Predicted eye positions in millimetres, or false if unavailable.
 *
 * The v1 call throws ~11 first-class exceptions per frame as routine internal
 * control flow (see the caller's comment) and the catch is mandatory. The v2
 * call returns an `SrResult` instead — the same condition, reported sanely —
 * but the try/catch stays on the v1 side because the v1 behaviour has not
 * changed. Units are millimetres on both: `rt_WeaverGetPredictedEyePositions`
 * forwards to the same getter.
 */
bool
w_get_predicted_eyes(leiasr_d3d11 *sr, float left_mm[3], float right_mm[3])
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		SrPoint3f l{};
		SrPoint3f r{};
		if (!SR_SUCCEEDED(srWeaverGetPredictedEyePositions(sr->weaver_v2, &l, &r))) {
			return false;
		}
		left_mm[0] = l.x;
		left_mm[1] = l.y;
		left_mm[2] = l.z;
		right_mm[0] = r.x;
		right_mm[1] = r.y;
		right_mm[2] = r.z;
		return true;
	}
#endif
	try {
		sr->weaver->getPredictedEyePositions(left_mm, right_mm);
	} catch (...) {
		return false;
	}
	return true;
}

/*!
 * Is there a usable weaver, on whichever path we are on?
 *
 * Every public entry point guards on this. Before the v2 path existed the
 * guards read `leiasr->weaver == nullptr` directly, which on v2 is ALWAYS true
 * — so left unchanged they would have turned eye tracking, sRGB configuration
 * and mode switching into silent no-ops returning `false`, on a path whose
 * weaver was working perfectly. Nothing would have crashed; the display would
 * simply have had no eye tracking.
 */
bool
w_ready(const leiasr_d3d11 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		return true;
	}
#endif
	return sr->weaver != nullptr;
}

//! Is a lens present at all? False means 2D/3D switching is unavailable.
bool
lens_present(leiasr_d3d11 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		return sr->lens_v2 != nullptr;
	}
#endif
	return sr->lens_hint != nullptr;
}

void
lens_set(leiasr_d3d11 *sr, bool enable)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		if (sr->lens_v2 != nullptr) {
			enable ? srLensEnable(sr->lens_v2) : srLensDisable(sr->lens_v2);
		}
		return;
	}
#endif
	if (sr->lens_hint != nullptr) {
		enable ? sr->lens_hint->enable() : sr->lens_hint->disable();
	}
}

bool
lens_is_enabled(leiasr_d3d11 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		SrBool32 enabled = SR_FALSE;
		if (sr->lens_v2 == nullptr || !SR_SUCCEEDED(srLensIsEnabled(sr->lens_v2, &enabled))) {
			return false;
		}
		return enabled == SR_TRUE;
	}
#endif
	return sr->lens_hint != nullptr && sr->lens_hint->isEnabled();
}

} // namespace

extern "C" {

xrt_result_t
leiasr_d3d11_create(double max_time,
                    void *d3d11_device,
                    void *d3d11_context,
                    void *hwnd,
                    uint32_t view_width,
                    uint32_t view_height,
                    struct leiasr_d3d11 **out)
{
	if (d3d11_device == nullptr || d3d11_context == nullptr) {
		U_LOG_E("D3D11 device or context is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	leiasr_d3d11 *sr = new leiasr_d3d11;
	sr->device = static_cast<ID3D11Device *>(d3d11_device);
	sr->d3d11_context = static_cast<ID3D11DeviceContext *>(d3d11_context);
	sr->view_width = view_width;
	sr->view_height = view_height;

#ifdef DXR_LEIA_HAS_SR_V2
	if (leia_sr_api_selected() == LEIA_SR_API_V2) {
		// Deliberately NOT falling back to v1 on failure.
		//
		// The selector already established that the v2 runtime loads and speaks
		// our API version; a failure past that point is a real fault (no SR
		// display, no device, a lost GPU) that v1 would hit for the same reason.
		// Silently retrying on the other path would convert a clear error into a
		// working-but-unexplained session on an API family nobody chose, and the
		// log would say "using v2" while the process ran v1. That is precisely
		// the divergence the selector exists to prevent.
		if (!create_v2(max_time, hwnd, *sr)) {
			U_LOG_E("SR v2 weaver creation failed - not falling back to v1 "
			        "(set DXR_LEIA_SR_API=v1 to force it)");
			delete sr;
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}
	} else
#endif
	{
		// Create SR context
		if (!create_sr_context(max_time, *sr)) {
			delete sr;
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}

		// Create D3D11 weaver (SR SDK installs its WndProc via SetWindowLongPtr).
		// Set DPI awareness so the SDK sees physical pixels when it queries the
		// HWND. See LeiaInc/LeiaSR@a8a9fb9 for the pattern.
		DPI_AWARENESS_CONTEXT oldDpiCtx =
		    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		WeaverErrorCode result = SR::CreateDX11Weaver(sr->context,
		                                               sr->d3d11_context,
		                                               static_cast<HWND>(hwnd),
		                                               &sr->weaver);
		if (oldDpiCtx != NULL) {
			SetThreadDpiAwarenessContext(oldDpiCtx);
		}
		if (result != WeaverErrorCode::WeaverSuccess) {
			U_LOG_E("Failed to create SR D3D11 weaver: %d", (int)result);
			SR::SRContext::deleteSRContext(sr->context);
			delete sr;
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}

		// Initialize the context AFTER the weaver has been registered.
		// initialize() starts the senses.
		sr->context->initialize();

		// Set default latency (1 frame). Kept as the fallback for
		// LEIA_D3D11_ADAPTIVE_LATENCY=0; the adaptive per-frame setLatency()
		// below overrides it (setLatency disables the SDK's frames mode).
		w_set_latency_in_frames(sr, 1);
	}

	// EVERYTHING BELOW IS SHARED BY BOTH PATHS and must stay that way. The
	// adaptive-latency configuration is runtime policy, not SDK plumbing: an
	// early return from the v2 branch above would silently skip it, leaving
	// v2 on the SDK default while v1 ran adaptive — which would present as
	// "v2 has worse latency" rather than as the configuration bug it is.

	// Adaptive-latency knobs, mirroring the VK arm's LEIA_VK_* set:
	//   LEIA_D3D11_ADAPTIVE_LATENCY=0   disable (keep SDK frames-based default)
	//   LEIA_D3D11_LATENCY_FIXED_US=N   bypass adaptive, pin setLatency(N) once
	//   LEIA_D3D11_LATENCY_FRAMES=f     N_buffered (default 1.0; 0 under late-weave)
	//   LEIA_D3D11_PANEL_HZ=hz          panel refresh for display term (default 60)
	//   LEIA_D3D11_LATENCY_DISPLAY_US=N override display term outright
	//   LEIA_D3D11_LATENCY_MIN_US/_MAX_US/_EMA_ALPHA  clamps + smoothing
	{
		auto getf = [](const char *n, float def) -> float {
			const char *v = std::getenv(n);
			return (v == nullptr || v[0] == '\0') ? def : (float)atof(v);
		};
		auto getu = [](const char *n, uint64_t def) -> uint64_t {
			const char *v = std::getenv(n);
			if (v == nullptr || v[0] == '\0') return def;
			long long x = atoll(v);
			return x < 0 ? def : (uint64_t)x;
		};

		const char *en = std::getenv("LEIA_D3D11_ADAPTIVE_LATENCY");
		sr->adaptive_latency_enabled = !(en != nullptr && en[0] == '0');
		// Heuristic fallback keeps the classic 1-frame default: paced paths now
		// get their horizon from the runtime's MEASURED feed (set_frame_timing),
		// so the heuristic only serves never-measured paths (e.g. VK DComp
		// transparent) — where pre-late-weave pacing still applies and N=1 is
		// the correct shape. Replaces the interim DXR_LATE_WEAVE env coupling.
		sr->latency_frames_factor = getf("LEIA_D3D11_LATENCY_FRAMES", 1.0f);
		sr->latency_min_us = getu("LEIA_D3D11_LATENCY_MIN_US", 5000);
		sr->latency_max_us = getu("LEIA_D3D11_LATENCY_MAX_US", 60000);
		sr->latency_fixed_us = getu("LEIA_D3D11_LATENCY_FIXED_US", 0);
		float a = getf("LEIA_D3D11_LATENCY_EMA_ALPHA", 0.15f);
		sr->latency_ema_alpha = a < 0.01f ? 0.01f : (a > 1.0f ? 1.0f : a);

		float panel_hz = getf("LEIA_D3D11_PANEL_HZ", 60.0f);
		uint64_t disp_default = (panel_hz > 1.0f) ? (uint64_t)(1.0e6 / panel_hz + 0.5) : 16667;
		sr->display_term_us = getu("LEIA_D3D11_LATENCY_DISPLAY_US", disp_default);

		if (sr->latency_fixed_us > 0) {
			w_set_latency(sr, sr->latency_fixed_us);
			sr->last_set_latency_us = sr->latency_fixed_us;
			sr->adaptive_latency_enabled = false;
			U_LOG_W("Leia D3D11 weave latency: FIXED %llu us (adaptive disabled)",
			        (unsigned long long)sr->latency_fixed_us);
		} else if (sr->adaptive_latency_enabled) {
			U_LOG_W("Leia D3D11 weave latency: ADAPTIVE (horizon = %.2f x frame_interval + %llu us display, clamp %llu..%llu us, alpha %.2f)",
			        (double)sr->latency_frames_factor,
			        (unsigned long long)sr->display_term_us,
			        (unsigned long long)sr->latency_min_us,
			        (unsigned long long)sr->latency_max_us, sr->latency_ema_alpha);
		} else {
			U_LOG_W("Leia D3D11 weave latency: SDK frames-based default (adaptive off)");
		}
	}

	*out = sr;

	U_LOG_I("Created D3D11 SR weaver for HWND %p, view size %ux%u", hwnd, view_width, view_height);

	return XRT_SUCCESS;
}

void
leiasr_d3d11_destroy(struct leiasr_d3d11 **leiasr_ptr)
{
	if (leiasr_ptr == nullptr || *leiasr_ptr == nullptr) {
		return;
	}

	leiasr_d3d11 *sr = *leiasr_ptr;

	// #625: tear down the phase-snap probe (weaver restores the probe window's
	// WndProc) before the SRContext goes away.
	if (sr->snap_probe_weaver != nullptr) {
		sr->snap_probe_weaver->destroy();
		sr->snap_probe_weaver = nullptr;
	}
	if (sr->snap_probe_hwnd != nullptr) {
		DestroyWindow(sr->snap_probe_hwnd);
		sr->snap_probe_hwnd = nullptr;
	}

#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		// Ownership differs from v1 in one important way: the v2 lens is an
		// independently-created handle we own and MUST destroy, whereas the v1
		// SwitchableLensHint belongs to the SRContext and destroying it is a
		// double-free. Same concept, opposite obligation — hence the separate
		// teardown rather than a shared one.
		if (sr->lens_v2 != nullptr) {
			srDestroyLens(sr->lens_v2);
			sr->lens_v2 = nullptr;
		}
		srDestroyWeaver(sr->weaver_v2);
		sr->weaver_v2 = nullptr;

		if (sr->instance_v2 != nullptr) {
			srDestroyInstance(sr->instance_v2);
			sr->instance_v2 = nullptr;
		}

		delete sr;
		*leiasr_ptr = nullptr;
		U_LOG_I("Destroyed D3D11 SR weaver (v2)");
		return;
	}
#endif

	// SwitchableLensHint is managed by SRContext — do NOT delete it manually.
	// SRContext::~SRContext() calls deleteAllSenses() which cleans it up.
	// Manually deleting it causes a crash (double-free).
	sr->lens_hint = nullptr;

	// Destroy weaver (SR SDK restores the app's original WndProc)
	if (sr->weaver != nullptr) {
		sr->weaver->destroy();
		sr->weaver = nullptr;
	}

	// Destroy context
	if (sr->context != nullptr) {
		SR::SRContext::deleteSRContext(sr->context);
		sr->context = nullptr;
	}

	delete sr;
	*leiasr_ptr = nullptr;

	U_LOG_I("Destroyed D3D11 SR weaver");
}

void
leiasr_d3d11_set_input_texture(struct leiasr_d3d11 *leiasr,
                               void *stereo_srv,
                               uint32_t view_width,
                               uint32_t view_height,
                               uint32_t format)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return;
	}

	// Log dimension changes (first time or when dimensions change)
	static uint32_t last_logged_width = 0, last_logged_height = 0;
	if (view_width != last_logged_width || view_height != last_logged_height) {
		U_LOG_I("SR weaver setInputViewTexture: view=%ux%u (expects side-by-side stereo SRV)",
		        view_width, view_height);
		last_logged_width = view_width;
		last_logged_height = view_height;
	}

	leiasr->input_srv = static_cast<ID3D11ShaderResourceView *>(stereo_srv);
	leiasr->view_width = view_width;
	leiasr->view_height = view_height;
	leiasr->input_format = static_cast<DXGI_FORMAT>(format);

	// Configure the weaver with the input texture
	// NOTE: view_width is single-eye width; SR SDK handles the side-by-side stereo layout internally
	w_set_input_texture(leiasr);
}

void
leiasr_d3d11_set_frame_timing(struct leiasr_d3d11 *leiasr,
                              uint64_t weave_to_scanout_ns,
                              uint64_t frame_period_ns)
{
	if (leiasr == nullptr) {
		return;
	}
	leiasr->measured_r_us = weave_to_scanout_ns / 1000;
	if (weave_to_scanout_ns > 0) {
		leiasr->measured_seen_ns = os_monotonic_get_ns();
	}
	// The panel period also upgrades the heuristic fallback's display term
	// (previously a hard-coded 60 Hz constant).
	if (frame_period_ns > 0) {
		leiasr->display_term_us = frame_period_ns / 1000;
	}
}

void
leiasr_d3d11_weave(struct leiasr_d3d11 *leiasr)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		U_LOG_W("leiasr_d3d11_weave called with null instance or weaver");
		return;
	}

	// Adaptive weave latency: estimate the motion-to-photon horizon from the
	// achieved weave() interval and feed it to the predictor via setLatency()
	// BEFORE weave() so this frame's eye prediction uses it. Same model and
	// deadband as the VK arm (leia_sr.cpp).
	if (leiasr->adaptive_latency_enabled && leiasr->latency_fixed_us == 0) {
		const uint64_t now_ns = os_monotonic_get_ns();
		if (leiasr->prev_weave_ns != 0) {
			const uint64_t dt_ns = now_ns - leiasr->prev_weave_ns;
			// Ignore hitches / first-frame gaps (>250 ms).
			if (dt_ns < 250ULL * 1000 * 1000) {
				if (leiasr->ema_interval_ns <= 0.0) {
					leiasr->ema_interval_ns = (double)dt_ns;
				} else {
					const double a = leiasr->latency_ema_alpha;
					leiasr->ema_interval_ns =
					    a * (double)dt_ns + (1.0 - a) * leiasr->ema_interval_ns;
				}
			}
		}
		leiasr->prev_weave_ns = now_ns;

		// Prefer the runtime's MEASURED weave→scanout residual (timing
		// feedback loop) when fresh; heuristic only as fallback.
		const bool measured_fresh = leiasr->measured_r_us > 0 &&
		                            (now_ns - leiasr->measured_seen_ns) < 250ULL * 1000 * 1000;
		if (measured_fresh || leiasr->ema_interval_ns > 0.0) {
			double horizon_us;
			if (measured_fresh) {
				// One-shot lifecycle log: the horizon source flipped from
				// heuristic to runtime-measured (set_frame_timing loop).
				if (leiasr->measured_ema_us <= 0.0) {
					U_LOG_W("Leia D3D11 weave latency: MEASURED horizon engaged (%llu us, display term %llu us)",
					        (unsigned long long)leiasr->measured_r_us,
					        (unsigned long long)leiasr->display_term_us);
				}
				const double a = leiasr->latency_ema_alpha;
				leiasr->measured_ema_us =
				    (leiasr->measured_ema_us <= 0.0)
				        ? (double)leiasr->measured_r_us
				        : a * (double)leiasr->measured_r_us + (1.0 - a) * leiasr->measured_ema_us;
				horizon_us = leiasr->measured_ema_us;
			} else {
				horizon_us = (double)leiasr->latency_frames_factor *
				                 leiasr->ema_interval_ns / 1000.0 +
				             (double)leiasr->display_term_us;
			}
			if (horizon_us < (double)leiasr->latency_min_us)
				horizon_us = (double)leiasr->latency_min_us;
			if (horizon_us > (double)leiasr->latency_max_us)
				horizon_us = (double)leiasr->latency_max_us;
			const uint64_t latency_us = (uint64_t)(horizon_us + 0.5);

			// Deadband: only re-push on a meaningful change (>=250 us).
			const uint64_t prev = leiasr->last_set_latency_us;
			const uint64_t diff = latency_us > prev ? latency_us - prev : prev - latency_us;
			if (prev == 0 || diff >= 250) {
				w_set_latency(leiasr, latency_us);
				if (prev == 0 || diff >= 2000) {
					U_LOG_I("Leia D3D11 adaptive latency: %llu us (%.2f ms/frame ~ %.0f fps; %.2f x iv + %llu us disp)",
					        (unsigned long long)latency_us,
					        leiasr->ema_interval_ns / 1e6,
					        1e9 / leiasr->ema_interval_ns,
					        (double)leiasr->latency_frames_factor,
					        (unsigned long long)leiasr->display_term_us);
				}
				leiasr->last_set_latency_us = latency_us;
			}
		}
	}

	// The weaver writes to the currently bound render target.
	// Make sure OMSetRenderTargets and RSSetViewports have been called.
	// Set DPI awareness so any internal GetClientRect returns physical pixels.
	DPI_AWARENESS_CONTEXT oldDpiCtx =
	    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	w_weave(leiasr);
	if (oldDpiCtx != NULL) {
		SetThreadDpiAwarenessContext(oldDpiCtx);
	}
}

/*!
 * #625: lazily create the hidden probe window + its SR weaver (which installs
 * the SDK's phase-snap WndProc subclass in-process). Reuses the live SRContext +
 * immediate context; the probe weaver never weaves. Returns true when the probe
 * is ready. Must run on the snap-calling thread (WndProc dispatch is
 * thread-affine) and under the render mutex (shared immediate context).
 */
static bool
leiasr_d3d11_snap_probe_ensure(struct leiasr_d3d11 *leiasr)
{
	if (leiasr->snap_probe_weaver != nullptr) {
		return true;
	}
	if (leiasr->snap_probe_failed) {
		return false; // tried once, don't thrash the SR SDK every drag step
	}
	if (leiasr->context == nullptr || leiasr->d3d11_context == nullptr) {
		leiasr->snap_probe_failed = true;
		return false;
	}

	// Register the probe window class once per process.
	static const wchar_t *kProbeClass = L"DXRLeiaSnapProbe";
	static bool class_registered = false;
	HINSTANCE hinst = GetModuleHandleW(nullptr);
	if (!class_registered) {
		WNDCLASSW wc = {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = hinst;
		wc.lpszClassName = kProbeClass;
		RegisterClassW(&wc); // benign if already registered
		class_registered = true;
	}

	// Hidden, non-activating popup on the 3D display. Position is overwritten
	// per-snap (SnapToPhase keys off absolute screen coords) and the window is
	// never shown, so it never paints.
	int px = leiasr->display_screen_left;
	int py = leiasr->display_screen_top;
	HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kProbeClass, L"", WS_POPUP, px, py, 1, 1,
	                            nullptr, nullptr, hinst, nullptr);
	if (hwnd == nullptr) {
		U_LOG_W("#625 snap: probe CreateWindowEx failed (err=%lu)", GetLastError());
		leiasr->snap_probe_failed = true;
		return false;
	}

	// Creating a 2nd weaver on the shared SR context transiently perturbs the
	// physical lens (a brief 2D blip) — which, with the present-owner still
	// feeding full-disparity content, would surface as one mismatched
	// 2D-lens+disparity frame (the drag-time crosstalk). Capture the lens state
	// NOW and re-assert it synchronously right after the create (below), so no
	// presented frame ever sees the lens demoted. (We only run lazily, after 3D
	// is already locked, so re-asserting is safe — it never forces 3D inside the
	// SR init-settle window, unlike pre-creating the probe early.)
	bool lens_was_3d = false;
	if (leiasr->lens_hint != nullptr) {
		try {
			lens_was_3d = leiasr->lens_hint->isEnabled();
		} catch (...) {
		}
	}

	// Bind an SR weaver to the probe window — this installs the SDK's real
	// phase-snap WndProc (SetWindowLongPtr on GA_ROOT of hwnd), in-process.
	DPI_AWARENESS_CONTEXT oldDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	SR::IDX11Weaver1 *probe = nullptr;
	WeaverErrorCode result = SR::CreateDX11Weaver(leiasr->context, leiasr->d3d11_context, hwnd, &probe);
	if (oldDpi != NULL) {
		SetThreadDpiAwarenessContext(oldDpi);
	}
	if (result != WeaverErrorCode::WeaverSuccess || probe == nullptr) {
		U_LOG_W("#625 snap: probe CreateDX11Weaver failed (%d) — drag phase-snap unavailable", (int)result);
		DestroyWindow(hwnd);
		leiasr->snap_probe_failed = true;
		return false;
	}

	// Re-assert the pre-create lens state so the probe's creation blip never
	// reaches a presented frame (see the capture above). Idempotent.
	if (lens_was_3d && leiasr->lens_hint != nullptr) {
		try {
			leiasr->lens_hint->enable();
		} catch (...) {
		}
	}

	leiasr->snap_probe_hwnd = hwnd;
	leiasr->snap_probe_weaver = probe;
	leiasr->snap_probe_thread = GetCurrentThreadId();
	U_LOG_W("#625 snap: probe window %p + weaver ready (thread %lu, lens_was_3d=%d)", (void *)hwnd,
	        leiasr->snap_probe_thread, (int)lens_was_3d);
	return true;
}

bool
leiasr_d3d11_snap_window_rect(struct leiasr_d3d11 *leiasr,
                              int32_t origin_x,
                              int32_t origin_y,
                              int32_t target_x,
                              int32_t target_y,
                              int32_t *out_x,
                              int32_t *out_y)
{
	if (out_x == nullptr || out_y == nullptr) {
		return false;
	}
	*out_x = target_x; // default: no-op snap
	*out_y = target_y;
	if (leiasr == nullptr) {
		return false;
	}

#ifdef DXR_LEIA_HAS_SR_V2
	if (leiasr->weaver_v2 != nullptr) {
		// The probe workaround is v1-only by construction: it needs a second
		// SR::IDX11Weaver1 built on an SR::SRContext, neither of which exists
		// here. The v2 replacement is srWeaverSnapToPhase — the windowless entry
		// point we asked for and Leia added (LeiaInc/LeiaSR#164) — but it is NOT
		// in the SDK drop this builds against, and a build that does export it
		// may still predate the orientation-canonicalisation fix (#169). Wiring
		// it unverified would reintroduce exactly the failure that cost days on
		// #163: a snap that silently returns its input is indistinguishable from
		// one that had nothing to correct.
		//
		// So: decline honestly and once. The caller keeps its unsnapped target,
		// which costs phase coherence during a cross-process drag and nothing
		// else. Wiring this up is the follow-up once #169 lands in a drop.
		static bool warned = false;
		if (!warned) {
			U_LOG_W("#625 snap: unavailable on the SR v2 path (srWeaverSnapToPhase not in this SDK) - "
			        "window drags will not phase-snap; use DXR_LEIA_SR_API=v1 if you need it");
			warned = true;
		}
		return false;
	}
#endif

	if (!leiasr_d3d11_snap_probe_ensure(leiasr)) {
		return false;
	}
	// The SDK WndProc only sees SetWindowPos/SendMessage dispatched on the
	// window's OWNING thread — we created the probe on this (IPC) thread.
	if (GetCurrentThreadId() != leiasr->snap_probe_thread) {
		static bool warned = false;
		if (!warned) {
			U_LOG_W("#625 snap: called from thread %lu but probe owned by %lu — skipping",
			        GetCurrentThreadId(), leiasr->snap_probe_thread);
			warned = true;
		}
		return false;
	}

	HWND h = leiasr->snap_probe_hwnd;
	DPI_AWARENESS_CONTEXT oldDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// Drive the SDK's real drag-snap sequence synthetically:
	//   1. place the probe at the drag-start ORIGIN (moving=false ⟹ no snap);
	const UINT kSwp = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;
	SetWindowPos(h, nullptr, origin_x, origin_y, 0, 0, kSwp);
	//   2. begin the move — the WndProc records initialX/Y = GetWindowRect (=origin);
	SendMessageW(h, WM_ENTERSIZEMOVE, 0, 0);
	//   3. propose the TARGET — WM_WINDOWPOSCHANGING runs SnapToPhase, rewrites pos->x/y;
	SetWindowPos(h, nullptr, target_x, target_y, 0, 0, kSwp);
	//   4. read back the phase-snapped position the SDK committed;
	RECT r = {};
	bool got = GetWindowRect(h, &r) != 0;
	//   5. end the move.
	SendMessageW(h, WM_EXITSIZEMOVE, 0, 0);

	if (oldDpi != NULL) {
		SetThreadDpiAwarenessContext(oldDpi);
	}
	if (!got) {
		return false;
	}
	*out_x = r.left;
	*out_y = r.top;
	return true;
}

/*!
 * Check if weaver's HWND is still valid (for debugging "window handle is invalid" errors).
 */
bool
leiasr_d3d11_check_window_valid(struct leiasr_d3d11 *leiasr, void *hwnd)
{
	if (leiasr == nullptr) {
		return false;
	}

	HWND h = static_cast<HWND>(hwnd);
	if (h == nullptr) {
		U_LOG_W("leiasr_d3d11: HWND is null");
		return false;
	}

	if (!IsWindow(h)) {
		U_LOG_W("leiasr_d3d11: HWND %p is not a valid window", h);
		return false;
	}

	// Check if window is visible
	if (!IsWindowVisible(h)) {
		static bool warned_invisible = false;
		if (!warned_invisible) {
			U_LOG_W("leiasr_d3d11: Window %p is not visible", h);
			warned_invisible = true;
		}
	}

	// Get window position to check if it's on a valid monitor
	RECT rect;
	if (GetWindowRect(h, &rect)) {
		HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
		if (monitor == nullptr) {
			U_LOG_W("leiasr_d3d11: Window %p is not on any monitor (rect: %ld,%ld-%ld,%ld)",
			        h, rect.left, rect.top, rect.right, rect.bottom);
			return false;
		}
	}

	return true;
}

bool
leiasr_d3d11_get_predicted_eye_positions(struct leiasr_d3d11 *leiasr,
                                         float out_left_eye[3],
                                         float out_right_eye[3])
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return false;
	}

	// On v1 the SDK's getPredictedEyePositions throws ~11 first-chance
	// exceptions per frame (10x leap::api_exception + 1x std::runtime_error)
	// as routine internal control flow when its async cache races the call.
	// Verified empirically in LeiaViewer.exe (Leia's own reference app) —
	// same pattern, same rate. The catch is mandatory: the std::runtime_error
	// must not cross the C ABI boundary back into the runtime. On v2 the same
	// condition arrives as a plain SrResult. Both are handled in w_get_*.
	float left_mm[3], right_mm[3];
	if (!w_get_predicted_eyes(leiasr, left_mm, right_mm)) {
		return false;
	}
	out_left_eye[0]  = left_mm[0]  / 1000.0f;
	out_left_eye[1]  = left_mm[1]  / 1000.0f;
	out_left_eye[2]  = left_mm[2]  / 1000.0f;
	out_right_eye[0] = right_mm[0] / 1000.0f;
	out_right_eye[1] = right_mm[1] / 1000.0f;
	out_right_eye[2] = right_mm[2] / 1000.0f;
	return true;
}

void
leiasr_d3d11_set_srgb_conversion(struct leiasr_d3d11 *leiasr,
                                 bool read_srgb,
                                 bool write_srgb)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return;
	}

	leiasr->srgb_read = read_srgb;
	leiasr->srgb_write = write_srgb;
	w_set_srgb(leiasr, read_srgb, write_srgb);
}

void
leiasr_d3d11_set_latency_in_frames(struct leiasr_d3d11 *leiasr,
                                   uint64_t latency_frames)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return;
	}

	w_set_latency_in_frames(leiasr, latency_frames);
}

bool
leiasr_d3d11_is_ready(struct leiasr_d3d11 *leiasr)
{
	if (leiasr == nullptr) {
		return false;
	}

	return w_ready(leiasr);
}

bool
leiasr_d3d11_get_display_dimensions(struct leiasr_d3d11 *leiasr, struct leiasr_display_dimensions *out_dims)
{
	if (leiasr == nullptr || out_dims == nullptr) {
		if (out_dims != nullptr) {
			out_dims->valid = false;
		}
		return false;
	}

	if (!leiasr->display_dims_valid) {
		out_dims->valid = false;
		return false;
	}

	out_dims->width_m = leiasr->display_width_m;
	out_dims->height_m = leiasr->display_height_m;
	out_dims->valid = true;

	return true;
}

bool
leiasr_d3d11_get_display_pixel_info(struct leiasr_d3d11 *leiasr,
                                     uint32_t *out_display_pixel_width,
                                     uint32_t *out_display_pixel_height,
                                     int32_t *out_display_screen_left,
                                     int32_t *out_display_screen_top,
                                     float *out_display_width_m,
                                     float *out_display_height_m)
{
	if (leiasr == nullptr || out_display_pixel_width == nullptr ||
	    out_display_pixel_height == nullptr || out_display_screen_left == nullptr ||
	    out_display_screen_top == nullptr || out_display_width_m == nullptr ||
	    out_display_height_m == nullptr) {
		return false;
	}

	if (!leiasr->display_pixel_dims_valid || !leiasr->display_dims_valid) {
		return false;
	}

	*out_display_pixel_width = leiasr->display_pixel_width;
	*out_display_pixel_height = leiasr->display_pixel_height;
	*out_display_screen_left = leiasr->display_screen_left;
	*out_display_screen_top = leiasr->display_screen_top;
	*out_display_width_m = leiasr->display_width_m;
	*out_display_height_m = leiasr->display_height_m;

	return true;
}

bool
leiasr_d3d11_get_recommended_view_dimensions(struct leiasr_d3d11 *leiasr,
                                              uint32_t *out_width,
                                              uint32_t *out_height)
{
	if (leiasr == nullptr || out_width == nullptr || out_height == nullptr) {
		return false;
	}

	if (!leiasr->recommended_dims_valid) {
		return false;
	}

	*out_width = leiasr->recommended_view_width;
	*out_height = leiasr->recommended_view_height;

	return true;
}

bool
leiasr_query_recommended_view_dimensions(double max_time,
                                          uint32_t *out_width,
                                          uint32_t *out_height,
                                          float *out_refresh_rate_hz,
                                          uint32_t *out_native_width,
                                          uint32_t *out_native_height)
{
	if (out_width == nullptr || out_height == nullptr) {
		return false;
	}

	const double start_time = (double)GetTickCount64() / 1000.0;

	// Create temporary SR context
	SR::SRContext *context = nullptr;
	while (context == nullptr) {
		try {
			context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException &e) {
			(void)e;
		}

		U_LOG_D("Waiting for SR context (dimension query)...");
		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (context == nullptr) {
		U_LOG_E("Failed to create SR context for dimension query within %.1f seconds", max_time);
		return false;
	}

	// Get display manager and query dimensions
	bool success = false;
	try {
		SR::IDisplayManager *displayManager = SR::GetDisplayManagerInstance(*context);
		if (displayManager != nullptr) {
			// Wait for display to be ready
			while (!success) {
				SR::IDisplay *display = displayManager->getPrimaryActiveSRDisplay();
				if (display != nullptr && display->isValid()) {
					SR_recti display_location = display->getLocation();
					int64_t native_width = display_location.right - display_location.left;
					int64_t native_height = display_location.bottom - display_location.top;
					if ((native_width != 0) && (native_height != 0)) {
						*out_width = display->getRecommendedViewsTextureWidth();
						*out_height = display->getRecommendedViewsTextureHeight();
						success = (*out_width > 0 && *out_height > 0);
						if (success) {
							U_LOG_I("SR query: recommended view dimensions %ux%u per eye",
							        *out_width, *out_height);

							// Return native display dimensions if requested
							if (out_native_width != nullptr) {
								*out_native_width = static_cast<uint32_t>(native_width);
							}
							if (out_native_height != nullptr) {
								*out_native_height = static_cast<uint32_t>(native_height);
							}
							U_LOG_I("SR query: native display dimensions %ux%u",
							        (uint32_t)native_width, (uint32_t)native_height);

							// Query monitor refresh rate via Win32
							if (out_refresh_rate_hz != nullptr) {
								DEVMODEW dm = {};
								dm.dmSize = sizeof(dm);
								if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
								    dm.dmDisplayFrequency > 1) {
									*out_refresh_rate_hz = (float)dm.dmDisplayFrequency;
									U_LOG_I("SR query: display refresh rate %.0f Hz",
									        *out_refresh_rate_hz);
								} else {
									*out_refresh_rate_hz = 60.0f;
									U_LOG_W("Could not query display refresh rate, defaulting to 60 Hz");
								}
							}
						}
						break;
					}
				}

				Sleep(100);

				double cur_time = (double)GetTickCount64() / 1000.0;
				if ((cur_time - start_time) > max_time) {
					break;
				}
			}
		}
	} catch (...) {
		U_LOG_E("Exception querying SR display dimensions");
	}

	// Clean up temporary context
	SR::SRContext::deleteSRContext(context);

	if (!success) {
		U_LOG_E("Failed to query SR recommended dimensions within %.1f seconds", max_time);
	}

	return success;
}

// Cached display dimensions for static queries
static float g_cached_display_width_m = 0.0f;
static float g_cached_display_height_m = 0.0f;
static float g_cached_nominal_x_m = 0.0f;
static float g_cached_nominal_y_m = 0.0f;
static float g_cached_nominal_z_m = 0.5f;
static bool g_display_dims_cached = false;

bool
leiasr_static_get_predicted_eye_positions(float out_left_eye[3],
                                          float out_right_eye[3])
{
	if (out_left_eye == nullptr || out_right_eye == nullptr) {
		return false;
	}

	// Eye position prediction requires an active weaver instance because:
	// 1. The weaver owns the LookaroundFilter that provides prediction
	// 2. Prediction is tuned to each application's update rate
	// Without a weaver, we cannot provide accurate predicted eye positions.
	// Callers should use leiasr_d3d11_get_predicted_eye_positions() with
	// their per-session weaver instance instead.

	// Return default center position for graceful fallback
	// This allows apps to start rendering before eye tracking is ready
	out_left_eye[0] = -0.032f;  // -32mm left of center
	out_left_eye[1] = 0.0f;
	out_left_eye[2] = 0.6f;     // 600mm from display

	out_right_eye[0] = 0.032f;  // +32mm right of center
	out_right_eye[1] = 0.0f;
	out_right_eye[2] = 0.6f;    // 600mm from display

	// Return false to indicate these are fallback values, not tracked
	return false;
}

bool
leiasr_static_get_display_dimensions(struct leiasr_display_dimensions *out_dims)
{
	if (out_dims == nullptr) {
		return false;
	}

	// Return cached values if available
	if (g_display_dims_cached) {
		out_dims->width_m = g_cached_display_width_m;
		out_dims->height_m = g_cached_display_height_m;
		out_dims->nominal_x_m = g_cached_nominal_x_m;
		out_dims->nominal_y_m = g_cached_nominal_y_m;
		out_dims->nominal_z_m = g_cached_nominal_z_m;
		out_dims->valid = true;
		return true;
	}

	// Need to query from SR SDK
	// Create temporary context
	SR::SRContext *context = nullptr;

	try {
		context = SR::SRContext::create();
	} catch (...) {
		return false;
	}

	if (context == nullptr) {
		return false;
	}

	bool success = false;
	try {
		SR::IDisplayManager *displayManager = SR::GetDisplayManagerInstance(*context);
		if (displayManager != nullptr) {
			SR::IDisplay *display = displayManager->getPrimaryActiveSRDisplay();
			if (display != nullptr && display->isValid()) {
				// Get physical dimensions using SR SDK's physical size API
				// Returns centimeters, convert to meters
				float raw_width_cm = display->getPhysicalSizeWidth();
				float raw_height_cm = display->getPhysicalSizeHeight();

				if (raw_width_cm > 0.0f && raw_height_cm > 0.0f) {
					g_cached_display_width_m = raw_width_cm / 100.0f;
					g_cached_display_height_m = raw_height_cm / 100.0f;

					// Query nominal viewing position from SR SDK (returns mm)
					float nom_x_mm = 0.0f, nom_y_mm = 0.0f, nom_z_mm = 0.0f;
					try {
						display->getDefaultViewingPosition(nom_x_mm, nom_y_mm, nom_z_mm);
						g_cached_nominal_x_m = nom_x_mm / 1000.0f;
						g_cached_nominal_y_m = nom_y_mm / 1000.0f;
						g_cached_nominal_z_m = nom_z_mm / 1000.0f;
						U_LOG_W("SR nominal viewing position: (%.1f, %.1f, %.1f) mm = (%.4f, %.4f, %.4f) m",
						        nom_x_mm, nom_y_mm, nom_z_mm,
						        g_cached_nominal_x_m, g_cached_nominal_y_m, g_cached_nominal_z_m);
					} catch (...) {
						g_cached_nominal_x_m = 0.0f;
						g_cached_nominal_y_m = 0.0f;
						g_cached_nominal_z_m = 0.5f;
						U_LOG_W("SR getDefaultViewingPosition failed, using fallback (0, 0, 0.5) m");
					}

					g_display_dims_cached = true;

					out_dims->width_m = g_cached_display_width_m;
					out_dims->height_m = g_cached_display_height_m;
					out_dims->nominal_x_m = g_cached_nominal_x_m;
					out_dims->nominal_y_m = g_cached_nominal_y_m;
					out_dims->nominal_z_m = g_cached_nominal_z_m;
					out_dims->valid = true;
					success = true;

					U_LOG_W("Static display dimensions: %.2fcm x %.2fcm = %.4fm x %.4fm",
					        raw_width_cm, raw_height_cm,
					        g_cached_display_width_m, g_cached_display_height_m);
				}
			}
		}
	} catch (...) {
		U_LOG_E("Exception querying static display dimensions");
	}

	// Clean up temporary context
	if (context != nullptr) {
		SR::SRContext::deleteSRContext(context);
	}

	return success;
}

bool
leiasr_d3d11_request_display_mode(struct leiasr_d3d11 *leiasr, bool enable_3d)
{
	if (leiasr == nullptr || !lens_present(leiasr)) {
		return false;
	}

	try {
		lens_set(leiasr, enable_3d);
		U_LOG_W("SR D3D11 display mode switched to %s", enable_3d ? "3D" : "2D");
		return true;
	} catch (...) {
		U_LOG_E("Failed to switch SR D3D11 display mode to %s", enable_3d ? "3D" : "2D");
		return false;
	}
}

bool
leiasr_d3d11_supports_display_mode_switch(struct leiasr_d3d11 *leiasr)
{
	if (leiasr == nullptr) {
		return false;
	}

	return lens_present(leiasr);
}

bool
leiasr_d3d11_get_hardware_3d_state(struct leiasr_d3d11 *leiasr, bool *out_is_3d)
{
	if (leiasr == nullptr || !lens_present(leiasr) || out_is_3d == nullptr) {
		return false;
	}

	try {
		*out_is_3d = lens_is_enabled(leiasr);
		return true;
	} catch (...) {
		return false;
	}
}

} // extern "C"
