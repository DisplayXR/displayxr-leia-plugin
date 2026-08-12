// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Leia SR OpenGL weaver wrapper implementation.
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_sr_gl.h"
#include "leia_sr_api_select.h"
#include "leia_sr_v2_common.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/weaver/glweaver.h>
#include <sr/world/display/display.h>
#include <sr/sense/display/switchablehint.h>
#include <sr/utility/exception.h>

#ifdef DXR_LEIA_HAS_SR_V2
#include <sr/sr_gl.h>
#include <sr/sr_weaver.h>
#endif

#include <windows.h>
#include <sysinfoapi.h>

#include <cmath>

/*!
 * GL SR weaver instance.
 */
struct leiasr_gl
{
	// SR SDK objects
	SR::SRContext *context = nullptr;
	SR::IGLWeaver1 *weaver = nullptr;
	SR::SwitchableLensHint *lens_hint = nullptr;

#ifdef DXR_LEIA_HAS_SR_V2
	// v2 (C99) objects; NULL on the v1 path. `weaver_v2 != nullptr` is the
	// discriminant — see leia_sr_d3d11.cpp.
	SrInstance instance_v2 = nullptr;
	SrWeaver weaver_v2 = nullptr;
	SrLens lens_v2 = nullptr;
#endif

	// Current input texture info
	uint32_t input_texture = 0;
	uint32_t view_width = 0;
	uint32_t view_height = 0;
	uint32_t input_format = 0; // GL_RGBA8

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
};

namespace {

/*!
 * Create and wait for the SR context to become ready.
 */
bool
create_sr_context(double max_time, leiasr_gl &sr)
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

		U_LOG_D("Waiting for SR context (GL)...");
		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (sr.context == nullptr) {
		U_LOG_E("Failed to create SR context (GL) within %.1f seconds", max_time);
		return false;
	}

	// Get display manager and wait for display to be ready.
	SR::IDisplayManager *displayManager = nullptr;
	SR::IDisplay *display = nullptr;
	bool display_ready = false;

	try {
		displayManager = SR::GetDisplayManagerInstance(*sr.context);
		if (displayManager == nullptr) {
			U_LOG_E("Failed to get SR DisplayManager instance (GL)");
			return false;
		}
	} catch (...) {
		U_LOG_E("Exception getting SR DisplayManager (GL) - requires runtime 1.34.8-RC1+");
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

				float raw_width_cm = display->getPhysicalSizeWidth();
				float raw_height_cm = display->getPhysicalSizeHeight();
				sr.display_width_m = raw_width_cm / 100.0f;
				sr.display_height_m = raw_height_cm / 100.0f;
				sr.display_dims_valid = true;

				sr.display_pixel_width = static_cast<uint32_t>(width);
				sr.display_pixel_height = static_cast<uint32_t>(height);
				sr.display_screen_left = static_cast<int32_t>(display_location.left);
				sr.display_screen_top = static_cast<int32_t>(display_location.top);
				sr.display_pixel_dims_valid = true;

				U_LOG_W("SR GL display: %ldx%ld px, physical %.2fcm x %.2fcm = %.4fm x %.4fm",
				        (long)width, (long)height,
				        raw_width_cm, raw_height_cm,
				        sr.display_width_m, sr.display_height_m);

				break;
			}
		}

		U_LOG_D("Waiting for SR display (GL)...");
		Sleep(100);

		double cur_time = (double)GetTickCount64() / 1000.0;
		if ((cur_time - start_time) > max_time) {
			break;
		}
	}

	if (!display_ready) {
		U_LOG_E("SR display not ready (GL) within %.1f seconds", max_time);
		return false;
	}

	// Create SwitchableLensHint for 2D/3D mode switching
	try {
		sr.lens_hint = SR::SwitchableLensHint::create(*sr.context);
		U_LOG_W("SR GL SwitchableLensHint created successfully");
	} catch (...) {
		sr.lens_hint = nullptr;
		U_LOG_W("SR GL SwitchableLensHint not available on this display");
	}

	return true;
}

#ifdef DXR_LEIA_HAS_SR_V2
/*!
 * The v2 creation sequence for the GL arm.
 *
 * Unlike D3D11/D3D12 the create-info carries no device or context: the SDK
 * uses the GL context CURRENT ON THIS THREAD, exactly as v1's
 * `CreateGLWeaver(context, hwnd, &weaver)` does. So the caller's threading
 * contract is unchanged by the migration — whatever had to be current before
 * still has to be current now.
 */
bool
create_v2(double max_time, void *hwnd, leiasr_gl &sr)
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

	SrWeaverCreateInfoGL ci{};
	ci.sType = SR_TYPE_WEAVER_CREATE_INFO_GL;
	ci.pNext = nullptr;
	ci.window = (SrNativeWindowHandle)hwnd;

	const SrResult wr = srCreateWeaverGL(sr.instance_v2, &ci, &sr.weaver_v2);
	if (!SR_SUCCEEDED(wr) || sr.weaver_v2 == nullptr) {
		U_LOG_E("srCreateWeaverGL failed: %s (%d)", leia_sr_v2_result_str(wr), (int)wr);
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

	// Late latching: automatic on this backend. It submits implicitly as it
	// records, so the weaver places its own completion marker inside weave()
	// (D3D11_QUERY_EVENT / glFenceSync) and needs nothing from us — unlike
	// Vulkan, where the compositor owns the submit and must call
	// srWeaverWeaveSubmittedVulkan.
	//
	// Read the EFFECTIVE state back rather than trusting the enable: the
	// unimplemented backends return a hardcoded false, and a live one clears
	// the flag itself on failure. Believing the enable is how a latency
	// predictor stands down in favour of a latch that never runs.
	if (SR_SUCCEEDED(srWeaverEnableLateLatching(sr.weaver_v2, SR_TRUE))) {
		SrBool32 ll = SR_FALSE;
		if (SR_SUCCEEDED(srWeaverIsLateLatchingEnabled(sr.weaver_v2, &ll))) {
			U_LOG_W("SR %s late latching: %s", "GL",
			        ll == SR_TRUE ? "ENABLED (effective)" : "declined by the backend");
		}
	}
	leia_sr_v2_create_lens(sr.instance_v2, &sr.lens_v2);

	U_LOG_W("SR GL weaver created via the v2 C API");
	return true;
}
#endif // DXR_LEIA_HAS_SR_V2

/* ------------------------------------------------------------------ *
 * v1/v2 dispatch — see leia_sr_d3d11.cpp.
 * ------------------------------------------------------------------ */

bool
w_ready(const leiasr_gl *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		return true;
	}
#endif
	return sr->weaver != nullptr;
}

bool
lens_present(const leiasr_gl *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		return sr->lens_v2 != nullptr;
	}
#endif
	return sr->lens_hint != nullptr;
}

void
lens_set(leiasr_gl *sr, bool enable)
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
lens_is_enabled(leiasr_gl *sr)
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

void
w_set_input_texture(leiasr_gl *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverSetInputTextureGL(sr->weaver_v2, (SrGLuint)sr->input_texture, (int32_t)sr->view_width,
		                          (int32_t)sr->view_height, (SrGLenum)sr->input_format);
		return;
	}
#endif
	sr->weaver->setInputViewTexture(static_cast<int>(sr->input_texture), static_cast<int>(sr->view_width),
	                                static_cast<int>(sr->view_height), static_cast<int>(sr->input_format));
}

void
w_weave(leiasr_gl *sr)
{
#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		srWeaverWeave(sr->weaver_v2);
		return;
	}
#endif
	sr->weaver->weave();
}

bool
w_get_predicted_eyes(leiasr_gl *sr, float left_mm[3], float right_mm[3])
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

} // namespace

extern "C" {

xrt_result_t
leiasr_gl_create(double max_time,
                  void *hwnd,
                  uint32_t view_width,
                  uint32_t view_height,
                  struct leiasr_gl **out)
{
	leiasr_gl *sr = new leiasr_gl;
	sr->view_width = view_width;
	sr->view_height = view_height;

	// Create SR context
#ifdef DXR_LEIA_HAS_SR_V2
	if (leia_sr_api_selected() == LEIA_SR_API_V2) {
		// No v1 fallback on failure — see leia_sr_d3d11.cpp.
		if (!create_v2(max_time, hwnd, *sr)) {
			U_LOG_E("SR v2 weaver creation failed - not falling back to v1 "
			        "(set DXR_LEIA_SR_API=v1 to force it)");
			delete sr;
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}
	} else
#endif
	{
		if (!create_sr_context(max_time, *sr)) {
			delete sr;
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}

		// Create GL weaver
		WeaverErrorCode result = SR::CreateGLWeaver(*sr->context,
		                                             static_cast<HWND>(hwnd),
		                                             &sr->weaver);
		if (result != WeaverErrorCode::WeaverSuccess) {
			U_LOG_E("Failed to create SR GL weaver: %d", (int)result);
			SR::SRContext::deleteSRContext(sr->context);
			delete sr;
			return XRT_ERROR_DEVICE_CREATION_FAILED;
		}

		// Initialize the context after creating the weaver.
		sr->context->initialize();

		// Set default latency (1 frame)
		sr->weaver->setLatencyInFrames(1);
	}

	*out = sr;

	U_LOG_I("Created GL SR weaver for HWND %p, view size %ux%u", hwnd, view_width, view_height);

	return XRT_SUCCESS;
}

void
leiasr_gl_destroy(struct leiasr_gl **leiasr_ptr)
{
	if (leiasr_ptr == nullptr || *leiasr_ptr == nullptr) {
		return;
	}

	leiasr_gl *sr = *leiasr_ptr;

	// SwitchableLensHint is managed by SRContext — do NOT delete it manually.
	sr->lens_hint = nullptr;

#ifdef DXR_LEIA_HAS_SR_V2
	if (sr->weaver_v2 != nullptr) {
		// Destroys the caller-owned handle wrapper only; the hint underneath
		// stays context-owned. See leia_sr_d3d11.cpp.
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
		U_LOG_I("Destroyed GL SR weaver (v2)");
		return;
	}
#endif

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

	U_LOG_I("Destroyed GL SR weaver");
}

void
leiasr_gl_set_input_texture(struct leiasr_gl *leiasr,
                             uint32_t stereo_texture,
                             uint32_t view_width,
                             uint32_t view_height,
                             uint32_t format)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		return;
	}

	static uint32_t last_logged_width = 0, last_logged_height = 0;
	if (view_width != last_logged_width || view_height != last_logged_height) {
		U_LOG_I("SR GL weaver setInputViewTexture: view=%ux%u", view_width, view_height);
		last_logged_width = view_width;
		last_logged_height = view_height;
	}

	leiasr->input_texture = stereo_texture;
	leiasr->view_width = view_width;
	leiasr->view_height = view_height;
	leiasr->input_format = format;

	// Configure the weaver with the input texture
	w_set_input_texture(leiasr);
}

void
leiasr_gl_weave(struct leiasr_gl *leiasr)
{
	if (leiasr == nullptr || !w_ready(leiasr)) {
		U_LOG_W("leiasr_gl_weave called with null instance or weaver");
		return;
	}

	// The weaver writes to the currently bound framebuffer
	w_weave(leiasr);
}

bool
leiasr_gl_get_predicted_eye_positions(struct leiasr_gl *leiasr,
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
leiasr_gl_get_display_dimensions(struct leiasr_gl *leiasr, struct leiasr_display_dimensions *out_dims)
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
leiasr_gl_get_display_pixel_info(struct leiasr_gl *leiasr,
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
leiasr_gl_request_display_mode(struct leiasr_gl *leiasr, bool enable_3d)
{
	if (leiasr == nullptr || !lens_present(leiasr)) {
		return false;
	}

	try {
		lens_set(leiasr, enable_3d);
		U_LOG_W("SR GL display mode switched to %s", enable_3d ? "3D" : "2D");
		return true;
	} catch (...) {
		U_LOG_E("Failed to switch SR GL display mode to %s", enable_3d ? "3D" : "2D");
		return false;
	}
}

bool
leiasr_gl_get_hardware_3d_state(struct leiasr_gl *leiasr, bool *out_is_3d)
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

bool
leiasr_gl_supports_display_mode_switch(struct leiasr_gl *leiasr)
{
	if (leiasr == nullptr) {
		return false;
	}

	return lens_present(leiasr);
}

} // extern "C"
