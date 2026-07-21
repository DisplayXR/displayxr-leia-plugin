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
#include "leia_bg_capture_linux.h"

#include "xrt/xrt_display_processor_vk.h"
#include "xrt/xrt_display_metrics.h"

#include "vk/vk_helpers.h"
#include "util/u_logging.h"

// SPIR-V for the compose-under-bg pass (shaders/ copied from ../drv_leia,
// compiled by spirv_shaders in CMakeLists → shaders_<name> byte arrays).
#include "shaders/fullscreen_tri.vert.h"
#include "shaders/compose_under_bg.frag.h"
#include "shaders/alpha_gate.frag.h"

#include <stdlib.h>
#include <string.h>


struct leia_dp_linux
{
	struct xrt_display_processor_vk base; //!< vtable, MUST be first (offset 0)
	struct vk_bundle *vk;                 //!< compositor's bundle — not owned
	struct leiasr_lnx *sr;                //!< weaver backend — owned
	uint32_t view_count;                  //!< from the last process_atlas grid

	// --- Transparency: pre-weave compose-under-bg (runtime#757) ---------
	// The srSDK weaver flattens alpha (contract R-W12), so — exactly like the
	// Windows DP — transparency is reconstructed DP-side: composite the app
	// content OVER a background into an opaque intermediate the weaver then
	// interlaces. On Linux the background is sampled directly (shared VkDevice,
	// no import): today the runtime's flattened 2D-under backdrop (set_background_2d);
	// WS1 (portal/PipeWire desktop capture) plugs the live desktop into the same
	// bg2d_view seam. Reuses ../drv_leia/shaders/compose_under_bg.frag.
	bool transparent_enabled;   //!< set_transparent_background, in-process path
	VkImageView bg2d_view;      //!< runtime's flattened 2D-under backdrop (or NULL)
	uint32_t bg2d_w, bg2d_h;    //!< backdrop dims (informational)
	struct leia_bg_capture_linux *bg_capture; //!< WS1 live-desktop capture (or NULL)

	// Compose pipeline (lazy, built on the first transparent multi-view frame).
	VkRenderPass compose_rp;              //!< R8G8B8A8_UNORM intermediate pass
	VkImage compose_fill_image;           //!< atlas-sized opaque intermediate
	VkImageView compose_fill_view;        //!< sampled by the weaver as its input
	VkDeviceMemory compose_fill_mem;
	VkFramebuffer compose_fill_fb;
	uint32_t compose_fill_w, compose_fill_h;
	VkSampler compose_sampler;
	VkDescriptorSetLayout compose_desc_layout;
	VkDescriptorPool compose_desc_pool;
	VkDescriptorSet compose_set;
	VkPipelineLayout compose_pipeline_layout;
	VkPipeline compose_pipeline;
	bool compose_inited;

	// Post-weave alpha-gate (runtime#757): the srSDK weave flattens alpha, so
	// after the weave we re-punch alpha=0 where EVERY view was transparent — the
	// compositor (mutter) then shows the live desktop through those holes. The
	// de-occlusion band (some views opaque) stays opaque and relies on the
	// compose-under-bg pass above having baked the desktop in pre-weave.
	VkRenderPass ag_rp;            //!< renders into the target (its format, COLOR_ATTACHMENT in/out)
	VkFormat ag_target_format;     //!< format ag_rp was built for
	VkImage ag_strip_image;        //!< sampleable copy of the woven target
	VkImageView ag_strip_view;
	VkDeviceMemory ag_strip_mem;
	uint32_t ag_strip_w, ag_strip_h;
	VkSampler ag_sampler;
	VkDescriptorSetLayout ag_desc_layout;
	VkDescriptorPool ag_desc_pool;
	VkDescriptorSet ag_set;
	VkPipelineLayout ag_pipeline_layout;
	VkPipeline ag_pipeline;
	bool ag_inited;
	//! Framebuffer cache keyed by target image (the compositor's swapchain ring).
	struct {
		VkImage image;
		VkImageView view;
		VkFramebuffer fb;
		uint32_t w, h;
	} ag_fbs[4];
	uint32_t ag_fbs_count;

	//! Windowed weaving (runtime#757 / LeiaSR#85): the app window's client-area
	//! top-left in panel-relative pixels, pushed each frame by the compositor via
	//! the set_present_origin slot. (0,0) = full-panel/display-scoped (default).
	//! Combined with the per-atlas canvas offset at weave time (present + viewport).
	int32_t present_origin_x;
	int32_t present_origin_y;
};

static inline struct leia_dp_linux *
leia_dp_linux(struct xrt_display_processor *xdp)
{
	return (struct leia_dp_linux *)xdp;
}


/*
 *
 * Transparency: pre-weave compose-under-bg (runtime#757).
 *
 * Ported from the Windows VK DP (../drv_leia/leia_display_processor.cpp), minus
 * the Windows-only pieces: no WGC capture / NT-handle import (that is WS1's
 * portal/PipeWire → dma-buf producer), no chroma-key fallback, no post-weave
 * alpha-gate. The background arrives already in the compositor's VkDevice via
 * set_background_2d, so it is sampled directly. Runs only for transparent
 * multi-view frames that have a background bound.
 *
 */

static void
compose_release(struct leia_dp_linux *ldp)
{
	struct vk_bundle *vk = ldp->vk;
	if (vk == NULL) {
		return;
	}
	if (ldp->compose_pipeline != VK_NULL_HANDLE) {
		vk->vkDestroyPipeline(vk->device, ldp->compose_pipeline, NULL);
	}
	if (ldp->compose_pipeline_layout != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineLayout(vk->device, ldp->compose_pipeline_layout, NULL);
	}
	if (ldp->compose_desc_pool != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorPool(vk->device, ldp->compose_desc_pool, NULL);
	}
	if (ldp->compose_desc_layout != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorSetLayout(vk->device, ldp->compose_desc_layout, NULL);
	}
	if (ldp->compose_sampler != VK_NULL_HANDLE) {
		vk->vkDestroySampler(vk->device, ldp->compose_sampler, NULL);
	}
	if (ldp->compose_fill_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, ldp->compose_fill_fb, NULL);
	}
	if (ldp->compose_fill_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, ldp->compose_fill_view, NULL);
	}
	if (ldp->compose_fill_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, ldp->compose_fill_image, NULL);
	}
	if (ldp->compose_fill_mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, ldp->compose_fill_mem, NULL);
	}
	if (ldp->compose_rp != VK_NULL_HANDLE) {
		vk->vkDestroyRenderPass(vk->device, ldp->compose_rp, NULL);
	}
	ldp->compose_pipeline = VK_NULL_HANDLE;
	ldp->compose_pipeline_layout = VK_NULL_HANDLE;
	ldp->compose_desc_pool = VK_NULL_HANDLE;
	ldp->compose_desc_layout = VK_NULL_HANDLE;
	ldp->compose_sampler = VK_NULL_HANDLE;
	ldp->compose_fill_fb = VK_NULL_HANDLE;
	ldp->compose_fill_view = VK_NULL_HANDLE;
	ldp->compose_fill_image = VK_NULL_HANDLE;
	ldp->compose_fill_mem = VK_NULL_HANDLE;
	ldp->compose_rp = VK_NULL_HANDLE;
	ldp->compose_fill_w = 0;
	ldp->compose_fill_h = 0;
	ldp->compose_inited = false;
}

// Create (or resize) the atlas-sized R8G8B8A8_UNORM intermediate the compose
// pass renders into and the weaver then samples.
static bool
compose_ensure_fill(struct leia_dp_linux *ldp, uint32_t w, uint32_t h)
{
	struct vk_bundle *vk = ldp->vk;
	if (ldp->compose_fill_image != VK_NULL_HANDLE && ldp->compose_fill_w == w && ldp->compose_fill_h == h) {
		return true;
	}

	if (ldp->compose_fill_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, ldp->compose_fill_fb, NULL);
		ldp->compose_fill_fb = VK_NULL_HANDLE;
	}
	if (ldp->compose_fill_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, ldp->compose_fill_view, NULL);
		ldp->compose_fill_view = VK_NULL_HANDLE;
	}
	if (ldp->compose_fill_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, ldp->compose_fill_image, NULL);
		ldp->compose_fill_image = VK_NULL_HANDLE;
	}
	if (ldp->compose_fill_mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, ldp->compose_fill_mem, NULL);
		ldp->compose_fill_mem = VK_NULL_HANDLE;
	}

	VkExtent2D ext = {w, h};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkResult res =
	    vk_create_image_simple(vk, ext, VK_FORMAT_R8G8B8A8_UNORM, usage, &ldp->compose_fill_mem, &ldp->compose_fill_image);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: compose fill image create failed: %d", res);
		return false;
	}
	VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	res = vk_create_view(vk, ldp->compose_fill_image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, sub,
	                     &ldp->compose_fill_view);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: compose fill view create failed: %d", res);
		return false;
	}
	VkFramebufferCreateInfo fbi = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = ldp->compose_rp,
	    .attachmentCount = 1,
	    .pAttachments = &ldp->compose_fill_view,
	    .width = w,
	    .height = h,
	    .layers = 1,
	};
	res = vk->vkCreateFramebuffer(vk->device, &fbi, NULL, &ldp->compose_fill_fb);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: compose fill fb create failed: %d", res);
		return false;
	}
	ldp->compose_fill_w = w;
	ldp->compose_fill_h = h;
	return true;
}

// Build the render pass + pipeline + descriptor plumbing once. Idempotent.
static bool
compose_ensure_pipeline(struct leia_dp_linux *ldp)
{
	if (ldp->compose_inited) {
		return true;
	}
	struct vk_bundle *vk = ldp->vk;
	VkResult res;

	// Render pass: opaque R8G8B8A8_UNORM intermediate, no presentation.
	{
		VkAttachmentDescription att = {
		    .format = VK_FORMAT_R8G8B8A8_UNORM,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		    .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};
		VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sub = {
		    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		    .colorAttachmentCount = 1,
		    .pColorAttachments = &ref,
		};
		VkRenderPassCreateInfo rpi = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		    .attachmentCount = 1,
		    .pAttachments = &att,
		    .subpassCount = 1,
		    .pSubpasses = &sub,
		};
		res = vk->vkCreateRenderPass(vk->device, &rpi, NULL, &ldp->compose_rp);
		if (res != VK_SUCCESS) {
			U_LOG_E("leia_lnx_dp: compose render pass failed: %d", res);
			return false;
		}
	}

	// 3-binding descriptor set: atlas (0) + bg/desktop (1) + 2D-under backdrop (2).
	{
		VkDescriptorSetLayoutBinding bs[3] = {
		    {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
		     .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		    {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
		     .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		    {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
		     .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		};
		VkDescriptorSetLayoutCreateInfo ci = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bs};
		res = vk->vkCreateDescriptorSetLayout(vk->device, &ci, NULL, &ldp->compose_desc_layout);
		if (res != VK_SUCCESS) {
			U_LOG_E("leia_lnx_dp: compose desc layout failed: %d", res);
			return false;
		}
	}

	// Push constants: 2*vec2 + uvec2 + uvec2 = 32 bytes (matches compose_under_bg.frag).
	{
		VkPushConstantRange pc = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 32};
		VkPipelineLayoutCreateInfo pli = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		    .setLayoutCount = 1,
		    .pSetLayouts = &ldp->compose_desc_layout,
		    .pushConstantRangeCount = 1,
		    .pPushConstantRanges = &pc,
		};
		res = vk->vkCreatePipelineLayout(vk->device, &pli, NULL, &ldp->compose_pipeline_layout);
		if (res != VK_SUCCESS) {
			U_LOG_E("leia_lnx_dp: compose pipeline layout failed: %d", res);
			return false;
		}
	}

	res = vk_create_sampler(vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, &ldp->compose_sampler);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: compose sampler failed: %d", res);
		return false;
	}

	{
		VkDescriptorPoolSize size = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 3};
		VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		                                  .maxSets = 1,
		                                  .poolSizeCount = 1,
		                                  .pPoolSizes = &size};
		res = vk->vkCreateDescriptorPool(vk->device, &dpi, NULL, &ldp->compose_desc_pool);
		if (res != VK_SUCCESS) {
			U_LOG_E("leia_lnx_dp: compose desc pool failed: %d", res);
			return false;
		}
		VkDescriptorSetAllocateInfo ai = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		    .descriptorPool = ldp->compose_desc_pool,
		    .descriptorSetCount = 1,
		    .pSetLayouts = &ldp->compose_desc_layout,
		};
		res = vk->vkAllocateDescriptorSets(vk->device, &ai, &ldp->compose_set);
		if (res != VK_SUCCESS) {
			U_LOG_E("leia_lnx_dp: compose desc set alloc failed: %d", res);
			return false;
		}
	}

	// Pipeline (fullscreen triangle + compose_under_bg, against compose_rp).
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo vsi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                .codeSize = sizeof(shaders_fullscreen_tri_vert),
	                                .pCode = shaders_fullscreen_tri_vert};
	res = vk->vkCreateShaderModule(vk->device, &vsi, NULL, &vs);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: compose vs create failed: %d", res);
		return false;
	}
	VkShaderModuleCreateInfo fsi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                .codeSize = sizeof(shaders_compose_under_bg_frag),
	                                .pCode = shaders_compose_under_bg_frag};
	res = vk->vkCreateShaderModule(vk->device, &fsi, NULL, &fs);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: compose fs create failed: %d", res);
		vk->vkDestroyShaderModule(vk->device, vs, NULL);
		return false;
	}

	VkPipelineVertexInputStateCreateInfo vi = {.sType =
	                                               VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {.sType =
	                                                 VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	                                             .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	                                         .viewportCount = 1,
	                                         .scissorCount = 1};
	VkPipelineRasterizationStateCreateInfo rs = {.sType =
	                                                 VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	                                             .polygonMode = VK_POLYGON_MODE_FILL,
	                                             .cullMode = VK_CULL_MODE_NONE,
	                                             .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	                                             .lineWidth = 1.0f};
	VkPipelineMultisampleStateCreateInfo ms = {.sType =
	                                               VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	                                           .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
	VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
	VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	                                          .attachmentCount = 1,
	                                          .pAttachments = &ba};
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynstate = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	                                             .dynamicStateCount = 2,
	                                             .pDynamicStates = dyn};
	VkPipelineShaderStageCreateInfo stages[2] = {
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	     .stage = VK_SHADER_STAGE_VERTEX_BIT,
	     .module = vs,
	     .pName = "main"},
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	     .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	     .module = fs,
	     .pName = "main"},
	};
	VkGraphicsPipelineCreateInfo pi = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2,
	    .pStages = stages,
	    .pVertexInputState = &vi,
	    .pInputAssemblyState = &ia,
	    .pViewportState = &vps,
	    .pRasterizationState = &rs,
	    .pMultisampleState = &ms,
	    .pColorBlendState = &cb,
	    .pDynamicState = &dynstate,
	    .layout = ldp->compose_pipeline_layout,
	    .renderPass = ldp->compose_rp,
	    .subpass = 0,
	};
	res = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pi, NULL, &ldp->compose_pipeline);
	vk->vkDestroyShaderModule(vk->device, fs, NULL);
	vk->vkDestroyShaderModule(vk->device, vs, NULL);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: compose pipeline create failed: %d", res);
		return false;
	}

	ldp->compose_inited = true;
	U_LOG_W("leia_lnx_dp: transparency compose-under-bg pipeline initialized");
	return true;
}

// Composite the tiled atlas OVER the bound background into the opaque
// intermediate, and return the intermediate view for the weaver to interlace.
// Returns VK_NULL_HANDLE (→ caller weaves the raw atlas) when disabled, when no
// background is bound, or on any setup failure.
static VkImageView
compose_pre_weave(struct leia_dp_linux *ldp,
                  VkCommandBuffer cmd,
                  VkImageView atlas_view,
                  uint32_t atlas_w,
                  uint32_t atlas_h,
                  uint32_t tile_columns,
                  uint32_t tile_rows)
{
	struct vk_bundle *vk = ldp->vk;
	if (!ldp->transparent_enabled) {
		return VK_NULL_HANDLE;
	}
	if (ldp->bg_capture == NULL && ldp->bg2d_view == VK_NULL_HANDLE) {
		return VK_NULL_HANDLE; // no background source → weave the raw atlas
	}
	if (!compose_ensure_pipeline(ldp) || !compose_ensure_fill(ldp, atlas_w, atlas_h)) {
		return VK_NULL_HANDLE;
	}

	// Pick the background under the atlas (Windows-parity): the live desktop
	// (WS1 capture) when a frame is available, else the runtime's flattened
	// 2D-under backdrop. When BOTH are present the 2D-under composites OVER the
	// desktop (has_backdrop=1) — a flat 2D plane behind the woven 3D that still
	// reveals the desktop where it is transparent.
	float uv_origin[2] = {0.0f, 0.0f};
	float uv_extent[2] = {1.0f, 1.0f};
	VkImageView bg = VK_NULL_HANDLE;
	VkImageView backdrop = VK_NULL_HANDLE;
	if (ldp->bg_capture != NULL && leia_bg_capture_linux_poll(ldp->bg_capture, cmd, uv_origin, uv_extent)) {
		bg = leia_bg_capture_linux_get_view(ldp->bg_capture); // captured monitor, poll'd UV sub-rect
		backdrop = ldp->bg2d_view;                            // may be NULL → gated off below
	} else if (ldp->bg2d_view != VK_NULL_HANDLE) {
		bg = ldp->bg2d_view; // 2D-under is the background; already canvas-space (UV 0..1)
	}
	if (bg == VK_NULL_HANDLE) {
		return VK_NULL_HANDLE; // capture not ready and no backdrop this frame
	}
	const bool have_backdrop = (backdrop != VK_NULL_HANDLE);

	// Bind atlas (0) + background (1) + backdrop (2). The backdrop slot is bound
	// to a valid image even when unused (gated by has_backdrop) so the set is
	// always complete. atlas + backdrop arrive in SHADER_READ_ONLY_OPTIMAL
	// (compositor + set_background_2d contract); the captured desktop is
	// transitioned to SHADER_READ inside poll().
	VkDescriptorImageInfo atlas_info = {.sampler = ldp->compose_sampler,
	                                    .imageView = atlas_view,
	                                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkDescriptorImageInfo bg_info = {.sampler = ldp->compose_sampler,
	                                 .imageView = bg,
	                                 .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkDescriptorImageInfo bd_info = {.sampler = ldp->compose_sampler,
	                                 .imageView = have_backdrop ? backdrop : bg,
	                                 .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet writes[3] = {
	    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ldp->compose_set, .dstBinding = 0,
	     .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &atlas_info},
	    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ldp->compose_set, .dstBinding = 1,
	     .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &bg_info},
	    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ldp->compose_set, .dstBinding = 2,
	     .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &bd_info},
	};
	vk->vkUpdateDescriptorSets(vk->device, 3, writes, 0, NULL);

	// Intermediate UNDEFINED → COLOR_ATTACHMENT (contents fully overwritten).
	VkImageMemoryBarrier pre = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = ldp->compose_fill_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
	                         0, NULL, 0, NULL, 1, &pre);

	VkRenderPassBeginInfo rpbi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = ldp->compose_rp,
	    .framebuffer = ldp->compose_fill_fb,
	    .renderArea = {{0, 0}, {atlas_w, atlas_h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ldp->compose_pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ldp->compose_pipeline_layout, 0, 1,
	                            &ldp->compose_set, 0, NULL);

	struct
	{
		float bg_uv_origin[2];
		float bg_uv_extent[2];
		uint32_t tile_count[2];
		uint32_t has_backdrop;
		uint32_t pad;
	} push = {0};
	// UV maps the app-window region onto the background: (0,0)-(1,1) for the
	// canvas-space 2D-under backdrop, or the poll'd window-on-monitor sub-rect
	// for the captured desktop.
	push.bg_uv_origin[0] = uv_origin[0];
	push.bg_uv_origin[1] = uv_origin[1];
	push.bg_uv_extent[0] = uv_extent[0];
	push.bg_uv_extent[1] = uv_extent[1];
	push.tile_count[0] = tile_columns;
	push.tile_count[1] = tile_rows;
	push.has_backdrop = have_backdrop ? 1u : 0u;
	vk->vkCmdPushConstants(cmd, ldp->compose_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

	VkViewport vp = {0.0f, 0.0f, (float)atlas_w, (float)atlas_h, 0.0f, 1.0f};
	VkRect2D sc = {{0, 0}, {atlas_w, atlas_h}};
	vk->vkCmdSetViewport(cmd, 0, 1, &vp);
	vk->vkCmdSetScissor(cmd, 0, 1, &sc);
	vk->vkCmdDraw(cmd, 3, 1, 0, 0);
	vk->vkCmdEndRenderPass(cmd);

	// Intermediate COLOR_ATTACHMENT → SHADER_READ for the weaver.
	VkImageMemoryBarrier post = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = ldp->compose_fill_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                         0, 0, NULL, 0, NULL, 1, &post);

	return ldp->compose_fill_view;
}


/*
 *
 * Post-weave alpha-gate (runtime#757).
 *
 * The srSDK weave flattens alpha to opaque. After it runs, sample the woven
 * output + the ORIGINAL atlas: where every view's atlas was transparent, punch
 * alpha=0 so the compositor (mutter) shows the live desktop; elsewhere keep the
 * woven RGB opaque. Ported from ../drv_leia (Windows), minus the #602
 * high-water-mark strip and the swapchain PRESENT_SRC layout — the Linux target
 * is a COLOR_ATTACHMENT the runtime presents. Shares no state with the compose
 * pass except the vk_bundle.
 *
 */

static void
ag_release(struct leia_dp_linux *ldp)
{
	struct vk_bundle *vk = ldp->vk;
	if (vk == NULL) {
		return;
	}
	for (uint32_t i = 0; i < ldp->ag_fbs_count; i++) {
		if (ldp->ag_fbs[i].fb != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, ldp->ag_fbs[i].fb, NULL);
		}
		if (ldp->ag_fbs[i].view != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, ldp->ag_fbs[i].view, NULL);
		}
	}
	ldp->ag_fbs_count = 0;
	memset(ldp->ag_fbs, 0, sizeof(ldp->ag_fbs));
	if (ldp->ag_pipeline != VK_NULL_HANDLE) {
		vk->vkDestroyPipeline(vk->device, ldp->ag_pipeline, NULL);
	}
	if (ldp->ag_pipeline_layout != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineLayout(vk->device, ldp->ag_pipeline_layout, NULL);
	}
	if (ldp->ag_desc_pool != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorPool(vk->device, ldp->ag_desc_pool, NULL);
	}
	if (ldp->ag_desc_layout != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorSetLayout(vk->device, ldp->ag_desc_layout, NULL);
	}
	if (ldp->ag_sampler != VK_NULL_HANDLE) {
		vk->vkDestroySampler(vk->device, ldp->ag_sampler, NULL);
	}
	if (ldp->ag_strip_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, ldp->ag_strip_view, NULL);
	}
	if (ldp->ag_strip_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, ldp->ag_strip_image, NULL);
	}
	if (ldp->ag_strip_mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, ldp->ag_strip_mem, NULL);
	}
	if (ldp->ag_rp != VK_NULL_HANDLE) {
		vk->vkDestroyRenderPass(vk->device, ldp->ag_rp, NULL);
	}
	ldp->ag_pipeline = VK_NULL_HANDLE;
	ldp->ag_pipeline_layout = VK_NULL_HANDLE;
	ldp->ag_desc_pool = VK_NULL_HANDLE;
	ldp->ag_desc_layout = VK_NULL_HANDLE;
	ldp->ag_sampler = VK_NULL_HANDLE;
	ldp->ag_strip_view = VK_NULL_HANDLE;
	ldp->ag_strip_image = VK_NULL_HANDLE;
	ldp->ag_strip_mem = VK_NULL_HANDLE;
	ldp->ag_rp = VK_NULL_HANDLE;
	ldp->ag_strip_w = 0;
	ldp->ag_strip_h = 0;
	ldp->ag_inited = false;
}

// Sampleable copy of the woven target (target format), sized (w, h).
static bool
ag_ensure_strip(struct leia_dp_linux *ldp, uint32_t w, uint32_t h)
{
	struct vk_bundle *vk = ldp->vk;
	if (ldp->ag_strip_image != VK_NULL_HANDLE && ldp->ag_strip_w == w && ldp->ag_strip_h == h) {
		return true;
	}
	if (ldp->ag_strip_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, ldp->ag_strip_view, NULL);
		ldp->ag_strip_view = VK_NULL_HANDLE;
	}
	if (ldp->ag_strip_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, ldp->ag_strip_image, NULL);
		ldp->ag_strip_image = VK_NULL_HANDLE;
	}
	if (ldp->ag_strip_mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, ldp->ag_strip_mem, NULL);
		ldp->ag_strip_mem = VK_NULL_HANDLE;
	}
	VkExtent2D ext = {w, h};
	VkResult res = vk_create_image_simple(vk, ext, ldp->ag_target_format,
	                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	                                      &ldp->ag_strip_mem, &ldp->ag_strip_image);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: alpha-gate strip image failed: %d", res);
		return false;
	}
	VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	res = vk_create_view(vk, ldp->ag_strip_image, VK_IMAGE_VIEW_TYPE_2D, ldp->ag_target_format, sub, &ldp->ag_strip_view);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: alpha-gate strip view failed: %d", res);
		return false;
	}
	ldp->ag_strip_w = w;
	ldp->ag_strip_h = h;
	return true;
}

// View + framebuffer for the target image (cached; the target rotates over the
// compositor's swapchain ring).
static VkFramebuffer
ag_get_fb(struct leia_dp_linux *ldp, VkImage target_image, uint32_t w, uint32_t h)
{
	struct vk_bundle *vk = ldp->vk;
	for (uint32_t i = 0; i < ldp->ag_fbs_count; i++) {
		if (ldp->ag_fbs[i].image == target_image && ldp->ag_fbs[i].w == w && ldp->ag_fbs[i].h == h) {
			return ldp->ag_fbs[i].fb;
		}
	}
	VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageView view = VK_NULL_HANDLE;
	if (vk_create_view(vk, target_image, VK_IMAGE_VIEW_TYPE_2D, ldp->ag_target_format, sub, &view) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	VkFramebufferCreateInfo fbi = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = ldp->ag_rp,
	    .attachmentCount = 1,
	    .pAttachments = &view,
	    .width = w,
	    .height = h,
	    .layers = 1,
	};
	VkFramebuffer fb = VK_NULL_HANDLE;
	if (vk->vkCreateFramebuffer(vk->device, &fbi, NULL, &fb) != VK_SUCCESS) {
		vk->vkDestroyImageView(vk->device, view, NULL);
		return VK_NULL_HANDLE;
	}
	if (ldp->ag_fbs_count >= 4) { // LRU-evict the oldest
		vk->vkDestroyFramebuffer(vk->device, ldp->ag_fbs[0].fb, NULL);
		vk->vkDestroyImageView(vk->device, ldp->ag_fbs[0].view, NULL);
		for (uint32_t i = 1; i < ldp->ag_fbs_count; i++) {
			ldp->ag_fbs[i - 1] = ldp->ag_fbs[i];
		}
		ldp->ag_fbs_count--;
	}
	uint32_t idx = ldp->ag_fbs_count++;
	ldp->ag_fbs[idx].image = target_image;
	ldp->ag_fbs[idx].view = view;
	ldp->ag_fbs[idx].fb = fb;
	ldp->ag_fbs[idx].w = w;
	ldp->ag_fbs[idx].h = h;
	return fb;
}

// Build the alpha-gate render pass + descriptor plumbing + pipeline. Idempotent
// (rebuilds if the target format changes).
static bool
ag_ensure_pipeline(struct leia_dp_linux *ldp, VkFormat target_format)
{
	if (ldp->ag_inited && ldp->ag_target_format == target_format) {
		return true;
	}
	if (ldp->ag_inited) {
		ag_release(ldp); // format changed — rebuild
	}
	struct vk_bundle *vk = ldp->vk;
	VkResult res;
	ldp->ag_target_format = target_format;

	// Render pass: write the target format, no load (the gate overwrites every
	// pixel), COLOR_ATTACHMENT in/out (the runtime owns present, contract R-W5).
	{
		VkAttachmentDescription att = {
		    .format = target_format,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		    .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};
		VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sub = {
		    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		    .colorAttachmentCount = 1,
		    .pColorAttachments = &ref,
		};
		VkRenderPassCreateInfo rpi = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		    .attachmentCount = 1,
		    .pAttachments = &att,
		    .subpassCount = 1,
		    .pSubpasses = &sub,
		};
		res = vk->vkCreateRenderPass(vk->device, &rpi, NULL, &ldp->ag_rp);
		if (res != VK_SUCCESS) {
			U_LOG_E("leia_lnx_dp: alpha-gate render pass failed: %d", res);
			return false;
		}
	}

	// 3-binding descriptor set: woven backbuffer copy (0) + original atlas (1) +
	// 2D-under backdrop (2, dummy when absent — gated by has_backdrop).
	{
		VkDescriptorSetLayoutBinding bs[3] = {
		    {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
		     .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		    {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
		     .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		    {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
		     .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		};
		VkDescriptorSetLayoutCreateInfo ci = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bs};
		res = vk->vkCreateDescriptorSetLayout(vk->device, &ci, NULL, &ldp->ag_desc_layout);
		if (res != VK_SUCCESS) {
			U_LOG_E("leia_lnx_dp: alpha-gate desc layout failed: %d", res);
			return false;
		}
	}

	// Push constants: uvec2 tile_count + uint has_backdrop + uint pad + vec2
	// strip_uv_scale = 24 bytes (matches alpha_gate.frag).
	{
		VkPushConstantRange pc = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 24};
		VkPipelineLayoutCreateInfo pli = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		    .setLayoutCount = 1,
		    .pSetLayouts = &ldp->ag_desc_layout,
		    .pushConstantRangeCount = 1,
		    .pPushConstantRanges = &pc,
		};
		res = vk->vkCreatePipelineLayout(vk->device, &pli, NULL, &ldp->ag_pipeline_layout);
		if (res != VK_SUCCESS) {
			U_LOG_E("leia_lnx_dp: alpha-gate pipeline layout failed: %d", res);
			return false;
		}
	}

	res = vk_create_sampler(vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, &ldp->ag_sampler);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: alpha-gate sampler failed: %d", res);
		return false;
	}

	{
		VkDescriptorPoolSize size = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 3};
		VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		                                  .maxSets = 1,
		                                  .poolSizeCount = 1,
		                                  .pPoolSizes = &size};
		res = vk->vkCreateDescriptorPool(vk->device, &dpi, NULL, &ldp->ag_desc_pool);
		if (res != VK_SUCCESS) {
			U_LOG_E("leia_lnx_dp: alpha-gate desc pool failed: %d", res);
			return false;
		}
		VkDescriptorSetAllocateInfo ai = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		    .descriptorPool = ldp->ag_desc_pool,
		    .descriptorSetCount = 1,
		    .pSetLayouts = &ldp->ag_desc_layout,
		};
		res = vk->vkAllocateDescriptorSets(vk->device, &ai, &ldp->ag_set);
		if (res != VK_SUCCESS) {
			U_LOG_E("leia_lnx_dp: alpha-gate desc set alloc failed: %d", res);
			return false;
		}
	}

	// Pipeline (fullscreen triangle + alpha_gate, against ag_rp).
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo vsi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                .codeSize = sizeof(shaders_fullscreen_tri_vert),
	                                .pCode = shaders_fullscreen_tri_vert};
	res = vk->vkCreateShaderModule(vk->device, &vsi, NULL, &vs);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: alpha-gate vs failed: %d", res);
		return false;
	}
	VkShaderModuleCreateInfo fsi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                .codeSize = sizeof(shaders_alpha_gate_frag),
	                                .pCode = shaders_alpha_gate_frag};
	res = vk->vkCreateShaderModule(vk->device, &fsi, NULL, &fs);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: alpha-gate fs failed: %d", res);
		vk->vkDestroyShaderModule(vk->device, vs, NULL);
		return false;
	}

	VkPipelineVertexInputStateCreateInfo vi = {.sType =
	                                               VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {.sType =
	                                                 VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	                                             .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	                                         .viewportCount = 1,
	                                         .scissorCount = 1};
	VkPipelineRasterizationStateCreateInfo rs = {.sType =
	                                                 VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	                                             .polygonMode = VK_POLYGON_MODE_FILL,
	                                             .cullMode = VK_CULL_MODE_NONE,
	                                             .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	                                             .lineWidth = 1.0f};
	VkPipelineMultisampleStateCreateInfo ms = {.sType =
	                                               VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	                                           .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
	VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
	VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	                                          .attachmentCount = 1,
	                                          .pAttachments = &ba};
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynstate = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	                                             .dynamicStateCount = 2,
	                                             .pDynamicStates = dyn};
	VkPipelineShaderStageCreateInfo stages[2] = {
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	     .stage = VK_SHADER_STAGE_VERTEX_BIT,
	     .module = vs,
	     .pName = "main"},
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	     .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	     .module = fs,
	     .pName = "main"},
	};
	VkGraphicsPipelineCreateInfo pi = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2,
	    .pStages = stages,
	    .pVertexInputState = &vi,
	    .pInputAssemblyState = &ia,
	    .pViewportState = &vps,
	    .pRasterizationState = &rs,
	    .pMultisampleState = &ms,
	    .pColorBlendState = &cb,
	    .pDynamicState = &dynstate,
	    .layout = ldp->ag_pipeline_layout,
	    .renderPass = ldp->ag_rp,
	    .subpass = 0,
	};
	res = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pi, NULL, &ldp->ag_pipeline);
	vk->vkDestroyShaderModule(vk->device, fs, NULL);
	vk->vkDestroyShaderModule(vk->device, vs, NULL);
	if (res != VK_SUCCESS) {
		U_LOG_E("leia_lnx_dp: alpha-gate pipeline failed: %d", res);
		return false;
	}

	ldp->ag_inited = true;
	U_LOG_W("leia_lnx_dp: post-weave alpha-gate initialized");
	return true;
}

// Post-weave: copy the woven target to a sampleable strip, then render the gate
// back into the target — punching alpha=0 where every view's ORIGINAL atlas was
// transparent (→ live desktop), keeping woven RGB opaque elsewhere. The target
// enters and leaves in COLOR_ATTACHMENT_OPTIMAL (contract R-W5).
static void
alpha_gate_run(struct leia_dp_linux *ldp,
               VkCommandBuffer cmd,
               VkImage target_image,
               VkImageView atlas_view,
               VkFormat target_format,
               uint32_t w,
               uint32_t h,
               uint32_t tile_columns,
               uint32_t tile_rows)
{
	struct vk_bundle *vk = ldp->vk;
	if (!ldp->transparent_enabled || target_image == VK_NULL_HANDLE || atlas_view == VK_NULL_HANDLE) {
		return;
	}
	if (!ag_ensure_pipeline(ldp, target_format)) {
		return;
	}
	if (!ag_ensure_strip(ldp, w, h)) {
		return;
	}
	VkFramebuffer fb = ag_get_fb(ldp, target_image, w, h);
	if (fb == VK_NULL_HANDLE) {
		return;
	}

	// Descriptors: 0 = strip (woven copy), 1 = atlas (original), 2 = backdrop
	// (dummy = strip; gated by has_backdrop=0 — no 2D-under on Linux yet).
	VkDescriptorImageInfo infos[3] = {
	    {.sampler = ldp->ag_sampler, .imageView = ldp->ag_strip_view,
	     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	    {.sampler = ldp->ag_sampler, .imageView = atlas_view,
	     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	    {.sampler = ldp->ag_sampler, .imageView = ldp->ag_strip_view,
	     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	};
	VkWriteDescriptorSet writes[3];
	for (uint32_t i = 0; i < 3; i++) {
		writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		                                   .dstSet = ldp->ag_set,
		                                   .dstBinding = i,
		                                   .descriptorCount = 1,
		                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		                                   .pImageInfo = &infos[i]};
	}
	vk->vkUpdateDescriptorSets(vk->device, 3, writes, 0, NULL);

	// target COLOR_ATTACHMENT → TRANSFER_SRC; strip UNDEFINED → TRANSFER_DST.
	VkImageMemoryBarrier pre[2] = {
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	     .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	     .image = target_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = 0,
	     .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	     .image = ldp->ag_strip_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                         NULL, 0, NULL, 2, pre);

	VkImageCopy region = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .extent = {w, h, 1},
	};
	vk->vkCmdCopyImage(cmd, target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ldp->ag_strip_image,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// target TRANSFER_SRC → COLOR_ATTACHMENT (gate draws into it); strip
	// TRANSFER_DST → SHADER_READ.
	VkImageMemoryBarrier mid[2] = {
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	     .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	     .image = target_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	     .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	     .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	     .image = ldp->ag_strip_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
	                         0, NULL, 0, NULL, 2, mid);

	VkRenderPassBeginInfo rpbi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = ldp->ag_rp,
	    .framebuffer = fb,
	    .renderArea = {{0, 0}, {w, h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ldp->ag_pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ldp->ag_pipeline_layout, 0, 1, &ldp->ag_set, 0,
	                            NULL);

	struct
	{
		uint32_t tile_count[2];
		uint32_t has_backdrop;
		uint32_t pad;
		float strip_uv_scale[2];
	} push = {0};
	push.tile_count[0] = tile_columns;
	push.tile_count[1] = tile_rows;
	push.has_backdrop = 0u;    // no 2D-under backdrop on Linux yet
	push.strip_uv_scale[0] = 1.0f; // strip sized exactly (w,h) — no high-water-mark
	push.strip_uv_scale[1] = 1.0f;
	vk->vkCmdPushConstants(cmd, ldp->ag_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

	VkViewport vp = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
	VkRect2D sc = {{0, 0}, {w, h}};
	vk->vkCmdSetViewport(cmd, 0, 1, &vp);
	vk->vkCmdSetScissor(cmd, 0, 1, &sc);
	vk->vkCmdDraw(cmd, 3, 1, 0, 0);
	vk->vkCmdEndRenderPass(cmd);
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

	// Transparency (runtime#757): the weaver flattens alpha, so composite the
	// atlas OVER the bound background into an opaque intermediate FIRST and weave
	// that. No-op (returns NULL) unless transparency is enabled and a background
	// is bound — then the raw atlas is woven as before. The intermediate is
	// R8G8B8A8_UNORM, so the weave input format is overridden to match.
	VkImage weave_atlas_image = (VkImage)atlas_image;
	VkImageView weave_atlas_view = (VkImageView)atlas_view;
	VkFormat weave_view_format = (VkFormat)view_format;
	VkImageView composed = compose_pre_weave(ldp, cmd_buffer, (VkImageView)atlas_view, view_width * tile_columns,
	                                         view_height * tile_rows, tile_columns, tile_rows);
	if (composed != VK_NULL_HANDLE) {
		weave_atlas_image = ldp->compose_fill_image;
		weave_atlas_view = composed;
		weave_view_format = VK_FORMAT_R8G8B8A8_UNORM;
	}

	// Multi-view: hand the whole tiled atlas to the backend (contract R-W4:
	// one atlas image + explicit grid). The atlas is guaranteed content-sized
	// by the compositor's crop-blit (ADR-030).
	struct leiasr_lnx_weave_input input = {
	    .atlas_image = weave_atlas_image,
	    .atlas_view = weave_atlas_view,
	    .view_width = view_width,
	    .view_height = view_height,
	    .view_format = weave_view_format,
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
	// Windowed weaving (runtime#757 / LeiaSR#85): the phase origin is the app
	// WINDOW's panel-relative top-left (pushed by the compositor via
	// set_present_origin; 0,0 = display-scoped default). The weave adds the
	// viewport (canvas) offset on top — phase = present_origin + canvas_offset —
	// so this must carry the window term ONLY, not the canvas offset (which the
	// `viewport` above already supplies), or the two would double-count.
	struct leiasr_lnx_phase_origin phase = {
	    .x = ldp->present_origin_x,
	    .y = ldp->present_origin_y,
	};

	leiasr_lnx_weave(ldp->sr, cmd_buffer, &input, &output, viewport, phase);

	// Post-weave alpha-gate (runtime#757): the weave flattened alpha to opaque,
	// so where EVERY view's ORIGINAL atlas was transparent, re-punch alpha=0 so
	// the compositor shows the live desktop through. The de-occlusion band (some
	// views opaque) was already baked opaque by compose_pre_weave above, so it
	// stays. Pass the ORIGINAL atlas_view (pre-compose) — that carries the app's
	// per-view alpha the gate keys on.
	if (ldp->transparent_enabled && target_image != (VkImage_XDP)0) {
		alpha_gate_run(ldp, cmd_buffer, (VkImage)target_image, (VkImageView)atlas_view,
		               (VkFormat)target_format, target_width, target_height, tile_columns, tile_rows);
	}
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

static bool
leia_lnx_dp_is_alpha_native(struct xrt_display_processor *xdp)
{
	(void)xdp;
	// The srSDK Vulkan weaver flattens alpha (contract R-W12); transparency is
	// reconstructed by the DP-side compose-under-bg pass, not a native-alpha
	// weave. Matches the Windows/D3D/GL DPs.
	return false;
}

static void
leia_lnx_dp_vk_set_transparent_background(struct xrt_display_processor_vk *xdp, bool enabled, bool client_presents)
{
	struct leia_dp_linux *ldp = (struct leia_dp_linux *)xdp;

	// client_presents: the runtime owns a transparent present and blends the
	// live desktop into the holes itself, so the DP must NOT compose-under.
	// (Not used by the desktop vk_native path today — client_presents=false —
	// but honored for symmetry with the Windows slot.)
	bool want = enabled && !client_presents;
	if (ldp->transparent_enabled == want) {
		return;
	}
	ldp->transparent_enabled = want;
	if (want) {
		// Start desktop capture for the live background (WS1). Display-scoped
		// today: capture the panel monitor via its screen origin. The producer
		// declines cleanly (NULL) if portal/PipeWire is unavailable, and the
		// compose pass falls back to the 2D-under backdrop.
		if (ldp->bg_capture == NULL) {
			struct leiasr_lnx_display_info info;
			int32_t sx = 0, sy = 0;
			if (leiasr_lnx_query_display_info(&info) && info.valid) {
				sx = info.screen_left;
				sy = info.screen_top;
			}
			ldp->bg_capture = leia_bg_capture_linux_create(ldp->vk, sx, sy);
		}
	} else {
		if (ldp->bg_capture != NULL) {
			leia_bg_capture_linux_destroy(ldp->bg_capture);
			ldp->bg_capture = NULL;
		}
		compose_release(ldp);
		ag_release(ldp);
	}
	U_LOG_W("leia_lnx_dp: transparency %s", want ? "= compose-under-bg" : "disabled");
}

// Store the runtime's flattened 2D-under backdrop as the background to compose
// under (#491 part 3). Shared VkDevice, so the view is sampled directly — no
// import. NULL clears (no background → compose no-ops, weaves the raw atlas).
// WS1 (portal/PipeWire desktop capture) will feed the live desktop through this
// same seam.
static void
leia_lnx_dp_set_background_2d(struct xrt_display_processor *xdp,
                             VkImageView background_view,
                             uint32_t width,
                             uint32_t height)
{
	struct leia_dp_linux *ldp = leia_dp_linux(xdp);
	ldp->bg2d_view = background_view;
	ldp->bg2d_w = width;
	ldp->bg2d_h = height;
}

#ifdef XRT_DP_VK_HAS_PRESENT_ORIGIN
// Windowed weaving (runtime#757 / LeiaSR#85): store the app window's panel-relative
// origin; process_atlas forwards it as the weave phase origin. Guarded so the
// plug-in still compiles against a runtime that predates the slot (the vtable
// assignment below is then skipped and the DP weaves display-scoped).
static void
leia_lnx_dp_set_present_origin(struct xrt_display_processor_vk *xdp_vk, int32_t panel_x, int32_t panel_y)
{
	struct leia_dp_linux *ldp = (struct leia_dp_linux *)xdp_vk;
	ldp->present_origin_x = panel_x;
	ldp->present_origin_y = panel_y;
}
#endif

static void
leia_lnx_dp_destroy(struct xrt_display_processor *xdp)
{
	struct leia_dp_linux *ldp = leia_dp_linux(xdp);
	if (ldp->bg_capture != NULL) {
		leia_bg_capture_linux_destroy(ldp->bg_capture); // WS1 capture (runtime#757)
	}
	compose_release(ldp);        // transparency resources (runtime#757)
	ag_release(ldp);             // post-weave alpha-gate (runtime#757)
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
	// Transparency stack (runtime#757): compose-under-bg reconstructs the
	// alpha the weaver flattens (R-W12). set_transparent_background enables it,
	// set_background_2d supplies the background, is_alpha_native reports false.
	ldp->base.base.is_alpha_native = leia_lnx_dp_is_alpha_native;
	ldp->base.base.set_background_2d = leia_lnx_dp_set_background_2d;
	ldp->base.set_transparent_background = leia_lnx_dp_vk_set_transparent_background;
#ifdef XRT_DP_VK_HAS_PRESENT_ORIGIN
	// Windowed weaving (runtime#757 / LeiaSR#85) — the R-W7 window-scoped phase.
	// Only wired when built against a runtime whose xrt_display_processor_vk
	// carries the slot; sizeof(base) (→ struct_size above) grows with it, so the
	// runtime's presence gate matches.
	ldp->base.set_present_origin = leia_lnx_dp_set_present_origin;
#endif
	// TODO(Track B): zone slots (get_local_zone_caps + publish/clear).
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

	// WARN not INFO: one-off lifecycle line (docs/reference/debug-logging.md)
	// — aux INFO is dropped from the hot path, which is exactly how this line
	// went missing in George's on-hardware trace (#81 smoke-test note).
	U_LOG_W("leia_lnx_dp: Linux VK display processor created (backend: %s)",
	        leiasr_lnx_get_render_pass(sr) == VK_NULL_HANDLE ? "stub passthrough" : "weaver");

	*out_xdp = &ldp->base.base;
	return XRT_SUCCESS;
}
