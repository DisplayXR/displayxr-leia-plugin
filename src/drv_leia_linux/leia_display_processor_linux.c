// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux Vulkan display processor — Track A skeleton over the
 *         weaver-backend seam (leia_sr_linux.h).
 *
 * Mirrors the shape of the Windows VK DP (../drv_leia/leia_display_processor.cpp)
 * and the runtime's sim_display VK DP, but drives the backend through the
 * contract-shaped seam so Track B swaps the stub for the real SDK without
 * touching this file. Per-frame trace = contract appendix §9:
 *
 *   - 1x1 grid  -> plain DP-side blit, weaver bypassed (2D path)
 *   - else      -> leiasr_lnx_weave() into the caller's command buffer
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#include "leia_display_processor_linux.h"
#include "leia_sr_linux.h"

#include "xrt/xrt_display_processor_vk.h"
#include "xrt/xrt_display_metrics.h"

#include "vk/vk_helpers.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>


struct leia_dp_linux
{
	struct xrt_display_processor_vk base; //!< vtable, MUST be first (offset 0)
	struct vk_bundle *vk;                 //!< compositor's bundle — not owned
	struct leiasr_lnx *sr;                //!< weaver backend — owned
	uint32_t view_count;                  //!< from the last process_atlas grid
};

static inline struct leia_dp_linux *
leia_dp_linux(struct xrt_display_processor *xdp)
{
	return (struct leia_dp_linux *)xdp;
}


/*
 *
 * Vtable callbacks.
 *
 */

static void
leia_lnx_dp_process_atlas(struct xrt_display_processor *xdp,
                          VkCommandBuffer cmd_buffer,
                          VkImage_XDP atlas_image,
                          VkImageView atlas_view,
                          uint32_t view_width,
                          uint32_t view_height,
                          uint32_t tile_columns,
                          uint32_t tile_rows,
                          VkFormat_XDP view_format,
                          VkFramebuffer target_fb,
                          VkImage_XDP target_image,
                          uint32_t target_width,
                          uint32_t target_height,
                          VkFormat_XDP target_format,
                          int32_t canvas_offset_x,
                          int32_t canvas_offset_y,
                          uint32_t canvas_width,
                          uint32_t canvas_height)
{
	struct leia_dp_linux *ldp = leia_dp_linux(xdp);
	struct vk_bundle *vk = ldp->vk;

	// runtime#542: atlas processing follows the CONTENT, not the lens —
	// the grid the runtime packed decides weave vs flat blit.
	ldp->view_count = (tile_columns * tile_rows > 1) ? tile_columns * tile_rows : 1;

	// Single-view content: bypass the weaver, blit atlas content to target
	// (same convention as the Windows DP's 2D path).
	if (ldp->view_count == 1 && target_image != (VkImage_XDP)0) {
		VkImageMemoryBarrier pre[2] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image = (VkImage)atlas_image,
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
		        .image = (VkImage)target_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		};
		vk->vkCmdPipelineBarrier(
		    cmd_buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre);

		VkImageBlit blit = {
		    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .srcOffsets = {{0, 0, 0}, {(int32_t)view_width, (int32_t)view_height, 1}},
		    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .dstOffsets = {{0, 0, 0}, {(int32_t)target_width, (int32_t)target_height, 1}},
		};
		vk->vkCmdBlitImage(cmd_buffer, (VkImage)atlas_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                   (VkImage)target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
		                   VK_FILTER_LINEAR);

		VkImageMemoryBarrier post[2] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image = (VkImage)atlas_image,
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
		        .image = (VkImage)target_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		};
		vk->vkCmdPipelineBarrier(
		    cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0,
		    NULL, 2, post);
		return;
	}

	// Multi-view: hand the whole tiled atlas to the backend (contract R-W4:
	// one atlas image + explicit grid). The atlas is guaranteed content-sized
	// by the compositor's crop-blit (ADR-030).
	struct leiasr_lnx_weave_input input = {
	    .atlas_image = (VkImage)atlas_image,
	    .atlas_view = (VkImageView)atlas_view,
	    .view_width = view_width,
	    .view_height = view_height,
	    .view_format = (VkFormat)view_format,
	    .tile_columns = tile_columns,
	    .tile_rows = tile_rows,
	    .y_flip = false,
	};
	struct leiasr_lnx_weave_output output = {
	    .framebuffer = (VkFramebuffer)target_fb,
	    .image = (VkImage)target_image,
	    .width = target_width,
	    .height = target_height,
	    .format = (VkFormat)target_format,
	};

	// Viewport and phase origin decoupled (contract R-W7): the woven pixels
	// land in the canvas rect; the lens phase anchors at the canvas position
	// on the panel. Zero canvas = full target (display-scoped weaving).
	VkRect2D viewport = {
	    .offset = {canvas_offset_x, canvas_offset_y},
	    .extent = {canvas_width != 0 ? canvas_width : target_width,
	               canvas_height != 0 ? canvas_height : target_height},
	};
	struct leiasr_lnx_phase_origin phase = {
	    // TODO(Track B): add the display's screen origin of the runtime
	    // window so the phase anchors in absolute panel coordinates
	    // (window-scoped weaving, runtime Phase 3b). Display-scoped today.
	    .x = canvas_offset_x,
	    .y = canvas_offset_y,
	};

	leiasr_lnx_weave(ldp->sr, cmd_buffer, &input, &output, viewport, phase);
}

static bool
leia_lnx_dp_get_predicted_eye_positions(struct xrt_display_processor *xdp, struct xrt_eye_positions *out_eye_pos)
{
	struct leia_dp_linux *ldp = leia_dp_linux(xdp);

	struct leiasr_lnx_eye_pair_mm pair;
	bool is_tracking = false;
	int64_t ts_ns = 0;
	if (!leiasr_lnx_get_predicted_eyes(ldp->sr, &pair, &is_tracking, &ts_ns)) {
		return false;
	}

	// Contract R-T1: backend reports millimeters, display-center; the DP
	// converts to meters. Center the pair for view counts != 2 the same way
	// the runtime expects stereo anchors (Track A only ever fills 2).
	memset(out_eye_pos, 0, sizeof(*out_eye_pos));
	out_eye_pos->eyes[0].x = pair.left_mm[0] / 1000.0f;
	out_eye_pos->eyes[0].y = pair.left_mm[1] / 1000.0f;
	out_eye_pos->eyes[0].z = pair.left_mm[2] / 1000.0f;
	out_eye_pos->eyes[1].x = pair.right_mm[0] / 1000.0f;
	out_eye_pos->eyes[1].y = pair.right_mm[1] / 1000.0f;
	out_eye_pos->eyes[1].z = pair.right_mm[2] / 1000.0f;
	out_eye_pos->count = 2;
	out_eye_pos->timestamp_ns = ts_ns;
	out_eye_pos->valid = true;
	out_eye_pos->is_tracking = is_tracking; // explicit state (R-T3), no collapse heuristic
	return true;
}

static bool
leia_lnx_dp_get_display_dimensions(struct xrt_display_processor *xdp, float *out_width_m, float *out_height_m)
{
	(void)xdp;
	struct leiasr_lnx_display_info info;
	if (!leiasr_lnx_query_display_info(&info) || !info.valid) {
		return false;
	}
	*out_width_m = info.width_m;
	*out_height_m = info.height_m;
	return true;
}

static bool
leia_lnx_dp_get_display_pixel_info(struct xrt_display_processor *xdp,
                                   uint32_t *out_pixel_width,
                                   uint32_t *out_pixel_height,
                                   int32_t *out_screen_left,
                                   int32_t *out_screen_top)
{
	(void)xdp;
	struct leiasr_lnx_display_info info;
	if (!leiasr_lnx_query_display_info(&info) || !info.valid) {
		return false;
	}
	*out_pixel_width = info.pixel_width;
	*out_pixel_height = info.pixel_height;
	*out_screen_left = info.screen_left;
	*out_screen_top = info.screen_top;
	return true;
}

static bool
leia_lnx_dp_request_display_mode(struct xrt_display_processor *xdp, bool enable_3d)
{
	struct leia_dp_linux *ldp = leia_dp_linux(xdp);
	return leiasr_lnx_request_display_mode(ldp->sr, enable_3d); // R-D2
}

static bool
leia_lnx_dp_get_hardware_3d_state(struct xrt_display_processor *xdp, bool *out_is_3d)
{
	struct leia_dp_linux *ldp = leia_dp_linux(xdp);
	return leiasr_lnx_get_hardware_3d_state(ldp->sr, out_is_3d); // R-D2
}

static VkRenderPass
leia_lnx_dp_get_render_pass(struct xrt_display_processor *xdp)
{
	struct leia_dp_linux *ldp = leia_dp_linux(xdp);
	return (VkRenderPass)leiasr_lnx_get_render_pass(ldp->sr); // VK_NULL_HANDLE for the blit stub
}

static void
leia_lnx_dp_set_eye_tracking_mode(struct xrt_display_processor *xdp, uint32_t mode)
{
	struct leia_dp_linux *ldp = leia_dp_linux(xdp);
	(void)leiasr_lnx_set_eye_tracking_mode(ldp->sr, (enum leiasr_lnx_tracking_mode)mode); // R-T4/R-T5
}

static void
leia_lnx_dp_notify_target_recreated(struct xrt_display_processor_vk *xdp_vk, uint32_t generation)
{
	struct leia_dp_linux *ldp = (struct leia_dp_linux *)xdp_vk;
	(void)generation;
	leiasr_lnx_output_invalidated(ldp->sr); // R-W8 (idempotent)
}

static void
leia_lnx_dp_destroy(struct xrt_display_processor *xdp)
{
	struct leia_dp_linux *ldp = leia_dp_linux(xdp);
	leiasr_lnx_destroy(ldp->sr); // R-W10
	free(ldp);
}


/*
 *
 * Factory.
 *
 */

xrt_result_t
leia_lnx_dp_factory_vk(void *vk_bundle,
                       void *vk_cmd_pool,
                       void *window_handle,
                       int32_t target_format,
                       struct xrt_display_processor **out_xdp)
{
	struct vk_bundle *vk = (struct vk_bundle *)vk_bundle;
	if (vk == NULL || out_xdp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	// Allocate + fill the vtable BEFORE creating the backend (mirrors the
	// Windows factory's heap-churn ordering, drv_leia).
	struct leia_dp_linux *ldp = calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	// ADR-020 rule 1: advertise the _vk variant so the runtime recognizes
	// the appended notify_target_recreated slot.
	ldp->base.base.struct_size = (uint32_t)sizeof(struct xrt_display_processor_vk);
	ldp->base.base.process_atlas = leia_lnx_dp_process_atlas;
	ldp->base.base.get_predicted_eye_positions = leia_lnx_dp_get_predicted_eye_positions;
	ldp->base.base.get_display_dimensions = leia_lnx_dp_get_display_dimensions;
	ldp->base.base.get_display_pixel_info = leia_lnx_dp_get_display_pixel_info;
	ldp->base.base.request_display_mode = leia_lnx_dp_request_display_mode;
	ldp->base.base.get_hardware_3d_state = leia_lnx_dp_get_hardware_3d_state;
	ldp->base.base.get_render_pass = leia_lnx_dp_get_render_pass;
	ldp->base.base.set_eye_tracking_mode = leia_lnx_dp_set_eye_tracking_mode;
	ldp->base.base.destroy = leia_lnx_dp_destroy;
	ldp->base.notify_target_recreated = leia_lnx_dp_notify_target_recreated;
	// TODO(Track B): get_window_metrics (window-scoped Kooima, needs the
	// X11 window position), is_alpha_native / set_background_2d /
	// set_transparent_background (transparency stack), zone slots
	// (get_local_zone_caps + publish/clear — needs R-W7 phase weaving).
	ldp->vk = vk;
	ldp->view_count = 2;

	// Backend creation on the compositor's Vulkan objects (R-W2). The
	// window handle is NULL on Linux today (display-scoped, contract §3.5);
	// runtime Phase 3b supplies an X11 Window for per-window phase.
	struct leiasr_lnx_create_info info = {
	    .physical_device = vk->physical_device,
	    .device = vk->device,
	    .graphics_queue = vk->main_queue->queue,
	    .graphics_queue_family = vk->main_queue->family_index,
	    .command_pool = (VkCommandPool)(uintptr_t)vk_cmd_pool,
	    .x11_window = window_handle,
	    .x11_connection = NULL,
	    .retry_budget_s = 5.0, // Windows-parity connect budget (R-W1)
	    .target_format = (VkFormat)target_format,
	};
	struct leiasr_lnx *sr = NULL;
	enum leiasr_lnx_result res = leiasr_lnx_create(&info, &sr);
	if (res != LEIASR_LNX_SUCCESS || sr == NULL) {
		U_LOG_W("leia_lnx_dp: weaver backend creation failed (%s)",
		        res == LEIASR_LNX_ERROR_SERVICE_UNAVAILABLE ? "SR service unavailable" : "error");
		free(ldp);
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	ldp->sr = sr;

	U_LOG_I("leia_lnx_dp: Linux VK display processor created (backend: %s)",
	        leiasr_lnx_get_render_pass(sr) == VK_NULL_HANDLE ? "stub passthrough" : "weaver");

	*out_xdp = &ldp->base.base;
	return XRT_SUCCESS;
}
