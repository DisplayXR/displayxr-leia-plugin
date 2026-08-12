// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Per-ABI dispatch table for the SR Vulkan weaver.
 * @ingroup drv_leia
 *
 * **This file is compiled twice** (see `CMakeLists.txt`), once per vendored
 * `vkweaver.h` under `sr_vk_abi/`:
 *
 * | object library          | include dir          | `LEIA_VK_OPS_SYMBOL`         |
 * |-------------------------|----------------------|------------------------------|
 * | `leia_vk_abi_legacy`    | `sr_vk_abi/legacy`   | `leia_vk_weaver_ops_legacy`  |
 * | `leia_vk_abi_stamp`     | `sr_vk_abi/stamp`    | `leia_vk_weaver_ops_stamp`   |
 *
 * Each build sees a *different* definition of `SR::IVulkanWeaver1`. That is the
 * entire point: the vtable layouts differ, so the compiler must generate the
 * call sequences separately. Nothing SR-Vulkan-typed leaves this file, so the
 * two definitions never meet — no type crosses a translation-unit boundary, and
 * the exported symbol is renamed per build.
 *
 * `CreateVulkanWeaver` is invoked through a `GetProcAddress` pointer rather than
 * an import library. Its mangled name is identical across versions, so linking
 * both `SimulatedRealityVulkanBeta.lib` and `SimulatedRealityVulkan.lib` would
 * collide on one symbol. Resolving dynamically also removes the `/DELAYLOAD`
 * hack the old single-DLL path needed.
 */

#include "leia_vk_weaver.h"

#include "util/u_logging.h"

#include <sr/weaver/vkweaver.h>

#ifndef LEIA_VK_OPS_SYMBOL
#error "LEIA_VK_OPS_SYMBOL must be defined (see CMakeLists.txt) - this file is compiled once per weaver ABI"
#endif

namespace {

//! Signature of the resolved `CreateVulkanWeaver`. Typed against *this* build's
//! `SR::IVulkanWeaver1`, which is what makes the two builds differ.
//! `WeaverErrorCode` is a global enum from `WeaverTypes.h`, not `SR`-scoped.
using create_vulkan_weaver_fn = ::WeaverErrorCode (*)(SR::SRContext &context,
                                                      VkDevice device,
                                                      VkPhysicalDevice physicalDevice,
                                                      VkQueue graphicsQueue,
                                                      VkCommandPool commandPool,
                                                      HWND window,
                                                      SR::IVulkanWeaver1 **weaver);

inline SR::IVulkanWeaver1 *
as_weaver(void *raw)
{
	return static_cast<SR::IVulkanWeaver1 *>(raw);
}

void *
impl_create(void *create_fn,
            SR::SRContext *context,
            VkDevice device,
            VkPhysicalDevice physical_device,
            VkQueue graphics_queue,
            VkCommandPool command_pool,
            HWND window,
            int *out_err)
{
	if (create_fn == nullptr || context == nullptr) {
		return nullptr;
	}

	SR::IVulkanWeaver1 *weaver = nullptr;
	::WeaverErrorCode rc;

	// The SR SDK throws from construction paths on some failures rather than
	// returning an error code; never let that cross the plug-in's C ABI.
	try {
		rc = reinterpret_cast<create_vulkan_weaver_fn>(create_fn)(
		    *context, device, physical_device, graphics_queue, command_pool, window, &weaver);
	} catch (...) {
		if (out_err != nullptr) {
			*out_err = -1;
		}
		U_LOG_E("SR VK weaver: CreateVulkanWeaver threw");
		return nullptr;
	}

	if (out_err != nullptr) {
		*out_err = static_cast<int>(rc);
	}
	if (rc != ::WeaverErrorCode::WeaverSuccess) {
		return nullptr;
	}
	return weaver;
}

void
impl_destroy(void *raw)
{
	if (raw != nullptr) {
		as_weaver(raw)->destroy();
	}
}

void
impl_set_viewport(void *raw, RECT viewport)
{
	as_weaver(raw)->setViewport(viewport);
}

void
impl_set_scissor_rect(void *raw, RECT scissor)
{
	as_weaver(raw)->setScissorRect(scissor);
}

void
impl_set_command_buffer(void *raw, VkCommandBuffer cmd)
{
	as_weaver(raw)->setCommandBuffer(cmd);
}

void
impl_set_input_view_texture(void *raw, VkImageView left, VkImageView right, int width, int height, VkFormat format)
{
	// Always the *stereo* overload. Under the stamp ABI a mono overload also
	// exists; we never use it, and overload resolution picks stereo from the
	// two-image-view argument list.
	as_weaver(raw)->setInputViewTexture(left, right, width, height, format);
}

void
impl_set_output_framebuffer(void *raw, VkFramebuffer fb, int width, int height, VkFormat format)
{
	as_weaver(raw)->setOutputFrameBuffer(fb, width, height, format);
}

void
impl_set_latency(void *raw, uint64_t latency_us)
{
	as_weaver(raw)->setLatency(latency_us);
}

void
impl_weave(void *raw)
{
	as_weaver(raw)->weave();
}

bool
impl_get_predicted_eye_positions(void *raw, float left[3], float right[3])
{
	// SR throws std::runtime_error ~per frame from inside this call as routine
	// internal control flow. Swallow it here so the DP boundary stays clean.
	try {
		as_weaver(raw)->getPredictedEyePositions(left, right);
	} catch (...) {
		return false;
	}
	return true;
}

const struct leia_vk_weaver_ops g_ops = {
    impl_create,
    impl_destroy,
    impl_set_viewport,
    impl_set_scissor_rect,
    impl_set_command_buffer,
    impl_set_input_view_texture,
    impl_set_output_framebuffer,
    impl_set_latency,
    impl_weave,
    impl_get_predicted_eye_positions,
    // weave_submitted / enable_late_latching: NULL on v1. `weaveSubmitted` has
    // no C++ surface on IVulkanWeaver1, so the v1 path can never drive late
    // latching — and enabling it without the submit hook produces a latch that
    // reports success and never runs. Left explicitly NULL rather than stubbed
    // so the caller's null-check is the thing that decides, in one place.
    nullptr,
    nullptr,
};

} // namespace

const struct leia_vk_weaver_ops *
LEIA_VK_OPS_SYMBOL(void)
{
	return &g_ops;
}
