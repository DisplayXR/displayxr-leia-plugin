// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Track A STUB implementation of the Linux weaver-backend seam
 *         (leia_sr_linux.h) — no LeiaSR Linux SDK.
 *
 * The interface it implements is shaped by docs/leia-linux-sdk-contract.md;
 * the behavior here is deliberately minimal:
 *   - display info / probe results come from one canned-panel constant block,
 *   - the weave is a passthrough SBS blit recorded into the caller's command
 *     buffer (so the DP -> backend seam is exercised end-to-end with zero
 *     interlacing math),
 *   - eye positions are the nominal viewer pair, never zeros (R-T2).
 *
 * TODO(Track B): replace every body in this file with calls into the real
 * LeiaSR Linux SDK behind the same signatures (select with
 * -DDXR_LEIA_LINUX_WEAVER=sdk).
 *
 * Also implements the two probe entry points from ../drv_leia/leia_interface.h
 * (`leiasr_probe_display` / `leiasr_get_probe_results`) that the shared
 * leia_device.c consumes.
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#include "leia_sr_linux.h"

#include "leia_interface.h"

#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>


/*
 *
 * Canned panel.
 *
 * TODO(Track B): every value below comes from the SDK's display query
 * (contract R-D1) instead of these constants. Plausible 15.6" SR-like
 * panel: 4K native, half-res per view, viewer at 45 cm.
 *
 */

#define STUB_PIXEL_W 3840u
#define STUB_PIXEL_H 2160u
#define STUB_WIDTH_M 0.3446f
#define STUB_HEIGHT_M 0.1938f
#define STUB_VIEW_W 1920u
#define STUB_VIEW_H 1080u
#define STUB_REFRESH_MHZ 60000u
#define STUB_NOMINAL_X_M 0.0f
#define STUB_NOMINAL_Y_M 0.0f
#define STUB_NOMINAL_Z_M 0.45f
#define STUB_HALF_IPD_MM 31.5f /* nominal 63 mm IPD */

struct leiasr_lnx
{
	struct leiasr_lnx_create_info info; /* compositor VK handles, recorded for Track B parity */
	uint64_t latency_us;
	bool hw_3d; /* last requested lens state (R-D2), canned */
};


/*
 *
 * Probe entry points (leia_interface.h, consumed by leia_device.c and the
 * plug-in probe).
 *
 */

static struct leiasr_probe_result g_probe_cache;
static bool g_probe_cached = false;

bool
leiasr_probe_display(double timeout_seconds)
{
	/*
	 * TODO(Track B): real hardware probe — SR-service reachability plus
	 * panel identification (DRM/EDID via /sys/class/drm/<*>/edid, or the
	 * SDK's own display query). The stub has no hardware to find; it only
	 * seeds the canned cache so leia_device.c and get_display_info have
	 * consistent values once the plug-in probe was force-accepted
	 * (DXR_LEIA_FORCE_PROBE=1 — see leia_plugin_linux.c).
	 */
	(void)timeout_seconds;
	g_probe_cache = (struct leiasr_probe_result){
	    .hw_found = true,
	    .pixel_w = STUB_PIXEL_W,
	    .pixel_h = STUB_PIXEL_H,
	    .refresh_hz = STUB_REFRESH_MHZ / 1000.0f,
	    .display_w_m = STUB_WIDTH_M,
	    .display_h_m = STUB_HEIGHT_M,
	    .nominal_z_m = STUB_NOMINAL_Z_M,
	};
	g_probe_cached = true;
	return true;
}

bool
leiasr_get_probe_results(struct leiasr_probe_result *out)
{
	if (!g_probe_cached || out == NULL) {
		return false;
	}
	*out = g_probe_cache;
	return true;
}


/*
 *
 * Surface (a) — Vulkan weaver.
 *
 */

enum leiasr_lnx_result
leiasr_lnx_create(const struct leiasr_lnx_create_info *info, struct leiasr_lnx **out_lnx)
{
	/*
	 * TODO(Track B): SR-service connect with the bounded retry budget
	 * (R-W1) + SDK weaver creation on the compositor's device/queue/pool
	 * (R-W2/R-W3). The stub always "connects".
	 */
	if (info == NULL || out_lnx == NULL) {
		return LEIASR_LNX_ERROR_FAILED;
	}

	struct leiasr_lnx *lnx = calloc(1, sizeof(*lnx));
	if (lnx == NULL) {
		return LEIASR_LNX_ERROR_FAILED;
	}
	lnx->info = *info;
	lnx->hw_3d = true;

	U_LOG_W("leia_sr_stub: STUB weaver backend active — passthrough SBS blit, no interlacing "
	        "(Track A; interface per docs/leia-linux-sdk-contract.md)");
	*out_lnx = lnx;
	return LEIASR_LNX_SUCCESS;
}

void
leiasr_lnx_destroy(struct leiasr_lnx *lnx)
{
	/* TODO(Track B): weaver + context teardown per R-W10 (bounded, no
	 * event-loop pumping). The stub owns nothing but its own struct. */
	free(lnx);
}

void
leiasr_lnx_weave(struct leiasr_lnx *lnx,
                 VkCommandBuffer cmd_buffer,
                 const struct leiasr_lnx_weave_input *input,
                 const struct leiasr_lnx_weave_output *output,
                 VkRect2D viewport,
                 struct leiasr_lnx_phase_origin phase_origin)
{
	/*
	 * TODO(Track B): real interlaced weave — set input atlas (R-W4) +
	 * output target (R-W5) + viewport/phase (R-W7) on the SDK weaver and
	 * record it into @p cmd_buffer (R-W6).
	 *
	 * Stub: passthrough — blit the atlas content region (all tiles,
	 * side-by-side) into the viewport rectangle of the output image. The
	 * stub manages its own layout transitions (atlas arrives
	 * SHADER_READ_ONLY_OPTIMAL, target COLOR_ATTACHMENT_OPTIMAL — same
	 * convention as the Windows DP's single-view blit path) and restores
	 * them, so the caller needs no barriers around this call.
	 *
	 * The blit records through the statically-linked aux_vk-free path:
	 * this .so links the Vulkan loader directly (vkCmd* below), keeping
	 * the stub independent of a vk_bundle.
	 */
	(void)lnx;
	(void)phase_origin; /* no lens phase in a passthrough (R-W7 consumed by Track B) */

	if (cmd_buffer == VK_NULL_HANDLE || input == NULL || output == NULL ||
	    input->atlas_image == VK_NULL_HANDLE || output->image == VK_NULL_HANDLE) {
		U_LOG_W("leia_sr_stub: weave called without blittable input/output — skipping");
		return;
	}

	const int32_t src_w = (int32_t)(input->view_width * input->tile_columns);
	const int32_t src_h = (int32_t)(input->view_height * input->tile_rows);

	VkImageMemoryBarrier pre[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = input->atlas_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = output->image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	};
	vkCmdPipelineBarrier(cmd_buffer,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre);

	const int32_t src_y0 = input->y_flip ? src_h : 0;
	const int32_t src_y1 = input->y_flip ? 0 : src_h;
	VkImageBlit blit = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .srcOffsets = {{0, src_y0, 0}, {src_w, src_y1, 1}},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffsets = {{viewport.offset.x, viewport.offset.y, 0},
	                   {viewport.offset.x + (int32_t)viewport.extent.width,
	                    viewport.offset.y + (int32_t)viewport.extent.height, 1}},
	};
	vkCmdBlitImage(cmd_buffer, input->atlas_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output->image,
	               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

	VkImageMemoryBarrier post[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = input->atlas_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = output->image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	};
	vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
	                     0, NULL, 0, NULL, 2, post);
}

VkRenderPass
leiasr_lnx_get_render_pass(struct leiasr_lnx *lnx)
{
	/* TODO(Track B): expose the SDK weave's render pass (R-W5) if it
	 * renders through one. The stub blits — no render pass. */
	(void)lnx;
	return VK_NULL_HANDLE;
}

void
leiasr_lnx_output_invalidated(struct leiasr_lnx *lnx)
{
	/* TODO(Track B): flush any SDK cache keyed on target VkImage/
	 * VkImageView handles (R-W8). The stub caches nothing. */
	(void)lnx;
}

void
leiasr_lnx_set_latency_us(struct leiasr_lnx *lnx, uint64_t latency_us)
{
	/* TODO(Track B): feed the SDK's prediction horizon (R-W9, the µs
	 * path). The stub records it for parity and does nothing. */
	if (lnx != NULL) {
		lnx->latency_us = latency_us;
	}
}


/*
 *
 * Surface (b) — eye tracking.
 *
 */

bool
leiasr_lnx_get_predicted_eyes(struct leiasr_lnx *lnx,
                              struct leiasr_lnx_eye_pair_mm *out_pair,
                              bool *out_is_tracking,
                              int64_t *out_timestamp_ns)
{
	/*
	 * TODO(Track B): pull the SDK's predicted pair (R-T1) — the same pair
	 * the last weave consumed — plus explicit tracking state (R-T3).
	 * Stub: the nominal viewer pair, never zeros (R-T2), untracked.
	 */
	(void)lnx;
	if (out_pair == NULL) {
		return false;
	}
	const float z_mm = STUB_NOMINAL_Z_M * 1000.0f;
	out_pair->left_mm[0] = -STUB_HALF_IPD_MM;
	out_pair->left_mm[1] = STUB_NOMINAL_Y_M * 1000.0f;
	out_pair->left_mm[2] = z_mm;
	out_pair->right_mm[0] = STUB_HALF_IPD_MM;
	out_pair->right_mm[1] = STUB_NOMINAL_Y_M * 1000.0f;
	out_pair->right_mm[2] = z_mm;
	if (out_is_tracking != NULL) {
		*out_is_tracking = false; /* explicit state (R-T3) — the stub has no camera */
	}
	if (out_timestamp_ns != NULL) {
		*out_timestamp_ns = 0;
	}
	return true;
}

bool
leiasr_lnx_set_eye_tracking_mode(struct leiasr_lnx *lnx, enum leiasr_lnx_tracking_mode mode)
{
	/* TODO(Track B): MANAGED = SDK owns the loss lifecycle (R-T4);
	 * MANUAL = SDK stands down (R-T5). Nothing to switch in the stub. */
	(void)lnx;
	return mode == LEIASR_LNX_TRACKING_MANAGED;
}


/*
 *
 * Surface (c) — display & calibration.
 *
 */

bool
leiasr_lnx_query_display_info(struct leiasr_lnx_display_info *out_info)
{
	/*
	 * TODO(Track B): the SDK's display enumeration (R-D1) — physical size,
	 * native resolution, X11 virtual-desktop position (anchors lens phase,
	 * contract §3.5), recommended view size, refresh, nominal viewer.
	 * Headless-tolerant per R-D4. Stub: the canned panel, always valid.
	 */
	if (out_info == NULL) {
		return false;
	}
	*out_info = (struct leiasr_lnx_display_info){
	    .valid = true,
	    .width_m = STUB_WIDTH_M,
	    .height_m = STUB_HEIGHT_M,
	    .pixel_width = STUB_PIXEL_W,
	    .pixel_height = STUB_PIXEL_H,
	    .screen_left = 0,
	    .screen_top = 0,
	    .recommended_view_width = STUB_VIEW_W,
	    .recommended_view_height = STUB_VIEW_H,
	    .refresh_mhz = STUB_REFRESH_MHZ,
	    .nominal_viewer_x_m = STUB_NOMINAL_X_M,
	    .nominal_viewer_y_m = STUB_NOMINAL_Y_M,
	    .nominal_viewer_z_m = STUB_NOMINAL_Z_M,
	};
	return true;
}

bool
leiasr_lnx_request_display_mode(struct leiasr_lnx *lnx, bool enable_3d)
{
	/* TODO(Track B): the SDK's lens/backlight 2D⇄3D switch (R-D2). The
	 * stub just remembers the request so get_hardware_3d_state echoes it. */
	if (lnx == NULL) {
		return false;
	}
	lnx->hw_3d = enable_3d;
	U_LOG_I("leia_sr_stub: request_display_mode(%s) — no hardware, state recorded only",
	        enable_3d ? "3D" : "2D");
	return true;
}

bool
leiasr_lnx_get_hardware_3d_state(struct leiasr_lnx *lnx, bool *out_is_3d)
{
	/* TODO(Track B): read the real lens state from the SDK (R-D2). */
	if (lnx == NULL || out_is_3d == NULL) {
		return false;
	}
	*out_is_3d = lnx->hw_3d;
	return true;
}
