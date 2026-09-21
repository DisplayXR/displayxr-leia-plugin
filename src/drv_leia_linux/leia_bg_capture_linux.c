// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*! @file  @brief Linux desktop background capture (mutter ScreenCast/PipeWire, window-excluded) — runtime#757. @ingroup drv_leia_linux */

#include "leia_bg_capture_linux.h"
#include "leia_mutter_capture_linux.h"
#include "util/u_logging.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/mman.h>

#ifdef DXR_LEIA_HAVE_PIPEWIRE

/*
 * Implementation notes:
 *   - Source: org.gnome.Mutter.ScreenCast RecordArea over the 3D panel's FULL
 *     logical rectangle, with our own windows excluded by the DisplayXR GNOME
 *     Shell extension (leia_mutter_capture_linux.h explains the three
 *     services). The PipeWire consumer and Vulkan import below are unchanged
 *     from the portal era; only where the node id comes from changed.
 *   - The capture is TRUSTED only while the exclusion is live. GNOME disables
 *     user extensions whenever the screen shield is up, which removes the
 *     effect: from that moment every frame contains our own window. The pump
 *     in poll() tracks the extension's bus name; losing it distrusts the
 *     capture (the DP falls back to silhouette intersection), regaining it
 *     re-registers the exclusion and trusts only frames published after that.
 *   - Our format offer carries no modifier, so mutter delivers MemFd/MemPtr
 *     frames (the shm path). The dma-buf import is kept for a producer that
 *     negotiates it anyway; the rear-depth-budget preview is produced from
 *     the shm frames only (see preview_* below).
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

#include <dbus/dbus.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/param/format.h>
#include <spa/param/props.h>
#include <spa/utils/result.h>
#include <spa/buffer/meta.h>

// DRM format-modifier sentinels. Defined locally (rather than pulling in
// libdrm-dev) so the module needs only libpipewire-0.3 + dbus-1. These two
// values are ABI-stable in drm_fourcc.h.
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

#define DXR_MAX_BUFFERS 16

/*! One PipeWire buffer slot, cached as a Vulkan-sampleable image. Two kinds:
 *  dma-buf (zero-copy import) or shm (MemFd/MemPtr staged through a host-visible
 *  buffer — what Mutter's Xorg screencast actually delivers, #109/#110). */
struct dxr_bg_buffer {
	bool imported;             //!< slot holds a live VkImage
	VkImage image;             //!< backing image (imported dma-buf, or device-local for shm)
	VkDeviceMemory memory;     //!< image memory
	VkImageView view;          //!< SHADER_READ_ONLY view

	// shm (MemFd/MemPtr) staging path.
	bool is_shm;               //!< slot is CPU-staged, not a dma-buf import
	void *map;                 //!< our mmap of the MemFd (NULL for MemPtr)
	size_t map_size;           //!< mmap length (0 when map == NULL)
	int memfd;                 //!< client-allocated memfd (-1 when producer-owned)
	VkBuffer staging;          //!< host-visible upload source
	VkDeviceMemory staging_mem; //!< staging allocation (persistently mapped)
	void *staging_ptr;         //!< persistent map of staging_mem
	_Atomic bool dirty;        //!< staging holds a frame the GPU hasn't copied yet
	bool image_initialized;    //!< image has left UNDEFINED (layout tracking)
};

//! One stage-1 panel preview (BGRA8, tightly packed) and the frame it came from.
struct panel_preview_buf
{
	uint8_t *px;
	uint32_t w, h;
	uint32_t seq; //!< publish seq of the source frame (0 = empty)
};

/*! Linux desktop background-capture context. */
struct leia_bg_capture_linux {
	struct vk_bundle *vk;      //!< borrowed; not owned

	// Loaded dynamically (not in the dispatch table).
	PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties; //!< dma-buf mem-type query

	// D-Bus. ONE private session-bus connection carries everything that has a
	// lifetime: the capture exclusion (the extension drops it when this
	// connection goes away) and the ScreenCast session (mutter tears it down
	// likewise). Touched from the render thread only (create/poll/destroy).
	DBusConnection *dbus;      //!< private session-bus connection
	char *session_path;        //!< mutter ScreenCast session object path
	char *stream_path;         //!< mutter ScreenCast stream object path

	// The panel as mutter lays it out (LOGICAL rect recorded, DEVICE size is
	// the space win_x/win_y/win_w/win_h arrive in). See leia_mutter_panel.
	struct leia_mutter_panel panel;

	// Trust (render thread only). The capture may be SAMPLED only while our
	// windows are excluded; see the implementation notes above.
	bool exclusion_live;       //!< the extension holds our registration right now
	uint32_t untrusted_through; //!< frames with seq <= this may contain our own window
	_Atomic uint32_t pool_buffers; //!< PipeWire buffers currently in the pool (frames that can be in flight)
	bool session_closed;       //!< mutter closed the session (user pressed "stop sharing", ...)
	bool wants_restart;        //!< the panel moved/rescaled: the recorded area is wrong
	bool logged_distrust;      //!< one WARN per distrust episode
	bool poll_ok;              //!< verdict of the last poll()
	struct leia_panel_identity panel_id; //!< EDID identity, read once at create (sysfs)
	bool have_panel_id;
	//! In-flight ASYNC D-Bus calls issued from the render thread (0 = none).
	//! Their replies are matched by serial in mutter_pump — the render thread
	//! never blocks on D-Bus after create().
	dbus_uint32_t exclude_serial;
	dbus_uint32_t layout_serial;
	bool stall_logged;         //!< one-shot render-thread stall WARN (see STALL_WARN_NS)

	// PipeWire.
	uint32_t node_id;          //!< stream node id from PipeWireStreamAdded
	struct pw_thread_loop *loop; //!< capture thread loop
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_hook stream_listener;

	// Negotiated video format.
	bool have_format;          //!< param_changed produced a valid format
	uint32_t width;            //!< negotiated frame width
	uint32_t height;           //!< negotiated frame height
	uint64_t modifier;         //!< negotiated DRM modifier
	VkFormat vk_format;        //!< Vulkan format matching the SPA format
	bool fmt_rgb_order;        //!< SPA format is RGBA/RGBx (preview swaps to BGRA)

	// Buffer cache (indexed by pw buffer id).
	struct dxr_bg_buffer buffers[DXR_MAX_BUFFERS];

	// Latest ready buffer index; written by pw thread, read by render thread.
	_Atomic int current_buffer; //!< -1 == none
	//! Publish sequence of the frame each slot holds (pw thread writes, render
	//! thread reads) — what the trust gate compares against untrusted_through.
	_Atomic uint32_t slot_seq[DXR_MAX_BUFFERS];
	VkImageView current_view;   //!< last view poll() selected (render thread only)

	// Frame-flow diagnostics (#109 follow-up). WARN-level, heavily throttled —
	// the whole point is that INFO is dropped from app logs in the field.
	_Atomic uint32_t dbg_frames;  //!< frames copied/published by on_process
	_Atomic uint32_t dbg_uploads; //!< shm uploads recorded by poll()
	bool dbg_first_frame_logged;
	bool dbg_first_upload_logged;
	bool dbg_no_frame_logged;

	// ---- rear depth budget background preview (runtime#224 contract) ------
	// Two stages, so the expensive part never runs on the render thread:
	//  1. pw thread, on_process: a 1/2^PANEL_PREVIEW_SHIFT box filter of the
	//     WHOLE captured panel, read from the PipeWire buffer itself (cached
	//     memory — NEVER the Vulkan staging buffer, which is host-visible
	//     uncached memory and ~500x slower to read: the 2.5 s/frame stall of
	//     the first DS1 run), throttled to <= 15 Hz.
	//  2. render thread, get_preview: crop the window's region out of it and
	//     box-reduce further until both sides are <= 512 into preview_out.
	//     Borrowed by the runtime until the next process_atlas.
	// The two threads hand the stage-1 result over with THREE buffers: the pw
	// thread fills pp_back with no lock held, then swaps it into pp_mid; the
	// render thread swaps pp_mid into pp_front. preview_lock guards ONLY those
	// pointer swaps, and the render thread only ever TRY-locks it — it never
	// waits on the PipeWire thread; on contention it serves the last preview.
	pthread_mutex_t preview_lock;
	bool preview_lock_inited;
	struct panel_preview_buf pp_back;  //!< pw thread only
	struct panel_preview_buf pp_mid;   //!< shared — under preview_lock
	struct panel_preview_buf pp_front; //!< render thread only
	uint64_t panel_preview_last_ns;    //!< pw-thread throttle
	uint8_t *preview_out;            //!< render-thread output (borrowed by the runtime)
	size_t preview_out_cap;
	uint32_t preview_out_w, preview_out_h;
	uint32_t preview_out_gen;        //!< bumps whenever preview_out is rebuilt
	uint32_t preview_src_seq;        //!< pp_front.seq preview_out was built from
	float preview_cu0, preview_cv0, preview_cu1, preview_cv1;
	bool preview_dmabuf_logged;
	// The window rect of the last poll (DEVICE px, panel-relative).
	int32_t last_win_x, last_win_y;
	uint32_t last_win_w, last_win_h;
	int32_t preview_win_x, preview_win_y;
	uint32_t preview_win_w, preview_win_h;
};

static bool s_pw_inited = false; //!< pw_init() guard (only global mutable state)

// ---------------------------------------------------------------------------
// Vulkan helpers
// ---------------------------------------------------------------------------

static VkFormat
spa_to_vk_format(uint32_t spa_format)
{
	switch (spa_format) {
	case SPA_VIDEO_FORMAT_BGRA: return VK_FORMAT_B8G8R8A8_UNORM;
	case SPA_VIDEO_FORMAT_RGBA: return VK_FORMAT_R8G8B8A8_UNORM;
	case SPA_VIDEO_FORMAT_BGRx: return VK_FORMAT_B8G8R8A8_UNORM;
	case SPA_VIDEO_FORMAT_RGBx: return VK_FORMAT_R8G8B8A8_UNORM;
	default: return VK_FORMAT_UNDEFINED;
	}
}

/*!
 * Pick a memory type whose bit is set in BOTH the image's requirement bits and
 * the dma-buf's fd-property bits. No host-visibility requirement.
 * Returns -1 if none matches.
 */
static int
pick_memory_type(struct leia_bg_capture_linux *c, uint32_t image_bits, uint32_t fd_bits)
{
	VkPhysicalDeviceMemoryProperties props;
	c->vk->vkGetPhysicalDeviceMemoryProperties(c->vk->physical_device, &props);

	uint32_t candidates = image_bits & fd_bits;
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((candidates & (1u << i)) != 0u) {
			return (int)i;
		}
	}
	return -1;
}

/*!
 * Import a single dma-buf plane into a sampleable VkImage and cache it in slot
 * @p slot. On failure logs a reason, leaves the slot un-imported, and returns
 * false without leaking the dup'd fd.
 */
static bool
import_dmabuf(struct leia_bg_capture_linux *c, uint32_t slot, int spa_fd,
              int64_t chunk_offset, int32_t chunk_stride)
{
	struct vk_bundle *vk = c->vk;
	VkResult res;

	if (slot >= DXR_MAX_BUFFERS) {
		U_LOG_W("leia_bg_capture_linux: buffer slot %u out of range", slot);
		return false;
	}
	if (c->buffers[slot].imported) {
		// Already imported (stable for the buffer's lifetime).
		return true;
	}
	if (c->vk_format == VK_FORMAT_UNDEFINED || !c->have_format) {
		U_LOG_W("leia_bg_capture_linux: import before format negotiated");
		return false;
	}

	// Keep PipeWire's copy of the fd — vkAllocateMemory takes ownership on
	// success, so we import a dup and PipeWire keeps recycling the original.
	int dupfd = dup(spa_fd);
	if (dupfd < 0) {
		U_LOG_W("leia_bg_capture_linux: dup(dmabuf fd) failed");
		return false;
	}

	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;

	// 1) External-memory image create info (dma-buf).
	VkExternalMemoryImageCreateInfo ext = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .pNext = NULL,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};

	// 2) Explicit DRM-format-modifier layout (single plane).
	VkSubresourceLayout plane = {
	    .offset = (VkDeviceSize)chunk_offset,
	    .size = 0,
	    .rowPitch = (VkDeviceSize)chunk_stride,
	    .arrayPitch = 0,
	    .depthPitch = 0,
	};
	VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
	    .pNext = &ext,
	    .drmFormatModifier = c->modifier,
	    .drmFormatModifierPlaneCount = 1,
	    .pPlaneLayouts = &plane,
	};

	// 3) Image create.
	VkImageCreateInfo ici = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &mod,
	    .flags = 0,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = c->vk_format,
	    .extent = {c->width, c->height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	    .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .queueFamilyIndexCount = 0,
	    .pQueueFamilyIndices = NULL,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	res = vk->vkCreateImage(vk->device, &ici, NULL, &image);
	if (res != VK_SUCCESS) {
		U_LOG_W("leia_bg_capture_linux: vkCreateImage(dmabuf) failed (%d)", res);
		close(dupfd);
		return false;
	}

	// 4) Memory requirements.
	VkImageMemoryRequirementsInfo2 ri = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
	    .pNext = NULL,
	    .image = image,
	};
	VkMemoryRequirements2 mr = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
	    .pNext = NULL,
	};
	vk->vkGetImageMemoryRequirements2(vk->device, &ri, &mr);

	// 5) dma-buf importable memory-type bits.
	VkMemoryFdPropertiesKHR fdp = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
	    .pNext = NULL,
	    .memoryTypeBits = 0,
	};
	res = c->getMemoryFdProperties(vk->device,
	                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	                               dupfd, &fdp);
	if (res != VK_SUCCESS) {
		U_LOG_W("leia_bg_capture_linux: vkGetMemoryFdPropertiesKHR failed (%d)", res);
		vk->vkDestroyImage(vk->device, image, NULL);
		close(dupfd);
		return false;
	}

	int type_index = pick_memory_type(c, mr.memoryRequirements.memoryTypeBits,
	                                  fdp.memoryTypeBits);
	if (type_index < 0) {
		U_LOG_W("leia_bg_capture_linux: no importable memory type (img=0x%x fd=0x%x)",
		        mr.memoryRequirements.memoryTypeBits, fdp.memoryTypeBits);
		vk->vkDestroyImage(vk->device, image, NULL);
		close(dupfd);
		return false;
	}

	// 6/7) Import + dedicated allocation. vkAllocateMemory owns dupfd on success.
	VkMemoryDedicatedAllocateInfo ded = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
	    .pNext = NULL,
	    .image = image,
	    .buffer = VK_NULL_HANDLE,
	};
	VkImportMemoryFdInfoKHR imp = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
	    .pNext = &ded,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	    .fd = dupfd,
	};
	VkMemoryAllocateInfo mai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &imp,
	    .allocationSize = mr.memoryRequirements.size,
	    .memoryTypeIndex = (uint32_t)type_index,
	};
	res = vk->vkAllocateMemory(vk->device, &mai, NULL, &memory);
	if (res != VK_SUCCESS) {
		U_LOG_W("leia_bg_capture_linux: vkAllocateMemory(import) failed (%d)", res);
		// Import failed — the fd was NOT consumed; we own it.
		vk->vkDestroyImage(vk->device, image, NULL);
		close(dupfd);
		return false;
	}
	// From here dupfd is owned by the driver; do not close it.

	// 8) Bind + view.
	res = vk->vkBindImageMemory(vk->device, image, memory, 0);
	if (res != VK_SUCCESS) {
		U_LOG_W("leia_bg_capture_linux: vkBindImageMemory failed (%d)", res);
		vk->vkFreeMemory(vk->device, memory, NULL);
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}

	VkImageViewCreateInfo vci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .pNext = NULL,
	    .flags = 0,
	    .image = image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = c->vk_format,
	    .components =
	        {
	            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
	            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
	            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
	            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
	        },
	    .subresourceRange =
	        {
	            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel = 0,
	            .levelCount = 1,
	            .baseArrayLayer = 0,
	            .layerCount = 1,
	        },
	};
	res = vk->vkCreateImageView(vk->device, &vci, NULL, &view);
	if (res != VK_SUCCESS) {
		U_LOG_W("leia_bg_capture_linux: vkCreateImageView failed (%d)", res);
		vk->vkFreeMemory(vk->device, memory, NULL);
		vk->vkDestroyImage(vk->device, image, NULL);
		return false;
	}

	// 9) Cache the slot.
	c->buffers[slot].image = image;
	c->buffers[slot].memory = memory;
	c->buffers[slot].view = view;
	c->buffers[slot].imported = true;

	U_LOG_I("leia_bg_capture_linux: imported dma-buf slot %u (%ux%u mod=0x%llx)",
	        slot, c->width, c->height, (unsigned long long)c->modifier);
	return true;
}

/*!
 * Create a CPU-staged slot for a MemFd/MemPtr PipeWire buffer: a host-visible
 * staging VkBuffer (persistently mapped; on_process memcpy's frames into it)
 * plus a device-local sampled image poll() uploads into on the compose command
 * buffer. This is the path Mutter's Xorg screencast actually takes — it
 * declines our dma-buf format offer and delivers MemFd frames (#109/#110).
 */
static bool
create_shm_slot(struct leia_bg_capture_linux *c, uint32_t slot, struct spa_data *d)
{
	struct vk_bundle *vk = c->vk;
	VkResult res;

	if (c->vk_format == VK_FORMAT_UNDEFINED || !c->have_format) {
		U_LOG_W("leia_bg_capture_linux: shm slot before format negotiated");
		return false;
	}

	// Three shm sub-cases:
	//   MemFd with fd      → mmap it ourselves (producer-allocated).
	//   MemPtr with data   → producer-provided pointer.
	//   no data, no fd     → CLIENT-ALLOCATES mode (PW_STREAM_FLAG_ALLOC_BUFFERS,
	//                        what this stack negotiates, #109): WE must provide
	//                        SHAREABLE storage. A raw pointer can't cross the
	//                        process boundary, so allocate a memfd, map it, and
	//                        hand PipeWire the fd — it marshals the fd to the
	//                        producer, which writes frames into it.
	void *map = NULL;
	size_t map_size = 0;
	int memfd = -1;
	bool client_alloc = false;
	if (d->data == NULL) {
		if (d->fd >= 0) {
			map_size = (size_t)d->maxsize + (size_t)d->mapoffset;
			map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, (int)d->fd, 0);
			if (map == MAP_FAILED) {
				U_LOG_W("leia_bg_capture_linux: mmap(MemFd) failed");
				return false;
			}
		} else {
			client_alloc = true;
			map_size = (size_t)c->width * c->height * 4;
			memfd = memfd_create("dxr-bgcap", MFD_CLOEXEC);
			if (memfd < 0 || ftruncate(memfd, (off_t)map_size) != 0) {
				U_LOG_W("leia_bg_capture_linux: memfd_create/ftruncate failed");
				if (memfd >= 0) {
					close(memfd);
				}
				return false;
			}
			map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
			if (map == MAP_FAILED) {
				U_LOG_W("leia_bg_capture_linux: mmap(client memfd) failed");
				close(memfd);
				return false;
			}
		}
	}

	const VkDeviceSize staging_size = (VkDeviceSize)c->width * c->height * 4;

	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	void *staging_ptr = NULL;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;

	VkBufferCreateInfo bci = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = staging_size,
	    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	res = vk->vkCreateBuffer(vk->device, &bci, NULL, &staging);
	if (res != VK_SUCCESS) {
		goto fail;
	}

	VkMemoryRequirements breq;
	vk->vkGetBufferMemoryRequirements(vk->device, staging, &breq);
	// Prefer a CACHED coherent type. The first merely HOST_VISIBLE|COHERENT
	// type is typically write-combined: on the DS1 box's Intel PTL iGPU a CPU
	// write of a 4K frame into it costs 11.7 ms vs 4.4 ms cached, and a CPU
	// READ costs 3.6 s vs 61 ms — which is how the first preview build (it
	// read this buffer) stalled every frame for ~2.5 s. Nothing reads it on
	// the CPU any more, but a cached type keeps the per-frame copy cheap and a
	// stray read survivable. Falls back to any HOST_VISIBLE|COHERENT type.
	uint32_t host_type = UINT32_MAX;
	const VkMemoryPropertyFlags want =
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (int pass = 0; pass < 2 && host_type == UINT32_MAX; pass++) {
		const VkMemoryPropertyFlags w = pass == 0 ? (want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : want;
		for (uint32_t i = 0; i < vk->device_memory_props.memoryTypeCount; i++) {
			if ((breq.memoryTypeBits & (1u << i)) &&
			    (vk->device_memory_props.memoryTypes[i].propertyFlags & w) == w) {
				host_type = i;
				break;
			}
		}
	}
	if (host_type == UINT32_MAX) {
		goto fail;
	}
	VkMemoryAllocateInfo bai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = breq.size,
	    .memoryTypeIndex = host_type,
	};
	res = vk->vkAllocateMemory(vk->device, &bai, NULL, &staging_mem);
	if (res != VK_SUCCESS) {
		goto fail;
	}
	if (vk->vkBindBufferMemory(vk->device, staging, staging_mem, 0) != VK_SUCCESS ||
	    vk->vkMapMemory(vk->device, staging_mem, 0, VK_WHOLE_SIZE, 0, &staging_ptr) != VK_SUCCESS) {
		goto fail;
	}

	VkImageCreateInfo ici = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = c->vk_format,
	    .extent = {c->width, c->height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	res = vk->vkCreateImage(vk->device, &ici, NULL, &image);
	if (res != VK_SUCCESS) {
		goto fail;
	}
	VkMemoryRequirements ireq;
	vk->vkGetImageMemoryRequirements(vk->device, image, &ireq);
	uint32_t dev_type = UINT32_MAX;
	for (uint32_t i = 0; i < vk->device_memory_props.memoryTypeCount; i++) {
		if ((ireq.memoryTypeBits & (1u << i)) &&
		    (vk->device_memory_props.memoryTypes[i].propertyFlags &
		     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			dev_type = i;
			break;
		}
	}
	if (dev_type == UINT32_MAX) {
		dev_type = __builtin_ctz(ireq.memoryTypeBits); // any supported type
	}
	VkMemoryAllocateInfo iai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = ireq.size,
	    .memoryTypeIndex = dev_type,
	};
	res = vk->vkAllocateMemory(vk->device, &iai, NULL, &memory);
	if (res != VK_SUCCESS || vk->vkBindImageMemory(vk->device, image, memory, 0) != VK_SUCCESS) {
		goto fail;
	}

	VkImageViewCreateInfo vci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = c->vk_format,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	res = vk->vkCreateImageView(vk->device, &vci, NULL, &view);
	if (res != VK_SUCCESS) {
		goto fail;
	}

	c->buffers[slot].image = image;
	c->buffers[slot].memory = memory;
	c->buffers[slot].view = view;
	c->buffers[slot].is_shm = true;
	c->buffers[slot].map = map;
	c->buffers[slot].map_size = map_size;
	c->buffers[slot].staging = staging;
	c->buffers[slot].staging_mem = staging_mem;
	c->buffers[slot].staging_ptr = staging_ptr;
	atomic_store(&c->buffers[slot].dirty, false);
	c->buffers[slot].image_initialized = false;
	c->buffers[slot].imported = true;

	c->buffers[slot].memfd = memfd;
	if (client_alloc) {
		// Fill the spa_data so PipeWire marshals our memfd to the producer.
		d->type = SPA_DATA_MemFd;
		d->fd = memfd;
		d->flags = SPA_DATA_FLAG_READWRITE;
		d->mapoffset = 0;
		d->maxsize = (uint32_t)map_size;
		d->data = map;
	}

	// WARN so it survives field logs: proves the shm path activated.
	U_LOG_W("leia_bg_capture_linux: shm slot %u ready (%ux%u, %s, staging memory type %u %s)", slot, c->width,
	        c->height, client_alloc ? "client memfd" : (map != NULL ? "MemFd mmap" : "MemPtr"), host_type,
	        (vk->device_memory_props.memoryTypes[host_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
	            ? "cached"
	            : "UNCACHED (write-combined)");
	return true;

fail:
	U_LOG_W("leia_bg_capture_linux: shm slot %u setup failed", slot);
	if (view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, view, NULL);
	}
	if (image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, image, NULL);
	}
	if (memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, memory, NULL);
	}
	if (staging != VK_NULL_HANDLE) {
		vk->vkDestroyBuffer(vk->device, staging, NULL);
	}
	if (staging_mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, staging_mem, NULL);
	}
	if (map != NULL) {
		munmap(map, map_size);
	}
	if (memfd >= 0) {
		close(memfd);
	}
	return false;
}

static void
destroy_buffer_slot(struct leia_bg_capture_linux *c, uint32_t slot)
{
	if (slot >= DXR_MAX_BUFFERS || !c->buffers[slot].imported) {
		return;
	}
	struct vk_bundle *vk = c->vk;
	if (c->buffers[slot].view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, c->buffers[slot].view, NULL);
	}
	if (c->buffers[slot].image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, c->buffers[slot].image, NULL);
	}
	if (c->buffers[slot].memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, c->buffers[slot].memory, NULL);
	}
	if (c->buffers[slot].staging != VK_NULL_HANDLE) {
		vk->vkDestroyBuffer(vk->device, c->buffers[slot].staging, NULL);
	}
	if (c->buffers[slot].staging_mem != VK_NULL_HANDLE) {
		// Persistent map dies with the allocation.
		vk->vkFreeMemory(vk->device, c->buffers[slot].staging_mem, NULL);
	}
	if (c->buffers[slot].map != NULL) {
		munmap(c->buffers[slot].map, c->buffers[slot].map_size);
	}
	if (c->buffers[slot].memfd > 0) {
		close(c->buffers[slot].memfd);
	}
	memset(&c->buffers[slot], 0, sizeof(c->buffers[slot]));
}

// ---------------------------------------------------------------------------
// mutter: exclusion + ScreenCast session + the per-frame trust pump
// ---------------------------------------------------------------------------

#define EXT_OWNER_RULE                                                                                                 \
	"type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"      \
	"arg0='" LEIA_MUTTER_EXT_BUS "'"
#define MONITORS_CHANGED_RULE "type='signal',interface='org.gnome.Mutter.DisplayConfig',member='MonitorsChanged'"

/*!
 * Say ONCE per process, clearly, why there is no captured background — this is
 * the line a user reads to learn that installing the extension is what makes
 * transparency correct on GNOME.
 */
static void
log_no_capture_once(enum leia_mutter_exclude_status st)
{
	static bool logged = false;
	if (logged) {
		return;
	}
	logged = true;
	if (st == LEIA_MUTTER_EXCLUDE_ABSENT) {
		U_LOG_W("leia_bg_capture_linux: desktop capture OFF — the DisplayXR GNOME Shell extension "
		        "(window-geometry@displayxr.org, org.displayxr.WindowGeometry) is not running, so a capture "
		        "would contain our own window and re-weave it. Transparency falls back to silhouette "
		        "intersection (edges shrink by the disparity). INSTALL AND ENABLE THE EXTENSION (then log "
		        "out and back in) to get the real desktop composed under the 3D fringe.");
	} else if (st == LEIA_MUTTER_EXCLUDE_OUTDATED) {
		U_LOG_W("leia_bg_capture_linux: desktop capture OFF — the installed window-geometry@displayxr.org "
		        "extension predates capture exclusion (org.displayxr.CaptureExclusion1, extension version 2). "
		        "Transparency falls back to silhouette intersection. UPDATE THE EXTENSION (then log out and "
		        "back in) to get the real desktop composed under the 3D fringe.");
	} else {
		U_LOG_W("leia_bg_capture_linux: desktop capture OFF — could not exclude our window from capture "
		        "(%s); transparency falls back to silhouette intersection",
		        leia_mutter_exclude_status_str(st));
	}
}

static bool
mutter_handshake(struct leia_bg_capture_linux *c, uint32_t panel_px_w, uint32_t panel_px_h)
{
	DBusError err;
	dbus_error_init(&err);

	c->dbus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (dbus_error_is_set(&err) || c->dbus == NULL) {
		U_LOG_W("leia_bg_capture_linux: no session bus: %s", dbus_error_is_set(&err) ? err.message : "(null)");
		dbus_error_free(&err);
		return false;
	}
	// We manage the connection's lifetime; don't let dbus exit the process.
	dbus_connection_set_exit_on_disconnect(c->dbus, FALSE);

	// Watch the extension's name BEFORE registering, so a disable that races
	// the registration is still seen by the pump.
	dbus_bus_add_match(c->dbus, EXT_OWNER_RULE, &err);
	dbus_error_free(&err);
	dbus_bus_add_match(c->dbus, MONITORS_CHANGED_RULE, &err);
	dbus_error_free(&err);

	// 1. Exclusion FIRST — without it every frame would contain our window, so
	//    there is no point in even starting the stream. Registering before
	//    Start also means no frame is ever recorded un-excluded.
	uint32_t windows = 0;
	const enum leia_mutter_exclude_status st = leia_mutter_capture_exclude(c->dbus, 1000, &windows);
	if (st != LEIA_MUTTER_EXCLUDE_OK) {
		log_no_capture_once(st);
		return false;
	}
	c->exclusion_live = true;
	c->untrusted_through = 0;

	// 2. The panel's LOGICAL rectangle, from mutter's own layout.
	c->have_panel_id = leia_mutter_panel_identity_from_sysfs(&c->panel_id);
	if (!leia_mutter_find_panel(c->dbus, c->have_panel_id ? &c->panel_id : NULL, panel_px_w, panel_px_h,
	                            &c->panel)) {
		return false;
	}

	// 3. Record exactly that rectangle.
	if (!leia_mutter_screencast_start(c->dbus, &c->panel, 3000, &c->session_path, &c->stream_path, &c->node_id)) {
		return false;
	}
	U_LOG_W("leia_bg_capture_linux: mutter RecordArea(%d,%d %ux%u LOGICAL) on panel %s, cursor hidden, our "
	        "windows excluded (%u mapped now) -> PipeWire node %u; expecting a %ux%u DEVICE-pixel stream",
	        c->panel.logical_x, c->panel.logical_y, c->panel.logical_w, c->panel.logical_h, c->panel.connector,
	        windows, c->node_id, c->panel.device_w, c->panel.device_h);
	return true;
}

/*!
 * Frames to distrust after a re-registration. PUBLISH order is not RECORD
 * order: a frame mutter recorded before the effect was back can still be in
 * the buffer pool and arrive later (seen in the nested-shell test). Mutter can
 * only record into a free pool buffer, so at most pool-size frames are in
 * flight — distrust that many more, plus one.
 */
static uint32_t
retrust_after(struct leia_bg_capture_linux *c)
{
	const uint32_t in_flight = atomic_load(&c->pool_buffers);
	return atomic_load(&c->dbg_frames) + (in_flight > 0 ? in_flight : 4u) + 1u;
}

/*!
 * Non-blocking per-frame pump of our private connection (render thread): track
 * the extension coming and going, mutter closing the session, and the layout
 * changing under the recorded area. NOTHING here waits: calls it needs to make
 * (re-register the exclusion, re-read the layout) are sent asynchronously and
 * their replies matched by serial on a later pump. Rare events log one WARN.
 */
static void
mutter_pump(struct leia_bg_capture_linux *c)
{
	if (c->dbus == NULL) {
		return;
	}
	if (!dbus_connection_read_write(c->dbus, 0)) {
		if (!c->session_closed) {
			c->session_closed = true;
			U_LOG_W("leia_bg_capture_linux: session bus connection lost — capture distrusted");
		}
		return;
	}
	DBusMessage *msg;
	while ((msg = dbus_connection_pop_message(c->dbus)) != NULL) {
		const int type = dbus_message_get_type(msg);
		const dbus_uint32_t rs = dbus_message_get_reply_serial(msg);
		if ((type == DBUS_MESSAGE_TYPE_METHOD_RETURN || type == DBUS_MESSAGE_TYPE_ERROR) && rs != 0 &&
		    rs == c->exclude_serial) {
			c->exclude_serial = 0;
			uint32_t w = 0;
			const enum leia_mutter_exclude_status st = leia_mutter_capture_exclude_from_reply(msg, &w);
			if (st == LEIA_MUTTER_EXCLUDE_OK) {
				c->untrusted_through = retrust_after(c);
				c->exclusion_live = true;
			}
			U_LOG_W("leia_bg_capture_linux: capture-exclusion extension is back — re-exclude %s (%u "
			        "window(s)); trusting frames after #%u (in-flight pool frames skipped)",
			        leia_mutter_exclude_status_str(st), w, c->untrusted_through);
		} else if ((type == DBUS_MESSAGE_TYPE_METHOD_RETURN || type == DBUS_MESSAGE_TYPE_ERROR) && rs != 0 &&
		           rs == c->layout_serial) {
			c->layout_serial = 0;
			struct leia_mutter_panel now;
			if (!leia_mutter_panel_from_reply(msg, c->have_panel_id ? &c->panel_id : NULL, c->panel.device_w,
			                                  c->panel.device_h, &now) ||
			    now.logical_x != c->panel.logical_x || now.logical_y != c->panel.logical_y ||
			    now.logical_w != c->panel.logical_w || now.logical_h != c->panel.logical_h ||
			    now.device_w != c->panel.device_w || now.device_h != c->panel.device_h) {
				if (!c->wants_restart) {
					c->wants_restart = true;
					U_LOG_W("leia_bg_capture_linux: the panel's layout changed — the recorded area is "
					        "stale; the capture will be restarted");
				}
			}
		} else if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
			const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
			                          DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
			    name != NULL && strcmp(name, LEIA_MUTTER_EXT_BUS) == 0) {
				if (new_owner == NULL || new_owner[0] == '\0') {
					// Screen lock, logout, or the user disabled it: the effect
					// is gone and every frame from now on contains our window.
					c->exclusion_live = false;
					c->exclude_serial = 0; // a reply to an older request no longer means anything
					U_LOG_W("leia_bg_capture_linux: capture-exclusion extension went away (screen lock / "
					        "disabled) — captured desktop DISTRUSTED, falling back to silhouette intersection");
				} else if (c->exclude_serial == 0) {
					// Back (e.g. unlock): re-register — asynchronously; the
					// capture stays distrusted until the reply lands.
					uint32_t serial = 0;
					if (leia_mutter_capture_exclude_send(c->dbus, &serial)) {
						c->exclude_serial = serial;
					}
				}
			}
		} else if (dbus_message_is_signal(msg, "org.gnome.Mutter.ScreenCast.Session", "Closed")) {
			if (c->session_path != NULL && dbus_message_has_path(msg, c->session_path) && !c->session_closed) {
				c->session_closed = true;
				U_LOG_W("leia_bg_capture_linux: mutter closed the ScreenCast session (e.g. the user pressed "
				        "\"Stop Screen Sharing\") — no captured desktop for the rest of this session; "
				        "transparency falls back to silhouette intersection");
			}
		} else if (dbus_message_is_signal(msg, "org.gnome.Mutter.DisplayConfig", "MonitorsChanged")) {
			if (c->layout_serial == 0) {
				uint32_t serial = 0;
				if (leia_mutter_request_layout(c->dbus, &serial)) {
					c->layout_serial = serial;
				}
			}
		}
		dbus_message_unref(msg);
	}
}

/*!
 * Render-thread stall detector. poll() and get_preview() are on the frame's
 * critical path and must never wait — on the PipeWire thread, on D-Bus, on
 * anything. If one ever takes longer than this, say so ONCE per capture
 * session (never per frame), with the entry point and the time, so a
 * regression like the 2.5 s preview-lock stall reports itself.
 */
#ifndef STALL_WARN_NS
#define STALL_WARN_NS (4ull * 1000ull * 1000ull)
#endif

static void
stall_check(struct leia_bg_capture_linux *c, const char *what, uint64_t t0_ns);

// ---------------------------------------------------------------------------
// Rear-depth-budget background preview (runtime#224 / xrt_dp_background_preview)
// ---------------------------------------------------------------------------

//! Stage-1 reduction of the whole captured panel (1/2^shift box filter).
#define PANEL_PREVIEW_SHIFT 2u
//! Both output dimensions must stay <= this (xrt_dp_background_preview).
#define PREVIEW_MAX_DIM 512u
//! Stage-1 throttle: the runtime polls at ~66 ms; producing faster is waste.
#define PANEL_PREVIEW_MIN_INTERVAL_NS (66ull * 1000ull * 1000ull)

static uint64_t
mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
stall_check(struct leia_bg_capture_linux *c, const char *what, uint64_t t0_ns)
{
	const uint64_t dt = mono_ns() - t0_ns;
	if (dt > STALL_WARN_NS && !c->stall_logged) {
		c->stall_logged = true;
		U_LOG_W("leia_bg_capture_linux: RENDER-THREAD STALL — %s took %.2f ms (limit %.1f ms). It must never "
		        "wait on the capture; logged once per session",
		        what, (double)dt / 1e6, (double)STALL_WARN_NS / 1e6);
	}
}

/*!
 * pw thread: box-filter a tightly packed width×height 4-byte frame by
 * 2^PANEL_PREVIEW_SHIFT into @p dst (BGRA8, alpha 255). Swizzles RGB-order
 * formats to BGRA, and ignores the source X/A byte (BGRx carries garbage).
 */
#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
#if defined(__GNUC__) && !defined(__clang__)
// The one hot loop in this module (~4.8 ms for a 3840x2160 panel at -O2, ~26 ms
// at -O0). It runs on the PipeWire thread, where a Debug build's cost would
// delay frame delivery, so optimise it regardless of the build type.
__attribute__((optimize("O2")))
#endif
static void
panel_preview_reduce(const uint8_t *src, size_t sstride, uint32_t w, uint32_t h, bool rgb_order, uint8_t *dst,
                     uint32_t pw, uint32_t ph)
{
	const uint32_t f = 1u << PANEL_PREVIEW_SHIFT;
	const uint32_t n = f * f;
	(void)w;
	uint32_t acc[3 * 2048];
	for (uint32_t oy = 0; oy < ph; oy++) {
		// Columns are processed in chunks so the accumulator stays on the stack.
		for (uint32_t ox0 = 0; ox0 < pw; ox0 += 2048) {
			const uint32_t cw = (pw - ox0) < 2048 ? (pw - ox0) : 2048;
			memset(acc, 0, sizeof(uint32_t) * 3 * cw);
			for (uint32_t ry = 0; ry < f; ry++) {
				const uint8_t *row = src + (size_t)(oy * f + ry) * sstride + (size_t)ox0 * f * 4u;
				for (uint32_t ox = 0; ox < cw; ox++) {
					const uint8_t *px = row + (size_t)ox * f * 4u;
					uint32_t *a = &acc[ox * 3];
					for (uint32_t rx = 0; rx < f; rx++) {
						a[0] += px[rx * 4 + 0];
						a[1] += px[rx * 4 + 1];
						a[2] += px[rx * 4 + 2];
					}
				}
			}
			uint8_t *out = dst + ((size_t)oy * pw + ox0) * 4u;
			for (uint32_t ox = 0; ox < cw; ox++) {
				const uint8_t c0 = (uint8_t)(acc[ox * 3 + 0] / n);
				const uint8_t c1 = (uint8_t)(acc[ox * 3 + 1] / n);
				const uint8_t c2 = (uint8_t)(acc[ox * 3 + 2] / n);
				out[ox * 4 + 0] = rgb_order ? c2 : c0; // B
				out[ox * 4 + 1] = c1;                  // G
				out[ox * 4 + 2] = rgb_order ? c0 : c2; // R
				out[ox * 4 + 3] = 255;
			}
		}
	}
	(void)h;
}

/*!
 * pw thread, while it still holds the PipeWire buffer @p src (CACHED memory —
 * the producer's memfd/MemPtr): refresh the stage-1 panel preview, at most
 * every PANEL_PREVIEW_MIN_INTERVAL_NS. The filter runs with NO lock held, into
 * the pw-thread-private back buffer; only the pointer swap is locked.
 */
static void
panel_preview_produce(struct leia_bg_capture_linux *c, const uint8_t *src, size_t src_stride, uint32_t seq)
{
	if (!c->preview_lock_inited || src == NULL) {
		return;
	}
	const uint64_t now = mono_ns();
	if (c->panel_preview_last_ns != 0 && now - c->panel_preview_last_ns < PANEL_PREVIEW_MIN_INTERVAL_NS) {
		return;
	}
	const uint32_t pw = c->width >> PANEL_PREVIEW_SHIFT;
	const uint32_t ph = c->height >> PANEL_PREVIEW_SHIFT;
	if (pw == 0 || ph == 0) {
		return;
	}
	struct panel_preview_buf *b = &c->pp_back;
	if (b->px == NULL || b->w != pw || b->h != ph) {
		free(b->px);
		b->px = malloc((size_t)pw * ph * 4u);
		b->w = b->px != NULL ? pw : 0;
		b->h = b->px != NULL ? ph : 0;
	}
	if (b->px == NULL) {
		return;
	}
	panel_preview_reduce(src, src_stride, c->width, c->height, c->fmt_rgb_order, b->px, pw, ph);
	b->seq = seq;
	c->panel_preview_last_ns = now;

	pthread_mutex_lock(&c->preview_lock); // pointer swap only — the render side never holds it longer
	const struct panel_preview_buf t = c->pp_mid;
	c->pp_mid = *b;
	*b = t;
	pthread_mutex_unlock(&c->preview_lock);
}
#endif // LEIA_BG_CAPTURE_HAS_PREVIEW

// ---------------------------------------------------------------------------
// PipeWire stream callbacks
// ---------------------------------------------------------------------------

/*! Find a free / matching slot for a pw buffer id, keyed by id itself. */
static uint32_t
slot_for_buffer(struct pw_buffer *b)
{
	// pw hands buffers back in a stable set; use the pointer's user_data as the
	// slot store. We simply hash by the buffer's own index in the pool by
	// stashing it on user_data at add_buffer time.
	uintptr_t s = (uintptr_t)b->user_data;
	return (uint32_t)s;
}

static void
on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct leia_bg_capture_linux *c = data;

	if (param == NULL || id != SPA_PARAM_Format) {
		return;
	}

	uint32_t media_type = 0, media_subtype = 0;
	if (spa_format_parse(param, &media_type, &media_subtype) < 0) {
		return;
	}
	if (media_type != SPA_MEDIA_TYPE_video ||
	    media_subtype != SPA_MEDIA_SUBTYPE_raw) {
		return;
	}

	struct spa_video_info_raw info;
	spa_zero(info);
	if (spa_format_video_raw_parse(param, &info) < 0) {
		U_LOG_W("leia_bg_capture_linux: failed to parse video format");
		return;
	}

	VkFormat vkf = spa_to_vk_format(info.format);
	if (vkf == VK_FORMAT_UNDEFINED) {
		U_LOG_W("leia_bg_capture_linux: unsupported SPA format %u", info.format);
		return;
	}

	c->width = info.size.width;
	c->height = info.size.height;
	c->vk_format = vkf;
	if (info.max_framerate.num > 0) {
		U_LOG_W("leia_bg_capture_linux: delivery capped at %u/%u fps (LEIA_DP_CAPTURE_MIN_INTERVAL_MS)",
		        info.max_framerate.num, info.max_framerate.denom);
	}
	c->fmt_rgb_order = (info.format == SPA_VIDEO_FORMAT_RGBA || info.format == SPA_VIDEO_FORMAT_RGBx);

	// spa_video_info_raw carries the negotiated DRM modifier directly in
	// .modifier (this SPA has no "modifier present" flag). We force LINEAR in
	// the advertised format (see build_format_params), so this is LINEAR (0)
	// today; honour whatever was negotiated for forward-compat.
	c->modifier = info.modifier; // DRM_FORMAT_MOD_LINEAR == 0

	c->have_format = true;
	// WARN so it survives field logs (INFO is dropped) — fires once per
	// (re)negotiation, which is rare and is exactly what we want to see.
	// UNITS: RecordArea streams area × the highest overlapping monitor scale,
	// so for an area that is exactly the panel this is the panel's DEVICE
	// size (up to rounding at a fractional scale). Say so if it is not.
	U_LOG_W("leia_bg_capture_linux: format negotiated %ux%u vkfmt=%d mod=0x%llx (panel DEVICE %ux%u%s)",
	        c->width, c->height, (int)c->vk_format, (unsigned long long)c->modifier, c->panel.device_w,
	        c->panel.device_h,
	        (c->width == c->panel.device_w && c->height == c->panel.device_h)
	            ? ", 1:1"
	            : " — NOT 1:1: window rects are rescaled into the stream");

	// Reply with buffer params: request dma-buf-capable buffers.
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *params[1];
	// Producer-allocated buffers, standard consumer recipe (what OBS uses
	// against Mutter). blocks/size/stride/align are REQUIRED for the
	// producer's allocator to size the pool — without them Mutter falls into
	// a degenerate client-alloc negotiation that never delivers frames (#109).
	const uint32_t frame_stride = c->width * 4;
	const uint32_t frame_size = frame_stride * c->height;
	params[0] = spa_pod_builder_add_object(
	    &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
	    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, DXR_MAX_BUFFERS),
	    SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
	    SPA_PARAM_BUFFERS_size, SPA_POD_Int((int)frame_size),
	    SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int)frame_stride),
	    SPA_PARAM_BUFFERS_align, SPA_POD_Int(16),
	    SPA_PARAM_BUFFERS_dataType,
	    SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) |
	                             (1 << SPA_DATA_MemPtr)));

	pw_stream_update_params(c->stream, params, 1);
}

static void
on_add_buffer(void *data, struct pw_buffer *buffer)
{
	struct leia_bg_capture_linux *c = data;
	struct spa_buffer *sbuf = buffer->buffer;

	if (sbuf->n_datas < 1) {
		return;
	}
	struct spa_data *d = &sbuf->datas[0];

	// Assign a stable slot: linear scan for a free slot.
	uint32_t slot = DXR_MAX_BUFFERS;
	for (uint32_t i = 0; i < DXR_MAX_BUFFERS; i++) {
		if (!c->buffers[i].imported) {
			slot = i;
			break;
		}
	}
	if (slot == DXR_MAX_BUFFERS) {
		U_LOG_W("leia_bg_capture_linux: buffer pool exhausted");
		return;
	}
	buffer->user_data = (void *)(uintptr_t)slot;
	atomic_fetch_add(&c->pool_buffers, 1);

	if (d->type != SPA_DATA_DmaBuf) {
		// MemFd/MemPtr delivery (what Mutter's Xorg screencast actually
		// gives us after declining the dma-buf format offer, #109/#110):
		// stage through a host-visible buffer; poll() uploads on the
		// compose command buffer.
		if (!create_shm_slot(c, slot, d)) {
			buffer->user_data = (void *)(uintptr_t)DXR_MAX_BUFFERS;
		}
		return;
	}

	int64_t offset = 0;
	int32_t stride = (int32_t)c->width * 4;
	if (d->chunk != NULL) {
		offset = d->chunk->offset;
		if (d->chunk->stride > 0) {
			stride = d->chunk->stride;
		}
	}

	if (!import_dmabuf(c, slot, (int)d->fd, offset, stride)) {
		buffer->user_data = (void *)(uintptr_t)DXR_MAX_BUFFERS;
	}
}

static void
on_remove_buffer(void *data, struct pw_buffer *buffer)
{
	struct leia_bg_capture_linux *c = data;
	uint32_t slot = slot_for_buffer(buffer);
	if (atomic_load(&c->pool_buffers) > 0) {
		atomic_fetch_sub(&c->pool_buffers, 1);
	}
	if (slot >= DXR_MAX_BUFFERS) {
		return;
	}
	// If the render thread currently points here, clear it first.
	int cur = atomic_load(&c->current_buffer);
	if (cur == (int)slot) {
		atomic_store(&c->current_buffer, -1);
	}
	destroy_buffer_slot(c, slot);
}

static void
on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state,
                 const char *error)
{
	(void)data;
	(void)old;
	if (state == PW_STREAM_STATE_ERROR) {
		U_LOG_W("leia_bg_capture_linux: stream error: %s", error ? error : "?");
	} else {
		U_LOG_I("leia_bg_capture_linux: stream state -> %s",
		        pw_stream_state_as_string(state));
	}
}

static void
on_process(void *data)
{
	struct leia_bg_capture_linux *c = data;
	struct pw_buffer *b = NULL;
	struct pw_buffer *newest = NULL;

	// Drain to the newest buffer; drop the rest.
	while ((b = pw_stream_dequeue_buffer(c->stream)) != NULL) {
		if (newest != NULL) {
			pw_stream_queue_buffer(c->stream, newest);
		}
		newest = b;
	}
	if (newest == NULL) {
		return;
	}

	uint32_t slot = slot_for_buffer(newest);
	if (slot < DXR_MAX_BUFFERS && c->buffers[slot].imported) {
		struct dxr_bg_buffer *bb = &c->buffers[slot];
		if (bb->is_shm) {
			// Copy the frame out BEFORE requeueing (the producer may
			// rewrite the buffer as soon as it's back in the pool).
			struct spa_data *d = &newest->buffer->datas[0];
			const uint8_t *src = d->data != NULL
			                         ? (const uint8_t *)d->data
			                         : (const uint8_t *)bb->map + d->mapoffset;
			if (src != NULL && bb->staging_ptr != NULL) {
				const uint32_t dst_stride = c->width * 4;
				int32_t src_stride = (int32_t)dst_stride;
				uint32_t off = 0;
				if (d->chunk != NULL) {
					off = d->chunk->offset;
					if (d->chunk->stride > 0) {
						src_stride = d->chunk->stride;
					}
				}
				src += off;
				if (src == (const uint8_t *)bb->staging_ptr) {
					// Client-alloc mode: the producer wrote straight into our
					// staging buffer. Only compact rows if its stride is
					// padded (forward per-row memmove is safe: dst < src).
					if (src_stride != (int32_t)dst_stride) {
						uint8_t *base = bb->staging_ptr;
						for (uint32_t y = 1; y < c->height; y++) {
							memmove(base + (size_t)y * dst_stride,
							        base + (size_t)y * src_stride, dst_stride);
						}
					}
				} else if (src_stride == (int32_t)dst_stride) {
					memcpy(bb->staging_ptr, src, (size_t)dst_stride * c->height);
				} else {
					uint8_t *dst = bb->staging_ptr;
					for (uint32_t y = 0; y < c->height; y++) {
						memcpy(dst + (size_t)y * dst_stride,
						       src + (size_t)y * src_stride, dst_stride);
					}
				}
				atomic_store(&bb->dirty, true);
			}
		}
		const uint32_t seq = atomic_load(&c->dbg_frames) + 1;
		atomic_store(&c->slot_seq[slot], seq);
		if (bb->is_shm) {
			// Rear-depth-budget preview, stage 1, off the render thread, read
			// from the PipeWire buffer (cached) — not from staging (uncached).
			struct spa_data *d = &newest->buffer->datas[0];
			const uint8_t *src = d->data != NULL ? (const uint8_t *)d->data : (const uint8_t *)bb->map + d->mapoffset;
			size_t stride = (size_t)c->width * 4u;
			if (d->chunk != NULL) {
				src += d->chunk->offset;
				if (d->chunk->stride > 0) {
					stride = (size_t)d->chunk->stride;
				}
			}
#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
			if (src != NULL && src != (const uint8_t *)bb->staging_ptr) {
				panel_preview_produce(c, src, stride, seq);
			}
#else
			(void)src;
			(void)stride;
#endif
		}
		// Publish the stable slot index; for dma-buf the VkImage import is
		// fixed for the buffer's lifetime, for shm the frame now sits in the
		// slot's staging buffer — either way the render thread can proceed
		// after requeue.
		atomic_store(&c->current_buffer, (int)slot);

		// Frame-flow diagnostics (#109 follow-up): FIRST frame only (WARN is
		// for one-off lifecycle events; recurring counts stay in dbg_frames).
		uint32_t n = atomic_fetch_add(&c->dbg_frames, 1) + 1;
		if (!c->dbg_first_frame_logged) {
			c->dbg_first_frame_logged = true;
			U_LOG_W("leia_bg_capture_linux: frame %u published (slot %u, %s) — capture is live", n,
			        slot, c->buffers[slot].is_shm ? "shm" : "dma-buf");
		}
	}
	pw_stream_queue_buffer(c->stream, newest);
}

static const struct pw_stream_events s_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .add_buffer = on_add_buffer,
    .remove_buffer = on_remove_buffer,
    .process = on_process,
};

/*! Build EnumFormat params advertising dma-buf + a MemFd/MemPtr fallback. */
static int
build_format_params(struct spa_pod_builder *b, const struct spa_pod **params,
                    int max_params)
{
	int n = 0;
	struct spa_rectangle size_def = SPA_RECTANGLE(1920, 1080);
	struct spa_rectangle size_min = SPA_RECTANGLE(1, 1);
	struct spa_rectangle size_max = SPA_RECTANGLE(8192, 8192);
	struct spa_fraction rate_def = SPA_FRACTION(60, 1);
	struct spa_fraction rate_min = SPA_FRACTION(0, 1);
	struct spa_fraction rate_max = SPA_FRACTION(240, 1);

	// Delivery cap — Windows parity (leia_bg_capture_win.cpp, the WGC
	// MinUpdateInterval knee measured at 66 ms). Mutter records the area on
	// every stage update that damages it, and each recording is a full
	// off-screen re-render of the panel; it honours the negotiated
	// maxFramerate by skipping records that come too soon. The compose only
	// reads the latest frame, so the cap costs nothing on a quiet desktop and
	// bounds both mutter's re-render and our CPU copy under motion; the trade
	// is fringe freshness, bounded by the cap.
	// LEIA_DP_CAPTURE_MIN_INTERVAL_MS: unset = 66, 0 = uncapped, N = N ms.
	long interval_ms = 66;
	const char *e = getenv("LEIA_DP_CAPTURE_MIN_INTERVAL_MS");
	if (e != NULL && e[0] != '\0') {
		interval_ms = atol(e);
		if (interval_ms < 0 || interval_ms > 1000) {
			interval_ms = 66;
		}
	}
	struct spa_fraction maxrate_def = interval_ms > 0 ? SPA_FRACTION(1000, (uint32_t)interval_ms) : SPA_FRACTION(240, 1);
	struct spa_fraction maxrate_min = SPA_FRACTION(1, 1);
	struct spa_fraction maxrate_max = maxrate_def;

	// SYSMEM-ONLY offer (#109): a format pod carrying a modifier property makes
	// Mutter's Xorg screen-cast fixate the dma-buf format, but its X11 path
	// then never actually allocates/delivers dma-buf frames — the stream runs
	// with a limbo buffer negotiation and the consumer sees empty spa_data
	// forever (verified live via pw-dump: Format fixated with modifier=0,
	// Buffers=null). The plain no-modifier format takes Mutter's classic
	// sysmem producer path (the one OBS has consumed for years). dma-buf
	// zero-copy is a hardware-bring-up follow-up: it needs the DONT_FIXATE
	// modifier-choice dance AND a compositor that honors it on X11.
	if (n < max_params) {
		params[n++] = spa_pod_builder_add_object(
		    b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,          //
		    SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),   //
		    SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), //
		    SPA_FORMAT_VIDEO_format,
		    SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA,
		                           SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRx,
		                           SPA_VIDEO_FORMAT_RGBx),
		    SPA_FORMAT_VIDEO_size,
		    SPA_POD_CHOICE_RANGE_Rectangle(&size_def, &size_min, &size_max), //
		    SPA_FORMAT_VIDEO_framerate,
		    SPA_POD_CHOICE_RANGE_Fraction(&rate_def, &rate_min, &rate_max), //
		    SPA_FORMAT_VIDEO_maxFramerate,
		    SPA_POD_CHOICE_RANGE_Fraction(&maxrate_def, &maxrate_min, &maxrate_max));
	}

	return n;
}

static bool
pipewire_start(struct leia_bg_capture_linux *c)
{
	if (!s_pw_inited) {
		pw_init(NULL, NULL);
		s_pw_inited = true;
	}

	c->loop = pw_thread_loop_new("dxr-bgcap", NULL);
	if (c->loop == NULL) {
		U_LOG_W("leia_bg_capture_linux: pw_thread_loop_new failed");
		return false;
	}
	if (pw_thread_loop_start(c->loop) < 0) {
		U_LOG_W("leia_bg_capture_linux: pw_thread_loop_start failed");
		return false;
	}

	bool ok = false;
	pw_thread_loop_lock(c->loop);

	c->context = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
	if (c->context == NULL) {
		U_LOG_W("leia_bg_capture_linux: pw_context_new failed");
		goto unlock;
	}
	// The mutter node lives on the user's default PipeWire daemon; unlike the
	// portal there is no restricted remote fd to go through.
	c->core = pw_context_connect(c->context, NULL, 0);
	if (c->core == NULL) {
		U_LOG_W("leia_bg_capture_linux: pw_context_connect (default socket) failed");
		goto unlock;
	}

	c->stream = pw_stream_new(
	    c->core, "dxr-desktop-capture",
	    pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
	                      "Capture", PW_KEY_MEDIA_ROLE, "Screen", NULL));
	if (c->stream == NULL) {
		U_LOG_W("leia_bg_capture_linux: pw_stream_new failed");
		goto unlock;
	}

	pw_stream_add_listener(c->stream, &c->stream_listener, &s_stream_events, c);

	uint8_t buf[2048];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *params[2];
	int n_params = build_format_params(&b, params, 2);
	if (n_params <= 0) {
		U_LOG_W("leia_bg_capture_linux: no format params built");
		goto unlock;
	}

	// Standard consumer recipe: producer allocates (NO ALLOC_BUFFERS — that
	// flag means the CLIENT allocates); MAP_BUFFERS maps CPU buffers into
	// d->data for the copy path. The dma-buf import path uses d->fd directly.
	int r = pw_stream_connect(c->stream, PW_DIRECTION_INPUT, c->node_id,
	                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
	                          params, (uint32_t)n_params);
	if (r < 0) {
		U_LOG_W("leia_bg_capture_linux: pw_stream_connect failed (%d)", r);
		goto unlock;
	}

	ok = true;

unlock:
	pw_thread_loop_unlock(c->loop);
	return ok;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

struct leia_bg_capture_linux *
leia_bg_capture_linux_create(struct vk_bundle *vk, uint32_t panel_px_w, uint32_t panel_px_h)
{
	if (vk == NULL) {
		U_LOG_W("leia_bg_capture_linux: NULL vk_bundle");
		return NULL;
	}

	// Gate on the Vulkan capabilities we depend on.
	if (!vk->has_EXT_external_memory_dma_buf) {
		U_LOG_W("leia_bg_capture_linux: VK_EXT_external_memory_dma_buf missing — "
		        "desktop capture unavailable (runtime#757)");
		return NULL;
	}
	if (!vk->has_EXT_image_drm_format_modifier) {
		U_LOG_W("leia_bg_capture_linux: VK_EXT_image_drm_format_modifier missing — "
		        "desktop capture unavailable (runtime#757)");
		return NULL;
	}
	if (!vk->has_KHR_external_memory) {
		U_LOG_W("leia_bg_capture_linux: VK_KHR_external_memory missing — "
		        "desktop capture unavailable (runtime#757)");
		return NULL;
	}

	struct leia_bg_capture_linux *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return NULL;
	}
	c->vk = vk;
	c->vk_format = VK_FORMAT_UNDEFINED;
	atomic_store(&c->current_buffer, -1);
	if (pthread_mutex_init(&c->preview_lock, NULL) == 0) {
		c->preview_lock_inited = true;
	}

	// vkGetMemoryFdPropertiesKHR is not in the dispatch table.
	c->getMemoryFdProperties = (PFN_vkGetMemoryFdPropertiesKHR)
	    vk->vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdPropertiesKHR");
	if (c->getMemoryFdProperties == NULL) {
		U_LOG_W("leia_bg_capture_linux: vkGetMemoryFdPropertiesKHR unavailable");
		goto fail;
	}

	if (!mutter_handshake(c, panel_px_w, panel_px_h)) {
		goto fail;
	}

	if (!pipewire_start(c)) {
		U_LOG_W("leia_bg_capture_linux: PipeWire start failed");
		goto fail;
	}

	U_LOG_W("leia_bg_capture_linux: window-excluded desktop capture started (panel %s)", c->panel.connector);
	return c;

fail:
	leia_bg_capture_linux_destroy(c);
	return NULL;
}

bool
leia_bg_capture_linux_wants_restart(struct leia_bg_capture_linux *c)
{
	return c != NULL && c->wants_restart;
}

VkImageView
leia_bg_capture_linux_get_view(struct leia_bg_capture_linux *c)
{
	if (c == NULL) {
		return VK_NULL_HANDLE;
	}
	return c->current_view;
}

void
leia_bg_capture_linux_get_size(struct leia_bg_capture_linux *c, uint32_t *out_width,
                               uint32_t *out_height)
{
	uint32_t w = 0, h = 0;
	if (c != NULL && c->have_format) {
		w = c->width;
		h = c->height;
	}
	if (out_width) {
		*out_width = w;
	}
	if (out_height) {
		*out_height = h;
	}
}

static bool
capture_poll_impl(struct leia_bg_capture_linux *c, VkCommandBuffer cmd,
                  int32_t win_x, int32_t win_y, uint32_t win_w, uint32_t win_h,
                  float out_bg_uv_origin[2], float out_bg_uv_extent[2])
{
	c->poll_ok = false;
	c->last_win_x = win_x;
	c->last_win_y = win_y;
	c->last_win_w = win_w;
	c->last_win_h = win_h;

	mutter_pump(c);

	// TRUST GATE. A frame is usable only if our windows were excluded when it
	// was recorded. Declining here makes the DP compose nothing and its
	// alpha-gate fall back to silhouette intersection for this frame.
	if (!c->exclusion_live || c->session_closed || c->wants_restart) {
		if (!c->logged_distrust) {
			c->logged_distrust = true;
			U_LOG_W("leia_bg_capture_linux: captured desktop not used (%s)",
			        c->session_closed ? "session closed"
			        : c->wants_restart ? "panel layout changed"
			                           : "capture exclusion not live");
		}
		return false;
	}

	int idx = atomic_load(&c->current_buffer);
	if (idx < 0 || idx >= DXR_MAX_BUFFERS || !c->buffers[idx].imported) {
		if (!c->dbg_no_frame_logged) {
			c->dbg_no_frame_logged = true;
			U_LOG_W("leia_bg_capture_linux: poll before any frame arrived "
			        "(stream up but no buffers yet — will report when frames flow)");
		}
		return false; // no frame yet — caller passes the raw atlas through
	}
	if (atomic_load(&c->slot_seq[idx]) <= c->untrusted_through) {
		return false; // recorded while the exclusion was down — may contain us
	}
	if (c->logged_distrust) {
		c->logged_distrust = false;
		U_LOG_W("leia_bg_capture_linux: captured desktop trusted again");
	}

	// UNITS. win_x/win_y/win_w/win_h are DEVICE pixels relative to the panel's
	// top-left (the runtime's present origin + the window's target extent).
	// The stream records exactly the panel, so a window rect maps into it by
	// normalising with the panel's DEVICE size — never a LOGICAL size (that was
	// the portal-era bug fixed in #254: 1920x1080 for a 3840x2160 panel at
	// 200% doubled every UV). The panel's device size comes from mutter's
	// current mode; the negotiated stream size is the fallback, and the two
	// agree whenever the scale is exact.
	const int32_t cap_w = c->panel.device_w > 0 ? (int32_t)c->panel.device_w : (int32_t)c->width;
	const int32_t cap_h = c->panel.device_h > 0 ? (int32_t)c->panel.device_h : (int32_t)c->height;
	if (cap_w > 0 && cap_h > 0 && win_w > 0 && win_h > 0) {
		// Window fully off the captured panel → decline (raw pass-through).
		if (win_x >= cap_w || win_y >= cap_h ||
		    win_x + (int32_t)win_w <= 0 || win_y + (int32_t)win_h <= 0) {
			return false;
		}
	}

	c->current_view = c->buffers[idx].view;

	struct dxr_bg_buffer *bb = &c->buffers[idx];
	if (bb->is_shm) {
		// CPU-staged slot: upload the latest frame from the staging buffer
		// on the compose command buffer, then transition for sampling.
		bool expected = true;
		const bool do_upload = atomic_compare_exchange_strong(&bb->dirty, &expected, false);
		if (!do_upload && !bb->image_initialized) {
			return false; // no frame ever uploaded — nothing valid to sample
		}
		if (do_upload) {
			VkImageMemoryBarrier to_dst = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = bb->image_initialized ? VK_ACCESS_SHADER_READ_BIT : 0,
			    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .oldLayout = bb->image_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			                                       : VK_IMAGE_LAYOUT_UNDEFINED,
			    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .image = bb->image,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			c->vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
			                            NULL, 1, &to_dst);
			VkBufferImageCopy region = {
			    .bufferOffset = 0,
			    .bufferRowLength = 0, // tightly packed by on_process
			    .bufferImageHeight = 0,
			    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			    .imageOffset = {0, 0, 0},
			    .imageExtent = {c->width, c->height, 1},
			};
			c->vk->vkCmdCopyBufferToImage(cmd, bb->staging, bb->image,
			                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			VkImageMemoryBarrier to_read = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .image = bb->image,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			c->vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
			                            NULL, 0, NULL, 1, &to_read);
			bb->image_initialized = true;

			uint32_t u = atomic_fetch_add(&c->dbg_uploads, 1) + 1;
			if (!c->dbg_first_upload_logged) {
				c->dbg_first_upload_logged = true;
				U_LOG_W("leia_bg_capture_linux: shm upload %u recorded (slot %d) — compose is fed", u,
				        idx);
			}
		}
		// Not dirty + initialized: image already SHADER_READ_ONLY with the
		// last frame — sample as-is, no barrier needed.
	} else {
		// ACQUIRE barrier: the dma-buf is written out-of-band by the compositor,
		// so we take ownership from the FOREIGN queue and transition UNDEFINED ->
		// SHADER_READ_ONLY_OPTIMAL for sampling in the fragment shader.
		VkImageMemoryBarrier acquire = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .pNext = NULL,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		    .dstQueueFamilyIndex = c->vk->main_queue->family_index,
		    .image = bb->image,
		    .subresourceRange =
		        {
		            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		            .baseMipLevel = 0,
		            .levelCount = 1,
		            .baseArrayLayer = 0,
		            .layerCount = 1,
		        },
		};
		c->vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
		                            NULL, 1, &acquire);
	}

	// Per-window background sub-rect: the compose shader samples the desktop
	// region directly UNDER the window, so each tile's transparent pixels line
	// up with what a user would see through a glass window at that position.
	// win_w/win_h == 0 (display-scoped present) maps the full monitor.
	float uox = 0.0f, uoy = 0.0f, uex = 1.0f, uey = 1.0f;
	if (cap_w > 0 && cap_h > 0 && win_w > 0 && win_h > 0) {
		uox = (float)win_x / (float)cap_w;
		uoy = (float)win_y / (float)cap_h;
		uex = (float)win_w / (float)cap_w;
		uey = (float)win_h / (float)cap_h;
	}
	if (out_bg_uv_origin) {
		out_bg_uv_origin[0] = uox;
		out_bg_uv_origin[1] = uoy;
	}
	if (out_bg_uv_extent) {
		out_bg_uv_extent[0] = uex;
		out_bg_uv_extent[1] = uey;
	}
	c->poll_ok = true;
	return true;
}

#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
/*!
 * Region of the window a preview crop actually covers, in window-normalised
 * coordinates (u right, v down, 0,0 = window top-left) — i.e.
 * xrt_dp_background_preview::canvas_u0..canvas_v1. Pure arithmetic.
 *
 * The crop is in panel-preview pixels (each @p dev_per_px DEVICE pixels wide
 * and tall), so it rarely lands exactly on the window's edges: it is rounded
 * OUTWARD, and the rect says so (u0 slightly < 0, u1 slightly > 1 — the
 * "margin" the runtime struct allows). Clamping to the panel is what makes it
 * strictly inside [0,1] for a window hanging off an edge; the runtime maps
 * every window-normalised region through this rect, so it must be exact.
 */
static void
preview_canvas_uv(float crop_x0_dev, float crop_y0_dev, float crop_x1_dev, float crop_y1_dev, int32_t win_x,
                  int32_t win_y, uint32_t win_w, uint32_t win_h, float out_uv[4])
{
	out_uv[0] = (crop_x0_dev - (float)win_x) / (float)win_w;
	out_uv[1] = (crop_y0_dev - (float)win_y) / (float)win_h;
	out_uv[2] = (crop_x1_dev - (float)win_x) / (float)win_w;
	out_uv[3] = (crop_y1_dev - (float)win_y) / (float)win_h;
}

static bool
capture_get_preview_impl(struct leia_bg_capture_linux *c, struct xrt_dp_background_preview *out)
{
	// "No source right now" — every case where the last poll declined:
	// exclusion down, session closed, window off the panel, no frame yet.
	if (!c->poll_ok) {
		return false;
	}
	if (c->width == 0 || c->height == 0 || c->panel.device_w == 0 || c->panel.device_h == 0) {
		return false;
	}
	int idx = atomic_load(&c->current_buffer);
	if (idx >= 0 && idx < DXR_MAX_BUFFERS && c->buffers[idx].imported && !c->buffers[idx].is_shm) {
		// The dma-buf path has no CPU copy to reduce. Not reachable with
		// today's modifier-less format offer; say so once if it ever is.
		if (!c->preview_dmabuf_logged) {
			c->preview_dmabuf_logged = true;
			U_LOG_W("leia_bg_capture_linux: dma-buf frames — no background preview (rear depth budget "
			        "stays clipped)");
		}
		return false;
	}

	// The window rect this preview is for (DEVICE px, panel-relative);
	// display-scoped (w,h = 0) means the whole panel.
	int32_t wx = c->last_win_x, wy = c->last_win_y;
	uint32_t ww = c->last_win_w, wh = c->last_win_h;
	if (ww == 0 || wh == 0) {
		wx = 0;
		wy = 0;
		ww = c->panel.device_w;
		wh = c->panel.device_h;
	}

	// NEVER wait on the PipeWire thread: take a newer stage-1 preview only if
	// the swap lock is free this instant; otherwise keep the one we hold.
	if (pthread_mutex_trylock(&c->preview_lock) == 0) {
		if (c->pp_mid.seq > c->pp_front.seq) {
			const struct panel_preview_buf t = c->pp_front;
			c->pp_front = c->pp_mid;
			c->pp_mid = t;
		}
		pthread_mutex_unlock(&c->preview_lock);
	}
	const uint32_t pseq = c->pp_front.seq;
	const bool fresh_src = pseq != 0 && pseq > c->untrusted_through;
	if (!fresh_src) {
		return false; // no preview yet, or only one built from a distrusted frame
	}
	const bool rect_changed = wx != c->preview_win_x || wy != c->preview_win_y || ww != c->preview_win_w ||
	                          wh != c->preview_win_h;
	if (pseq != c->preview_src_seq || rect_changed || c->preview_out == NULL) {
		// DEVICE → stream → panel-preview pixels. stream/device == 1 unless a
		// fractional scale rounded the stream differently from the mode.
		const double s_x = (double)c->width / (double)c->panel.device_w;
		const double s_y = (double)c->height / (double)c->panel.device_h;
		const double f = (double)(1u << PANEL_PREVIEW_SHIFT);
		const int32_t pw = (int32_t)c->pp_front.w, ph = (int32_t)c->pp_front.h;
		int32_t x0 = (int32_t)floor((double)wx * s_x / f);
		int32_t y0 = (int32_t)floor((double)wy * s_y / f);
		int32_t x1 = (int32_t)ceil((double)(wx + (int32_t)ww) * s_x / f);
		int32_t y1 = (int32_t)ceil((double)(wy + (int32_t)wh) * s_y / f);
		x0 = x0 < 0 ? 0 : x0;
		y0 = y0 < 0 ? 0 : y0;
		x1 = x1 > pw ? pw : x1;
		y1 = y1 > ph ? ph : y1;
		if (x1 - x0 < 2 || y1 - y0 < 2) {
			return false; // (almost) nothing of the window is on the panel
		}
		// Stage 2: a further 2^k box so both sides are <= PREVIEW_MAX_DIM.
		uint32_t k = 0;
		while ((((uint32_t)(x1 - x0)) >> k) > PREVIEW_MAX_DIM || (((uint32_t)(y1 - y0)) >> k) > PREVIEW_MAX_DIM) {
			k++;
		}
		const uint32_t ow = ((uint32_t)(x1 - x0)) >> k, oh = ((uint32_t)(y1 - y0)) >> k;
		const size_t need = (size_t)ow * oh * 4u;
		if (need > c->preview_out_cap) {
			uint8_t *nb = realloc(c->preview_out, need);
			if (nb == NULL) {
				return false;
			}
			c->preview_out = nb;
			c->preview_out_cap = need;
		}
		const uint32_t blk = 1u << k, n = blk * blk;
		for (uint32_t oy = 0; oy < oh; oy++) {
			for (uint32_t ox = 0; ox < ow; ox++) {
				uint32_t sb = 0, sg = 0, sr = 0;
				for (uint32_t by = 0; by < blk; by++) {
					const uint8_t *px = c->pp_front.px +
					                    ((size_t)(y0 + (int32_t)(oy * blk + by)) * (size_t)pw +
					                     (size_t)(x0 + (int32_t)(ox * blk))) * 4u;
					for (uint32_t bx = 0; bx < blk; bx++) {
						sb += px[bx * 4 + 0];
						sg += px[bx * 4 + 1];
						sr += px[bx * 4 + 2];
					}
				}
				uint8_t *o = c->preview_out + ((size_t)oy * ow + ox) * 4u;
				o[0] = (uint8_t)(sb / n);
				o[1] = (uint8_t)(sg / n);
				o[2] = (uint8_t)(sr / n);
				o[3] = 255;
			}
		}
		// What the bytes cover, back in DEVICE px, then window-normalised.
		const double dev_x0 = (double)x0 * f / s_x, dev_y0 = (double)y0 * f / s_y;
		const double dev_x1 = (double)(x0 + (int32_t)(ow << k)) * f / s_x;
		const double dev_y1 = (double)(y0 + (int32_t)(oh << k)) * f / s_y;
		float uv[4];
		preview_canvas_uv((float)dev_x0, (float)dev_y0, (float)dev_x1, (float)dev_y1, wx, wy, ww, wh, uv);
		c->preview_cu0 = uv[0];
		c->preview_cv0 = uv[1];
		c->preview_cu1 = uv[2];
		c->preview_cv1 = uv[3];
		c->preview_out_w = ow;
		c->preview_out_h = oh;
		c->preview_src_seq = pseq;
		c->preview_win_x = wx;
		c->preview_win_y = wy;
		c->preview_win_w = ww;
		c->preview_win_h = wh;
		if (c->preview_out_gen == 0) {
			U_LOG_W("leia_bg_capture_linux: background preview ready — window %ux%u DEVICE px -> %ux%u "
			        "BGRA8 (1/%u box: 1/%u on the pw thread, 1/%u here), covers canvas u[%.4f,%.4f] "
			        "v[%.4f,%.4f]",
			        ww, wh, ow, oh, (1u << PANEL_PREVIEW_SHIFT) << k, 1u << PANEL_PREVIEW_SHIFT, 1u << k,
			        uv[0], uv[2], uv[1], uv[3]);
		}
		c->preview_out_gen++;
	}

	// Field-by-field against the runtime's struct_size (ADR-020): a runtime
	// whose struct is too small to describe a buffer cannot be handed one.
#define LEIA_BGP_FITS(field)                                                                                           \
	((size_t)offsetof(struct xrt_dp_background_preview, field) + sizeof(out->field) <= (size_t)out->struct_size)
	if (!LEIA_BGP_FITS(width) || !LEIA_BGP_FITS(height) || !LEIA_BGP_FITS(stride_bytes) || !LEIA_BGP_FITS(bgra)) {
		return false;
	}
	if (LEIA_BGP_FITS(generation)) {
		out->generation = c->preview_out_gen;
	}
	out->width = c->preview_out_w;
	out->height = c->preview_out_h;
	out->stride_bytes = c->preview_out_w * 4u;
	out->bgra = c->preview_out; // borrowed until the next process_atlas (render thread owns it)
	if (LEIA_BGP_FITS(canvas_v1)) {
		out->canvas_u0 = c->preview_cu0;
		out->canvas_v0 = c->preview_cv0;
		out->canvas_u1 = c->preview_cu1;
		out->canvas_v1 = c->preview_cv1;
	}
	// STALE is for a preview the source KNOWS no longer reflects the screen;
	// every such case (exclusion down, session closed, layout changed, window
	// off-panel) already returned false above. An unchanged desktop is not
	// stale — mutter only records on damage.
	if (LEIA_BGP_FITS(flags)) {
		out->flags = 0u;
	}
#undef LEIA_BGP_FITS
	return true;
}
#endif // LEIA_BG_CAPTURE_HAS_PREVIEW

bool
leia_bg_capture_linux_poll(struct leia_bg_capture_linux *c, VkCommandBuffer cmd,
                           int32_t win_x, int32_t win_y, uint32_t win_w, uint32_t win_h,
                           float out_bg_uv_origin[2], float out_bg_uv_extent[2])
{
	if (c == NULL) {
		return false;
	}
	const uint64_t t0 = mono_ns();
	const bool ok = capture_poll_impl(c, cmd, win_x, win_y, win_w, win_h, out_bg_uv_origin, out_bg_uv_extent);
	stall_check(c, "poll()", t0);
	return ok;
}

#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
bool
leia_bg_capture_linux_get_preview(struct leia_bg_capture_linux *c, struct xrt_dp_background_preview *out)
{
	if (c == NULL || out == NULL || !c->preview_lock_inited) {
		return false;
	}
	const uint64_t t0 = mono_ns();
	const bool ok = capture_get_preview_impl(c, out);
	stall_check(c, "get_background_preview()", t0);
	return ok;
}
#endif

void
leia_bg_capture_linux_destroy(struct leia_bg_capture_linux *c)
{
	if (c == NULL) {
		return;
	}

	// Tear down PipeWire first (stops the callback thread touching buffers).
	if (c->loop != NULL) {
		pw_thread_loop_lock(c->loop);
		if (c->stream != NULL) {
			pw_stream_disconnect(c->stream);
			pw_stream_destroy(c->stream);
			c->stream = NULL;
		}
		if (c->core != NULL) {
			pw_core_disconnect(c->core);
			c->core = NULL;
		}
		if (c->context != NULL) {
			pw_context_destroy(c->context);
			c->context = NULL;
		}
		pw_thread_loop_unlock(c->loop);
		pw_thread_loop_stop(c->loop);
		pw_thread_loop_destroy(c->loop);
		c->loop = NULL;
	}

	// Destroy all cached Vulkan resources (safe: pw thread is stopped).
	for (uint32_t i = 0; i < DXR_MAX_BUFFERS; i++) {
		destroy_buffer_slot(c, i);
	}

	// D-Bus teardown. Stop the session explicitly; closing the connection is
	// ALSO what releases our capture exclusion in the extension and what makes
	// mutter drop the session if Stop never arrives (crash-safe either way).
	if (c->dbus != NULL) {
		if (c->session_path != NULL) {
			leia_mutter_screencast_stop(c->dbus, c->session_path);
		}
		dbus_connection_close(c->dbus);
		dbus_connection_unref(c->dbus);
		c->dbus = NULL;
	}

	if (c->preview_lock_inited) {
		pthread_mutex_destroy(&c->preview_lock);
	}
	free(c->pp_back.px);
	free(c->pp_mid.px);
	free(c->pp_front.px);
	free(c->preview_out);
	free(c->session_path);
	free(c->stream_path);
	free(c);
}

#else // !DXR_LEIA_HAVE_PIPEWIRE

struct leia_bg_capture_linux *
leia_bg_capture_linux_create(struct vk_bundle *vk, uint32_t panel_px_w, uint32_t panel_px_h)
{
	(void)vk; (void)panel_px_w; (void)panel_px_h;
	U_LOG_W("leia_bg_capture_linux: built without libpipewire-0.3 / dbus-1 — desktop "
	        "capture unavailable; transparency falls back to silhouette intersection (runtime#757)");
	return NULL;
}
VkImageView leia_bg_capture_linux_get_view(struct leia_bg_capture_linux *c) { (void)c; return VK_NULL_HANDLE; }
void leia_bg_capture_linux_get_size(struct leia_bg_capture_linux *c, uint32_t *out_width, uint32_t *out_height)
{ (void)c; if (out_width) *out_width = 0; if (out_height) *out_height = 0; }
bool leia_bg_capture_linux_poll(struct leia_bg_capture_linux *c, VkCommandBuffer cmd, int32_t win_x, int32_t win_y,
                                uint32_t win_w, uint32_t win_h, float o[2], float e[2])
{ (void)c; (void)cmd; (void)win_x; (void)win_y; (void)win_w; (void)win_h; (void)o; (void)e; return false; }
#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
bool leia_bg_capture_linux_get_preview(struct leia_bg_capture_linux *c, struct xrt_dp_background_preview *out)
{ (void)c; (void)out; return false; }
#endif
bool leia_bg_capture_linux_wants_restart(struct leia_bg_capture_linux *c) { (void)c; return false; }
void leia_bg_capture_linux_destroy(struct leia_bg_capture_linux *c) { (void)c; }

#endif // DXR_LEIA_HAVE_PIPEWIRE
