// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief GPU unit test: the compose-under-capture pass's colour contract.
 *
 * Runs the plug-in's REAL compose shader (the SPIR-V compiled into
 * DisplayXR-LeiaSR.so, shaders/compose_under_bg.frag) on whatever Vulkan
 * device is present — Mesa lavapipe in CI, the laptop iGPU on a dev box —
 * through the same pipeline shape leia_display_processor_linux.c builds
 * (3 combined image samplers, the runtime's vk_create_sampler() sampler, the
 * shared push-constant block, an R8G8B8A8_UNORM target), and reads back what
 * the weaver would be handed.
 *
 * Invariants asserted (OpenXR 1.1 §10.6.2 / §10.6.4, see the shader header):
 *
 *   I1  An OPAQUE atlas pixel (alpha == 1) reaches the weaver BIT-IDENTICAL to
 *       the atlas, whatever desktop is composed under it. The capture may only
 *       touch pixels with alpha < 1 — exactly the passthrough (capture off)
 *       output for every opaque pixel.
 *   I2  A partially transparent pixel is blended per §10.6.2 with the atlas
 *       PREMULTIPLIED (no XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
 *       survives into the atlas) and in LINEAR light ("composition ... in
 *       linear color space"): out = enc(dec(C) + dec(desktop) * (1 - A)).
 *       Tolerance: 1 code value (8-bit quantisation of a float blend).
 *   I3  A fully transparent pixel (A = 0, C = 0) shows the desktop verbatim.
 *
 * Needs no display, no panel and no SR SDK: an offscreen render pass only.
 * Exits 77 (ctest SKIP_RETURN_CODE) when no Vulkan device can be created, so
 * a box without a Vulkan driver reports "skipped", never a false pass.
 * DXR_TEST_VK_DEVICE=<n> picks the physical device (default: the first).
 */

#include "leia_compose_push_linux.h"

#include "shaders/compose_under_bg.frag.h"
#include "shaders/fullscreen_tri.vert.h"

#include <vulkan/vulkan.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKIP 77
#define CHECK_VK(x)                                                                                                    \
	do {                                                                                                           \
		VkResult r_ = (x);                                                                                     \
		if (r_ != VK_SUCCESS) {                                                                                \
			fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, (int)r_);                           \
			exit(1);                                                                                       \
		}                                                                                                      \
	} while (0)

/*
 * Test image: W x H, two tiles side by side (tile_count = 2x1) like a stereo
 * atlas, so the per-tile desktop UV mapping is exercised too.
 */
#define W 256
#define H 4

struct gpu
{
	VkInstance inst;
	VkPhysicalDevice phys;
	VkDevice dev;
	VkQueue queue;
	uint32_t qfi;
	VkCommandPool pool;
	VkPhysicalDeviceMemoryProperties mem;
};

struct img
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	uint32_t w, h;
};

/* ---- reference colour maths (IEC 61966-2-1 piecewise sRGB) ------------- */

static double
srgb_to_linear(double c)
{
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double
linear_to_srgb(double c)
{
	if (c <= 0.0) {
		return 0.0;
	}
	if (c >= 1.0) {
		return 1.0;
	}
	return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

static uint8_t
to_u8(double c)
{
	double v = floor(c * 255.0 + 0.5);
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* ---- tiny Vulkan harness ------------------------------------------------ */

static bool
gpu_init(struct gpu *g)
{
	memset(g, 0, sizeof(*g));
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	if (vkCreateInstance(&ici, NULL, &g->inst) != VK_SUCCESS) {
		return false;
	}
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(g->inst, &n, NULL);
	if (n == 0) {
		return false;
	}
	VkPhysicalDevice devs[16];
	n = n > 16 ? 16 : n;
	vkEnumeratePhysicalDevices(g->inst, &n, devs);
	uint32_t pick = 0;
	const char *e = getenv("DXR_TEST_VK_DEVICE");
	if (e != NULL && (uint32_t)atoi(e) < n) {
		pick = (uint32_t)atoi(e);
	}
	g->phys = devs[pick];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(g->phys, &props);
	printf("device: %s\n", props.deviceName);

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &qn, NULL);
	VkQueueFamilyProperties qp[16];
	qn = qn > 16 ? 16 : qn;
	vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &qn, qp);
	g->qfi = UINT32_MAX;
	for (uint32_t i = 0; i < qn; i++) {
		if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			g->qfi = i;
			break;
		}
	}
	if (g->qfi == UINT32_MAX) {
		return false;
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	                               .queueFamilyIndex = g->qfi,
	                               .queueCount = 1,
	                               .pQueuePriorities = &prio};
	VkDeviceCreateInfo dci = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
	if (vkCreateDevice(g->phys, &dci, NULL, &g->dev) != VK_SUCCESS) {
		return false;
	}
	vkGetDeviceQueue(g->dev, g->qfi, 0, &g->queue);
	vkGetPhysicalDeviceMemoryProperties(g->phys, &g->mem);
	VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g->qfi};
	CHECK_VK(vkCreateCommandPool(g->dev, &pci, NULL, &g->pool));
	return true;
}

static uint32_t
mem_type(struct gpu *g, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < g->mem.memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (g->mem.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	fprintf(stderr, "no memory type\n");
	exit(1);
}

static void
buffer_create(struct gpu *g, VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem)
{
	VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                          .size = size,
	                          .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
	CHECK_VK(vkCreateBuffer(g->dev, &bci, NULL, buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(g->dev, *buf, &mr);
	VkMemoryAllocateInfo mai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = mr.size,
	    .memoryTypeIndex = mem_type(g, mr.memoryTypeBits,
	                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
	CHECK_VK(vkAllocateMemory(g->dev, &mai, NULL, mem));
	CHECK_VK(vkBindBufferMemory(g->dev, *buf, *mem, 0));
}

static struct img
image_create(struct gpu *g, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage)
{
	struct img im = {.w = w, .h = h};
	VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                         .imageType = VK_IMAGE_TYPE_2D,
	                         .format = fmt,
	                         .extent = {w, h, 1},
	                         .mipLevels = 1,
	                         .arrayLayers = 1,
	                         .samples = VK_SAMPLE_COUNT_1_BIT,
	                         .tiling = VK_IMAGE_TILING_OPTIMAL,
	                         .usage = usage,
	                         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	CHECK_VK(vkCreateImage(g->dev, &ici, NULL, &im.image));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(g->dev, im.image, &mr);
	VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                            .allocationSize = mr.size,
	                            .memoryTypeIndex =
	                                mem_type(g, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	CHECK_VK(vkAllocateMemory(g->dev, &mai, NULL, &im.memory));
	CHECK_VK(vkBindImageMemory(g->dev, im.image, im.memory, 0));
	VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	                             .image = im.image,
	                             .viewType = VK_IMAGE_VIEW_TYPE_2D,
	                             .format = fmt,
	                             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	CHECK_VK(vkCreateImageView(g->dev, &vci, NULL, &im.view));
	return im;
}

static VkCommandBuffer
cmd_begin(struct gpu *g)
{
	VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                  .commandPool = g->pool,
	                                  .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                  .commandBufferCount = 1};
	VkCommandBuffer cmd;
	CHECK_VK(vkAllocateCommandBuffers(g->dev, &ai, &cmd));
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	CHECK_VK(vkBeginCommandBuffer(cmd, &bi));
	return cmd;
}

static void
cmd_submit(struct gpu *g, VkCommandBuffer cmd)
{
	CHECK_VK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
	CHECK_VK(vkQueueSubmit(g->queue, 1, &si, VK_NULL_HANDLE));
	CHECK_VK(vkQueueWaitIdle(g->queue));
	vkFreeCommandBuffers(g->dev, g->pool, 1, &cmd);
}

static void
barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	                          .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
	                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	                          .oldLayout = from,
	                          .newLayout = to,
	                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                          .image = image,
	                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
	                     NULL, 1, &b);
}

//! Upload tightly packed 4-byte texels (in the image's own channel order).
static void
image_upload(struct gpu *g, struct img *im, const uint8_t *px)
{
	VkDeviceSize size = (VkDeviceSize)im->w * im->h * 4;
	VkBuffer buf;
	VkDeviceMemory mem;
	buffer_create(g, size, &buf, &mem);
	void *map;
	CHECK_VK(vkMapMemory(g->dev, mem, 0, size, 0, &map));
	memcpy(map, px, (size_t)size);
	vkUnmapMemory(g->dev, mem);
	VkCommandBuffer cmd = cmd_begin(g);
	barrier(cmd, im->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {im->w, im->h, 1}};
	vkCmdCopyBufferToImage(cmd, buf, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	barrier(cmd, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	cmd_submit(g, cmd);
	vkDestroyBuffer(g->dev, buf, NULL);
	vkFreeMemory(g->dev, mem, NULL);
}

static void
image_download(struct gpu *g, struct img *im, VkImageLayout layout, uint8_t *px)
{
	VkDeviceSize size = (VkDeviceSize)im->w * im->h * 4;
	VkBuffer buf;
	VkDeviceMemory mem;
	buffer_create(g, size, &buf, &mem);
	VkCommandBuffer cmd = cmd_begin(g);
	barrier(cmd, im->image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {im->w, im->h, 1}};
	vkCmdCopyImageToBuffer(cmd, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
	cmd_submit(g, cmd);
	void *map;
	CHECK_VK(vkMapMemory(g->dev, mem, 0, size, 0, &map));
	memcpy(px, map, (size_t)size);
	vkUnmapMemory(g->dev, mem);
	vkDestroyBuffer(g->dev, buf, NULL);
	vkFreeMemory(g->dev, mem, NULL);
}

/* ---- the compose pass, built exactly like compose_ensure_pipeline() ------ */

struct compose
{
	VkRenderPass rp;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet set;
};

static void
compose_init(struct gpu *g, struct compose *c)
{
	VkAttachmentDescription att = {.format = VK_FORMAT_R8G8B8A8_UNORM,
	                               .samples = VK_SAMPLE_COUNT_1_BIT,
	                               .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	                               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	                               .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	                               .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	                               .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                               .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	                            .colorAttachmentCount = 1,
	                            .pColorAttachments = &ref};
	VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	                              .attachmentCount = 1,
	                              .pAttachments = &att,
	                              .subpassCount = 1,
	                              .pSubpasses = &sub};
	CHECK_VK(vkCreateRenderPass(g->dev, &rpi, NULL, &c->rp));

	VkDescriptorSetLayoutBinding bs[3];
	for (uint32_t i = 0; i < 3; i++) {
		bs[i] = (VkDescriptorSetLayoutBinding){.binding = i,
		                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		                                       .descriptorCount = 1,
		                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
	}
	VkDescriptorSetLayoutCreateInfo dci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bs};
	CHECK_VK(vkCreateDescriptorSetLayout(g->dev, &dci, NULL, &c->dsl));
	VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	                           .offset = 0,
	                           .size = sizeof(struct leia_compose_push)};
	VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	                                  .setLayoutCount = 1,
	                                  .pSetLayouts = &c->dsl,
	                                  .pushConstantRangeCount = 1,
	                                  .pPushConstantRanges = &pcr};
	CHECK_VK(vkCreatePipelineLayout(g->dev, &pli, NULL, &c->layout));

	// Same sampler as the runtime's vk_create_sampler(CLAMP_TO_EDGE).
	VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	                           .magFilter = VK_FILTER_LINEAR,
	                           .minFilter = VK_FILTER_LINEAR,
	                           .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
	                           .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                           .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                           .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                           .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK};
	CHECK_VK(vkCreateSampler(g->dev, &sci, NULL, &c->sampler));

	VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 3};
	VkDescriptorPoolCreateInfo dpi = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
	CHECK_VK(vkCreateDescriptorPool(g->dev, &dpi, NULL, &c->pool));
	VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	                                    .descriptorPool = c->pool,
	                                    .descriptorSetCount = 1,
	                                    .pSetLayouts = &c->dsl};
	CHECK_VK(vkAllocateDescriptorSets(g->dev, &dsai, &c->set));

	VkShaderModule vs, fs;
	VkShaderModuleCreateInfo vsi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                .codeSize = sizeof(shaders_fullscreen_tri_vert),
	                                .pCode = shaders_fullscreen_tri_vert};
	VkShaderModuleCreateInfo fsi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                .codeSize = sizeof(shaders_compose_under_bg_frag),
	                                .pCode = shaders_compose_under_bg_frag};
	CHECK_VK(vkCreateShaderModule(g->dev, &vsi, NULL, &vs));
	CHECK_VK(vkCreateShaderModule(g->dev, &fsi, NULL, &fs));
	VkPipelineShaderStageCreateInfo st[2] = {
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	     .stage = VK_SHADER_STAGE_VERTEX_BIT,
	     .module = vs,
	     .pName = "main"},
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	     .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	     .module = fs,
	     .pName = "main"},
	};
	VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {.sType =
	                                                 VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	                                             .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkPipelineViewportStateCreateInfo vps = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
	VkPipelineRasterizationStateCreateInfo rs = {.sType =
	                                                 VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	                                             .polygonMode = VK_POLYGON_MODE_FILL,
	                                             .cullMode = VK_CULL_MODE_NONE,
	                                             .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	                                             .lineWidth = 1.0f};
	VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	                                           .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
	VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
	VkPipelineColorBlendStateCreateInfo cb = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
	VkGraphicsPipelineCreateInfo gpi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	                                    .stageCount = 2,
	                                    .pStages = st,
	                                    .pVertexInputState = &vi,
	                                    .pInputAssemblyState = &ia,
	                                    .pViewportState = &vps,
	                                    .pRasterizationState = &rs,
	                                    .pMultisampleState = &ms,
	                                    .pColorBlendState = &cb,
	                                    .pDynamicState = &ds,
	                                    .layout = c->layout,
	                                    .renderPass = c->rp};
	CHECK_VK(vkCreateGraphicsPipelines(g->dev, VK_NULL_HANDLE, 1, &gpi, NULL, &c->pipeline));
	vkDestroyShaderModule(g->dev, vs, NULL);
	vkDestroyShaderModule(g->dev, fs, NULL);
}

//! One compose draw of @p atlas over @p bg into @p fill (all W x H).
static void
compose_run(struct gpu *g, struct compose *c, struct img *atlas, struct img *bg, struct img *fill,
            const struct leia_compose_push *push)
{
	VkDescriptorImageInfo ii[3] = {
	    {.sampler = c->sampler, .imageView = atlas->view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	    {.sampler = c->sampler, .imageView = bg->view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	    {.sampler = c->sampler, .imageView = bg->view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	};
	VkWriteDescriptorSet w[3];
	for (uint32_t i = 0; i < 3; i++) {
		w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		                              .dstSet = c->set,
		                              .dstBinding = i,
		                              .descriptorCount = 1,
		                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		                              .pImageInfo = &ii[i]};
	}
	vkUpdateDescriptorSets(g->dev, 3, w, 0, NULL);

	VkFramebuffer fb;
	VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	                               .renderPass = c->rp,
	                               .attachmentCount = 1,
	                               .pAttachments = &fill->view,
	                               .width = fill->w,
	                               .height = fill->h,
	                               .layers = 1};
	CHECK_VK(vkCreateFramebuffer(g->dev, &fci, NULL, &fb));

	VkCommandBuffer cmd = cmd_begin(g);
	barrier(cmd, fill->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	                             .renderPass = c->rp,
	                             .framebuffer = fb,
	                             .renderArea = {{0, 0}, {fill->w, fill->h}}};
	vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->layout, 0, 1, &c->set, 0, NULL);
	vkCmdPushConstants(cmd, c->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(*push), push);
	VkViewport vp = {0, 0, (float)fill->w, (float)fill->h, 0, 1};
	VkRect2D sc = {{0, 0}, {fill->w, fill->h}};
	vkCmdSetViewport(cmd, 0, 1, &vp);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	cmd_submit(g, cmd);
	vkDestroyFramebuffer(g->dev, fb, NULL);
}

/* ---- the test ------------------------------------------------------------ */

static int g_failures = 0;

static void
fail(const char *what, uint32_t x, uint32_t y, const uint8_t in[4], const uint8_t bg[3], const uint8_t got[3],
     const uint8_t want[3])
{
	if (g_failures < 12 || getenv("DXR_TEST_VERBOSE") != NULL) {
		fprintf(stderr,
		        "FAIL %s at (%u,%u): atlas rgba=(%u,%u,%u,%u) desktop=(%u,%u,%u) -> got (%u,%u,%u) want "
		        "(%u,%u,%u)\n",
		        what, x, y, in[0], in[1], in[2], in[3], bg[0], bg[1], bg[2], got[0], got[1], got[2], want[0],
		        want[1], want[2]);
	}
	g_failures++;
}

int
main(void)
{
	struct gpu g;
	if (!gpu_init(&g)) {
		printf("SKIP: no usable Vulkan device\n");
		return SKIP;
	}
	struct compose c;
	compose_init(&g, &c);

	const VkImageUsageFlags samp = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	// The runtime hands the DP a B8G8R8A8_UNORM atlas (ENCODED, runtime#1484)
	// and the mutter capture arrives as B8G8R8A8_UNORM (SPA BGRx/BGRA).
	struct img atlas = image_create(&g, W, H, VK_FORMAT_B8G8R8A8_UNORM, samp);
	struct img bg = image_create(&g, W, H, VK_FORMAT_B8G8R8A8_UNORM, samp);
	struct img fill = image_create(&g, W, H, VK_FORMAT_R8G8B8A8_UNORM,
	                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	/*
	 * Atlas rows (RGBA, uploaded as BGRA):
	 *   row 0: opaque grey ramp, every code value 0..255     (I1)
	 *   row 1: opaque hue ramp, awkward channel values       (I1)
	 *   row 2: premultiplied partial alpha, a = 1..254       (I2)
	 *   row 3: fully transparent, then premultiplied mid-alpha colours (I3/I2)
	 */
	static uint8_t atlas_rgba[H][W][4];
	for (uint32_t x = 0; x < W; x++) {
		uint8_t v = (uint8_t)x;
		memcpy(atlas_rgba[0][x], (uint8_t[4]){v, v, v, 255}, 4);
		memcpy(atlas_rgba[1][x], (uint8_t[4]){(uint8_t)(x * 7), (uint8_t)(255 - x), (uint8_t)(x * 13 + 5), 255}, 4);
		// Straight colour s at alpha a, premultiplied in LINEAR light and
		// re-encoded: exactly what the runtime's compose writes (linear
		// blend, _SRGB attachment encode) and what an MSAA resolve of s
		// over transparent black produces.
		uint8_t a = (uint8_t)(1 + (x % 254));
		double s[3] = {0.9, 0.5, 0.2};
		uint8_t p[3];
		for (int k = 0; k < 3; k++) {
			p[k] = to_u8(linear_to_srgb(srgb_to_linear(s[k]) * (a / 255.0)));
		}
		memcpy(atlas_rgba[2][x], (uint8_t[4]){p[0], p[1], p[2], a}, 4);
		if (x < W / 4) {
			memcpy(atlas_rgba[3][x], (uint8_t[4]){0, 0, 0, 0}, 4);
		} else {
			uint8_t a3 = 128;
			double s3[3] = {(x % 3) / 2.0, ((x / 3) % 3) / 2.0, ((x / 9) % 3) / 2.0};
			for (int k = 0; k < 3; k++) {
				p[k] = to_u8(linear_to_srgb(srgb_to_linear(s3[k]) * (a3 / 255.0)));
			}
			memcpy(atlas_rgba[3][x], (uint8_t[4]){p[0], p[1], p[2], a3}, 4);
		}
	}
	static uint8_t atlas_bgra[H][W][4];
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			uint8_t *s = atlas_rgba[y][x];
			memcpy(atlas_bgra[y][x], (uint8_t[4]){s[2], s[1], s[0], s[3]}, 4);
		}
	}
	image_upload(&g, &atlas, &atlas_bgra[0][0][0]);

	// Desktops: black, white, and a saturated mid colour — an opaque pixel
	// must come out the same over every one of them.
	const uint8_t desktops[][3] = {{0, 0, 0}, {255, 255, 255}, {200, 30, 90}, {60, 170, 240}};
	static uint8_t out_rgba[H][W][4];
	static uint8_t bg_bgra[H][W][4];

	const size_t n_desk = sizeof(desktops) / sizeof(desktops[0]);
	// Both declared atlas encodings (runtime#1484): ENCODED (what the runtime
	// declares today) and LINEAR. The desktop is always sRGB-encoded.
	for (size_t run = 0; run < 2 * n_desk; run++) {
		const uint32_t linear = run >= n_desk ? 1u : 0u;
		const uint8_t *dc = desktops[run % n_desk];
		for (uint32_t y = 0; y < H; y++) {
			for (uint32_t x = 0; x < W; x++) {
				memcpy(bg_bgra[y][x], (uint8_t[4]){dc[2], dc[1], dc[0], 255}, 4);
			}
		}
		image_upload(&g, &bg, &bg_bgra[0][0][0]);

		struct leia_compose_push push;
		memset(&push, 0, sizeof(push));
		push.bg_uv_extent[0] = 1.0f;
		push.bg_uv_extent[1] = 1.0f;
		push.tile_count[0] = 2;
		push.tile_count[1] = 1;
		push.atlas_linear = linear;
		compose_run(&g, &c, &atlas, &bg, &fill, &push);
		image_download(&g, &fill, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &out_rgba[0][0][0]);

		for (uint32_t y = 0; y < H; y++) {
			for (uint32_t x = 0; x < W; x++) {
				const uint8_t *in = atlas_rgba[y][x];
				const uint8_t *got = out_rgba[y][x];
				uint8_t want[3];
				if (got[3] != 255) {
					fail("output alpha must be 1", x, y, in, dc, got, (uint8_t[3]){0, 0, 0});
				}
				if (in[3] == 255) {
					// I1: bit-identical.
					if (memcmp(got, in, 3) != 0) {
						fail("I1 opaque pixel not bit-identical", x, y, in, dc, got, in);
					}
					continue;
				}
				// I2/I3: premultiplied over, linear light.
				double A = in[3] / 255.0;
				for (int k = 0; k < 3; k++) {
					double c = linear ? in[k] / 255.0 : srgb_to_linear(in[k] / 255.0);
					double lin = c + srgb_to_linear(dc[k] / 255.0) * (1.0 - A);
					want[k] = to_u8(linear ? (lin > 1.0 ? 1.0 : lin) : linear_to_srgb(lin));
				}
				bool ok = true;
				for (int k = 0; k < 3; k++) {
					if (abs((int)got[k] - (int)want[k]) > 1) {
						ok = false;
					}
				}
				if (in[3] == 0 && !linear && memcmp(got, dc, 3) != 0) {
					ok = false; // I3: exact desktop
				}
				if (!ok) {
					fail(in[3] == 0 ? "I3 transparent pixel is not the desktop"
					                : "I2 partial alpha not premultiplied-over in linear light",
					     x, y, in, dc, got, in[3] == 0 ? dc : want);
				}
			}
		}
	}

	if (g_failures != 0) {
		fprintf(stderr, "%d pixel check(s) failed\n", g_failures);
		return 1;
	}
	printf("PASS: opaque bit-identical over %zu desktops x {ENCODED, LINEAR} atlas; partial alpha "
	       "premultiplied-over in linear light\n",
	       n_desk);
	return 0;
}
