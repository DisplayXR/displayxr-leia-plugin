// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Leia 3D display HMD device.
 *
 * Creates an xrt_device representing a Leia light field display.
 * When SR SDK is available (Windows), queries the hardware for
 * pixel dimensions, refresh rate, and physical size.  Otherwise
 * falls back to sensible defaults.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_interface.h"
#ifdef XRT_HAVE_LEIA_SR_D3D11
/* #187: live SR display queries, used when the probe cache is empty (the common
 * case -- see leia_hmd_create). Guarded because this file is shared with the
 * Linux arm (src/drv_leia_linux), which has no SR SDK. */
#include "leia_sr_d3d11.h"
#endif

#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_mathinclude.h"

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_var.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Structs and helpers.
 *
 */

/*!
 * Leia 3D display device.
 * @implements xrt_device
 * @ingroup drv_leia
 */
struct leia_hmd
{
	struct xrt_device base;

	//! Stationary pose (looking at the display from nominal distance).
	struct xrt_pose pose;

	//! Optional external device providing pose (e.g. qwerty HMD).
	//! When set, get_tracked_pose delegates to this device.
	struct xrt_device *pose_source;

	//! Physical display dimensions in meters.
	float display_width_m;
	float display_height_m;

	//! Nominal viewer distance in meters.
	float nominal_z_m;

	enum u_logging_level log_level;
};

static inline struct leia_hmd *
leia_hmd(struct xrt_device *xdev)
{
	return (struct leia_hmd *)xdev;
}


/*
 *
 * xrt_device interface methods.
 *
 */

static xrt_result_t
leia_hmd_get_tracked_pose(struct xrt_device *xdev,
                          enum xrt_input_name name,
                          int64_t at_timestamp_ns,
                          struct xrt_space_relation *out_relation)
{
	struct leia_hmd *hmd = leia_hmd(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		U_LOG_E("Unknown input name: 0x%08x", name);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	// Delegate to external pose source (e.g. qwerty HMD for WASD/mouse).
	if (hmd->pose_source != NULL) {
		struct xrt_space_relation src_rel;
		hmd->pose_source->get_tracked_pose(hmd->pose_source, name, at_timestamp_ns, &src_rel);

		out_relation->pose = src_rel.pose;
		out_relation->relation_flags = (enum xrt_space_relation_flags)(
		    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		    XRT_SPACE_RELATION_POSITION_VALID_BIT |
		    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
		    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
		return XRT_SUCCESS;
	}

	// Static pose: viewer at nominal position in front of the display.
	out_relation->pose = hmd->pose;
	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	    XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

	return XRT_SUCCESS;
}

static void
leia_hmd_destroy(struct xrt_device *xdev)
{
	struct leia_hmd *hmd = leia_hmd(xdev);

	u_var_remove_root(hmd);
	u_device_free(&hmd->base);
}

void
leia_hmd_set_pose_source(struct xrt_device *leia_dev, struct xrt_device *source)
{
	struct leia_hmd *hmd = leia_hmd(leia_dev);
	hmd->pose_source = source;
}


/*
 *
 * Creation function.
 *
 */

//! #187: seconds to wait for the live SR geometry query when the probe cache is
//! empty. Short on purpose -- SR is already warm by the time a device is created,
//! so this is a bounded safety net, not a blocking probe.
#define LEIA_DEVICE_SR_QUERY_TIMEOUT_S 2.0

//! Fallback per-view scale when nothing can tell us better: the 2x1 SBS half.
//! Also what every shipping SR panel evaluates to, since the SDK defaults
//! recommendedViewsTexture{Width,Height} to physicalResolution/2 and no
//! product screen.ini overrides it.
#define LEIA_DEFAULT_VIEW_SCALE 0.5f


/*
 *
 * Per-view scale -- ONE derivation, two consumers.
 *
 */

/*
 * The per-view scale reaches apps through two entirely separate paths:
 *
 *   - xrt_rendering_mode::view_scale_x/y (the MODE TABLE below), which sizes
 *     the mode's tiles, the worst-case atlas and the compositor's tile grid
 *     (u_tiling_compute_mode / u_tiling_compute_canvas_view), and is what
 *     XrDisplayRenderingModeInfoDXR reports;
 *   - xrt_plugin_display_info::recommended_view_scale_x/y (the SCALAR that
 *     leia_plugin_get_display_info fills), which sizes
 *     XrViewConfigurationView.recommended* at xrCreateInstance.
 *
 * They MUST agree: if they disagree, an app is sized per view from one number
 * and tiled from the other, and the declared atlas can be too small to hold two
 * real tiles. The mode table used to carry a hardcoded 0.5f "overridden by SR
 * SDK" placeholder that nothing ever overrode -- correct only because the SR
 * default happens to be exactly one half.
 *
 * So the pair is derived HERE, once, and both call sites read it back. The
 * derivation is cached, so the probe-cache fast path in leia_hmd_create() (which
 * skips the live geometry query) still gets the SR-derived pair rather than a
 * second, hardcoded one.
 */
static bool g_view_scale_valid = false;
static float g_view_scale_x = LEIA_DEFAULT_VIEW_SCALE;
static float g_view_scale_y = LEIA_DEFAULT_VIEW_SCALE;

void
leia_view_scale_set_from_dims(uint32_t view_w, uint32_t view_h, uint32_t native_w, uint32_t native_h)
{
	if (g_view_scale_valid) {
		/* FIRST writer wins, on purpose. By the time a second caller arrives
		 * the value may already be published in the device's mode table, and
		 * a fresher number that disagrees with what the app was sized from is
		 * strictly worse than a slightly staler one that agrees. */
		return;
	}
	if (view_w == 0 || view_h == 0 || native_w == 0 || native_h == 0) {
		return; /* Not a usable answer — leave the fallback in place. */
	}
	g_view_scale_x = (float)view_w / (float)native_w;
	g_view_scale_y = (float)view_h / (float)native_h;
	g_view_scale_valid = true;
}

void
leia_view_scale_get(float *out_scale_x, float *out_scale_y)
{
	if (!g_view_scale_valid) {
#ifdef XRT_HAVE_LEIA_SR_D3D11
		/* The SR context is warm by here (probe + get_display_info both ran
		 * first), so this resolves from cached SR state rather than paying
		 * the full timeout. Result is memoised, so at most one query. */
		uint32_t view_w = 0, view_h = 0, nat_w = 0, nat_h = 0;
		float hz = 0.0f;
		if (leiasr_query_recommended_view_dimensions(LEIA_DEVICE_SR_QUERY_TIMEOUT_S, &view_w, &view_h, &hz,
		                                             &nat_w, &nat_h)) {
			leia_view_scale_set_from_dims(view_w, view_h, nat_w, nat_h);
		}
#endif
		U_LOG_W("Leia per-view scale %.4f x %.4f (%s)", (double)g_view_scale_x, (double)g_view_scale_y,
		        g_view_scale_valid ? "derived from the backend's recommended view dimensions"
		                           : "fallback — no backend answer");
		/* Latch either way: the fallback is the answer for this process too,
		 * and re-querying every call would re-pay the timeout on a box with
		 * no SR service. */
		g_view_scale_valid = true;
	}

	if (out_scale_x != NULL) {
		*out_scale_x = g_view_scale_x;
	}
	if (out_scale_y != NULL) {
		*out_scale_y = g_view_scale_y;
	}
}

struct xrt_device *
leia_hmd_create(void)
{
	// Default values — used when SR SDK is not available.
	int pixel_w = 3840;
	int pixel_h = 2160;
	float refresh_hz = 60.0f;
	float display_w_m = 0.344f;
	float display_h_m = 0.194f;
	float nominal_z = 0.65f;

	/*
	 * Where the real geometry comes from.
	 *
	 * #187: the probe cache below is populated ONLY by leiasr_probe_display(),
	 * which leia_plugin_probe() calls only when the EDID table MISSES. A panel
	 * that is IN the table -- the common case -- therefore left this cache empty
	 * and the device kept the hardcoded 4K defaults above forever. Harmless on a
	 * 3840x2160 panel, where they happen to be right; on an 8K panel the device
	 * reported half the size in both axes, which also roughly halves the FOV
	 * derived from it below (atan(0.172/0.65)=14.8deg vs 28.2deg).
	 *
	 * So: prefer the cache when it has something, then fall back to the same live
	 * SR queries leia_plugin_get_display_info() already uses -- which are correct
	 * on every panel -- and only then to the hardcoded defaults.
	 */
	const char *geom_src = "hardcoded defaults";
	{
		struct leiasr_probe_result probe;
		if (leiasr_get_probe_results(&probe) && probe.hw_found) {
			pixel_w = (int)probe.pixel_w;
			pixel_h = (int)probe.pixel_h;
			if (probe.refresh_hz > 0.0f) {
				refresh_hz = probe.refresh_hz;
			}
			display_w_m = probe.display_w_m;
			display_h_m = probe.display_h_m;
			if (probe.nominal_z_m > 0.0f) {
				nominal_z = probe.nominal_z_m;
			}
			geom_src = "SR probe cache";
		}
#ifdef XRT_HAVE_LEIA_SR_D3D11
		else {
			/* The SR context is already warm by here (the plug-in's probe and
			 * get_display_info both run first), so these resolve from cached SR
			 * state rather than paying the full timeout. */
			uint32_t nat_w = 0, nat_h = 0, view_w = 0, view_h = 0;
			float hz = 0.0f;
			bool got_px = leiasr_query_recommended_view_dimensions(LEIA_DEVICE_SR_QUERY_TIMEOUT_S, &view_w,
			                                                       &view_h, &hz, &nat_w, &nat_h);
			/* Feed the ONE per-view-scale derivation with what we just
			 * queried, so leia_view_scale_get() below answers from these
			 * numbers instead of re-querying SR. */
			if (got_px) {
				leia_view_scale_set_from_dims(view_w, view_h, nat_w, nat_h);
			}
			if (got_px && nat_w > 0 && nat_h > 0) {
				pixel_w = (int)nat_w;
				pixel_h = (int)nat_h;
				if (hz > 0.0f) {
					refresh_hz = hz;
				}
				geom_src = "live SR query";
			}
			struct leiasr_display_dimensions dims = {0};
			if (leiasr_static_get_display_dimensions(&dims) && dims.valid && dims.width_m > 0.0f &&
			    dims.height_m > 0.0f) {
				display_w_m = dims.width_m;
				display_h_m = dims.height_m;
				if (dims.nominal_z_m > 0.0f) {
					nominal_z = dims.nominal_z_m;
				}
				geom_src = "live SR query";
			}
		}
#endif
	}

	enum u_device_alloc_flags flags =
	    (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);
	struct leia_hmd *hmd = U_DEVICE_ALLOCATE(struct leia_hmd, flags, 1, 0);

	// Store config.
	hmd->display_width_m = display_w_m;
	hmd->display_height_m = display_h_m;
	hmd->nominal_z_m = nominal_z;
	hmd->log_level = U_LOGGING_INFO;

	// xrt_device methods.
	hmd->base.update_inputs = u_device_noop_update_inputs;
	hmd->base.get_tracked_pose = leia_hmd_get_tracked_pose;
	hmd->base.get_view_poses = u_device_get_view_poses;
	hmd->base.get_visibility_mask = u_device_get_visibility_mask;
	hmd->base.destroy = leia_hmd_destroy;
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;

	// Pose is delegated to the qwerty HMD (via pose_source), which already
	// includes the Y=1.6 standing height.  Mark our tracking origin as
	// OTHER so u_builder_setup_tracking_origins does NOT add a redundant
	// Y=1.6 offset — that would double-count the height and place
	// controllers (which share the qwerty origin) 1.6 m below the head.
	hmd->base.tracking_origin->type = XRT_TRACKING_TYPE_OTHER;

	// Static pose: centered, at nominal viewing distance.
	hmd->pose.orientation.w = 1.0f;
	hmd->pose.position.z = -nominal_z; // Negative Z = looking at display

	hmd->base.hmd->view_count = 2;

	snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "Leia 3D Display");
	snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "leia_display_0");

	// Rendering modes: Leia exposes 2 modes (2D + LeiaSR).
	hmd->base.rendering_mode_count = 2;

	// Mode 0: 2D (mono, full resolution, 1×1 tile). Untracked — mode_flags
	// stays 0 unless/until a "2D tracked" product mode is added (#29).
	hmd->base.rendering_modes[0].mode_index = 0;
	snprintf(hmd->base.rendering_modes[0].mode_name, XRT_DEVICE_NAME_LEN, "2D");
	hmd->base.rendering_modes[0].view_count = 1;
	hmd->base.rendering_modes[0].view_scale_x = 1.0f;
	hmd->base.rendering_modes[0].view_scale_y = 1.0f;
	hmd->base.rendering_modes[0].hardware_display_3d = false;
	hmd->base.rendering_modes[0].tile_columns = 1;
	hmd->base.rendering_modes[0].tile_rows = 1;
	hmd->base.rendering_modes[0].mode_flags = 0;

	// Mode 1: LeiaSR (two views, 2×1 tile atlas, scale from the ONE derivation in
	// leia_view_scale_get() — the same pair leia_plugin_get_display_info() reports
	// as recommended_view_scale_x/y, never a second hardcoded number).
	// Consumes live SR eye tracking → HAS_TRACKING (#441 ABI v3).
	// reserved[] stays zeroed via the calloc'd U_DEVICE_ALLOCATE block.
	hmd->base.rendering_modes[1].mode_index = 1;
	snprintf(hmd->base.rendering_modes[1].mode_name, XRT_DEVICE_NAME_LEN, "LeiaSR");
	hmd->base.rendering_modes[1].view_count = 2;
	leia_view_scale_get(&hmd->base.rendering_modes[1].view_scale_x,
	                    &hmd->base.rendering_modes[1].view_scale_y);
	hmd->base.rendering_modes[1].hardware_display_3d = true;
	hmd->base.rendering_modes[1].tile_columns = 2;
	hmd->base.rendering_modes[1].tile_rows = 1;
	hmd->base.rendering_modes[1].mode_flags = XRT_RENDERING_MODE_FLAG_HAS_TRACKING;

	hmd->base.hmd->active_rendering_mode_index = 1; // Default to LeiaSR (3D)

	// Head pose input.
	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;

	// Display geometry using helper struct.
	struct u_device_simple_info info;
	info.display.w_pixels = pixel_w;
	info.display.h_pixels = pixel_h;
	info.display.w_meters = display_w_m;
	info.display.h_meters = display_h_m;
	const float leia_ipd_m = 0.063f; // ~63mm IPD
	info.lens_horizontal_separation_meters = leia_ipd_m;
	info.lens_vertical_position_meters = display_h_m / 2.0f;

	// Per-view eye-box offsets (display-local space). Leia is currently
	// stereo (view_count=2); future N-view lenticular modes can extend this
	// via the SR SDK lookaround filter — see #246 Phase 4.
	{
		const float half_ipd = leia_ipd_m / 2.0f;
		hmd->base.hmd->view_eye_offsets[0] = (struct xrt_vec3){-half_ipd, 0.0f, nominal_z};
		hmd->base.hmd->view_eye_offsets[1] = (struct xrt_vec3){ half_ipd, 0.0f, nominal_z};
		for (uint32_t v = 2; v < XRT_MAX_VIEWS; v++) {
			hmd->base.hmd->view_eye_offsets[v] = (struct xrt_vec3){0.0f, 0.0f, nominal_z};
		}
	}

	// Compute FOV from display geometry and viewing distance.
	float half_fov_h = atanf((display_w_m / 2.0f) / nominal_z);
	float half_fov_v = atanf((display_h_m / 2.0f) / nominal_z);
	info.fov[0] = half_fov_h * 2.0f;
	info.fov[1] = half_fov_h * 2.0f;

	(void)half_fov_v; // Used implicitly by u_device_setup_split_side_by_side

	bool setup_ok = u_device_setup_split_side_by_side(&hmd->base, &info);
	if (!setup_ok) {
		U_LOG_E("Failed to setup Leia display device info");
		leia_hmd_destroy(&hmd->base);
		return NULL;
	}

	// Advertise ALPHA_BLEND alongside the default OPAQUE that the device
	// helper installed. The Leia DPs deliver alpha-correct output two ways:
	//   - Standalone (XR_DXR_win32_window_binding + transparentBackgroundEnabled):
	//     WGC compose-under-bg + post-weave alpha-gate on D3D11/D3D12/VK.
	//   - Workspace (IPC): service compositor honours
	//     XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT per layer.
	// Advertising ALPHA_BLEND lets well-behaved apps discover transparency
	// support via the standard xrEnumerateEnvironmentBlendModes path instead
	// of relying on extension detection alone.
	hmd->base.hmd->blend_modes[hmd->base.hmd->blend_mode_count++] = XRT_BLEND_MODE_ALPHA_BLEND;

	// No distortion for Leia display.
	u_distortion_mesh_set_none(&hmd->base);

	// Debug variables.
	u_var_add_root(hmd, "Leia 3D Display", true);
	u_var_add_pose(hmd, &hmd->pose, "pose");
	u_var_add_f32(hmd, &hmd->display_width_m, "display_width_m");
	u_var_add_f32(hmd, &hmd->display_height_m, "display_height_m");
	u_var_add_f32(hmd, &hmd->nominal_z_m, "nominal_z_m");
	u_var_add_log_level(hmd, &hmd->log_level, "log_level");

	U_LOG_W("Created Leia 3D display: %dx%d px, %.4fx%.4f m, nominal Z=%.2f m, %.1f Hz (geometry from %s)",
	        pixel_w, pixel_h, display_w_m, display_h_m, nominal_z, refresh_hz, geom_src);

	(void)refresh_hz; // Logged above; will be used for frame timing later.

	return &hmd->base;
}
