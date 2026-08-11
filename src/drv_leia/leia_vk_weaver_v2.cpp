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
 * Not used. The v2 weaver is created in `leia_sr.cpp` via `srCreateWeaverVK`,
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
	srWeaverSetViewportVK(as_weaver(raw), (int32_t)vp.left, (int32_t)vp.top, (int32_t)vp.right,
	                      (int32_t)vp.bottom);
}

void
v2_set_scissor_rect(void *raw, RECT rc)
{
	srWeaverSetScissorRectVK(as_weaver(raw), (int32_t)rc.left, (int32_t)rc.top, (int32_t)rc.right,
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
	const SrResult r = srWeaverSetCommandBufferVK(as_weaver(raw), cmd);
	if (!SR_SUCCEEDED(r)) {
		U_LOG_E("srWeaverSetCommandBufferVK failed: %s (%d)", leia_sr_v2_result_str(r), (int)r);
	}
}

void
v2_set_input_view_texture(void *raw, VkImageView left, VkImageView right, int width, int height, VkFormat format)
{
	// Separate left/right views, matching what this arm has always passed.
	// srWeaverSetInputTextureVK (singular) takes one side-by-side image and is
	// the wrong entry point here — the two differ by one character.
	srWeaverSetInputTexturesVK(as_weaver(raw), left, right, (int32_t)width, (int32_t)height, format);
}

void
v2_set_output_framebuffer(void *raw, VkFramebuffer fb, int width, int height, VkFormat format)
{
	// NOTE: passing VK_NULL_HANDLE here means "the command buffer already has a
	// render pass open", NOT "keep the previous framebuffer" — it clears the
	// binding. The caller retains a framebuffer by not calling this at all.
	// See the long comment at the leia_sr.cpp call site.
	srWeaverSetOutputFramebufferVK(as_weaver(raw), fb, (int32_t)width, (int32_t)height, format);
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
};

} // namespace

const struct leia_vk_weaver_ops *
leia_vk_weaver_ops_v2(void)
{
	return &g_ops_v2;
}

#endif // DXR_LEIA_HAS_SR_V2
