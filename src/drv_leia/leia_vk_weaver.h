// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  ABI-neutral wrapper over the SR Vulkan weaver.
 * @ingroup drv_leia
 *
 * ## Why this layer exists
 *
 * `SR::IVulkanWeaver1` is **not binary-stable across LeiaSR versions**. In
 * 1.37.1 (ST-5318, the "Vulkan goes official" change) the interface gained a
 * *mono* `setInputViewTexture(VkImageView, int, int, VkFormat)` overload,
 * declared **before** the existing stereo one. MSVC lays contiguous same-name
 * virtual overloads out in **reverse declaration order**, so the stereo slot is
 * preserved but every method declared after that group shifts down by one:
 * a binary compiled against the old header calls `setOutputFrameBuffer` and
 * lands on mono `setInputViewTexture`. Silent corruption, not a clean failure.
 *
 * `CreateVulkanWeaver`'s exported mangling is **identical** across every
 * version, so the export name cannot be used to detect which ABI a DLL
 * implements. The ABI must be selected from the environment instead — see
 * `leia_vk_weaver_select_backend()`.
 *
 * Only `IVulkanWeaver1`-declared methods are affected. `weave()`, `setLatency()`,
 * `getPredictedEyePositions()` and `destroy()` come from `IWeaverBase1` /
 * `IDestroyable`, which are **virtual** bases — they dispatch through a separate
 * subobject vtable that did not change. They are routed through this layer
 * anyway so callers never hold a version-typed pointer.
 *
 * ## How it is used
 *
 * `leia_vk_weaver_impl.cpp` is compiled **twice** (see CMakeLists), once against
 * each vendored `vkweaver.h` under `sr_vk_abi/`, producing two `ops` tables. No
 * SR Vulkan type ever crosses this header, so the two translation units never
 * share a definition of `SR::IVulkanWeaver1`.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <windows.h>

#include <cstdint>

namespace SR {
class SRContext;
} // namespace SR

//! Which weaver ABI a backend DLL implements.
enum leia_vk_abi
{
	//! Pre-ST-5318 vtable. Correct only against a pre-stamp core (SR <= 1.34.x).
	LEIA_VK_ABI_LEGACY = 0,
	//! ST-5318 vtable, consumes the subpixel pixel-stamp. Required for SR >= 1.36.
	LEIA_VK_ABI_STAMP = 1,
};

/*!
 * Per-ABI dispatch table. One instance per vendored header variant, emitted by
 * the two builds of `leia_vk_weaver_impl.cpp`.
 *
 * `raw` is an `SR::IVulkanWeaver1 *` typed by whichever header that translation
 * unit was compiled against. It is opaque here on purpose.
 */
struct leia_vk_weaver_ops
{
	//! Calls the resolved `CreateVulkanWeaver`. Returns nullptr on failure and
	//! writes the SR `WeaverErrorCode` to `out_err` when non-null.
	void *(*create)(void *create_fn,
	                SR::SRContext *context,
	                VkDevice device,
	                VkPhysicalDevice physical_device,
	                VkQueue graphics_queue,
	                VkCommandPool command_pool,
	                HWND window,
	                int *out_err);

	void (*destroy)(void *raw);

	void (*set_viewport)(void *raw, RECT viewport);
	void (*set_scissor_rect)(void *raw, RECT scissor);
	void (*set_command_buffer)(void *raw, VkCommandBuffer cmd);
	void (*set_input_view_texture)(
	    void *raw, VkImageView left, VkImageView right, int width, int height, VkFormat format);
	void (*set_output_framebuffer)(void *raw, VkFramebuffer fb, int width, int height, VkFormat format);

	void (*set_latency)(void *raw, uint64_t latency_us);
	void (*weave)(void *raw);

	//! Returns false if the SDK threw — it throws ~per frame as routine control
	//! flow, and that must never cross the plug-in's C ABI.
	bool (*get_predicted_eye_positions)(void *raw, float left[3], float right[3]);
};

//! Dispatch table compiled against `sr_vk_abi/legacy/sr/weaver/vkweaver.h`.
const struct leia_vk_weaver_ops *
leia_vk_weaver_ops_legacy(void);

//! Dispatch table compiled against `sr_vk_abi/stamp/sr/weaver/vkweaver.h`.
const struct leia_vk_weaver_ops *
leia_vk_weaver_ops_stamp(void);

/*!
 * The weaver DLL chosen for this machine, resolved once per process.
 */
struct leia_vk_weaver_backend
{
	enum leia_vk_abi abi;
	//! Loaded weaver module. Deliberately never freed — the SR weaver is not
	//! safe to unload once a context has touched it.
	HMODULE module;
	//! Resolved `CreateVulkanWeaver` (mangled name is version-independent).
	void *create_fn;
	//! Dispatch table matching `abi`.
	const struct leia_vk_weaver_ops *ops;
	//! Absolute path of the DLL actually loaded, for diagnostics.
	wchar_t dll_path[MAX_PATH];
};

/*!
 * Pick and load the correct weaver for the installed LeiaSR, idempotent and
 * cached. Selection is **capability-keyed, not version-parsed** — see the
 * implementation for the ordered rules.
 *
 * @return nullptr if no usable weaver could be resolved.
 */
const struct leia_vk_weaver_backend *
leia_vk_weaver_select_backend(void);
