// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  `leia_vk_weaver_ops` implemented on the SR v2 C99 Vulkan surface.
 * @ingroup drv_leia
 *
 * The VK arm already routes every weaver call through `leia_vk_weaver_ops`,
 * an indirection that exists *because* the C++ ABI was unstable across SR
 * versions (see `leia_vk_weaver_select.cpp`). That workaround turns out to be
 * the cleanest possible seam for this migration: the v2 path is simply a third
 * dispatch table alongside `_legacy` and `_stamp`, and **not one call site in
 * `leia_sr.cpp` changes**.
 *
 * Where the other three arms needed per-operation `w_*` helpers to hide the
 * v1/v2 branch, this arm needed none — the branch is the table pointer.
 *
 * `raw` here is an `SrWeaver`, not an `SR::IVulkanWeaver1 *`. Nothing outside
 * this file may assume either; that is the point of the opaque `void *`.
 */

#include "leia_vk_weaver.h"

#ifdef DXR_LEIA_HAS_SR_V2

#include "leia_sr_v2_common.h"
#include "util/u_logging.h"

#include <sr/sr_vk.h>
#include <sr/sr_weaver.h>

namespace {

SrWeaver
as_weaver(void *raw)
{
	return static_cast<SrWeaver>(raw);
}

/*!
 * Not used. The v2 weaver is created in `leia_sr.cpp` via `srCreateWeaverVulkan`,
 * because creation needs the `SrInstance` — which the v1 signature has no room
 * for (it takes an `SR::SRContext *`). Rather than widen the shared vtable for
 * one backend, creation stays out of the table and only the per-frame ops go
 * through it.
 */
void *
v2_create(void *, SR::SRContext *, VkDevice, VkPhysicalDevice, VkQueue, VkCommandPool, HWND, int *out_err)
{
	if (out_err != nullptr) {
		*out_err = -1;
	}
	U_LOG_E("leia_vk_weaver_ops_v2: create() is not routed through the vtable - this is a bug");
	return nullptr;
}

void
v2_destroy(void *raw)
{
	srDestroyWeaver(as_weaver(raw));
}

void
v2_set_viewport(void *raw, RECT vp)
{
	// left/top/RIGHT/BOTTOM — not left/top/width/height. Both are int32_t x4,
	// so the wrong reading compiles cleanly and weaves into a wrong rectangle.
	// (Note this differs from the DX12 viewport call, which is x/y/w/h floats.)
	srWeaverSetViewportVulkan(as_weaver(raw), (int32_t)vp.left, (int32_t)vp.top, (int32_t)vp.right,
	                      (int32_t)vp.bottom);
}

void
v2_set_scissor_rect(void *raw, RECT rc)
{
	srWeaverSetScissorRectVulkan(as_weaver(raw), (int32_t)rc.left, (int32_t)rc.top, (int32_t)rc.right,
	                         (int32_t)rc.bottom);
}

void
v2_set_command_buffer(void *raw, VkCommandBuffer cmd)
{
	// v2 rejects a null command buffer with SR_ERROR_VALIDATION_FAILURE, where
	// v1's weave() silently early-returned and produced a black screen. The
	// caller already substitutes its pre-allocated buffer, so this should never
	// fire — but log it rather than discard the result, because a black screen
	// with no explanation is exactly what this API change was made to prevent.
	const SrResult r = srWeaverSetCommandBufferVulkan(as_weaver(raw), (SrVkCommandBuffer)cmd);
	if (!SR_SUCCEEDED(r)) {
		U_LOG_E("srWeaverSetCommandBufferVulkan failed: %s (%d)", leia_sr_v2_result_str(r), (int)r);
	}
}

void
v2_set_input_view_texture(void *raw, VkImageView left, VkImageView right, int width, int height, VkFormat format)
{
	// SINGLE side-by-side image. `left` IS the SBS atlas and `right` is always
	// VK_NULL_HANDLE — see the only caller, leia_display_processor.cpp:
	//   "// SR weaver expects SBS atlas as left_view, VK_NULL_HANDLE as right"
	// The two-parameter shape is vestigial, inherited from IVulkanWeaver1's
	// setInputViewTexture(left, right, ...). v2 makes the contract honest by
	// taking one image; the unused parameter stays only because the shared
	// leia_vk_weaver_ops vtable is also implemented by the two v1 backends.
	(void)right;
	srWeaverSetInputTextureVulkan(as_weaver(raw), (SrVkImageView)left, (int32_t)width, (int32_t)height,
	                              (SrVkFormat)format);
}

void
v2_set_output_framebuffer(void *raw, VkFramebuffer fb, int width, int height, VkFormat format)
{
	// NOTE: passing VK_NULL_HANDLE here means "the command buffer already has a
	// render pass open", NOT "keep the previous framebuffer" — it clears the
	// binding. The caller retains a framebuffer by not calling this at all.
	// See the long comment at the leia_sr.cpp call site.
	srWeaverSetOutputFrameBufferVulkan(as_weaver(raw), (SrVkFramebuffer)fb, (int32_t)width,
	                                   (int32_t)height, (SrVkFormat)format);
}

void
v2_set_latency(void *raw, uint64_t latency_us)
{
	srWeaverSetLatency(as_weaver(raw), latency_us);
}

void
v2_weave(void *raw)
{
	srWeaverWeave(as_weaver(raw));
}

bool
v2_get_predicted_eye_positions(void *raw, float left[3], float right[3])
{
	SrPoint3f l{};
	SrPoint3f r{};
	if (!SR_SUCCEEDED(srWeaverGetPredictedEyePositions(as_weaver(raw), &l, &r))) {
		return false;
	}
	left[0] = l.x;
	left[1] = l.y;
	left[2] = l.z;
	right[0] = r.x;
	right[1] = r.y;
	right[2] = r.z;
	return true;
}

void
v2_weave_submitted(void *raw, VkQueue queue)
{
	// MUST be the queue the weave command buffer went to: the weaver puts an
	// empty fence-carrying submit on it to count frames in flight, so a
	// different queue tracks the wrong thing. A null queue is rejected by the
	// SDK with VALIDATION_FAILURE rather than silently accepted.
	const SrResult r = srWeaverWeaveSubmittedVulkan(as_weaver(raw), (SrVkQueue)queue);
	if (!SR_SUCCEEDED(r)) {
		// Once, not per frame — this sits on the hot path.
		static bool warned = false;
		if (!warned) {
			U_LOG_E("srWeaverWeaveSubmittedVulkan failed: %s (%d) - late latching will not run",
			        leia_sr_v2_result_str(r), (int)r);
			warned = true;
		}
	}
}

bool
v2_enable_late_latching(void *raw, bool enable)
{
	SrWeaver w = as_weaver(raw);
	const SrResult r = srWeaverEnableLateLatching(w, enable ? SR_TRUE : SR_FALSE);
	if (!SR_SUCCEEDED(r)) {
		U_LOG_W("srWeaverEnableLateLatching failed: %s (%d)", leia_sr_v2_result_str(r), (int)r);
		return false;
	}

	// Read back the EFFECTIVE state rather than trusting the enable. The
	// unimplemented backends return a hardcoded false here, and a live backend
	// clears the flag itself on failure — so this is the only honest answer to
	// "did it take". Believing the enable is how you end up standing a latency
	// predictor down in favour of a latch that never runs.
	SrBool32 effective = SR_FALSE;
	if (!SR_SUCCEEDED(srWeaverIsLateLatchingEnabled(w, &effective))) {
		return false;
	}
	return (effective == SR_TRUE) == enable;
}

const struct leia_vk_weaver_ops g_ops_v2 = {
    v2_create,
    v2_destroy,
    v2_set_viewport,
    v2_set_scissor_rect,
    v2_set_command_buffer,
    v2_set_input_view_texture,
    v2_set_output_framebuffer,
    v2_set_latency,
    v2_weave,
    v2_get_predicted_eye_positions,
    v2_weave_submitted,
    v2_enable_late_latching,
};

} // namespace

const struct leia_vk_weaver_ops *
leia_vk_weaver_ops_v2(void)
{
	return &g_ops_v2;
}

#endif // DXR_LEIA_HAS_SR_V2
