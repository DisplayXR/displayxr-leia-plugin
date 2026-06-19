// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia display processor: wraps SR SDK Vulkan weaver
 *         as an @ref xrt_display_processor.
 *
 * The display processor owns the leiasr handle — it creates it
 * via the factory function and destroys it on cleanup.
 *
 * The SR SDK weaver expects side-by-side (SBS) stereo input. The Leia
 * device defines its 3D mode as tile_columns=2, tile_rows=1, so the
 * compositor always delivers SBS. The compositor crop-blit guarantees
 * the atlas texture dimensions match exactly 2*view_width x view_height.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor.h"
#include "leia_sr.h"

#include "xrt/xrt_display_processor_vk.h" // #573 — the Vulkan transparency-enable variant
#include "xrt/xrt_display_metrics.h"
#include "vk/vk_helpers.h"
#include "vk/vk_cmd_pool.h" // #613 — one-shot cmd buffer for the zone-mask reduction
#include "util/u_logging.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

// SPIR-V shaders (generated at build time by spirv_shaders()). The fullscreen
// triangle vertex shader is shared by the compose-under-bg + alpha-gate passes.
#include "shaders/fullscreen_tri.vert.h"
// #613 / ADR-027 — compute "any non-zero" reduction of the zone wish mask.
// Cross-platform (zones drive the lens on every VK target), so NOT gated behind
// the Windows-only compose/alpha-gate frags below.
#include "shaders/zone_reduce.comp.h"

#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#include "leia_bg_capture_win.h"
#include "shaders/compose_under_bg.frag.h"
#include "shaders/alpha_gate.frag.h"
#endif

// Maximum number of cached strip framebuffers (one per swapchain image view).
// Typical swapchains have 2–3 images; 8 is a safe upper bound.
static constexpr uint32_t kMaxStripFramebuffers = 8;


/*!
 * Implementation struct wrapping leiasr as xrt_display_processor.
 */
struct leia_display_processor
{
	// #573 — the Vulkan variant: the generic base at offset 0 plus the appended
	// set_transparent_background slot (the v4 transparency enable that replaced
	// set_chroma_key). Base vtable members are reached via base.base.
	struct xrt_display_processor_vk base;
	struct leiasr *leiasr; //!< Owned — destroyed in leia_dp_destroy.
	struct vk_bundle *vk;  //!< Cached vk_bundle (not owned).

	VkRenderPass render_pass;   //!< Render pass for framebuffer compatibility.
	uint32_t view_count; //!< Active mode view count (1=2D, 2=stereo).

	//
	// Shared render targets + render passes for the transparency passes
	// (compose-under-bg pre-weave + alpha-gate post-weave). #573 removed the
	// chroma-key fill/strip passes; the `ck_`-named resources below survive
	// because the compose/alpha-gate passes reuse them:
	//   - ck_fill_image / ck_fill_view / ck_fill_fb / ck_fill_rp — the
	//     R8G8B8A8_UNORM intermediate the compose pass renders into and the
	//     weaver then samples (ck_ensure_fill_target).
	//   - ck_strip_image / ck_strip_view + ck_strip_fbs + ck_strip_rp — the
	//     back-buffer copy the alpha-gate samples and the per-target framebuffer
	//     cache it renders back through (ck_ensure_strip_source / ck_get_strip_fb).
	// All lazy-allocated; ck_init_pipeline builds the two render passes once.
	//
	bool ck_inited;                //!< True once the render passes are built.
	VkRenderPass ck_fill_rp;       //!< Render pass writing to ck_fill_image (compose intermediate).
	VkRenderPass ck_strip_rp;      //!< Render pass writing the swapchain image (final layout PRESENT_SRC_KHR; alpha-gate output).
	VkFormat ck_strip_target_format; //!< Swapchain format the strip render pass was built for.

	// Compose intermediate target — RT-bindable and SRV-readable.
	VkImage ck_fill_image;
	VkImageView ck_fill_view;
	VkDeviceMemory ck_fill_mem;
	VkFramebuffer ck_fill_fb;
	uint32_t ck_fill_w, ck_fill_h;

	// Alpha-gate back-buffer copy — sampled by the alpha-gate pass.
	VkImage ck_strip_image;
	VkImageView ck_strip_view;
	VkDeviceMemory ck_strip_mem;
	uint32_t ck_strip_w, ck_strip_h;

	// Per-swapchain-image alpha-gate output cache. The DP vtable doesn't expose a
	// VkImageView for the target, only the target VkImage — so we own a view
	// per image and cache the framebuffer alongside it.
	struct {
		VkImage image;               //!< Cache key: target swapchain VkImage.
		VkImageView view;            //!< Owned by the DP.
		VkFramebuffer fb;            //!< Owned by the DP.
		uint32_t w, h;
	} ck_strip_fbs[kMaxStripFramebuffers];
	uint32_t ck_strip_fbs_count;

	//! #602: last target image-set generation we flushed ck_strip_fbs for.
	//! The compositor bumps its generation on every resize and notifies us via
	//! notify_target_recreated; we drop the (now-dangling) strip cache once per
	//! new generation. Init 0 matches the compositor's freshly-created target.
	uint32_t ck_target_generation;

	//
	// Compose-under-bg transparency support (Windows, when WGC desktop capture
	// is available). Reuses ck_fill_image / ck_fill_view / ck_fill_fb / ck_fill_rp
	// as the intermediate target — same R8G8B8A8_UNORM format. Distinct pipeline
	// (2 SRVs + 24-byte push constants for bg_uv + tile_count) and descriptor set.
	//
	// On non-Windows or WGC-init failure, bg_compose_enabled stays false and the
	// DP stays opaque.
	//
	void *hwnd_opaque;                       //!< HWND from factory (Win-only). void* so the struct stays cross-platform.
	struct leia_bg_capture *bg_capture;      //!< Owned (Win-only). NULL → DP stays opaque.
	bool bg_compose_enabled;

	VkImage bg_image;
	VkImageView bg_view;
	VkDeviceMemory bg_mem;
	uint32_t bg_w, bg_h;

	// #491 part 3 — the runtime's flattened 2D-under backdrop for the next
	// process_atlas (set via set_background_2d). The Leia VK DP shares the
	// compositor's VkDevice (ldp->vk), so this is sampled directly — no import
	// dance like bg_image (which crosses the WGC D3D11 producer device). When
	// set, the compose pass composites `backdrop over captured-desktop` before
	// the atlas-over. VK_NULL_HANDLE ⟹ desktop-only background (today's path).
	VkImageView backdrop_view;
	uint32_t backdrop_w, backdrop_h;

	VkSampler compose_sampler;               //!< Linear/clamp.
	VkDescriptorSetLayout compose_desc_layout;
	VkPipelineLayout compose_pipeline_layout;
	VkDescriptorPool compose_desc_pool;
	VkDescriptorSet compose_set;
	VkPipeline compose_pipeline;
	bool compose_inited;

	// Post-weave alpha-gate (compose-mode replacement for ck_strip).
	// 2-binding descriptor set (back-buffer copy + atlas), 16-byte push
	// constants (tile_count). Uses ck_strip_rp as the output render pass
	// (writes the swap-chain image, finalLayout = PRESENT_SRC_KHR).
	VkDescriptorSetLayout alpha_gate_desc_layout;
	VkPipelineLayout alpha_gate_pipeline_layout;
	VkDescriptorPool alpha_gate_desc_pool;
	VkDescriptorSet alpha_gate_set;
	VkPipeline alpha_gate_pipeline;

	//
	// #613 / ADR-027 — local 2D/3D zones (1×1 collapse, VK port of the D3D11
	// slots 11/12/13 → base slots 18/19/20). The wish mask drives ONLY the SR
	// lens hint (request_display_mode) — never content (ADR-028). State machine
	// mirrors leia_display_processor_d3d11.cpp:300-314.
	//
	uint64_t zone_eval_seq;  //!< Generation last content-evaluated.
	bool zone_eval_valid;    //!< zone_eval_seq / zone_want_3d are valid.
	bool zone_want_3d;       //!< Verdict for zone_eval_seq (any non-zero mask pixel).
	bool zone_hint_3d;       //!< Lens-hint state the zone path last set.
	bool zone_active;        //!< A publish is live (not cleared).

	//! #68 (task #5) — set by set_shared_texture_present (from the compositor's
	//! has_shared_texture). Gates the compose-under-bg canvas bg-UV remap skip.
	bool shared_texture_present;

	//
	// Zone-mask "any non-zero" reduction (compute atomicOr → host-visible word).
	// The base publish slot hands only a VkImageView (no VkImage), so the verdict
	// is read via a compute dispatch rather than the D3D11 CopyResource+Map. All
	// lazy-allocated on the first publish (zone_reduce_init); torn down in destroy.
	//
	bool zone_reduce_inited;
	VkShaderModule zone_reduce_shader;
	VkDescriptorSetLayout zone_reduce_desc_layout;
	VkPipelineLayout zone_reduce_pipeline_layout;
	VkPipeline zone_reduce_pipeline;
	VkDescriptorPool zone_reduce_desc_pool;
	VkDescriptorSet zone_reduce_set;
	VkSampler zone_reduce_sampler;
	VkBuffer zone_reduce_buf;       //!< Host-visible coherent, holds one uint result.
	VkDeviceMemory zone_reduce_mem; //!< Backing memory for zone_reduce_buf.
	void *zone_reduce_ptr;          //!< Persistent map of zone_reduce_mem.
	struct vk_cmd_pool zone_pool;   //!< Own pool for the one-shot reduction submit.
	bool zone_pool_inited;
};

static inline struct leia_display_processor *
leia_display_processor(struct xrt_display_processor *xdp)
{
	return (struct leia_display_processor *)xdp;
}

// #613 — defined alongside the zone vtable (below the factory helpers) but used
// earlier in leia_dp_destroy; forward-declare so the destroy path can free the
// zone-reduction resources.
static void
zone_reduce_fini(struct leia_display_processor *ldp);


/*
 *
 * Transparency render-target helpers (compose-under-bg + alpha-gate support).
 *
 * Lazy-allocated on the first transparent frame. ck_init_pipeline builds the
 * two render passes (fill = R8G8B8A8_UNORM compose intermediate; strip =
 * swapchain-format alpha-gate output → PRESENT_SRC_KHR); the compose pass renders
 * ck_fill_image (which the weaver then samples) and the alpha-gate pass copies the
 * presented swapchain image to ck_strip_image and renders it back through a
 * per-target-view framebuffer. (#573 removed the chroma-key fill/strip passes; the
 * `ck_`-named resources are retained because the compose/alpha-gate passes reuse
 * them.)
 *
 */

static VkResult
ck_create_shader_module(struct vk_bundle *vk,
                        const uint32_t *code,
                        size_t code_size,
                        VkShaderModule *out_module)
{
	VkShaderModuleCreateInfo ci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = code_size,
	    .pCode = code,
	};
	return vk->vkCreateShaderModule(vk->device, &ci, NULL, out_module);
}

// Build the two render passes the transparency passes render into (fill = compose
// intermediate, strip = alpha-gate swapchain output). Idempotent: returns true if
// already inited.
static bool
ck_init_pipeline(struct leia_display_processor *ldp, VkFormat target_format)
{
	if (ldp->ck_inited && ldp->ck_strip_target_format == target_format) {
		return true;
	}
	if (ldp->ck_inited) {
		// Target format changed (rare — swapchain re-created with different
		// format). Tear down strip pipeline only; rebuild below.
		// In the common case the format never changes; the cleanup happens
		// in ck_release_resources at destroy time.
		U_LOG_W("Leia VK DP: ck strip render pass format change %d -> %d not supported (no rebuild)",
		        (int)ldp->ck_strip_target_format, (int)target_format);
		return false;
	}

	struct vk_bundle *vk = ldp->vk;
	VkResult res;

	// Render pass for the fill pass: writes R8G8B8A8_UNORM, no presentation.
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
		res = vk->vkCreateRenderPass(vk->device, &rpi, NULL, &ldp->ck_fill_rp);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: ck_fill_rp create failed: %d", res);
			return false;
		}
	}

	// Render pass for the strip pass: writes the swapchain format and
	// transitions to PRESENT_SRC_KHR so the swapchain image is presentable
	// when we end the render pass.
	{
		VkAttachmentDescription att = {
		    .format = target_format,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		    .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
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
		res = vk->vkCreateRenderPass(vk->device, &rpi, NULL, &ldp->ck_strip_rp);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: ck_strip_rp create failed: %d", res);
			return false;
		}
		ldp->ck_strip_target_format = target_format;
	}

	ldp->ck_inited = true;
	U_LOG_W("Leia VK DP: transparency render passes initialized (target_fmt=%d)", (int)target_format);
	return true;
}

// Create or recreate the fill image+view+framebuffer to match (w, h).
static bool
ck_ensure_fill_target(struct leia_display_processor *ldp, uint32_t w, uint32_t h)
{
	if (ldp->ck_fill_image != VK_NULL_HANDLE && ldp->ck_fill_w == w && ldp->ck_fill_h == h) {
		return true;
	}

	struct vk_bundle *vk = ldp->vk;

	// Tear down existing.
	if (ldp->ck_fill_fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, ldp->ck_fill_fb, NULL);
		ldp->ck_fill_fb = VK_NULL_HANDLE;
	}
	if (ldp->ck_fill_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, ldp->ck_fill_view, NULL);
		ldp->ck_fill_view = VK_NULL_HANDLE;
	}
	if (ldp->ck_fill_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, ldp->ck_fill_image, NULL);
		ldp->ck_fill_image = VK_NULL_HANDLE;
	}
	if (ldp->ck_fill_mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, ldp->ck_fill_mem, NULL);
		ldp->ck_fill_mem = VK_NULL_HANDLE;
	}

	VkExtent2D ext = {w, h};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                          VK_IMAGE_USAGE_SAMPLED_BIT;
	VkResult res = vk_create_image_simple(
	    vk, ext, VK_FORMAT_R8G8B8A8_UNORM, usage,
	    &ldp->ck_fill_mem, &ldp->ck_fill_image);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck_fill_image create failed: %d", res);
		return false;
	}

	VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	res = vk_create_view(vk, ldp->ck_fill_image, VK_IMAGE_VIEW_TYPE_2D,
	                     VK_FORMAT_R8G8B8A8_UNORM, sub, &ldp->ck_fill_view);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck_fill_view create failed: %d", res);
		return false;
	}

	VkFramebufferCreateInfo fbi = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = ldp->ck_fill_rp,
	    .attachmentCount = 1,
	    .pAttachments = &ldp->ck_fill_view,
	    .width = w,
	    .height = h,
	    .layers = 1,
	};
	res = vk->vkCreateFramebuffer(vk->device, &fbi, NULL, &ldp->ck_fill_fb);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck_fill_fb create failed: %d", res);
		return false;
	}

	ldp->ck_fill_w = w;
	ldp->ck_fill_h = h;
	return true;
}

// Ensure the strip sampling image+view can hold at least (w, h). #602: the
// runtime hands a content-fit display zone (P6) a target size that renegotiates
// ~6x/sec; previously this recreated the image+view on every change. Allocate
// at a high-water-mark and never shrink so it stops churning — the alpha-gate
// copies/samples only the top-left (w, h) sub-rect (the shader applies a UV
// scale), so an over-allocated backing image is correct. ck_strip_w/h hold the
// ALLOCATED extent.
static bool
ck_ensure_strip_source(struct leia_display_processor *ldp, uint32_t w, uint32_t h)
{
	if (ldp->ck_strip_image != VK_NULL_HANDLE && ldp->ck_strip_w >= w && ldp->ck_strip_h >= h) {
		return true;
	}

	struct vk_bundle *vk = ldp->vk;

	// Grow to the per-dimension high-water-mark; never shrink.
	uint32_t alloc_w = w > ldp->ck_strip_w ? w : ldp->ck_strip_w;
	uint32_t alloc_h = h > ldp->ck_strip_h ? h : ldp->ck_strip_h;

	if (ldp->ck_strip_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, ldp->ck_strip_view, NULL);
		ldp->ck_strip_view = VK_NULL_HANDLE;
	}
	if (ldp->ck_strip_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, ldp->ck_strip_image, NULL);
		ldp->ck_strip_image = VK_NULL_HANDLE;
	}
	if (ldp->ck_strip_mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, ldp->ck_strip_mem, NULL);
		ldp->ck_strip_mem = VK_NULL_HANDLE;
	}

	VkExtent2D ext = {alloc_w, alloc_h};
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                          VK_IMAGE_USAGE_SAMPLED_BIT;
	// Use the swapchain target format for the sampling image so vkCmdCopyImage
	// is a straight format-matched copy.
	VkResult res = vk_create_image_simple(
	    vk, ext, ldp->ck_strip_target_format, usage,
	    &ldp->ck_strip_mem, &ldp->ck_strip_image);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck_strip_image create failed: %d", res);
		return false;
	}

	VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	res = vk_create_view(vk, ldp->ck_strip_image, VK_IMAGE_VIEW_TYPE_2D,
	                     ldp->ck_strip_target_format, sub, &ldp->ck_strip_view);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck_strip_view create failed: %d", res);
		return false;
	}

	ldp->ck_strip_w = alloc_w;
	ldp->ck_strip_h = alloc_h;
	return true;
}

// Build a fresh VkImageView+VkFramebuffer for (target_image, w, h). Both are
// owned by the cache and torn down in ck_release_resources or when an entry
// is evicted. On failure both fb and view are NULL and false is returned.
static bool
ck_build_strip_entry(struct leia_display_processor *ldp,
                     VkImage target_image,
                     uint32_t w,
                     uint32_t h,
                     VkImageView *out_view,
                     VkFramebuffer *out_fb)
{
	struct vk_bundle *vk = ldp->vk;

	VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageView view = VK_NULL_HANDLE;
	VkResult r = vk_create_view(vk, target_image, VK_IMAGE_VIEW_TYPE_2D,
	                            ldp->ck_strip_target_format, sub, &view);
	if (r != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck strip target view failed: %d", r);
		return false;
	}

	VkFramebufferCreateInfo fbi = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = ldp->ck_strip_rp,
	    .attachmentCount = 1,
	    .pAttachments = &view,
	    .width = w,
	    .height = h,
	    .layers = 1,
	};
	VkFramebuffer fb = VK_NULL_HANDLE;
	r = vk->vkCreateFramebuffer(vk->device, &fbi, NULL, &fb);
	if (r != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: ck strip fb create failed: %d", r);
		vk->vkDestroyImageView(vk->device, view, NULL);
		return false;
	}
	*out_view = view;
	*out_fb = fb;
	return true;
}

// Get (or lazily create) the strip output framebuffer for the given target
// image. Cached; the DP owns both the view and the framebuffer.
static VkFramebuffer
ck_get_strip_fb(struct leia_display_processor *ldp,
                VkImage target_image,
                uint32_t w,
                uint32_t h)
{
	struct vk_bundle *vk = ldp->vk;

	// #602: the cached entry's w/h are the ALLOCATED framebuffer extent (a
	// high-water-mark). The alpha-gate renders only a (w, h) top-left sub-rect
	// (renderArea/viewport/scissor), so a larger framebuffer over the same
	// target image is reused as long as it covers (w, h) — only a genuine grow
	// rebuilds. This stops the per-frame rebuild as a content-fit zone resizes.
	for (uint32_t i = 0; i < ldp->ck_strip_fbs_count; i++) {
		if (ldp->ck_strip_fbs[i].image == target_image) {
			if (ldp->ck_strip_fbs[i].w >= w && ldp->ck_strip_fbs[i].h >= h) {
				return ldp->ck_strip_fbs[i].fb;
			}
			// Needs a bigger framebuffer — grow to the high-water-mark.
			uint32_t alloc_w = w > ldp->ck_strip_fbs[i].w ? w : ldp->ck_strip_fbs[i].w;
			uint32_t alloc_h = h > ldp->ck_strip_fbs[i].h ? h : ldp->ck_strip_fbs[i].h;
			vk->vkDestroyFramebuffer(vk->device, ldp->ck_strip_fbs[i].fb, NULL);
			vk->vkDestroyImageView(vk->device, ldp->ck_strip_fbs[i].view, NULL);
			VkImageView v = VK_NULL_HANDLE;
			VkFramebuffer f = VK_NULL_HANDLE;
			if (!ck_build_strip_entry(ldp, target_image, alloc_w, alloc_h, &v, &f)) {
				ldp->ck_strip_fbs[i].view = VK_NULL_HANDLE;
				ldp->ck_strip_fbs[i].fb = VK_NULL_HANDLE;
				return VK_NULL_HANDLE;
			}
			ldp->ck_strip_fbs[i].view = v;
			ldp->ck_strip_fbs[i].fb = f;
			ldp->ck_strip_fbs[i].w = alloc_w;
			ldp->ck_strip_fbs[i].h = alloc_h;
			return f;
		}
	}

	if (ldp->ck_strip_fbs_count >= kMaxStripFramebuffers) {
		// LRU eviction. This recurs when the present target is recreated as a
		// content-fit zone (P6 XR_EXT_display_zones, e.g. the avatar tracking
		// its silhouette) renegotiates size every few frames: each fresh present
		// image adds an entry keyed by image, so the bounded cache cycles. The
		// eviction is correct and harmless — only the per-occurrence WARN is
		// noise (runtime convention: never spam WARN). Log it once for
		// visibility, then stay silent.
		static bool warned_cache_full = false;
		if (!warned_cache_full) {
			warned_cache_full = true;
			U_LOG_W("Leia VK DP: ck strip fb cache full (%u entries) — evicting oldest (LRU). "
			        "Recurs as a content-fit zone resizes; subsequent evictions are not logged.",
			        ldp->ck_strip_fbs_count);
		}
		vk->vkDestroyFramebuffer(vk->device, ldp->ck_strip_fbs[0].fb, NULL);
		vk->vkDestroyImageView(vk->device, ldp->ck_strip_fbs[0].view, NULL);
		for (uint32_t i = 1; i < ldp->ck_strip_fbs_count; i++) {
			ldp->ck_strip_fbs[i - 1] = ldp->ck_strip_fbs[i];
		}
		ldp->ck_strip_fbs_count--;
	}

	VkImageView v = VK_NULL_HANDLE;
	VkFramebuffer f = VK_NULL_HANDLE;
	if (!ck_build_strip_entry(ldp, target_image, w, h, &v, &f)) {
		return VK_NULL_HANDLE;
	}
	uint32_t idx = ldp->ck_strip_fbs_count++;
	ldp->ck_strip_fbs[idx].image = target_image;
	ldp->ck_strip_fbs[idx].view = v;
	ldp->ck_strip_fbs[idx].fb = f;
	ldp->ck_strip_fbs[idx].w = w;
	ldp->ck_strip_fbs[idx].h = h;
	return f;
}

static void
ck_release_resources(struct leia_display_processor *ldp)
{
	if (ldp == NULL || ldp->vk == NULL) return;
	struct vk_bundle *vk = ldp->vk;

	for (uint32_t i = 0; i < ldp->ck_strip_fbs_count; i++) {
		if (ldp->ck_strip_fbs[i].fb != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, ldp->ck_strip_fbs[i].fb, NULL);
		}
		if (ldp->ck_strip_fbs[i].view != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, ldp->ck_strip_fbs[i].view, NULL);
		}
	}
	ldp->ck_strip_fbs_count = 0;

	if (ldp->ck_fill_fb != VK_NULL_HANDLE)   vk->vkDestroyFramebuffer(vk->device, ldp->ck_fill_fb, NULL);
	if (ldp->ck_fill_view != VK_NULL_HANDLE) vk->vkDestroyImageView(vk->device, ldp->ck_fill_view, NULL);
	if (ldp->ck_fill_image != VK_NULL_HANDLE) vk->vkDestroyImage(vk->device, ldp->ck_fill_image, NULL);
	if (ldp->ck_fill_mem != VK_NULL_HANDLE)  vk->vkFreeMemory(vk->device, ldp->ck_fill_mem, NULL);

	if (ldp->ck_strip_view != VK_NULL_HANDLE) vk->vkDestroyImageView(vk->device, ldp->ck_strip_view, NULL);
	if (ldp->ck_strip_image != VK_NULL_HANDLE) vk->vkDestroyImage(vk->device, ldp->ck_strip_image, NULL);
	if (ldp->ck_strip_mem != VK_NULL_HANDLE)  vk->vkFreeMemory(vk->device, ldp->ck_strip_mem, NULL);

	if (ldp->ck_fill_rp != VK_NULL_HANDLE) vk->vkDestroyRenderPass(vk->device, ldp->ck_fill_rp, NULL);
	if (ldp->ck_strip_rp != VK_NULL_HANDLE) vk->vkDestroyRenderPass(vk->device, ldp->ck_strip_rp, NULL);

	std::memset(&ldp->ck_strip_fbs, 0, sizeof(ldp->ck_strip_fbs));
	ldp->ck_fill_fb = VK_NULL_HANDLE;
	ldp->ck_fill_view = VK_NULL_HANDLE;
	ldp->ck_fill_image = VK_NULL_HANDLE;
	ldp->ck_fill_mem = VK_NULL_HANDLE;
	ldp->ck_strip_view = VK_NULL_HANDLE;
	ldp->ck_strip_image = VK_NULL_HANDLE;
	ldp->ck_strip_mem = VK_NULL_HANDLE;
	ldp->ck_fill_rp = VK_NULL_HANDLE;
	ldp->ck_strip_rp = VK_NULL_HANDLE;
	ldp->ck_inited = false;
}


/*
 *
 * Compose-under-bg pipeline (preferred over chroma-key on Windows).
 *
 * Reuses ck_fill_image / ck_fill_view / ck_fill_fb / ck_fill_rp as the
 * intermediate target — ck_ensure_fill_target + ck_init_pipeline still
 * create those. Distinct pipeline + descriptor set with 2 SRVs.
 *
 * Cross-API sync note: WGC capture (~60 Hz, sub-ms copy) is much slower
 * than the consumer's render rate (~120 Hz). The producer's D3D11 context
 * is Flushed after every Copy. Without a GPU-level semaphore wait, the
 * consumer may very occasionally see a torn frame mid-copy; in practice
 * the temporal gap dwarfs the copy duration. Proper VK_KHR_external_
 * semaphore_win32 import via D3D12_FENCE handle type is a follow-up.
 *
 */

#ifdef _WIN32

static bool
compose_should_run(struct leia_display_processor *ldp)
{
	return ldp->bg_compose_enabled && ldp->bg_capture != nullptr && ldp->bg_view != VK_NULL_HANDLE;
}

// Import the bg_capture's shared NT-handle texture as VkImage + VkImageView.
// One-shot on session start; the imported memory's lifetime is tied to the DP.
static bool
compose_import_bg_image(struct leia_display_processor *ldp)
{
	struct vk_bundle *vk = ldp->vk;
	HANDLE handle = leia_bg_capture_get_shared_handle(ldp->bg_capture);
	if (handle == nullptr) {
		U_LOG_W("Leia VK DP: bg_capture has no shared handle");
		return false;
	}
	leia_bg_capture_get_size(ldp->bg_capture, &ldp->bg_w, &ldp->bg_h);
	if (ldp->bg_w == 0 || ldp->bg_h == 0) {
		U_LOG_W("Leia VK DP: bg_capture has zero size");
		return false;
	}

	// NT-handle import path: VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT.
	// Format must match the D3D11 staging tex (BGRA8).
	VkExternalMemoryImageCreateInfo ext_ci = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
	};
	VkImageCreateInfo image_ci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &ext_ci,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .extent = {ldp->bg_w, ldp->bg_h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkResult res = vk->vkCreateImage(vk->device, &image_ci, NULL, &ldp->bg_image);
	if (res != VK_SUCCESS) {
		U_LOG_W("Leia VK DP: bg vkCreateImage failed: %d", res);
		return false;
	}

	VkMemoryRequirements reqs = {};
	vk->vkGetImageMemoryRequirements(vk->device, ldp->bg_image, &reqs);

	VkImportMemoryWin32HandleInfoKHR import = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
	    .handle = handle,
	};
	VkMemoryDedicatedAllocateInfoKHR dedicated = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
	    .pNext = &import,
	    .image = ldp->bg_image,
	};
	VkPhysicalDeviceMemoryProperties mp = {};
	vk->vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mp);
	uint32_t mti = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((reqs.memoryTypeBits & (1u << i)) != 0) {
			mti = i;
			break;
		}
	}
	if (mti == UINT32_MAX) {
		U_LOG_W("Leia VK DP: no memory type for bg import");
		vk->vkDestroyImage(vk->device, ldp->bg_image, NULL);
		ldp->bg_image = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryAllocateInfo alloc = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &dedicated,
	    .allocationSize = reqs.size,
	    .memoryTypeIndex = mti,
	};
	res = vk->vkAllocateMemory(vk->device, &alloc, NULL, &ldp->bg_mem);
	if (res != VK_SUCCESS) {
		U_LOG_W("Leia VK DP: bg vkAllocateMemory failed: %d", res);
		vk->vkDestroyImage(vk->device, ldp->bg_image, NULL);
		ldp->bg_image = VK_NULL_HANDLE;
		return false;
	}
	res = vk->vkBindImageMemory(vk->device, ldp->bg_image, ldp->bg_mem, 0);
	if (res != VK_SUCCESS) {
		U_LOG_W("Leia VK DP: bg vkBindImageMemory failed: %d", res);
		vk->vkFreeMemory(vk->device, ldp->bg_mem, NULL);
		vk->vkDestroyImage(vk->device, ldp->bg_image, NULL);
		ldp->bg_mem = VK_NULL_HANDLE;
		ldp->bg_image = VK_NULL_HANDLE;
		return false;
	}

	VkImageViewCreateInfo vi = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = ldp->bg_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	res = vk->vkCreateImageView(vk->device, &vi, NULL, &ldp->bg_view);
	if (res != VK_SUCCESS) {
		U_LOG_W("Leia VK DP: bg vkCreateImageView failed: %d", res);
		vk->vkFreeMemory(vk->device, ldp->bg_mem, NULL);
		vk->vkDestroyImage(vk->device, ldp->bg_image, NULL);
		ldp->bg_mem = VK_NULL_HANDLE;
		ldp->bg_image = VK_NULL_HANDLE;
		return false;
	}

	U_LOG_W("Leia VK DP: bg D3D11 texture imported (%ux%u)", ldp->bg_w, ldp->bg_h);
	return true;
}

// Build compose-specific descriptor layout / pipeline layout / sampler /
// descriptor pool / pipeline. Reuses ck_fill_rp as the render pass (same
// R8G8B8A8_UNORM attachment). Requires ck_init_pipeline to have run first
// so ck_fill_rp exists. Idempotent.
static bool
compose_init_pipeline(struct leia_display_processor *ldp)
{
	if (ldp->compose_inited) return true;
	if (ldp->ck_fill_rp == VK_NULL_HANDLE) {
		U_LOG_W("Leia VK DP: compose_init_pipeline before ck_fill_rp exists");
		return false;
	}

	struct vk_bundle *vk = ldp->vk;
	VkResult res;

	// 3-binding descriptor set: atlas (0) + bg/desktop (1) + 2D-under backdrop
	// (2, #491 part 3).
	{
		VkDescriptorSetLayoutBinding bs[3] = {
		    {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		    {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		    {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		};
		VkDescriptorSetLayoutCreateInfo ci = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .bindingCount = 3, .pBindings = bs,
		};
		res = vk->vkCreateDescriptorSetLayout(vk->device, &ci, NULL, &ldp->compose_desc_layout);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: compose desc layout failed: %d", res);
			return false;
		}
	}

	// Push constants: 2*vec2 + uvec2 + uvec2 pad = 32 bytes (no more chroma_rgb
	// — alpha-gate handles transparency holes, not the compose shader).
	{
		VkPushConstantRange pc = {
		    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		    .offset = 0,
		    .size = 32,
		};
		VkPipelineLayoutCreateInfo pli = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		    .setLayoutCount = 1, .pSetLayouts = &ldp->compose_desc_layout,
		    .pushConstantRangeCount = 1, .pPushConstantRanges = &pc,
		};
		res = vk->vkCreatePipelineLayout(vk->device, &pli, NULL, &ldp->compose_pipeline_layout);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: compose pipeline layout failed: %d", res);
			return false;
		}
	}

	// Linear sampler.
	{
		VkSamplerCreateInfo si = {
		    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		    .magFilter = VK_FILTER_LINEAR,
		    .minFilter = VK_FILTER_LINEAR,
		    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		    .maxLod = 1.0f,
		};
		res = vk->vkCreateSampler(vk->device, &si, NULL, &ldp->compose_sampler);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: compose sampler failed: %d", res);
			return false;
		}
	}

	// Descriptor pool + 1 set.
	{
		VkDescriptorPoolSize size = {
		    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .descriptorCount = 3, // atlas + bg + backdrop (#491 part 3)
		};
		VkDescriptorPoolCreateInfo dpi = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		    .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size,
		};
		res = vk->vkCreateDescriptorPool(vk->device, &dpi, NULL, &ldp->compose_desc_pool);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: compose desc pool failed: %d", res);
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
			U_LOG_E("Leia VK DP: compose desc set alloc failed: %d", res);
			return false;
		}
	}

	// Bind the bg view to slot 1 once — it doesn't change across frames. Also
	// seed slot 2 (#491 part 3 backdrop) with the bg view as a valid dummy so
	// the set is fully written before first use; compose_run_pre_weave
	// overwrites slot 2 per-frame with the real backdrop when one is present
	// (and gates sampling via the has_backdrop push constant otherwise).
	{
		VkDescriptorImageInfo info = {
		    .sampler = ldp->compose_sampler,
		    .imageView = ldp->bg_view,
		    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkWriteDescriptorSet w[2] = {
		    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		     .dstSet = ldp->compose_set,
		     .dstBinding = 1,
		     .descriptorCount = 1,
		     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     .pImageInfo = &info},
		    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		     .dstSet = ldp->compose_set,
		     .dstBinding = 2,
		     .descriptorCount = 1,
		     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     .pImageInfo = &info},
		};
		vk->vkUpdateDescriptorSets(vk->device, 2, w, 0, NULL);
	}

	// Build pipeline.
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	res = ck_create_shader_module(vk, shaders_fullscreen_tri_vert,
	                              sizeof(shaders_fullscreen_tri_vert), &vs);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: compose vs create failed: %d", res);
		return false;
	}
	res = ck_create_shader_module(vk, shaders_compose_under_bg_frag,
	                              sizeof(shaders_compose_under_bg_frag), &fs);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: compose fs create failed: %d", res);
		vk->vkDestroyShaderModule(vk->device, vs, NULL);
		return false;
	}

	VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo vps = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1, .scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rs = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_NONE,
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo ms = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState ba = {
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo cb = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1, .pAttachments = &ba,
	};
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynstate = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2, .pDynamicStates = dyn,
	};
	VkPipelineShaderStageCreateInfo stages[2] = {
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	     .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	     .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
	};
	VkGraphicsPipelineCreateInfo pi = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2, .pStages = stages,
	    .pVertexInputState = &vi,
	    .pInputAssemblyState = &ia,
	    .pViewportState = &vps,
	    .pRasterizationState = &rs,
	    .pMultisampleState = &ms,
	    .pColorBlendState = &cb,
	    .pDynamicState = &dynstate,
	    .layout = ldp->compose_pipeline_layout,
	    .renderPass = ldp->ck_fill_rp,
	    .subpass = 0,
	};
	res = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pi, NULL, &ldp->compose_pipeline);
	vk->vkDestroyShaderModule(vk->device, fs, NULL);
	vk->vkDestroyShaderModule(vk->device, vs, NULL);
	if (res != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: compose pipeline create failed: %d", res);
		return false;
	}

	// Alpha-gate pipeline — pairs with compose pass. Reuses ck_strip_rp as
	// the output render pass (writes swap-chain image, finalLayout =
	// PRESENT_SRC_KHR), and the ck_strip_image / ck_strip_view backbuffer
	// copy plumbing. Distinct descriptor set (2 image samplers — backbuffer
	// copy + atlas) and push constants (tile_count).
	if (ldp->alpha_gate_pipeline == VK_NULL_HANDLE) {
		// 3-binding descriptor set: backbuffer (0) + atlas (1) + 2D-under
		// backdrop (2, #491 part 3).
		{
			VkDescriptorSetLayoutBinding bs[3] = {
			    {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
			    {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
			    {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			     .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
			};
			VkDescriptorSetLayoutCreateInfo ci = {
			    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			    .bindingCount = 3, .pBindings = bs,
			};
			res = vk->vkCreateDescriptorSetLayout(vk->device, &ci, NULL, &ldp->alpha_gate_desc_layout);
			if (res != VK_SUCCESS) {
				U_LOG_E("Leia VK DP: alpha-gate desc layout failed: %d", res);
				return false;
			}
		}
		// Push constants: uvec2 tile_count + uint has_backdrop + uint pad +
		// vec2 strip_uv_scale (#602) = 24 bytes.
		{
			VkPushConstantRange pc = {
			    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			    .offset = 0,
			    .size = 24,
			};
			VkPipelineLayoutCreateInfo pli = {
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			    .setLayoutCount = 1, .pSetLayouts = &ldp->alpha_gate_desc_layout,
			    .pushConstantRangeCount = 1, .pPushConstantRanges = &pc,
			};
			res = vk->vkCreatePipelineLayout(vk->device, &pli, NULL, &ldp->alpha_gate_pipeline_layout);
			if (res != VK_SUCCESS) {
				U_LOG_E("Leia VK DP: alpha-gate pipeline layout failed: %d", res);
				return false;
			}
		}
		// Descriptor pool + 1 set.
		{
			VkDescriptorPoolSize size = {
			    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			    .descriptorCount = 3, // backbuffer + atlas + backdrop (#491 part 3)
			};
			VkDescriptorPoolCreateInfo dpi = {
			    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			    .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size,
			};
			res = vk->vkCreateDescriptorPool(vk->device, &dpi, NULL, &ldp->alpha_gate_desc_pool);
			if (res != VK_SUCCESS) {
				U_LOG_E("Leia VK DP: alpha-gate desc pool failed: %d", res);
				return false;
			}
			VkDescriptorSetAllocateInfo ai = {
			    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			    .descriptorPool = ldp->alpha_gate_desc_pool,
			    .descriptorSetCount = 1,
			    .pSetLayouts = &ldp->alpha_gate_desc_layout,
			};
			res = vk->vkAllocateDescriptorSets(vk->device, &ai, &ldp->alpha_gate_set);
			if (res != VK_SUCCESS) {
				U_LOG_E("Leia VK DP: alpha-gate desc set alloc failed: %d", res);
				return false;
			}
		}
		// Build pipeline against ck_strip_rp (writes swap-chain image).
		VkShaderModule ag_vs = VK_NULL_HANDLE, ag_fs = VK_NULL_HANDLE;
		res = ck_create_shader_module(vk, shaders_fullscreen_tri_vert,
		                              sizeof(shaders_fullscreen_tri_vert), &ag_vs);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: alpha-gate vs create failed: %d", res);
			return false;
		}
		res = ck_create_shader_module(vk, shaders_alpha_gate_frag,
		                              sizeof(shaders_alpha_gate_frag), &ag_fs);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: alpha-gate fs create failed: %d", res);
			vk->vkDestroyShaderModule(vk->device, ag_vs, NULL);
			return false;
		}

		VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};
		VkPipelineViewportStateCreateInfo vps = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		    .viewportCount = 1, .scissorCount = 1,
		};
		VkPipelineRasterizationStateCreateInfo rs = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		    .polygonMode = VK_POLYGON_MODE_FILL,
		    .cullMode = VK_CULL_MODE_NONE,
		    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		    .lineWidth = 1.0f,
		};
		VkPipelineMultisampleStateCreateInfo ms = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};
		VkPipelineColorBlendAttachmentState ba = {
		    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		};
		VkPipelineColorBlendStateCreateInfo cb = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		    .attachmentCount = 1, .pAttachments = &ba,
		};
		VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynstate = {
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		    .dynamicStateCount = 2, .pDynamicStates = dyn,
		};
		VkPipelineShaderStageCreateInfo stages[2] = {
		    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		     .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = ag_vs, .pName = "main"},
		    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		     .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = ag_fs, .pName = "main"},
		};
		VkGraphicsPipelineCreateInfo pi = {
		    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		    .stageCount = 2, .pStages = stages,
		    .pVertexInputState = &vi,
		    .pInputAssemblyState = &ia,
		    .pViewportState = &vps,
		    .pRasterizationState = &rs,
		    .pMultisampleState = &ms,
		    .pColorBlendState = &cb,
		    .pDynamicState = &dynstate,
		    .layout = ldp->alpha_gate_pipeline_layout,
		    .renderPass = ldp->ck_strip_rp,
		    .subpass = 0,
		};
		res = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pi, NULL, &ldp->alpha_gate_pipeline);
		vk->vkDestroyShaderModule(vk->device, ag_fs, NULL);
		vk->vkDestroyShaderModule(vk->device, ag_vs, NULL);
		if (res != VK_SUCCESS) {
			U_LOG_E("Leia VK DP: alpha-gate pipeline create failed: %d", res);
			return false;
		}
	}

	ldp->compose_inited = true;
	U_LOG_W("Leia VK DP: compose-under-bg + alpha-gate pipelines ready");
	return true;
}

// Pre-weave compose. Polls WGC, transitions bg → SHADER_READ once, renders the
// compose pass into ck_fill_image. Returns ck_fill_view for the weaver.
static VkImageView
compose_run_pre_weave(struct leia_display_processor *ldp,
                      VkCommandBuffer cmd,
                      VkImageView atlas_view,
                      uint32_t atlas_w,
                      uint32_t atlas_h,
                      uint32_t tile_columns,
                      uint32_t tile_rows,
                      int32_t canvas_offset_x,
                      int32_t canvas_offset_y,
                      uint32_t canvas_width,
                      uint32_t canvas_height,
                      uint32_t target_width,
                      uint32_t target_height)
{
	struct vk_bundle *vk = ldp->vk;
	if (!ck_ensure_fill_target(ldp, atlas_w, atlas_h)) return VK_NULL_HANDLE;
	if (!compose_init_pipeline(ldp)) return VK_NULL_HANDLE;

	float bg_origin[2] = {0.0f, 0.0f};
	float bg_extent[2] = {0.0f, 0.0f};
	uint64_t unused = 0;
	bool have_bg = leia_bg_capture_poll(ldp->bg_capture, bg_origin, bg_extent, &unused);
	if (!have_bg) {
		return VK_NULL_HANDLE;
	}

	// #68 / #613 — canvas bg-UV remap. The atlas is full-size but the weaver
	// downscales it into the canvas sub-rect viewport, so the composited desktop
	// must be pre-remapped to the sub-rect's fraction of the window to land 1:1
	// behind the sub-rect after the weave downscale. SKIP for a shared-texture
	// zones frame: there the app's window shows ONLY the canvas (the canvas fills
	// the window), so leia_bg_capture_poll already returns the window = canvas
	// footprint and the extra canvas/target factor would over-shrink the desktop
	// (~target/window× magnified). Mirrors the D3D11 reference
	// (leia_display_processor_d3d11.cpp ~911-937). A full-window canvas makes the
	// factor the identity, so handle apps are unaffected.
	bool skip_bg_remap = ldp->shared_texture_present && ldp->zone_active;
	if (!skip_bg_remap && canvas_width > 0 && canvas_height > 0 && target_width > 0 && target_height > 0) {
		float fx = (float)canvas_offset_x / (float)target_width;
		float fy = (float)canvas_offset_y / (float)target_height;
		float fw = (float)canvas_width / (float)target_width;
		float fh = (float)canvas_height / (float)target_height;
		bg_origin[0] += fx * bg_extent[0];
		bg_origin[1] += fy * bg_extent[1];
		bg_extent[0] *= fw;
		bg_extent[1] *= fh;
	}

	// First-use barrier on the imported bg image: UNDEFINED → SHADER_READ_ONLY.
	// Subsequent frames keep it in SHADER_READ_ONLY (the D3D11 producer writes
	// through the shared memory without going through VK layouts).
	static thread_local bool bg_transitioned = false;
	if (!bg_transitioned) {
		VkImageMemoryBarrier b = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .image = ldp->bg_image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd,
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		    0, 0, NULL, 0, NULL, 1, &b);
		bg_transitioned = true;
	}

	// Refresh atlas SRV in compose set (slot 0). bg SRV (slot 1) is stable.
	VkDescriptorImageInfo atlas_info = {
	    .sampler = ldp->compose_sampler,
	    .imageView = atlas_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet w = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = ldp->compose_set,
	    .dstBinding = 0,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &atlas_info,
	};
	vk->vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);

	// #491 part 3 — refresh slot 2 with the runtime's flattened 2D-under
	// backdrop when present (same VkDevice → sampled directly). Gated by
	// has_backdrop below; when absent the slot keeps its dummy and is ignored.
	const bool have_backdrop = (ldp->backdrop_view != VK_NULL_HANDLE);
	if (have_backdrop) {
		VkDescriptorImageInfo bd_info = {
		    .sampler = ldp->compose_sampler,
		    .imageView = ldp->backdrop_view,
		    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkWriteDescriptorSet bw = {
		    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		    .dstSet = ldp->compose_set,
		    .dstBinding = 2,
		    .descriptorCount = 1,
		    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .pImageInfo = &bd_info,
		};
		vk->vkUpdateDescriptorSets(vk->device, 1, &bw, 0, NULL);

		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("Leia VK DP #491 part3: compositing 2D-under backdrop %ux%u over desktop",
			        ldp->backdrop_w, ldp->backdrop_h);
		}
	}

	// ck_fill_image UNDEFINED → COLOR_ATTACHMENT.
	VkImageMemoryBarrier pre = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .image = ldp->ck_fill_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    0, 0, NULL, 0, NULL, 1, &pre);

	VkRenderPassBeginInfo rpbi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = ldp->ck_fill_rp,
	    .framebuffer = ldp->ck_fill_fb,
	    .renderArea = {{0, 0}, {atlas_w, atlas_h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ldp->compose_pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                             ldp->compose_pipeline_layout, 0, 1, &ldp->compose_set, 0, NULL);

	struct {
		float bg_uv_origin[2];
		float bg_uv_extent[2];
		uint32_t tile_count[2];
		uint32_t has_backdrop; // #491 part 3 — 1 ⟹ composite backdrop over desktop
		uint32_t pad;
	} push = {};
	push.bg_uv_origin[0] = bg_origin[0]; push.bg_uv_origin[1] = bg_origin[1];
	push.bg_uv_extent[0] = bg_extent[0]; push.bg_uv_extent[1] = bg_extent[1];
	push.tile_count[0] = tile_columns;   push.tile_count[1] = tile_rows;
	push.has_backdrop = have_backdrop ? 1u : 0u;
	vk->vkCmdPushConstants(cmd, ldp->compose_pipeline_layout,
	                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

	VkViewport vp = {0.0f, 0.0f, (float)atlas_w, (float)atlas_h, 0.0f, 1.0f};
	VkRect2D sc = {{0, 0}, {atlas_w, atlas_h}};
	vk->vkCmdSetViewport(cmd, 0, 1, &vp);
	vk->vkCmdSetScissor(cmd, 0, 1, &sc);

	vk->vkCmdDraw(cmd, 3, 1, 0, 0);
	vk->vkCmdEndRenderPass(cmd);

	// ck_fill_image COLOR_ATTACHMENT → SHADER_READ for weaver.
	VkImageMemoryBarrier post = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .image = ldp->ck_fill_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    0, 0, NULL, 0, NULL, 1, &post);

	return ldp->ck_fill_view;
}

/*
 * Post-weave alpha-gate. Copies the presented back-buffer to ck_strip_image then
 * binds 2 SRVs (strip_view = back-buffer copy, atlas_view) and the alpha-gate
 * pipeline. Output is premultiplied RGBA — pixels matching the "all views α==0"
 * mask get (0,0,0,0); others get woven RGB at α=1.
 *
 * On entry: target_image is in PRESENT_SRC_KHR. On exit: same.
 */
static void
alpha_gate_run_post_weave(struct leia_display_processor *ldp,
                          VkCommandBuffer cmd,
                          VkImage target_image,
                          VkImageView atlas_view,
                          uint32_t w, uint32_t h,
                          uint32_t tile_columns, uint32_t tile_rows)
{
	struct vk_bundle *vk = ldp->vk;

	if (!ck_ensure_strip_source(ldp, w, h)) return;
	VkFramebuffer strip_fb = ck_get_strip_fb(ldp, target_image, w, h);
	if (strip_fb == VK_NULL_HANDLE) return;

	// Update alpha-gate descriptor set: slot 0 = strip_view (backbuffer copy),
	// slot 1 = atlas_view, slot 2 = 2D-under backdrop (#491 part 3; dummy when
	// absent — gated by has_backdrop below).
	const bool have_backdrop = (ldp->backdrop_view != VK_NULL_HANDLE);
	VkDescriptorImageInfo infos[3] = {
	    {.sampler = ldp->compose_sampler, .imageView = ldp->ck_strip_view,
	     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	    {.sampler = ldp->compose_sampler, .imageView = atlas_view,
	     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	    {.sampler = ldp->compose_sampler,
	     .imageView = have_backdrop ? ldp->backdrop_view : ldp->ck_strip_view,
	     .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	};
	VkWriteDescriptorSet writes[3] = {
	    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	     .dstSet = ldp->alpha_gate_set, .dstBinding = 0, .descriptorCount = 1,
	     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	     .pImageInfo = &infos[0]},
	    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	     .dstSet = ldp->alpha_gate_set, .dstBinding = 1, .descriptorCount = 1,
	     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	     .pImageInfo = &infos[1]},
	    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	     .dstSet = ldp->alpha_gate_set, .dstBinding = 2, .descriptorCount = 1,
	     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	     .pImageInfo = &infos[2]},
	};
	vk->vkUpdateDescriptorSets(vk->device, 3, writes, 0, NULL);

	// target PRESENT_SRC_KHR → TRANSFER_SRC; strip_image UNDEFINED/SR → TRANSFER_DST.
	VkImageMemoryBarrier pre[2] = {
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	     .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	     .image = target_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	     .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	     .image = ldp->ck_strip_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, NULL, 0, NULL, 2, pre);

	VkImageCopy region = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .extent = {w, h, 1},
	};
	vk->vkCmdCopyImage(cmd,
	    target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    ldp->ck_strip_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    1, &region);

	// target TRANSFER_SRC → COLOR_ATTACHMENT (will be written by the gate
	// draw via ck_strip_rp); strip_image TRANSFER_DST → SHADER_READ.
	VkImageMemoryBarrier mid[2] = {
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	     .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	     .image = target_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	    {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	     .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	     .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	     .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	     .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	     .image = ldp->ck_strip_image,
	     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
	};
	vk->vkCmdPipelineBarrier(cmd,
	    VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	    0, 0, NULL, 0, NULL, 2, mid);

	VkRenderPassBeginInfo rpbi = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = ldp->ck_strip_rp,
	    .framebuffer = strip_fb,
	    .renderArea = {{0, 0}, {w, h}},
	};
	vk->vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ldp->alpha_gate_pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                             ldp->alpha_gate_pipeline_layout, 0, 1, &ldp->alpha_gate_set, 0, NULL);

	struct {
		uint32_t tile_count[2];
		uint32_t has_backdrop;
		uint32_t pad;
		float strip_uv_scale[2]; // #602
	} push = {};
	push.tile_count[0] = tile_columns;
	push.tile_count[1] = tile_rows;
	push.has_backdrop = have_backdrop ? 1u : 0u; // #491 part 3
	// #602 — ck_strip_image is allocated at a high-water-mark; only its
	// top-left (w, h) holds this frame's back-buffer copy. Scale screen UV into
	// that sub-rect so the gate samples the valid region (1,1 when exact).
	push.strip_uv_scale[0] = ldp->ck_strip_w > 0 ? (float)w / (float)ldp->ck_strip_w : 1.0f;
	push.strip_uv_scale[1] = ldp->ck_strip_h > 0 ? (float)h / (float)ldp->ck_strip_h : 1.0f;
	vk->vkCmdPushConstants(cmd, ldp->alpha_gate_pipeline_layout,
	                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

	VkViewport vp = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
	VkRect2D sc = {{0, 0}, {w, h}};
	vk->vkCmdSetViewport(cmd, 0, 1, &vp);
	vk->vkCmdSetScissor(cmd, 0, 1, &sc);

	vk->vkCmdDraw(cmd, 3, 1, 0, 0);
	vk->vkCmdEndRenderPass(cmd);
}

static void
compose_release_resources(struct leia_display_processor *ldp)
{
	if (ldp == NULL || ldp->vk == NULL) return;
	struct vk_bundle *vk = ldp->vk;

	if (ldp->alpha_gate_pipeline != VK_NULL_HANDLE) {
		vk->vkDestroyPipeline(vk->device, ldp->alpha_gate_pipeline, NULL);
		ldp->alpha_gate_pipeline = VK_NULL_HANDLE;
	}
	if (ldp->alpha_gate_desc_pool != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorPool(vk->device, ldp->alpha_gate_desc_pool, NULL);
		ldp->alpha_gate_desc_pool = VK_NULL_HANDLE;
	}
	if (ldp->alpha_gate_pipeline_layout != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineLayout(vk->device, ldp->alpha_gate_pipeline_layout, NULL);
		ldp->alpha_gate_pipeline_layout = VK_NULL_HANDLE;
	}
	if (ldp->alpha_gate_desc_layout != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorSetLayout(vk->device, ldp->alpha_gate_desc_layout, NULL);
		ldp->alpha_gate_desc_layout = VK_NULL_HANDLE;
	}

	if (ldp->compose_pipeline != VK_NULL_HANDLE) {
		vk->vkDestroyPipeline(vk->device, ldp->compose_pipeline, NULL);
		ldp->compose_pipeline = VK_NULL_HANDLE;
	}
	if (ldp->compose_desc_pool != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorPool(vk->device, ldp->compose_desc_pool, NULL);
		ldp->compose_desc_pool = VK_NULL_HANDLE;
	}
	if (ldp->compose_pipeline_layout != VK_NULL_HANDLE) {
		vk->vkDestroyPipelineLayout(vk->device, ldp->compose_pipeline_layout, NULL);
		ldp->compose_pipeline_layout = VK_NULL_HANDLE;
	}
	if (ldp->compose_desc_layout != VK_NULL_HANDLE) {
		vk->vkDestroyDescriptorSetLayout(vk->device, ldp->compose_desc_layout, NULL);
		ldp->compose_desc_layout = VK_NULL_HANDLE;
	}
	if (ldp->compose_sampler != VK_NULL_HANDLE) {
		vk->vkDestroySampler(vk->device, ldp->compose_sampler, NULL);
		ldp->compose_sampler = VK_NULL_HANDLE;
	}
	if (ldp->bg_view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, ldp->bg_view, NULL);
		ldp->bg_view = VK_NULL_HANDLE;
	}
	if (ldp->bg_image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, ldp->bg_image, NULL);
		ldp->bg_image = VK_NULL_HANDLE;
	}
	if (ldp->bg_mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, ldp->bg_mem, NULL);
		ldp->bg_mem = VK_NULL_HANDLE;
	}
	if (ldp->bg_capture != nullptr) {
		leia_bg_capture_destroy(ldp->bg_capture);
		ldp->bg_capture = nullptr;
	}
	ldp->bg_compose_enabled = false;
	ldp->compose_inited = false;
}

#else // !_WIN32

static inline bool compose_should_run(struct leia_display_processor *) { return false; }
static inline void compose_release_resources(struct leia_display_processor *) {}

#endif // _WIN32


/*
 *
 * xrt_display_processor interface methods.
 *
 */

static void
leia_dp_process_atlas(struct xrt_display_processor *xdp,
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
	// #85 / #613 — honor the canvas sub-rect. A `_texture` app confines the
	// weave/blit to a sub-region of the panel-sized target
	// (xrSetSharedTextureOutputRectEXT; in a zones frame the canvas is the full
	// client window, so this is the identity). canvas_*==0 ⟹ full target. The SR
	// Vulkan weaver reads this VkRect2D via setViewport/setScissorRect
	// (leia_sr.cpp), incorporating vpX/vpY into the interlacing phase — so unlike
	// the stale TODO claimed, no SR SDK change is needed; the offset just has to
	// be forwarded. Mirrors the D3D11 reference (leia_display_processor_d3d11.cpp
	// :1232-1241).
	int32_t vp_x = 0, vp_y = 0;
	uint32_t vp_w = target_width, vp_h = target_height;
	if (canvas_width > 0 && canvas_height > 0) {
		vp_x = canvas_offset_x;
		vp_y = canvas_offset_y;
		vp_w = canvas_width;
		vp_h = canvas_height;
	}

	struct leia_display_processor *ldp = leia_display_processor(xdp);
	struct vk_bundle *vk = ldp->vk;

	// runtime#542: atlas processing follows the CONTENT, not the lens. The
	// runtime hands us the grid it packed; a multi-view atlas weaves, a
	// single-view atlas flat-blits — regardless of the hardware state set
	// via request_display_mode. view_count also feeds the eye-position
	// centering below.
	ldp->view_count = (tile_columns * tile_rows > 1) ? tile_columns * tile_rows : 1;

	// Single-view content: bypass weaver, blit atlas content directly to target
	if (ldp->view_count == 1 && target_image != (VkImage_XDP)VK_NULL_HANDLE) {
		// Barrier: atlas SHADER_READ → TRANSFER_SRC, target COLOR_ATTACHMENT → TRANSFER_DST
		VkImageMemoryBarrier pre[2] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .image = (VkImage)atlas_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .image = (VkImage)target_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		};
		vk->vkCmdPipelineBarrier(cmd_buffer,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    VK_PIPELINE_STAGE_TRANSFER_BIT,
		    0, 0, NULL, 0, NULL, 2, pre);

		// Blit atlas content region (single view) into the canvas sub-rect (full
		// target when no canvas). Outside-canvas pixels are owned by the
		// compositor's Local2D/surround composite, not this blit.
		VkImageBlit blit = {
		    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .srcOffsets = {{0, 0, 0}, {(int32_t)view_width, (int32_t)view_height, 1}},
		    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .dstOffsets = {{vp_x, vp_y, 0}, {vp_x + (int32_t)vp_w, vp_y + (int32_t)vp_h, 1}},
		};
		vk->vkCmdBlitImage(cmd_buffer,
		    (VkImage)atlas_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    (VkImage)target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    1, &blit, VK_FILTER_LINEAR);

		// Barrier: restore atlas → SHADER_READ, target → COLOR_ATTACHMENT_OPTIMAL
		VkImageMemoryBarrier post[2] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        .image = (VkImage)atlas_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		        .image = (VkImage)target_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		    },
		};
		vk->vkCmdPipelineBarrier(cmd_buffer,
		    VK_PIPELINE_STAGE_TRANSFER_BIT,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    0, 0, NULL, 0, NULL, 2, post);
		return;
	}

	// Atlas is guaranteed content-sized SBS (2*view_width x view_height)
	// by compositor crop-blit. Pass directly to weaver.

	// Weave region = the canvas sub-rect (full target when no canvas). The SR
	// weaver folds vp_x/vp_y into the interlacing phase (#85).
	VkRect2D viewport = {};
	viewport.offset.x = vp_x;
	viewport.offset.y = vp_y;
	viewport.extent.width = vp_w;
	viewport.extent.height = vp_h;

	// Transparency (#573 — chroma-key-free). Windows + WGC only: compose the
	// captured desktop region UNDER each per-view atlas tile pre-weave so the
	// weaver consumes opaque RGB with the desktop already integrated, then run a
	// post-weave alpha-gate to reconstruct the α==0 holes. Correct on AA edges
	// and semi-transparent pixels. On non-Windows / no WGC the DP stays opaque.
	VkImageView weaver_input = atlas_view;
#ifdef _WIN32
	bool compose_active = compose_should_run(ldp) && (target_image != (VkImage_XDP)VK_NULL_HANDLE);
	if (compose_active) {
		uint32_t atlas_w = view_width * tile_columns;
		uint32_t atlas_h = view_height * tile_rows;
		// Compose reuses the fill render pass + intermediate (ck_fill_rp +
		// ck_fill_image). ck_init_pipeline is idempotent and self-contained.
		if (ck_init_pipeline(ldp, (VkFormat)target_format)) {
			VkImageView composed = compose_run_pre_weave(ldp, cmd_buffer, atlas_view,
			                                              atlas_w, atlas_h,
			                                              tile_columns, tile_rows,
			                                              canvas_offset_x, canvas_offset_y,
			                                              canvas_width, canvas_height,
			                                              target_width, target_height);
			if (composed != VK_NULL_HANDLE) {
				weaver_input = composed;
			}
			// On compose failure (e.g. no WGC frame yet) just pass through
			// — atlas alpha=0 regions render as black RGB through the weaver,
			// recovers next frame once WGC produces a frame.
		}
	}
#endif

	// SR weaver expects SBS atlas as left_view, VK_NULL_HANDLE as right
	leiasr_weave(ldp->leiasr,
	             cmd_buffer,
	             weaver_input,
	             (VkImageView)VK_NULL_HANDLE,
	             viewport,
	             (int)view_width,
	             (int)view_height,
	             (VkFormat)view_format,
	             target_fb,
	             (int)target_width,
	             (int)target_height,
	             (VkFormat)target_format);

	// Post-weave alpha-gate (compose path): samples the ORIGINAL atlas to derive
	// the screen-space "all views α==0" mask and zeroes alpha on those pixels —
	// DWM blends the LIVE desktop into the holes (no captured-bg lag, no fringe
	// at silhouettes).
#ifdef _WIN32
	if (compose_active && target_image != (VkImage_XDP)VK_NULL_HANDLE) {
		alpha_gate_run_post_weave(ldp, cmd_buffer,
		                          (VkImage)target_image, atlas_view,
		                          target_width, target_height,
		                          tile_columns, tile_rows);
	}
#endif
}

static bool
leia_dp_get_predicted_eye_positions(struct xrt_display_processor *xdp, struct xrt_eye_positions *out_eye_pos)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	// leiasr_eye_pair is #defined to xrt_eye_positions in leia_types.h
	if (!leiasr_get_predicted_eye_positions(ldp->leiasr, (struct leiasr_eye_pair *)out_eye_pos)) {
		return false;
	}
	// In 2D mode, average L/R to a single midpoint eye.
	if (ldp->view_count == 1 && out_eye_pos->count >= 2) {
		out_eye_pos->eyes[0].x = (out_eye_pos->eyes[0].x + out_eye_pos->eyes[1].x) * 0.5f;
		out_eye_pos->eyes[0].y = (out_eye_pos->eyes[0].y + out_eye_pos->eyes[1].y) * 0.5f;
		out_eye_pos->eyes[0].z = (out_eye_pos->eyes[0].z + out_eye_pos->eyes[1].z) * 0.5f;
		out_eye_pos->count = 1;
	}
	return true;
}

static bool
leia_dp_get_window_metrics(struct xrt_display_processor *xdp, struct xrt_window_metrics *out_metrics)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	// leiasr_window_metrics is #defined to xrt_window_metrics in leia_types.h
	return leiasr_get_window_metrics(ldp->leiasr, (struct leiasr_window_metrics *)out_metrics);
}

static bool
leia_dp_request_display_mode(struct xrt_display_processor *xdp, bool enable_3d)
{
	// runtime#542: HARDWARE only — drive the SR lens hint and nothing else.
	// Atlas processing (weave vs flat blit) follows the CONTENT: view_count
	// tracks the per-frame atlas grid in process_atlas, so a hardware
	// override (xrRequestDisplayModeEXT) leaves the weave running and the
	// panel shows the woven atlas flat — the app-authored transition state.
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	return leiasr_request_display_mode(ldp->leiasr, enable_3d);
}

static bool
leia_dp_get_hardware_3d_state(struct xrt_display_processor *xdp, bool *out_is_3d)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	return leiasr_get_hardware_3d_state(ldp->leiasr, out_is_3d);
}

static bool
leia_dp_get_display_dimensions(struct xrt_display_processor *xdp, float *out_width_m, float *out_height_m)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	struct leiasr_display_dimensions dims = {};
	if (!leiasr_get_display_dimensions(ldp->leiasr, &dims) || !dims.valid) {
		return false;
	}
	*out_width_m = dims.width_m;
	*out_height_m = dims.height_m;
	return true;
}

static bool
leia_dp_get_display_pixel_info(struct xrt_display_processor *xdp,
                               uint32_t *out_pixel_width,
                               uint32_t *out_pixel_height,
                               int32_t *out_screen_left,
                               int32_t *out_screen_top)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	float w_m, h_m; // unused but required by API
	return leiasr_get_display_pixel_info(ldp->leiasr, out_pixel_width, out_pixel_height, out_screen_left,
	                                     out_screen_top, &w_m, &h_m);
}

static VkRenderPass
leia_dp_get_render_pass(struct xrt_display_processor *xdp)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	return ldp->render_pass;
}

static bool
leia_dp_is_alpha_native(struct xrt_display_processor *xdp)
{
	(void)xdp;
	// SR Vulkan weaver outputs opaque RGB; transparency is reconstructed by the
	// compose-under-bg + post-weave alpha-gate passes this DP runs (enabled via
	// set_transparent_background), not by a natively-alpha weave.
	return false;
}

// #573 — the sole transparency enable (chroma-key removed). The transparency is
// pure compose-under-bg + post-weave alpha-gate (no chroma-key fill/strip pass).
// The leia VK DP only ever runs IN-PROCESS on Windows (the runtime's vk_native
// compositor presents opaque and the DP owns see-through via WGC compose-under-bg,
// client_presents=false); IPC VK clients route through the D3D11 service DP, so
// the client-present branch never fires here but is kept for symmetry with the
// D3D11/D3D12 slots.
static void
leia_dp_vk_set_transparent_background(struct xrt_display_processor_vk *xdp, bool enabled, bool client_presents)
{
	struct leia_display_processor *ldp = leia_display_processor(&xdp->base);

	// Client-present mode: the runtime owns a transparent present and blends the
	// live screen into the holes, so the DP must NOT compose-under-bg.
	if (enabled && client_presents) {
#ifdef _WIN32
		if (ldp->bg_compose_enabled) {
			compose_release_resources(ldp);
			ldp->bg_compose_enabled = false;
		}
#endif
		U_LOG_W("Leia VK DP: transparency = client-present (alpha-gate only, no WGC)");
		return;
	}

#ifdef _WIN32
	// In-process path: WGC desktop capture + per-tile compose-under-bg.
	if (enabled && !ldp->bg_compose_enabled && ldp->hwnd_opaque != nullptr) {
		ldp->bg_capture = leia_bg_capture_create((HWND)ldp->hwnd_opaque);
		if (ldp->bg_capture != nullptr) {
			if (compose_import_bg_image(ldp)) {
				ldp->bg_compose_enabled = true;
				U_LOG_W("Leia VK DP: transparency = compose-under-bg (WGC)");
				return;
			}
			U_LOG_W("Leia VK DP: bg image import failed — staying opaque");
			leia_bg_capture_destroy(ldp->bg_capture);
			ldp->bg_capture = nullptr;
		}
	}
#else
	(void)enabled;
#endif
}

// #613 / #68 — VK port of the D3D11 shared-texture-present slot. Pure state set:
// records whether the app self-presents only the canvas (texture apps) vs the
// full target (handle apps). Combined with zone_active, gates the compose-under-bg
// canvas bg-UV remap skip (see compose_run_pre_weave). Reads no hardware/mask.
static void
leia_dp_vk_set_shared_texture_present(struct xrt_display_processor_vk *xdp, bool enabled)
{
	struct leia_display_processor *ldp = leia_display_processor(&xdp->base);
	ldp->shared_texture_present = enabled;
	U_LOG_W("Leia VK DP: shared-texture present = %d (#68 bg-UV remap %s in zones frames)", enabled,
	        enabled ? "SKIPPED" : "applied");
}

// #491 part 3 — store the runtime's flattened 2D-under backdrop for the next
// process_atlas. The compose-under-bg path (when WGC transparency is active)
// composites it OVER the captured desktop before the atlas-over. Same VkDevice
// as the compositor, so the view is used directly. NULL ⟹ clear (desktop-only).
static void
leia_dp_set_background_2d(struct xrt_display_processor *xdp,
                          VkImageView background_view,
                          uint32_t width,
                          uint32_t height)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	ldp->backdrop_view = background_view;
	ldp->backdrop_w = width;
	ldp->backdrop_h = height;
}

// #602: the compositor recreated its target image set (window resize rebuilt
// the swapchain / DComp-bridge ring). Our ck_strip_fbs cache keys entries by
// the target VkImage handle (see ck_strip_get_fb) — but Vulkan recycles freed
// image handles, so an entry can survive the size-high-water check and alias a
// destroyed image, faulting the device on the next strip render. Drop the whole
// strip framebuffer cache; process_atlas rebuilds it over the fresh targets.
// The compositor drains the device before calling us, so destroying these
// objects synchronously here is safe. ck_fill_* / ck_strip_image are DP-owned
// offscreen resources (not keyed by the target image) and stay put.
static void
leia_dp_notify_target_recreated(struct xrt_display_processor_vk *xdp, uint32_t generation)
{
	struct leia_display_processor *ldp = leia_display_processor(&xdp->base);
	if (ldp->vk == NULL) {
		return;
	}
	if (generation == ldp->ck_target_generation) {
		return; // already flushed for this generation — idempotent.
	}
	ldp->ck_target_generation = generation;

	struct vk_bundle *vk = ldp->vk;
	for (uint32_t i = 0; i < ldp->ck_strip_fbs_count; i++) {
		if (ldp->ck_strip_fbs[i].fb != VK_NULL_HANDLE) {
			vk->vkDestroyFramebuffer(vk->device, ldp->ck_strip_fbs[i].fb, NULL);
		}
		if (ldp->ck_strip_fbs[i].view != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, ldp->ck_strip_fbs[i].view, NULL);
		}
	}
	std::memset(&ldp->ck_strip_fbs, 0, sizeof(ldp->ck_strip_fbs));
	ldp->ck_strip_fbs_count = 0;
	U_LOG_W("Leia VK DP: #602 target images recreated (gen %u) — strip fb cache flushed", generation);
}

static void
leia_dp_destroy(struct xrt_display_processor *xdp)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	struct vk_bundle *vk = ldp->vk;

	if (vk != NULL) {
		compose_release_resources(ldp);
		ck_release_resources(ldp);
		zone_reduce_fini(ldp); // #613 — zone-mask reduction resources
		if (ldp->render_pass != VK_NULL_HANDLE) {
			vk->vkDestroyRenderPass(vk->device, ldp->render_pass, NULL);
		}
	}

	leiasr_destroy(ldp->leiasr);
	free(ldp);
}


#ifdef _WIN32
/*!
 * Ensure the SR Vulkan weaver DLL (SimulatedRealityVulkanBeta.dll) is loaded
 * from this plug-in's own directory before the first weaver call.
 *
 * The SR SDK import lib pulls SimulatedRealityVulkanBeta.dll in as a
 * *delay-loaded* dependency. The Windows loader resolves delay-loaded deps
 * against the host *process's* search path (exe dir, System32, PATH) — NOT
 * the directory of the DLL that triggers the load. We ship the weaver DLL
 * next to this plug-in, but that directory is on none of those search paths,
 * so any host that doesn't carry the DLL next to its own exe (e.g. a Unity
 * Player) hits ERROR_MOD_NOT_FOUND (raised as 0xC06D007E by the VC++
 * delay-load helper) at the first weaver call and crashes inside
 * xrCreateSession. (The D3D weaver, SimulatedRealityDirectX.dll, escapes this
 * because the SR Platform installer puts it on PATH.)
 *
 * Pre-loading by absolute path from our own directory makes the module
 * resident before the delay-load thunk fires; the thunk then binds by base
 * name. LOAD_WITH_ALTERED_SEARCH_PATH additionally lets the weaver DLL resolve
 * its own co-located dependencies from our directory. Idempotent and cheap.
 */
static void
ensure_sr_vulkan_dll_loaded(void)
{
	static bool tried = false;
	if (tried) {
		return;
	}
	tried = true;

	HMODULE self = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCWSTR>(&ensure_sr_vulkan_dll_loaded), &self)) {
		U_LOG_W("SR VK weaver preload: GetModuleHandleExW failed (err=%lu)", GetLastError());
		return;
	}

	wchar_t path[MAX_PATH];
	DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		U_LOG_W("SR VK weaver preload: GetModuleFileNameW failed (err=%lu)", GetLastError());
		return;
	}

	// Replace this plug-in's filename with the SR weaver DLL name.
	wchar_t *slash = wcsrchr(path, L'\\');
	if (slash == NULL) {
		return;
	}
	slash[1] = L'\0';
	static const wchar_t kSrVkDll[] = L"SimulatedRealityVulkanBeta.dll";
	if (wcslen(path) + wcslen(kSrVkDll) >= MAX_PATH) {
		return;
	}
	wcscat_s(path, MAX_PATH, kSrVkDll);

	HMODULE sr = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (sr == NULL) {
		U_LOG_W("SR VK weaver preload: LoadLibraryExW('%ls') failed (err=%lu) — "
		        "weaver delay-load will likely fail",
		        path, GetLastError());
	} else {
		U_LOG_W("SR VK weaver preload: loaded SimulatedRealityVulkanBeta.dll from plug-in dir");
	}
}
#endif // _WIN32

/*
 *
 * #613 / ADR-027 — local 2D/3D zone vtable (base slots 18/19/20).
 *
 * The published R8_UNORM wish mask drives ONLY the SR lens hint
 * (leiasr_request_display_mode) — never content compositing (ADR-028). The
 * 1×1-grid collapse ("any non-zero pixel ⟹ this client wishes 3D", OR-union'd
 * across clients by the runtime) is evaluated once per mask content generation
 * (seq), then edge-triggered onto the lens. Ported from the D3D11 reference
 * (leia_display_processor_d3d11.cpp:1499-1651); the only divergence is the mask
 * readback — the VK base slot hands a VkImageView (no VkImage), so the verdict
 * is read via a compute reduction (zone_reduce.comp) rather than CopyResource+Map.
 *
 */

// Lazy-build the compute reduction pipeline + result buffer + own command pool.
// Returns false (leaving the DP in tier-1 fallback) on any failure.
static bool
zone_reduce_init(struct leia_display_processor *ldp)
{
	if (ldp->zone_reduce_inited) {
		return true;
	}
	struct vk_bundle *vk = ldp->vk;
	if (vk == NULL) {
		return false;
	}

	// Own command pool for the one-shot reduction submit (kept off the SR
	// weaver's pool to avoid recording/reset hazards).
	if (!ldp->zone_pool_inited) {
		if (vk_cmd_pool_init(vk, &ldp->zone_pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) !=
		    VK_SUCCESS) {
			return false;
		}
		ldp->zone_pool_inited = true;
	}

	VkShaderModuleCreateInfo smci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(shaders_zone_reduce_comp),
	    .pCode = shaders_zone_reduce_comp,
	};
	if (vk->vkCreateShaderModule(vk->device, &smci, NULL, &ldp->zone_reduce_shader) != VK_SUCCESS) {
		return false;
	}

	VkDescriptorSetLayoutBinding binds[2] = {
	    {.binding = 0,
	     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	     .descriptorCount = 1,
	     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
	    {.binding = 1,
	     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	     .descriptorCount = 1,
	     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
	};
	VkDescriptorSetLayoutCreateInfo dlci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 2,
	    .pBindings = binds,
	};
	if (vk->vkCreateDescriptorSetLayout(vk->device, &dlci, NULL, &ldp->zone_reduce_desc_layout) != VK_SUCCESS) {
		return false;
	}

	VkPushConstantRange pcr = {
	    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	    .offset = 0,
	    .size = 2 * sizeof(uint32_t),
	};
	VkPipelineLayoutCreateInfo plci = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &ldp->zone_reduce_desc_layout,
	    .pushConstantRangeCount = 1,
	    .pPushConstantRanges = &pcr,
	};
	if (vk->vkCreatePipelineLayout(vk->device, &plci, NULL, &ldp->zone_reduce_pipeline_layout) != VK_SUCCESS) {
		return false;
	}

	VkComputePipelineCreateInfo cpci = {
	    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	    .stage =
	        {
	            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
	            .module = ldp->zone_reduce_shader,
	            .pName = "main",
	        },
	    .layout = ldp->zone_reduce_pipeline_layout,
	};
	if (vk->vkCreateComputePipelines(vk->device, VK_NULL_HANDLE, 1, &cpci, NULL, &ldp->zone_reduce_pipeline) !=
	    VK_SUCCESS) {
		return false;
	}

	VkDescriptorPoolSize psizes[2] = {
	    {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1},
	    {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1},
	};
	VkDescriptorPoolCreateInfo dpci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = 1,
	    .poolSizeCount = 2,
	    .pPoolSizes = psizes,
	};
	if (vk->vkCreateDescriptorPool(vk->device, &dpci, NULL, &ldp->zone_reduce_desc_pool) != VK_SUCCESS) {
		return false;
	}
	VkDescriptorSetAllocateInfo dsai = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool = ldp->zone_reduce_desc_pool,
	    .descriptorSetCount = 1,
	    .pSetLayouts = &ldp->zone_reduce_desc_layout,
	};
	if (vk->vkAllocateDescriptorSets(vk->device, &dsai, &ldp->zone_reduce_set) != VK_SUCCESS) {
		return false;
	}

	// texelFetch ignores filtering, but a combined image sampler still needs a sampler.
	VkSamplerCreateInfo sci = {
	    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter = VK_FILTER_NEAREST,
	    .minFilter = VK_FILTER_NEAREST,
	    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
	    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	if (vk->vkCreateSampler(vk->device, &sci, NULL, &ldp->zone_reduce_sampler) != VK_SUCCESS) {
		return false;
	}

	VkBufferCreateInfo bci = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = sizeof(uint32_t),
	    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (vk->vkCreateBuffer(vk->device, &bci, NULL, &ldp->zone_reduce_buf) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements mreq = {};
	vk->vkGetBufferMemoryRequirements(vk->device, ldp->zone_reduce_buf, &mreq);
	uint32_t mtype = 0;
	if (!vk_get_memory_type(vk, mreq.memoryTypeBits,
	                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mtype)) {
		return false;
	}
	VkMemoryAllocateInfo mai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mreq.size,
	    .memoryTypeIndex = mtype,
	};
	if (vk->vkAllocateMemory(vk->device, &mai, NULL, &ldp->zone_reduce_mem) != VK_SUCCESS) {
		return false;
	}
	if (vk->vkBindBufferMemory(vk->device, ldp->zone_reduce_buf, ldp->zone_reduce_mem, 0) != VK_SUCCESS) {
		return false;
	}
	if (vk->vkMapMemory(vk->device, ldp->zone_reduce_mem, 0, VK_WHOLE_SIZE, 0, &ldp->zone_reduce_ptr) !=
	    VK_SUCCESS) {
		return false;
	}

	// Bind the stable result buffer once; the mask view (binding 0) is refreshed per call.
	VkDescriptorBufferInfo dbi = {.buffer = ldp->zone_reduce_buf, .offset = 0, .range = VK_WHOLE_SIZE};
	VkWriteDescriptorSet bw = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = ldp->zone_reduce_set,
	    .dstBinding = 1,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	    .pBufferInfo = &dbi,
	};
	vk->vkUpdateDescriptorSets(vk->device, 1, &bw, 0, NULL);

	ldp->zone_reduce_inited = true;
	return true;
}

// Evaluate "any non-zero pixel" in the published mask view via a compute
// dispatch. On any failure returns true conservatively (treat as a 3D wish) so
// a transient GPU error never strands the panel flat under an active mask.
static bool
zone_mask_any_nonzero(struct leia_display_processor *ldp, VkImageView mask_view, uint32_t w, uint32_t h)
{
	struct vk_bundle *vk = ldp->vk;
	if (!zone_reduce_init(ldp)) {
		return true; // conservative fallback (see contract above)
	}

	VkDescriptorImageInfo dii = {
	    .sampler = ldp->zone_reduce_sampler,
	    .imageView = mask_view,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet iw = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = ldp->zone_reduce_set,
	    .dstBinding = 0,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &dii,
	};
	vk->vkUpdateDescriptorSets(vk->device, 1, &iw, 0, NULL);

	// Zero the result. The queue submit makes this host write visible to the device.
	*(uint32_t *)ldp->zone_reduce_ptr = 0;

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vk_cmd_pool_create_and_begin_cmd_buffer(vk, &ldp->zone_pool, 0, &cmd) != VK_SUCCESS) {
		return true;
	}

	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ldp->zone_reduce_pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ldp->zone_reduce_pipeline_layout, 0, 1,
	                            &ldp->zone_reduce_set, 0, NULL);
	uint32_t pc[2] = {w, h};
	vk->vkCmdPushConstants(cmd, ldp->zone_reduce_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
	vk->vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);

	// Make the compute store available to the post-fence host read.
	VkBufferMemoryBarrier bmb = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .buffer = ldp->zone_reduce_buf,
	    .offset = 0,
	    .size = VK_WHOLE_SIZE,
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
	                         &bmb, 0, NULL);

	if (vk_cmd_pool_end_submit_wait_and_free_cmd_buffer(vk, &ldp->zone_pool, cmd) != VK_SUCCESS) {
		return true;
	}

	return *(const uint32_t *)ldp->zone_reduce_ptr != 0;
}

static bool
leia_dp_get_local_zone_caps(struct xrt_display_processor *xdp, struct xrt_dp_local_zone_caps *out_caps)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	if (out_caps == NULL || out_caps->struct_size < XRT_DP_LOCAL_ZONE_CAPS_SIZE_V1) {
		// V1 is the floor — reject only callers older than the zone API itself;
		// the appended ADR-027 fields are written only when struct_size covers them.
		return false;
	}
	// Needs the VK device (factory path) + the SR lens-hint channel. The legacy
	// create path sets neither ldp->vk nor a mode-switch-capable weaver, so it
	// reports unsupported and the runtime keeps the tier-1 global fallback.
	if (ldp->vk == NULL || !leiasr_supports_display_mode_switch(ldp->leiasr)) {
		return false;
	}
	out_caps->supported = 1;
	out_caps->zone_grid_width = 1; // single-zone panel: union collapses to global on/off
	out_caps->zone_grid_height = 1;
	out_caps->max_mask_width = 0;  // no preference — content is reduced to one bit
	out_caps->max_mask_height = 0;
	out_caps->max_update_hz = 0;   // edge-triggered internally (verdict changes only)
	if (out_caps->struct_size >= sizeof(struct xrt_dp_local_zone_caps)) {
		out_caps->wish_fractional = 0; // SR weaver drives a binary panel state
		out_caps->switch_granularity = (uint32_t)XRT_DP_SWITCH_GRANULARITY_UNKNOWN;
		memset(out_caps->reserved, 0, sizeof(out_caps->reserved));
	}
	return true;
}

static bool
leia_dp_publish_local_zone_mask(struct xrt_display_processor *xdp,
                                VkImageView mask_view,
                                uint32_t mask_width,
                                uint32_t mask_height,
                                int32_t screen_x,
                                int32_t screen_y,
                                uint32_t screen_w,
                                uint32_t screen_h,
                                uint64_t seq)
{
	// 1×1 grid: the screen anchor can't change the verdict — only content
	// matters, and content only changes per generation (seq).
	(void)screen_x;
	(void)screen_y;
	(void)screen_w;
	(void)screen_h;

	struct leia_display_processor *ldp = leia_display_processor(xdp);
	if (ldp->vk == NULL || mask_view == VK_NULL_HANDLE || mask_width == 0 || mask_height == 0) {
		return false;
	}

	// Content evaluation, once per generation: any non-zero mask pixel ⟹ 3D
	// (an all-zero mask — Tier-1 enable3D=FALSE — must collapse to 2D).
	if (!ldp->zone_eval_valid || seq != ldp->zone_eval_seq) {
		ldp->zone_want_3d = zone_mask_any_nonzero(ldp, mask_view, mask_width, mask_height);
		ldp->zone_eval_seq = seq;
		ldp->zone_eval_valid = true;
		U_LOG_W("SR VK zone: generation %llu evaluated → %s (mask %ux%u, 1x1 collapse)",
		        (unsigned long long)seq, ldp->zone_want_3d ? "3D" : "2D", mask_width, mask_height);
	}

	// Edge-triggered lens hint: flip only when the verdict changes (or on the
	// first publish), so per-frame republish costs nothing SR-side.
	if (!ldp->zone_active || ldp->zone_hint_3d != ldp->zone_want_3d) {
		if (!leiasr_request_display_mode(ldp->leiasr, ldp->zone_want_3d)) {
			return false;
		}
		ldp->zone_hint_3d = ldp->zone_want_3d;
	}
	ldp->zone_active = true;
	return true;
}

static bool
leia_dp_clear_local_zone_mask(struct xrt_display_processor *xdp)
{
	struct leia_display_processor *ldp = leia_display_processor(xdp);
	// End of zone authority for this client — hand the lens back to the MODE
	// authority: restore the hint to what the active rendering mode implies
	// (view_count ≥ 2 ⟹ a 3D mode is active and the compositor snaps back to the
	// canvas weave, needing a 3D panel; view_count 1 ⟹ 2D). Blindly disabling
	// would strand a 3D-mode canvas weave on a 2D panel after mask destroy.
	if (ldp->zone_active) {
		bool mode_wants_3d = ldp->view_count >= 2;
		if (ldp->zone_hint_3d != mode_wants_3d) {
			leiasr_request_display_mode(ldp->leiasr, mode_wants_3d);
		}
		ldp->zone_hint_3d = false;
		U_LOG_W("SR VK zone: cleared — lens handed back to mode authority (%s)", mode_wants_3d ? "3D" : "2D");
	}
	ldp->zone_active = false;
	ldp->zone_eval_valid = false;
	return true;
}

// Tear down the zone-reduction resources (zone_reduce_init counterpart).
static void
zone_reduce_fini(struct leia_display_processor *ldp)
{
	struct vk_bundle *vk = ldp->vk;
	if (vk == NULL) {
		return;
	}
	if (ldp->zone_reduce_ptr != NULL) {
		vk->vkUnmapMemory(vk->device, ldp->zone_reduce_mem);
		ldp->zone_reduce_ptr = NULL;
	}
	if (ldp->zone_reduce_buf != VK_NULL_HANDLE)
		vk->vkDestroyBuffer(vk->device, ldp->zone_reduce_buf, NULL);
	if (ldp->zone_reduce_mem != VK_NULL_HANDLE)
		vk->vkFreeMemory(vk->device, ldp->zone_reduce_mem, NULL);
	if (ldp->zone_reduce_sampler != VK_NULL_HANDLE)
		vk->vkDestroySampler(vk->device, ldp->zone_reduce_sampler, NULL);
	if (ldp->zone_reduce_pipeline != VK_NULL_HANDLE)
		vk->vkDestroyPipeline(vk->device, ldp->zone_reduce_pipeline, NULL);
	if (ldp->zone_reduce_pipeline_layout != VK_NULL_HANDLE)
		vk->vkDestroyPipelineLayout(vk->device, ldp->zone_reduce_pipeline_layout, NULL);
	if (ldp->zone_reduce_desc_pool != VK_NULL_HANDLE)
		vk->vkDestroyDescriptorPool(vk->device, ldp->zone_reduce_desc_pool, NULL);
	if (ldp->zone_reduce_desc_layout != VK_NULL_HANDLE)
		vk->vkDestroyDescriptorSetLayout(vk->device, ldp->zone_reduce_desc_layout, NULL);
	if (ldp->zone_reduce_shader != VK_NULL_HANDLE)
		vk->vkDestroyShaderModule(vk->device, ldp->zone_reduce_shader, NULL);
	if (ldp->zone_pool_inited) {
		vk_cmd_pool_destroy(vk, &ldp->zone_pool);
		ldp->zone_pool_inited = false;
	}
}


/*
 *
 * Factory function — matches xrt_dp_factory_vk_fn_t signature.
 *
 */

extern "C" xrt_result_t
leia_dp_factory_vk(void *vk_bundle_ptr,
                   void *vk_cmd_pool,
                   void *window_handle,
                   int32_t target_format,
                   struct xrt_display_processor **out_xdp)
{
#ifdef _WIN32
	// SimulatedRealityVulkanBeta.dll is delay-loaded and ships next to this
	// plug-in, which is on no default DLL search path — preload it from our
	// own directory so the first weaver call doesn't AV on a missing module.
	ensure_sr_vulkan_dll_loaded();
#endif

	// Extract Vulkan handles from vk_bundle.
	struct vk_bundle *vk = (struct vk_bundle *)vk_bundle_ptr;

	// Allocate and fully initialize the display-processor struct BEFORE
	// creating the SR Vulkan weaver. CreateVulkanWeaver triggers heavy
	// NVIDIA Vulkan shader-compiler heap churn (host-side LLVM pipeline
	// compilation). If this small struct is allocated *after* that churn it
	// can land on a heap block the driver's compiler later reuses, stomping
	// the vtable — observed standalone on the Leia box as a garbage
	// get_display_pixel_info pointer (vtable offset 0x38) crashing
	// xrCreateSession. Allocating + initializing first keeps the block out
	// of the weaver/driver's freed-and-reused region.
	struct leia_display_processor *ldp = (struct leia_display_processor *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	// ADR-020 rule 1: advertise the vtable size (calloc zeroed reserved_0).
	// Tells the runtime which slots this plug-in actually built. #573: advertise
	// the _vk variant size so the runtime recognizes the appended
	// set_transparent_background slot (the per-API struct_size gate).
	ldp->base.base.struct_size = static_cast<uint32_t>(sizeof(struct xrt_display_processor_vk));
	ldp->base.base.process_atlas = leia_dp_process_atlas;
	ldp->base.base.get_render_pass = leia_dp_get_render_pass;
	ldp->base.base.get_predicted_eye_positions = leia_dp_get_predicted_eye_positions;
	ldp->base.base.get_window_metrics = leia_dp_get_window_metrics;
	ldp->base.base.request_display_mode = leia_dp_request_display_mode;
	ldp->base.base.get_hardware_3d_state = leia_dp_get_hardware_3d_state;
	ldp->base.base.get_display_dimensions = leia_dp_get_display_dimensions;
	ldp->base.base.get_display_pixel_info = leia_dp_get_display_pixel_info;
	ldp->base.base.is_alpha_native = leia_dp_is_alpha_native;
	ldp->base.base.set_background_2d = leia_dp_set_background_2d; // #491 part 3
	ldp->base.base.destroy = leia_dp_destroy;
	ldp->base.notify_target_recreated = leia_dp_notify_target_recreated; // #602 (appended VK-variant slot)
	ldp->base.set_transparent_background = leia_dp_vk_set_transparent_background; // #573 (appended slot)
	// #613 / ADR-027 — local 2D/3D zones (base slots 18/19/20). struct_size
	// already covers them (the _vk variant size > sizeof(xrt_display_processor)),
	// so no ABI bump — the slots ship in the runtime base header already.
	ldp->base.base.get_local_zone_caps = leia_dp_get_local_zone_caps;
	ldp->base.base.publish_local_zone_mask = leia_dp_publish_local_zone_mask;
	ldp->base.base.clear_local_zone_mask = leia_dp_clear_local_zone_mask;
	ldp->base.set_shared_texture_present = leia_dp_vk_set_shared_texture_present; // #613/#68 (appended VK slot)
	ldp->vk = vk;
	ldp->view_count = 2;
	ldp->hwnd_opaque = window_handle;

	// Now create the SR Vulkan weaver (spins up SR senses + NV VK pipelines).
	struct leiasr *leiasr = NULL;
	xrt_result_t ret = leiasr_create(5.0, vk->device, vk->physical_device, vk->main_queue->queue,
	                                 (VkCommandPool)(uintptr_t)vk_cmd_pool, window_handle, &leiasr);
	if (ret != XRT_SUCCESS || leiasr == NULL) {
		U_LOG_W("Failed to create SR Vulkan weaver, continuing without interlacing");
		free(ldp);
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	ldp->leiasr = leiasr;

	// Create a render pass compatible with the SR weaver's output.
	// The weaver renders to a single color attachment (no depth).
	// Use the target_format passed by the compositor, or B8G8R8A8_UNORM as default.
	VkFormat rp_format = (target_format != 0) ? (VkFormat)target_format : VK_FORMAT_B8G8R8A8_UNORM;
	VkAttachmentDescription color_attachment = {
	    .format = rp_format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	};
	VkAttachmentReference color_ref = {
	    .attachment = 0,
	    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription subpass = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &color_ref,
	};
	VkRenderPassCreateInfo rp_info = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &color_attachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	};
	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkResult vk_ret = vk->vkCreateRenderPass(vk->device, &rp_info, NULL, &render_pass);
	if (vk_ret != VK_SUCCESS) {
		U_LOG_E("Leia VK DP: failed to create render pass: %d", vk_ret);
		leiasr_destroy(leiasr);
		free(ldp);
		return XRT_ERROR_VULKAN;
	}

	// vtable + leiasr/vk/view_count/hwnd_opaque were set above (before the
	// weaver) for heap-isolation; only the render pass remains.
	ldp->render_pass = render_pass;

	*out_xdp = &ldp->base.base;

	U_LOG_W("Created Leia SR display processor (factory, owns weaver, render_pass=%p)",
	        (void *)render_pass);

	return XRT_SUCCESS;
}


/*
 *
 * Legacy creation function — wraps an existing leiasr handle.
 * Kept for backward compatibility during the refactoring transition.
 *
 */

extern "C" xrt_result_t
leia_display_processor_create(struct leiasr *leiasr, struct xrt_display_processor **out_xdp)
{
	if (leiasr == NULL || out_xdp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor *ldp = (struct leia_display_processor *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	// ADR-020 rule 1: advertise the vtable size (calloc zeroed reserved_0).
	// #573: advertise the _vk variant size (see factory note).
	ldp->base.base.struct_size = static_cast<uint32_t>(sizeof(struct xrt_display_processor_vk));
	ldp->base.base.process_atlas = leia_dp_process_atlas;
	ldp->base.base.get_predicted_eye_positions = leia_dp_get_predicted_eye_positions;
	ldp->base.base.get_window_metrics = leia_dp_get_window_metrics;
	ldp->base.base.request_display_mode = leia_dp_request_display_mode;
	ldp->base.base.get_hardware_3d_state = leia_dp_get_hardware_3d_state;
	ldp->base.base.get_display_dimensions = leia_dp_get_display_dimensions;
	ldp->base.base.get_display_pixel_info = leia_dp_get_display_pixel_info;
	ldp->base.base.is_alpha_native = leia_dp_is_alpha_native;
	ldp->base.base.set_background_2d = leia_dp_set_background_2d; // #491 part 3
	// #573 — appended transparency slot. The legacy path never sets hwnd_opaque,
	// so the enable degrades to a graceful no-op (compose can't start without an
	// HWND); kept wired for vtable parity with the factory.
	ldp->base.set_transparent_background = leia_dp_vk_set_transparent_background;
	// Legacy: does NOT own leiasr — use a destroy that skips leiasr_destroy.
	// For now just assign the full destroy; callers will be migrated to factory.
	ldp->base.base.destroy = leia_dp_destroy;
	ldp->base.notify_target_recreated = leia_dp_notify_target_recreated; // #602 (appended VK-variant slot)
	// #613 — zone slots wired for vtable parity; the legacy path leaves ldp->vk
	// NULL, so get_local_zone_caps reports unsupported and the runtime keeps the
	// tier-1 global fallback (no readback path needed here).
	ldp->base.base.get_local_zone_caps = leia_dp_get_local_zone_caps;
	ldp->base.base.publish_local_zone_mask = leia_dp_publish_local_zone_mask;
	ldp->base.base.clear_local_zone_mask = leia_dp_clear_local_zone_mask;
	ldp->base.set_shared_texture_present = leia_dp_vk_set_shared_texture_present; // #613/#68 (appended VK slot)

	ldp->leiasr = leiasr;
	ldp->view_count = 2;

	*out_xdp = &ldp->base.base;

	U_LOG_W("Created Leia SR display processor (legacy, owns weaver)");

	return XRT_SUCCESS;
}
