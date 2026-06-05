// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Android variant of the Leia plug-in entry point.
 *
 * Mirrors @ref leia_plugin.c (Windows/SR) but wraps CNSDK instead of
 * the SR SDK. Builds into `libdxrp050_leia_cnsdk.so`, shipped in the
 * runtime APK's `jniLibs/<ABI>/` and discovered by the runtime's
 * `target_plugin_loader.c` Android branch (PR #309 / commit c96c93ce8).
 *
 * Differences from the Windows plug-in:
 *   - No `probe()` registry/EDID check — the CNSDK device is the only
 *     thing this plug-in supports, so probe always succeeds.
 *   - Only `create_dp_vk` is non-NULL (Android has no D3D/Metal/GL
 *     compositor path today, and CNSDK is Vulkan-only).
 *   - `get_display_info` returns Lume Pad 2 defaults; CNSDK-derived
 *     metrics surface lazily via the DP's `get_display_dimensions` /
 *     `get_display_pixel_info` calls once the async core init completes.
 *
 * @author leaiss
 * @ingroup drv_leia_android
 */

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_device.h"

#include "util/u_device.h"
#include "util/u_logging.h"

#include "leia_cnsdk.h"
#include "leia_display_processor_cnsdk.h"

#include <math.h>
#include <stddef.h>


/*
 *
 * Minimal HMD device (no SR SDK probe — Lume Pad 2 hardcoded defaults).
 *
 */

struct leia_android_hmd
{
	struct xrt_device base;
};

static void
leia_android_hmd_destroy(struct xrt_device *xdev)
{
	struct leia_android_hmd *hmd = (struct leia_android_hmd *)xdev;
	u_device_free(&hmd->base);
}

static xrt_result_t
leia_android_hmd_get_tracked_pose(struct xrt_device *xdev,
                                  enum xrt_input_name name,
                                  int64_t at_timestamp_ns,
                                  struct xrt_space_relation *out_relation)
{
	(void)xdev;
	(void)name;
	(void)at_timestamp_ns;
	out_relation->pose.orientation = (struct xrt_quat){0.0f, 0.0f, 0.0f, 1.0f};
	out_relation->pose.position = (struct xrt_vec3){0.0f, 0.0f, 0.0f};
	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT);
	return XRT_SUCCESS;
}

static struct xrt_device *
leia_android_hmd_create(void)
{
	enum u_device_alloc_flags flags =
	    (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);
	struct leia_android_hmd *hmd = U_DEVICE_ALLOCATE(struct leia_android_hmd, flags, 1, 0);

	hmd->base.update_inputs = u_device_noop_update_inputs;
	hmd->base.get_tracked_pose = leia_android_hmd_get_tracked_pose;
	hmd->base.get_view_poses = u_device_get_view_poses;
	hmd->base.get_visibility_mask = u_device_get_visibility_mask;
	hmd->base.destroy = leia_android_hmd_destroy;
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;

	// Lume Pad 2 default panel + viewing distance. CNSDK fills these
	// in lazily on first DP query once the async core init completes.
	struct u_device_simple_info info = {
	    .display.w_pixels = 2560,
	    .display.h_pixels = 1600,
	    .display.w_meters = 0.235f,
	    .display.h_meters = 0.147f,
	    .lens_horizontal_separation_meters = 0.063f,
	    .lens_vertical_position_meters = 0.0735f,
	    .fov = {85.0f * (float)M_PI / 180.0f,
	            85.0f * (float)M_PI / 180.0f},
	};
	u_device_setup_split_side_by_side(&hmd->base, &info);

	snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "Leia CNSDK (Android)");
	snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "lume-pad-cnsdk");

	return &hmd->base;
}


/*
 *
 * Vtable callbacks.
 *
 */

static xrt_result_t
leia_plugin_android_probe(struct xrt_plugin_instance **out_inst)
{
	// CNSDK device probe runs async on a worker thread; the plug-in
	// can't synchronously confirm hardware presence at xrCreateInstance
	// time. POC pattern: always claim the slot on Android. The
	// downstream leia_dp_factory_cnsdk path bails cleanly if CNSDK
	// fails to initialize later.

	// Log the debug.dxr.leia.* calibration knobs at xrCreateInstance
	// time. Idempotent — the function caches after first call. We log
	// here (not from CNSDK init) so the values show up in logcat even
	// on emulators that never reach DP creation due to missing Vulkan
	// extensions. See docs/cnsdk-android-calibration.md.
	leia_cnsdk_log_calibration_knobs();

	*out_inst = NULL;
	return XRT_SUCCESS;
}

static xrt_result_t
leia_plugin_android_create_device(struct xrt_plugin_instance *inst, struct xrt_device **out_dev)
{
	(void)inst;
	struct xrt_device *xdev = leia_android_hmd_create();
	if (xdev == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	*out_dev = xdev;
	return XRT_SUCCESS;
}

static void
leia_plugin_android_destroy(struct xrt_plugin_instance *inst)
{
	(void)inst;
}

static bool
leia_plugin_android_get_display_info(struct xrt_plugin_instance *inst,
                                     struct xrt_device *xdev,
                                     struct xrt_plugin_display_info *out_info)
{
	(void)inst;
	(void)xdev;

	// Lume Pad 2 defaults. CNSDK device-config is async — the DP's
	// get_display_dimensions / get_display_pixel_info read it lazily,
	// so these baseline values keep the runtime usable while the
	// CNSDK core is still booting.
	out_info->display_pixel_width = 2560;
	out_info->display_pixel_height = 1600;
	out_info->recommended_view_scale_x = 1.0f;
	out_info->recommended_view_scale_y = 1.0f;
	out_info->display_width_m = 0.235f;
	out_info->display_height_m = 0.147f;
	out_info->nominal_viewer_x_m = 0.0f;
	out_info->nominal_viewer_y_m = 0.0f;
	out_info->nominal_viewer_z_m = 0.50f;

	// CNSDK exposes face position only — no MANUAL pose-stream API.
	out_info->supported_eye_tracking_modes = 1u; // MANAGED_BIT
	out_info->default_eye_tracking_mode = 0u;    // MANAGED

	return true;
}


/*
 *
 * Vtable.
 *
 */

static struct xrt_plugin_iface g_leia_android_iface = {
    .struct_size = sizeof(struct xrt_plugin_iface),
    .reserved_0 = 0,

    .id = "leia-cnsdk",
    .display_name = "DisplayXR Leia CNSDK (Android)",
    .vendor = "Leia Inc.",
    .version = NULL,

    .probe = leia_plugin_android_probe,
    .create_device = leia_plugin_android_create_device,

    .create_dp_vk = leia_dp_factory_cnsdk,
    .create_dp_d3d11 = NULL,
    .create_dp_d3d12 = NULL,
    .create_dp_gl = NULL,
    .create_dp_metal = NULL,

    .destroy = leia_plugin_android_destroy,

    .get_display_info = leia_plugin_android_get_display_info,

    .set_pose_source = NULL, // CNSDK pose comes from face tracking, not an external source

    // probe_displays (v1.9.0 / ADR-015 / #69): deliberately omitted on Android.
    // It exists for per-monitor claim discovery on desktop multi-display setups
    // (the Windows arm in drv_leia/leia_plugin.c implements it over EDID
    // enumeration). Android is a single fixed-panel device with no monitor
    // enumeration, so we leave it NULL — struct_size still spans the full v2
    // iface, so the runtime's struct_size gate sees the slot, finds it NULL, and
    // falls back to query_source_claims()'s synthesized primary claim off a
    // successful binary probe(). Validated end-to-end via android-smoketest.sh.
    .probe_displays = NULL,
};


/*
 *
 * Entry point.
 *
 */

XRT_PLUGIN_EXPORT xrt_result_t
xrtPluginNegotiate(uint32_t runtime_api_version,
                   const struct xrt_plugin_host_iface *host,
                   struct xrt_plugin_iface **out_iface,
                   uint32_t *out_plugin_api_version)
{
	// Capture the host's Android JavaVM/Activity accessors for CNSDK init.
	// Our statically-linked aux_android copy of the VM globals is never
	// populated by the runtime (hidden-visibility binds locally), so CNSDK
	// must get the JavaVM/Activity through the host iface. These named
	// fields were carved from the formerly zero-initialized reserved[]
	// block (struct_size unchanged), so reading them is safe on any
	// runtime; the values are NULL on non-Android / older runtimes, in
	// which case leia_cnsdk falls back to its own android_globals.
	if (host != NULL) {
		leia_cnsdk_set_host_android_accessors(host->get_android_vm, host->get_android_activity);
	}

	*out_plugin_api_version = XRT_PLUGIN_API_VERSION_CURRENT;

	if (runtime_api_version != XRT_PLUGIN_API_VERSION_CURRENT) {
		*out_iface = NULL;
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}

	*out_iface = &g_leia_android_iface;
	return XRT_SUCCESS;
}
