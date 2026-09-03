// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Leia SR D3D12 weaver wrapper implementation.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_sr_d3d12.h"
#include "leia_sr_api_select.h"
#include "leia_sr_v2_common.h"
#include "leia_sr_liveness.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/weaver/dx12weaver.h>
#include <sr/world/display/display.h>
#include <sr/sense/display/switchablehint.h>
#include <sr/utility/exception.h>

#ifdef DXR_LEIA_HAS_SR_V2
#include <sr/sr_dx12.h>
#include <sr/sr_weaver.h>
#endif

#include <d3d12.h>

#include <windows.h>
#include <sysinfoapi.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>

/*!
 * D3D12 SR weaver instance.
 */
struct leiasr_d3d12
{
	// SR SDK objects, v1 (legacy C++). NULL on the v2 path.
	SR::SRContext *context = nullptr;
	SR::IDX12Weaver1 *weaver = nullptr;
	SR::SwitchableLensHint *lens_hint = nullptr;

#ifdef DXR_LEIA_HAS_SR_V2
	// SR SDK objects, v2 (C99). NULL on the v1 path. `weaver_v2 != nullptr` is
	// the discriminant at every call site — see leia_sr_d3d11.cpp for why the
	// selector is consulted once, at create, rather than per call.
	SrInstance instance_v2 = nullptr;
	SrWeaver weaver_v2 = nullptr;
	SrLens lens_v2 = nullptr;
#endif

	// D3D12 resources (references, not owned)
	ID3D12Device *device = nullptr;
	ID3D12CommandQueue *command_queue = nullptr;

	// Current input texture info
	ID3D12Resource *input_resource = nullptr;
	uint32_t view_width = 0;
	uint32_t view_height = 0;
	DXGI_FORMAT input_format = DXGI_FORMAT_R8G8B8A8_UNORM;

	// Display dimensions in meters (for Kooima FOV calculation)
	float display_width_m = 0.0f;
	float display_height_m = 0.0f;
	bool display_dims_valid = false;

	// Display pixel resolution and screen position
	uint32_t display_pixel_width = 0;
	uint32_t display_pixel_height = 0;
	int32_t display_screen_left = 0;
	int32_t display_screen_top = 0;
	bool display_pixel_dims_valid = false;

	// --- Adaptive weave-latency estimation (D3D12), microseconds ------------
	// Same additive model + env knobs as the D3D11 arm (leia_sr_d3d11.cpp) and
	// VK arm (leia_sr.cpp): horizon = N_buffered * frame_interval + T_display,
	// pushed via setLatency() each weave. LEIA_D3D12_LATENCY_FRAMES=0 aligns
	// the predictor with the runtime's late-weave scheduling (DXR_LATE_WEAVE=1,
	// measured R = 16.65 ms on the D3D12 in-process path). Defaults reproduce
	// the SDK frames-heuristic values.
	bool     adaptive_latency_enabled = true;
	float    latency_frames_factor = 1.0f;
	uint64_t display_term_us = 16667;
	uint64_t latency_min_us = 5000;
	uint64_t latency_max_us = 60000;
	uint64_t latency_fixed_us = 0;
	double   latency_ema_alpha = 0.15;
	uint64_t prev_weave_ns = 0;
	double   ema_interval_ns = 0.0;
	uint64_t last_set_latency_us = 0;

	// Measured weave→scanout residual from the runtime's timing feedback
	// loop (xrt_display_processor set_frame_timing). When fresh, it
	// REPLACES the heuristic: exact per-path, per-panel-Hz horizon.
	uint64_t measured_r_us = 0;
	uint64_t measured_seen_ns = 0;
	double   measured_ema_us = 0.0;

	// #206: per-weave FORWARD horizon from the runtime's vsync-locked vblank
	// grid (set_predicted_scanout). When fresh it outranks both paths below
	// and is fed RAW — no EMA, no deadband (see the VK arm's note).
	uint64_t forward_horizon_us = 0;
	uint64_t forward_seen_ns = 0;
	bool     forward_logged = false;

	// --- #158 SR platform restart detection -------------------------------
	// Reporting only on this arm. Unlike D3D11 — which rebuilds its SDK
	// objects IN PLACE off a detached worker, behind the async_state gate every
	// entry point already honours — there is no async create/publish machinery
	// here, so there is no safe point at which to swap the weaver out from
	// under the render thread. A generation change is therefore reported as
	// STALE, and the remedy is the caller recreating the display processor.
	// TODO(#158 follow-up): port the D3D11 in-place reconnect.
	uint64_t platform_generation = 0;  //!< Incarnation at create (0 = unknown).
	bool warned_platform_down = false; //!< One-shot WARN edges: the poll runs at
	bool warned_client_skew = false;   //!< ~1 Hz forever, so a plain log would be
	bool warned_stale = false;         //!< per-frame-class bloat.

	// --- #215 async lens hint ---------------------------------------------
	// srLensEnable/disable (v2) and SwitchableLensHint::enable/disable (v1)
	// are round trips to the SR service process — ~tens of ms, and the runtime
	// calls request_display_mode from the compositor frame path under its own
	// mutex. The wish goes into a mailbox drained by a short-lived worker
	// instead: latest wish wins, so a burst of flips costs one SR call per
	// value the worker actually observes, and the caller never blocks.
	std::atomic<int> lens_mailbox{-1};         //!< latest wish wins: -1 none, 0 → 2D, 1 → 3D
	std::atomic<bool> lens_worker_busy{false}; //!< a worker owns the mailbox right now
	std::thread lens_thread;                   //!< the current/last worker, joined at destroy
	std::mutex lens_thread_mtx;                //!< guards lens_thread (spawn vs join)
	bool async_lens = true;                    //!< false ⟹ synchronous lens call (kill-switch)
};

namespace {

/*!
 * Create and wait for the SR context to become ready.
 */
bool
create_sr_context(double max_time, leiasr_d3d12 &sr)
{
	const double start_time = (double)GetTickCount64() / 1000.0;

	// Create SR context.
	while (sr.context == nullptr) {
		try {
			sr.context = SR::SRContext::create();
			break;
		} catch (SR::ServerNotAvailableException &e) {
			(void)e;
		}

		U_LOG_D("Waiting for SR context (D3D12)...");
		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (sr.context == nullptr) {
		U_LOG_E("Failed to create SR context within %.1f seconds", max_time);
		return false;
	}

	// Get display manager and wait for display to be ready.
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

				// Cache display dimensions in meters
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

				U_LOG_W("SR D3D12 display: %ldx%ld px, physical %.2fcm x %.2fcm = %.4fm x %.4fm",
				        (long)width, (long)height,
				        raw_width_cm, raw_height_cm,
				        sr.display_width_m, sr.display_height_m);

				break;
			}
		}

		U_LOG_D("Waiting for SR display (D3D12)...");
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
		U_LOG_W("SR D3D12 SwitchableLensHint created successfully");
	} catch (...) {
		sr.lens_hint = nullptr;
		U_LOG_W("SR D3D12 SwitchableLensHint not available on this display");
	}

	return true;
}

#ifdef DXR_LEIA_HAS_SR_V2
/*!
 * The v2 creation sequence. Mirrors the D3D11 arm exactly, including the
 * ordering constraint: `srInitialize` runs AFTER the weaver exists, because
 * initialisation starts the senses.
 *
 * Note the weaver is created from the DEVICE, not the command queue — same as
 * v1's `CreateDX12Weaver(context, device, hwnd, &weaver)`. The queue is kept on
 * the struct for the runtime's own use, not the SDK's.
 */
bool
create_v2(double max_time, void *hwnd, leiasr_d3d12 &sr)
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
	// NOTE: unlike the D3D11 arm this struct caches no recommended_* fields —
	// its v1 path never read them either, so leaving them unused keeps the two
	// paths matched. info carries them if this arm ever needs them.

	DPI_AWARENESS_CONTEXT oldDpiCtx = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	SrWeaverCreateInfoDX12 ci{};
	ci.sType = SR_TYPE_WEAVER_CREATE_INFO_DX12;
	ci.pNext = nullptr;
	ci.d3d12Device = sr.device;
	ci.window = (SrNativeWindowHandle)hwnd;

	const SrResult wr = srCreateWeaverDX12(sr.instance_v2, &ci, &sr.weaver_v2);

	if (oldDpiCtx != NULL) {
		SetThreadDpiAwarenessContext(oldDpiCtx);
	}

	if (!SR_SUCCEEDED(wr) || sr.weaver_v2 == nullptr) {
		U_LOG_E("srCreateWeaverDX12 failed: %s (%d)", leia_sr_v2_result_str(wr), (int)wr);
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

	srWeaverSetLatencyInFrames(sr.weaver_v2, 1);

	// Late latching deliberately NOT enabled on D3D12: the vendor's D3D12
	// weaver is a stub ({ /*Not implemented*/ }, and isLateLatchingEnabled
	// returns a hardcoded false). It needs the same submit hook Vulkan has —
	// D3D12 hands submission to the application too — and that hook does not
	// exist. Calling enable here would return SR_SUCCESS and do nothing.
	leia_sr_v2_create_lens(sr.instance_v2, &sr.lens_v2);

	U_LOG_W("SR D3D12 weaver created via the v2 C API");
	return true;
}
#endif // DXR_LEIA_HAS_SR_V2

/* ------------------------------------------------------------------ *
 * v1/v2 dispatch — one helper per operation. See leia_sr_d3d11.cpp.
 * ------------------------------------------------------------------ */

bool
w_ready(const leiasr_d3d12 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		return true;
	}
#endif
	return sr->weaver != nullptr;
}

bool
lens_present(const leiasr_d3d12 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		return sr->lens_v2 != nullptr;
	}
#endif
	return sr->lens_hint != nullptr;
}

void
lens_set(leiasr_d3d12 *sr, bool enable)
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
lens_is_enabled(leiasr_d3d12 *sr)
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

/*!
 * #215: drain the lens mailbox on our own thread, then exit. Latest wish wins
 * — a burst of flips collapses to however many values we actually observe.
 *
 * The whole body is wrapped in a catch-all: an exception escaping a
 * std::thread body terminates the process, and the SR SDK throws as routine
 * control flow.
 *
 * NOTE: unlike D3D11 there is no #158 in-place reconnect worker on this arm
 * (a generation change is reported STALE and the DP is recreated), so the only
 * other toucher of the SDK objects is destroy — and that joins this worker
 * before it frees anything. lens_present() plus the try/catch still guards the
 * window between a NULL-ing and our next call.
 */
void
lens_worker_body(leiasr_d3d12 *sr)
{
	try {
		for (;;) {
			int wish = sr->lens_mailbox.exchange(-1, std::memory_order_acq_rel);
			while (wish >= 0) {
				LARGE_INTEGER t0 = {}, t1 = {}, freq = {};
				QueryPerformanceFrequency(&freq);
				QueryPerformanceCounter(&t0);
				if (lens_present(sr)) {
					try {
						lens_set(sr, wish == 1);
						QueryPerformanceCounter(&t1);
						const double ms =
						    freq.QuadPart > 0
						        ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
						              (double)freq.QuadPart
						        : 0.0;
						U_LOG_W("SR D3D12 display mode switched to %s (async lens, SR call %.1f ms)",
						        wish == 1 ? "3D" : "2D", ms);
					} catch (...) {
						U_LOG_E("Failed to switch SR D3D12 display mode to %s (async lens)",
						        wish == 1 ? "3D" : "2D");
					}
				}
				wish = sr->lens_mailbox.exchange(-1, std::memory_order_acq_rel);
			}

			sr->lens_worker_busy.store(false, std::memory_order_release);

			// Close the race: a wish posted after our last exchange and
			// before busy dropped saw busy == true and did not spawn.
			if (sr->lens_mailbox.load(std::memory_order_acquire) < 0) {
				break;
			}
			if (sr->lens_worker_busy.exchange(true, std::memory_order_acq_rel)) {
				break; // a poster took over — it owns the mailbox now
			}
		}
	} catch (...) {
		// Nothing may escape a thread body. Drop the claim so the next wish
		// can spawn a replacement worker.
		sr->lens_worker_busy.store(false, std::memory_order_release);
		U_LOG_E("SR D3D12 async lens worker aborted on an exception");
	}
}

/*!
 * #215: join whatever lens worker is still running and empty the mailbox.
 * Bounded by one in-flight SR call.
 */
void
lens_worker_join(leiasr_d3d12 *sr)
{
	std::lock_guard<std::mutex> g(sr->lens_thread_mtx);
	if (sr->lens_thread.joinable()) {
		sr->lens_thread.join();
	}
	sr->lens_mailbox.store(-1, std::memory_order_release);
}

void
w_set_latency(leiasr_d3d12 *sr, uint64_t latency_us)
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
w_set_latency_in_frames(leiasr_d3d12 *sr, uint64_t frames)
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
w_set_output_format(leiasr_d3d12 *sr, DXGI_FORMAT fmt)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetOutputFormatDX12(sr->weaver_v2, (int32_t)fmt);
		return;
	}
#endif
	sr->weaver->setOutputFormat(fmt);
}

void
w_set_input_texture(leiasr_d3d12 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetInputTextureDX12(sr->weaver_v2, sr->input_resource, (int32_t)sr->view_width,
		                            (int32_t)sr->view_height, (int32_t)sr->input_format);
		return;
	}
#endif
	sr->weaver->setInputViewTexture(sr->input_resource, static_cast<int>(sr->view_width),
	                                static_cast<int>(sr->view_height), sr->input_format);
}

void
w_set_command_list(leiasr_d3d12 *sr, ID3D12GraphicsCommandList *cmd_list)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetCommandListDX12(sr->weaver_v2, cmd_list);
		return;
	}
#endif
	sr->weaver->setCommandList(cmd_list);
}

//! v2 takes scalars where v1 takes a D3D12_VIEWPORT; the caller keeps the
//! struct anyway because it also feeds RSSetViewports on the command list.
void
w_set_viewport(leiasr_d3d12 *sr, const D3D12_VIEWPORT &vp)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetViewportDX12(sr->weaver_v2, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth,
		                        vp.MaxDepth);
		return;
	}
#endif
	sr->weaver->setViewport(vp);
}

void
w_set_scissor(leiasr_d3d12 *sr, const D3D12_RECT &rc)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetScissorRectDX12(sr->weaver_v2, (int32_t)rc.left, (int32_t)rc.top, (int32_t)rc.right,
		                           (int32_t)rc.bottom);
		return;
	}
#endif
	sr->weaver->setScissorRect(rc);
}

void
w_weave(leiasr_d3d12 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverWeave(sr->weaver_v2);
		return;
	}
#endif
	/*
	 * v1 throws as routine internal control flow, and the caller above us is
	 * the runtime's C compositor, which cannot catch. An exception escaping a
	 * per-frame display-processor method is std::terminate -- the whole app, or
	 * the whole service, gone with no WER record. Every other SR SDK call in
	 * this file is already wrapped; weave() was the one that was not.
	 *
	 * Dropping the frame is the right degradation: the target keeps whatever was
	 * last presented, and the next frame retries. The log is deliberately NOT
	 * per-frame -- a throwing weaver throws every frame, and a per-frame U_LOG
	 * would bury the log it is supposed to help you read.
	 *
	 * The v2 branch above needs no guard: srWeaverWeave catches internally
	 * (SR_CATCH_ALL in the SR runtime) and reports through SrResult instead.
	 */
	try {
		sr->weaver->weave();
	} catch (...) {
		static uint64_t throws;
		if ((throws++ % 600) == 0) {
			U_LOG_E("SR D3D12 v1 weave() threw -- frame not woven (throw #%llu; "
			        "logged 1-in-600 to stay off the per-frame path)",
			        (unsigned long long)throws);
		}
	}
}

bool
w_get_predicted_eyes(leiasr_d3d12 *sr, float left_mm[3], float right_mm[3])
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
 * The arm's v2 `SrInstance` as an opaque pointer for the leia_sr_liveness_*
 * entry points, or NULL on the v1 path (and on a build without the v2 SDK,
 * where the member does not exist at all).
 *
 * Every liveness call site MUST pass this, stamp sites included: mixing an
 * SDK-id stamp with an SCM-derived poll would read as a restart that never
 * happened.
 */
void *
v2_instance_of(const struct leiasr_d3d12 *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	return sr != nullptr ? static_cast<void *>(sr->instance_v2) : nullptr;
#else
	(void)sr;
	return nullptr;
#endif
}

} // namespace

extern "C" {

xrt_result_t
leiasr_d3d12_create(double max_time,
                    void *d3d12_device,
                    void *d3d12_command_queue,
                    void *hwnd,
                    uint32_t view_width,
                    uint32_t view_height,
                    struct leiasr_d3d12 **out)
{
	if (d3d12_device == nullptr) {
		U_LOG_E("D3D12 device is null");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	leiasr_d3d12 *sr = new leiasr_d3d12;
	sr->device = static_cast<ID3D12Device *>(d3d12_device);
	sr->command_queue = static_cast<ID3D12CommandQueue *>(d3d12_command_queue);
	sr->view_width = view_width;
	sr->view_height = view_height;

#ifdef DXR_LEIA_HAS_SR_V2
	if (leia_sr_api_selected() == LEIA_SR_API_V2) {
		// No fallback to v1 on failure — see leia_sr_d3d11.cpp for why a
		// silent retry on the other path is worse than a clear error.
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

		// Create D3D12 weaver — set DPI awareness so the SDK sees physical pixels
		// when it queries the HWND. See LeiaInc/LeiaSR@a8a9fb9 for the pattern.
		DPI_AWARENESS_CONTEXT oldDpiCtx =
		    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		WeaverErrorCode result = SR::CreateDX12Weaver(sr->context,
		                                               sr->device,
		                                               static_cast<HWND>(hwnd),
		                                               &sr->weaver);
		if (oldDpiCtx != NULL) {
			SetThreadDpiAwarenessContext(oldDpiCtx);
		}
		if (result != WeaverErrorCode::WeaverSuccess) {
			U_LOG_E("Failed to create SR D3D12 weaver: %d", (int)result);
			SR::SRContext::deleteSRContext(sr->context);
			delete sr;
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}

		// Initialize the context after creating the weaver.
		sr->context->initialize();

		// Set default latency (1 frame). Fallback for
		// LEIA_D3D12_ADAPTIVE_LATENCY=0; the adaptive per-frame setLatency()
		// below overrides it (setLatency disables the SDK's frames mode).
		sr->weaver->setLatencyInFrames(1);
	}

	// EVERYTHING BELOW IS SHARED BY BOTH PATHS. The adaptive-latency block is
	// runtime policy, not SDK plumbing — an early return from the v2 branch
	// would silently leave v2 on the SDK default and present as "v2 is slower".

	// Adaptive-latency knobs, mirroring LEIA_D3D11_* / LEIA_VK_*.
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

		const char *en = std::getenv("LEIA_D3D12_ADAPTIVE_LATENCY");
		sr->adaptive_latency_enabled = !(en != nullptr && en[0] == '0');
		// Heuristic fallback keeps the classic 1-frame default: paced paths now
		// get their horizon from the runtime's MEASURED feed (set_frame_timing),
		// so the heuristic only serves never-measured paths (e.g. VK DComp
		// transparent) — where pre-late-weave pacing still applies and N=1 is
		// the correct shape. Replaces the interim DXR_LATE_WEAVE env coupling.
		sr->latency_frames_factor = getf("LEIA_D3D12_LATENCY_FRAMES", 1.0f);
		sr->latency_min_us = getu("LEIA_D3D12_LATENCY_MIN_US", 5000);
		sr->latency_max_us = getu("LEIA_D3D12_LATENCY_MAX_US", 60000);
		sr->latency_fixed_us = getu("LEIA_D3D12_LATENCY_FIXED_US", 0);
		float a = getf("LEIA_D3D12_LATENCY_EMA_ALPHA", 0.15f);
		sr->latency_ema_alpha = a < 0.01f ? 0.01f : (a > 1.0f ? 1.0f : a);

		float panel_hz = getf("LEIA_D3D12_PANEL_HZ", 60.0f);
		uint64_t disp_default = (panel_hz > 1.0f) ? (uint64_t)(1.0e6 / panel_hz + 0.5) : 16667;
		sr->display_term_us = getu("LEIA_D3D12_LATENCY_DISPLAY_US", disp_default);

		if (sr->latency_fixed_us > 0) {
			w_set_latency(sr, sr->latency_fixed_us);
			sr->last_set_latency_us = sr->latency_fixed_us;
			sr->adaptive_latency_enabled = false;
			U_LOG_W("Leia D3D12 weave latency: FIXED %llu us (adaptive disabled)",
			        (unsigned long long)sr->latency_fixed_us);
		} else if (sr->adaptive_latency_enabled) {
			U_LOG_W("Leia D3D12 weave latency: ADAPTIVE (horizon = %.2f x frame_interval + %llu us display, clamp %llu..%llu us, alpha %.2f)",
			        (double)sr->latency_frames_factor,
			        (unsigned long long)sr->display_term_us,
			        (unsigned long long)sr->latency_min_us,
			        (unsigned long long)sr->latency_max_us, sr->latency_ema_alpha);
		} else {
			U_LOG_W("Leia D3D12 weave latency: SDK frames-based default (adaptive off)");
		}
	}

	// #158: stamp the SR platform incarnation we connected to, so a later poll
	// can tell "the platform restarted" from "the viewer is holding still".
	sr->platform_generation = leia_sr_liveness_platform_generation_ex(v2_instance_of(sr));

	*out = sr;

	U_LOG_I("Created D3D12 SR weaver for HWND %p, view size %ux%u", hwnd, view_width, view_height);

	return XRT_SUCCESS;
}

void
leiasr_d3d12_destroy(struct leiasr_d3d12 **leiasr_ptr)
{
	if (leiasr_ptr == nullptr || *leiasr_ptr == nullptr) {
		return;
	}

	leiasr_d3d12 *sr = *leiasr_ptr;

	// #215: the async lens worker holds a raw sr pointer — it must be gone
	// before the SDK objects it calls into are. Ahead of the v2 early-return
	// below so BOTH teardown paths are covered. On the caller's thread, but
	// bounded by one in-flight SR call (the mailbox is emptied, so the worker
	// cannot pick up more work); this arm has no async destroy to hide it in.
	lens_worker_join(sr);

#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		// srDestroyLens frees the caller-owned handle WRAPPER only; the hint
		// underneath stays owned by the context. So unlike v1 (where deleting
		// is a double-free) this must be called, and there is no hazard in
		// calling it. Full rationale in leia_sr_d3d11.cpp.
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
		U_LOG_I("Destroyed D3D12 SR weaver (v2)");
		return;
	}
#endif

	// SwitchableLensHint is managed by SRContext — do NOT delete it manually.
	sr->lens_hint = nullptr;

	// Destroy weaver
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

	U_LOG_I("Destroyed D3D12 SR weaver");
}

void
leiasr_d3d12_set_output_format(struct leiasr_d3d12 *leiasr, uint32_t format)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return;
	}

	DXGI_FORMAT dxgi_format = static_cast<DXGI_FORMAT>(format);
	w_set_output_format(leiasr, dxgi_format);
	U_LOG_W("SR D3D12 weaver output format set to %u", (unsigned)format);
}

void
leiasr_d3d12_set_input_texture(struct leiasr_d3d12 *leiasr,
                               void *stereo_resource,
                               uint32_t view_width,
                               uint32_t view_height,
                               uint32_t format)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return;
	}

	// Skip if nothing changed — match reference pattern of calling setInputViewTexture once
	if (leiasr->input_resource == static_cast<ID3D12Resource *>(stereo_resource) &&
	    leiasr->view_width == view_width &&
	    leiasr->view_height == view_height &&
	    leiasr->input_format == static_cast<DXGI_FORMAT>(format)) {
		return;
	}

	U_LOG_I("SR D3D12 weaver setInputViewTexture: view=%ux%u", view_width, view_height);

	leiasr->input_resource = static_cast<ID3D12Resource *>(stereo_resource);
	leiasr->view_width = view_width;
	leiasr->view_height = view_height;
	leiasr->input_format = static_cast<DXGI_FORMAT>(format);

	// Configure the weaver with the input texture
	// SR SDK DX12 weaver takes ID3D12Resource*, width, height, format
w_set_input_texture(leiasr);
}

// Records SR SDK weave commands onto `command_list` constrained to
// (viewport_x, viewport_y, viewport_width, viewport_height) of the bound RTV.
//
// Gotcha: the SR SDK D3D12 weaver's setViewport/setScissorRect APIs are used
// internally for phase calculation only — they do NOT call RSSetViewports or
// RSSetScissorRects on the cmd list. We must set both on the cmd list
// ourselves before weave(), or the woven output lands at the cmd list's
// default viewport (full RT) instead of the canvas sub-rect. The D3D11 path
// gets this for free because the SDK D3D11 weaver reads RSGetViewports from
// the immediate context. Removing the RSSetViewports/RSSetScissorRects calls
// below will reintroduce the canvas-subrect black-screen bug.
void
leiasr_d3d12_set_frame_timing(struct leiasr_d3d12 *leiasr,
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
leiasr_d3d12_set_predicted_scanout(struct leiasr_d3d12 *leiasr, uint64_t predicted_weave_to_scanout_ns)
{
	if (leiasr == nullptr) {
		return;
	}
	leiasr->forward_horizon_us = predicted_weave_to_scanout_ns / 1000;
	if (predicted_weave_to_scanout_ns > 0) {
		leiasr->forward_seen_ns = os_monotonic_get_ns();
	}
}

void
leiasr_d3d12_weave(struct leiasr_d3d12 *leiasr,
                   void *command_list,
                   int32_t viewport_x,
                   int32_t viewport_y,
                   uint32_t viewport_width,
                   uint32_t viewport_height)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		U_LOG_W("leiasr_d3d12_weave called with null instance or weaver");
		return;
	}

	ID3D12GraphicsCommandList *cmd_list = static_cast<ID3D12GraphicsCommandList *>(command_list);

	// Adaptive weave latency: same model and deadband as the D3D11/VK arms —
	// push the estimated motion-to-photon horizon via setLatency() BEFORE
	// weave() so this frame's eye prediction uses it.
	if (leiasr->adaptive_latency_enabled && leiasr->latency_fixed_us == 0) {
		const uint64_t now_ns = os_monotonic_get_ns();
		if (leiasr->prev_weave_ns != 0) {
			const uint64_t dt_ns = now_ns - leiasr->prev_weave_ns;
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

		// #206: the runtime's per-weave FORWARD horizon outranks everything
		// below — exact for THIS weave, fed RAW (no EMA, no deadband).
		// Freshness 100 ms: recomputed per weave; older = feed stopped.
		const bool forward_fresh = leiasr->forward_horizon_us > 0 &&
		                           (now_ns - leiasr->forward_seen_ns) < 100ULL * 1000 * 1000;
		// Prefer the runtime's MEASURED weave→scanout residual (timing
		// feedback loop) when fresh; heuristic only as fallback.
		const bool measured_fresh = leiasr->measured_r_us > 0 &&
		                            (now_ns - leiasr->measured_seen_ns) < 250ULL * 1000 * 1000;
		if (forward_fresh) {
			uint64_t latency_us = leiasr->forward_horizon_us;
			if (latency_us < leiasr->latency_min_us) {
				latency_us = leiasr->latency_min_us;
			}
			if (latency_us > leiasr->latency_max_us) {
				latency_us = leiasr->latency_max_us;
			}
			w_set_latency(leiasr, latency_us);
			leiasr->last_set_latency_us = latency_us;
			if (!leiasr->forward_logged) {
				leiasr->forward_logged = true;
				U_LOG_W("Leia D3D12 weave latency: #206 FORWARD per-weave horizon engaged "
				        "(%llu us this weave; raw, no smoothing, no deadband)",
				        (unsigned long long)latency_us);
			}
		} else if (measured_fresh || leiasr->ema_interval_ns > 0.0) {
			double horizon_us;
			if (measured_fresh) {
				// One-shot lifecycle log: the horizon source flipped from
				// heuristic to runtime-measured (set_frame_timing loop).
				if (leiasr->measured_ema_us <= 0.0) {
					U_LOG_W("Leia D3D12 weave latency: MEASURED horizon engaged (%llu us, display term %llu us)",
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

			const uint64_t prev = leiasr->last_set_latency_us;
			const uint64_t diff = latency_us > prev ? latency_us - prev : prev - latency_us;
			if (prev == 0 || diff >= 250) {
				w_set_latency(leiasr, latency_us);
				if (prev == 0 || diff >= 2000) {
					U_LOG_I("Leia D3D12 adaptive latency: %llu us (%.2f ms/frame ~ %.0f fps; %.2f x iv + %llu us disp)",
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

	// Diagnostic: log weave parameters periodically
	static uint32_t weave_counter = 0;
	bool weave_log = (weave_counter % 60 == 0);
	weave_counter++;
	if (weave_log) {
		U_LOG_I("SR D3D12 weave: cmd_list=%p, vp=(%d,%d %ux%u), input=%p (%ux%u fmt=%u)",
		        (void *)cmd_list, viewport_x, viewport_y, viewport_width, viewport_height,
		        (void *)leiasr->input_resource,
		        leiasr->view_width, leiasr->view_height,
		        (unsigned)leiasr->input_format);
	}

	// Set command list for the weaver to record commands onto
	w_set_command_list(leiasr, cmd_list);

	// #740: the weaver's setViewport feeds BOTH the phase term
	//   xOffset = window_WeavingX + vpX
	// and (with the scissor below) where the woven pixels land in the target.
	// Those two uses coincide because vpX/vpY are window-global: this DP weaves
	// ONCE per frame over the whole bound window, so the only offset the term
	// can carry is the content's origin within that window, which is (0,0) by
	// construction. The runtime agrees from the other side —
	// d3d12_effective_canvas() returns x=0,y=0,w/h=client for every zones /
	// mask / Local2D frame.
	//
	// Do NOT reintroduce a phase-only offset here. v2.0.1 did (#95), on the
	// premise that SR anchors window_WeavingX to the app's top-level container
	// and a WS_CHILD pane therefore needed the pane-vs-container delta added
	// back. That premise is refuted: SR phases from the HWND handed to
	// CreateDX12Weaver, and the D3D11 DP — which never had the correction —
	// exhibits the same docked behaviour. All the offset achieved was an error
	// of (pane_offset mod lens pitch): a uniform random phase per dock layout,
	// i.e. a coin flip on eye assignment.
	D3D12_VIEWPORT viewport = {};
	viewport.TopLeftX = static_cast<float>(viewport_x);
	viewport.TopLeftY = static_cast<float>(viewport_y);
	viewport.Width = static_cast<float>(viewport_width);
	viewport.Height = static_cast<float>(viewport_height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	w_set_viewport(leiasr, viewport);

	// Set scissor rect to match the RENDER viewport sub-rect.
	D3D12_RECT scissor = {};
	scissor.left = static_cast<LONG>(viewport_x);
	scissor.top = static_cast<LONG>(viewport_y);
	scissor.right = static_cast<LONG>(viewport_x) + static_cast<LONG>(viewport_width);
	scissor.bottom = static_cast<LONG>(viewport_y) + static_cast<LONG>(viewport_height);
	w_set_scissor(leiasr, scissor);

	// Also set viewport + scissor on the command list itself. The SR SDK
	// weaver records draw commands but does not call RSSetViewports/Scissor
	// on the cmd list — so without this, the canvas sub-rect is ignored
	// and the woven output lands at the cmd list's default viewport
	// (typically full target). D3D11 path gets this for free because
	// the DP sets RSSetViewports on the immediate context before weave().
	cmd_list->RSSetViewports(1, &viewport);
	cmd_list->RSSetScissorRects(1, &scissor);

	// Perform weaving — records draw commands onto the command list.
	// Set DPI awareness so any internal GetClientRect returns physical pixels.
	DPI_AWARENESS_CONTEXT oldDpiCtx =
	    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	w_weave(leiasr);
	if (oldDpiCtx != NULL) {
		SetThreadDpiAwarenessContext(oldDpiCtx);
	}
}

bool
leiasr_d3d12_get_predicted_eye_positions(struct leiasr_d3d12 *leiasr,
                                         float out_left_eye[3],
                                         float out_right_eye[3])
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return false;
	}

	// On v1 the SR SDK throws std::runtime_error ~per frame from inside this
	// call as routine internal control flow; the catch keeps it from crossing
	// the C ABI. See [[feedback_leia_eye_pos_throws_intrinsic]]. On v2 the same
	// condition arrives as an SrResult. Both handled inside the helper.
	float left_mm[3], right_mm[3];
	if (!w_get_predicted_eyes(leiasr, left_mm, right_mm)) {
		return false;
	}

	// Convert to meters
	out_left_eye[0] = left_mm[0] / 1000.0f;
	out_left_eye[1] = left_mm[1] / 1000.0f;
	out_left_eye[2] = left_mm[2] / 1000.0f;
	out_right_eye[0] = right_mm[0] / 1000.0f;
	out_right_eye[1] = right_mm[1] / 1000.0f;
	out_right_eye[2] = right_mm[2] / 1000.0f;

	return true;
}

bool
leiasr_d3d12_get_display_dimensions(struct leiasr_d3d12 *leiasr,
                                    struct leiasr_display_dimensions *out_dims)
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
leiasr_d3d12_get_display_pixel_info(struct leiasr_d3d12 *leiasr,
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
leiasr_d3d12_supports_display_mode_switch(struct leiasr_d3d12 *leiasr)
{
	if (leiasr == nullptr) {
		return false;
	}

	return lens_present(leiasr);
}

void
leiasr_d3d12_set_async_lens(struct leiasr_d3d12 *leiasr, bool enabled)
{
	if (leiasr == nullptr) {
		return;
	}

	// Set once, at DP create, before any frame can call request_display_mode
	// — so a plain store is enough; no reader can be in flight yet.
	leiasr->async_lens = enabled;
}

bool
leiasr_d3d12_request_display_mode(struct leiasr_d3d12 *leiasr, bool enable_3d)
{
	if (leiasr == nullptr) {
		return false;
	}

	if (!leiasr->async_lens) {
		if (!lens_present(leiasr)) {
			return false;
		}

		try {
			LARGE_INTEGER t0 = {}, t1 = {}, freq = {};
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&t0);
			lens_set(leiasr, enable_3d);
			QueryPerformanceCounter(&t1);
			const double ms = freq.QuadPart > 0
			                      ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart
			                      : 0.0;
			// The caller's thread paid for this call — say how much (#215).
			U_LOG_W("SR D3D12 display mode switched to %s (sync lens, SR call %.1f ms)",
			        enable_3d ? "3D" : "2D", ms);
			return true;
		} catch (...) {
			U_LOG_E("Failed to switch SR D3D12 display mode to %s", enable_3d ? "3D" : "2D");
			return false;
		}
	}

	// #215: the SR call is a round trip to the service process and the runtime
	// gets here from the compositor frame path under its own mutex. Post the
	// wish and let a short-lived worker make the call.
	if (!lens_present(leiasr)) {
		return false;
	}

	leiasr->lens_mailbox.store(enable_3d ? 1 : 0, std::memory_order_release);
	if (!leiasr->lens_worker_busy.exchange(true, std::memory_order_acq_rel)) {
		std::lock_guard<std::mutex> g(leiasr->lens_thread_mtx);
		if (leiasr->lens_thread.joinable()) {
			// The previous worker has already dropped busy → it is on its way
			// out; the join is ~immediate.
			leiasr->lens_thread.join();
		}
		try {
			leiasr->lens_thread = std::thread([leiasr]() { lens_worker_body(leiasr); });
		} catch (...) {
			// Thread creation failed: fall back to synchronous, drop busy.
			leiasr->lens_worker_busy.store(false, std::memory_order_release);
			int wish = leiasr->lens_mailbox.exchange(-1, std::memory_order_acq_rel);
			if (wish >= 0) {
				try {
					lens_set(leiasr, wish == 1);
				} catch (...) {
					return false;
				}
			}
		}
	}

	U_LOG_W("SR D3D12 display mode wish (%s) queued — async lens worker (#215)",
	        enable_3d ? "3D" : "2D");
	return true; // a request API — acceptance, not physical completion
}

bool
leiasr_d3d12_get_hardware_3d_state(struct leiasr_d3d12 *leiasr, bool *out_is_3d)
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

uint32_t
leiasr_d3d12_poll_backend_state(struct leiasr_d3d12 *leiasr)
{
	if (leiasr == nullptr) {
		return LEIA_SR_BACKEND_OK;
	}

	// No weaver at all — the DP already degrades on its own NULL checks, and
	// there is no connection whose generation could have changed.
	if (!(w_ready(leiasr))) {
		return LEIA_SR_BACKEND_DEGRADED;
	}

	// One source for the whole poll: the SDK-reported platform id when this arm
	// has a v2 instance and the SDK is new enough, the SCM probe otherwise
	// (leia_sr_liveness.h).
	void *const v2 = v2_instance_of(leiasr);
	const uint64_t gen = leia_sr_liveness_platform_generation_ex(v2);

	// #169: taken BEFORE the "platform down" / "generation changed"
	// early-returns below — an upgrade trips both of those, so after them this
	// check never ran on the only path it describes. TELEMETRY ONLY: it logs
	// the upgrade window and deliberately leaves the reported state alone. The
	// skew is transient (it clears once a weaver is rebuilt on the new images),
	// and the #169 crash it was briefly gated on was a race against the
	// installer's file-replacement window, not a durable skew — see
	// leia_sr_d3d11.cpp's poll for the full reasoning and the hardware evidence.
	if (!leia_sr_liveness_client_matches_platform()) {
		if (!leiasr->warned_client_skew) {
			leiasr->warned_client_skew = true;
			U_LOG_W("SR platform was upgraded while this process was running (D3D12 arm) — the "
			        "mapped SR client DLLs no longer match the installed set. This is "
			        "expected to clear once the weaver is rebuilt; if tracking does not come "
			        "back after that, restart the DisplayXR service.");
		}
	} else {
		leiasr->warned_client_skew = false;
	}

	if (gen == 0) {
		// Platform down. The weaver keeps weaving on the last known eye
		// positions, which beats no picture — do not tear anything down; the
		// generation check below fires once the service is back.
		if (!leiasr->warned_platform_down) {
			leiasr->warned_platform_down = true;
			U_LOG_W("SR platform is DOWN (D3D12 arm) — weaving untracked");
		}
		return LEIA_SR_BACKEND_DEGRADED;
	}
	leiasr->warned_platform_down = false;

	// The SDK's own LIVENESS signal — independent of the identity comparison
	// below, which the vendor expects to keep as the belt-and-braces backup. A
	// no-op unless the SDK is new enough (DXR_LEIA_HAS_SR_CONNECTION_STATE).
	// DISCONNECTED is terminal for the instance, and this arm cannot rebuild in
	// place, so it is exactly the STALE verdict.
	//
	// TODO(#158 follow-up): SR_ERROR_PLATFORM_DISCONNECTED can also come back
	// from the per-frame eye-position path; deliberately not wired there in this
	// change — this 1 Hz poll covers it and the hot path stays untouched.
	if (leia_sr_liveness_connection_is_dead(v2)) {
		if (!leiasr->warned_stale) {
			leiasr->warned_stale = true;
			U_LOG_W("SR platform connection reported DEAD by the SDK (D3D12 arm) — the "
			        "weaver is STALE and cannot rebuild in place; recreate the display "
			        "processor (or restart the DisplayXR service) to recover eye tracking");
		}
		return LEIA_SR_BACKEND_STALE;
	}

	// A zero stamp means the token was never readable at create time, so
	// detection is simply off rather than firing on "unknown -> known".
	if (leiasr->platform_generation != 0 && gen != leiasr->platform_generation) {
		if (!leiasr->warned_stale) {
			leiasr->warned_stale = true;
			U_LOG_W("SR platform restarted (generation %llu -> %llu) — the D3D12 weaver "
			        "is STALE and cannot rebuild in place; recreate the display processor "
			        "(or restart the DisplayXR service) to recover eye tracking",
			        (unsigned long long)leiasr->platform_generation, (unsigned long long)gen);
		}
		return LEIA_SR_BACKEND_STALE;
	}

	// The skew check that used to sit here is now taken right after the
	// generation read — see the #169 block above. It logs; it never changes
	// the state reported from here.

	return LEIA_SR_BACKEND_OK;
}

} // extern "C"
