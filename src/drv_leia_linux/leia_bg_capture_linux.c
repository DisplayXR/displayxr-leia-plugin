// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*! @file  @brief Linux desktop background capture (portal/PipeWire → dma-buf) — runtime#757. @ingroup drv_leia_linux */

#include "leia_bg_capture_linux.h"
#include "util/u_logging.h"
#include <stdlib.h>
#include <string.h>

#ifdef DXR_LEIA_HAVE_PIPEWIRE

/*
 * Implementation notes:
 *   - Run-tested only on the Intel Arc / Ubuntu-24.04 bring-up box; this is
 *     hardware bring-up code and cannot be exercised in CI.
 *   - Modifier negotiation is first-cut: we advertise DRM_FORMAT_MOD_LINEAR +
 *     DRM_FORMAT_MOD_INVALID and let the compositor pick. TODO: narrow the
 *     advertised modifier set to those the Vulkan driver reports as importable
 *     via VkDrmFormatModifierPropertiesListEXT.
 *   - The per-window background UV sub-rect is a TODO: poll() currently returns
 *     the full monitor mapping (origin 0,0 extent 1,1) because the window size
 *     is not threaded into this module yet — only the window's top-left is.
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

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

/*! One imported PipeWire dma-buf, cached as a Vulkan-sampleable image. */
struct dxr_bg_buffer {
	bool imported;             //!< slot holds a live VkImage
	VkImage image;             //!< imported dma-buf backing image
	VkDeviceMemory memory;     //!< memory imported from the dup'd fd
	VkImageView view;          //!< SHADER_READ_ONLY view
};

/*! Linux desktop background-capture context. */
struct leia_bg_capture_linux {
	struct vk_bundle *vk;      //!< borrowed; not owned

	// Loaded dynamically (not in the dispatch table).
	PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties; //!< dma-buf mem-type query

	// Window anchor (screen-space top-left of the app window).
	int32_t window_screen_left; //!< app window left edge, screen pixels
	int32_t window_screen_top;  //!< app window top edge, screen pixels

	// D-Bus / portal.
	DBusConnection *dbus;      //!< private session-bus connection
	char *sender_token;        //!< unique-name-derived request-path token
	char *session_handle;      //!< portal ScreenCast session object path
	bool match_added;          //!< Request::Response match installed
	uint32_t token_seq;        //!< monotonic unique-token counter

	// Captured monitor geometry (screen pixels).
	int32_t mon_x;             //!< monitor origin x
	int32_t mon_y;             //!< monitor origin y
	int32_t mon_w;             //!< monitor width
	int32_t mon_h;             //!< monitor height

	// PipeWire.
	int pw_fd;                 //!< fd from OpenPipeWireRemote
	uint32_t node_id;          //!< stream node id from Start()
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

	// Buffer cache (indexed by pw buffer id).
	struct dxr_bg_buffer buffers[DXR_MAX_BUFFERS];

	// Latest ready buffer index; written by pw thread, read by render thread.
	_Atomic int current_buffer; //!< -1 == none
	VkImageView current_view;   //!< last view poll() selected (render thread only)
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
	memset(&c->buffers[slot], 0, sizeof(c->buffers[slot]));
}

// ---------------------------------------------------------------------------
// D-Bus / xdg-desktop-portal ScreenCast handshake
// ---------------------------------------------------------------------------

#define PORTAL_BUS "org.freedesktop.portal.Desktop"
#define PORTAL_OBJ "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST_IFACE "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQUEST_IFACE "org.freedesktop.portal.Request"

static void
make_unique_token(struct leia_bg_capture_linux *c, char *out, size_t out_len)
{
	snprintf(out, out_len, "dxr%u", c->token_seq++);
}

/*! Append `handle_token`/`session_handle_token` (or just handle_token). */
static void
append_string_variant(DBusMessageIter *dict, const char *key, const char *value)
{
	DBusMessageIter entry, variant;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(dict, &entry);
}

static void
append_uint32_variant(DBusMessageIter *dict, const char *key, uint32_t value)
{
	DBusMessageIter entry, variant;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(dict, &entry);
}

static void
append_bool_variant(DBusMessageIter *dict, const char *key, dbus_bool_t value)
{
	DBusMessageIter entry, variant;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(dict, &entry);
}

/*!
 * Build the expected `/org/freedesktop/portal/desktop/request/<sender>/<token>`
 * object path for a given handle token. Caller frees.
 */
static char *
expected_request_path(struct leia_bg_capture_linux *c, const char *token)
{
	size_t n = strlen(PORTAL_OBJ) + strlen("/request/") + strlen(c->sender_token) +
	           1 + strlen(token) + 1;
	char *path = malloc(n);
	if (path == NULL) {
		return NULL;
	}
	snprintf(path, n, "%s/request/%s/%s", PORTAL_OBJ, c->sender_token, token);
	return path;
}

/*!
 * Pump the bus until the Response signal for @p request_path arrives.
 * Returns the signal message (caller unrefs) with @p *out_code set, or NULL on
 * error/timeout.
 */
static DBusMessage *
wait_for_response(struct leia_bg_capture_linux *c, const char *request_path,
                  uint32_t *out_code)
{
	// Bounded spin so a wedged portal can't hang bring-up forever.
	const int max_iterations = 6000; // ~60s at 10ms slices
	for (int i = 0; i < max_iterations; i++) {
		if (!dbus_connection_read_write_dispatch(c->dbus, 10)) {
			U_LOG_W("leia_bg_capture_linux: dbus connection closed while waiting");
			return NULL;
		}
		DBusMessage *msg;
		while ((msg = dbus_connection_pop_message(c->dbus)) != NULL) {
			if (dbus_message_is_signal(msg, PORTAL_REQUEST_IFACE, "Response") &&
			    dbus_message_get_path(msg) != NULL &&
			    strcmp(dbus_message_get_path(msg), request_path) == 0) {
				DBusMessageIter it;
				if (!dbus_message_iter_init(msg, &it) ||
				    dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) {
					U_LOG_W("leia_bg_capture_linux: malformed Response signal");
					dbus_message_unref(msg);
					return NULL;
				}
				dbus_message_iter_get_basic(&it, out_code);
				return msg; // caller inspects the trailing a{sv} results
			}
			dbus_message_unref(msg);
		}
	}
	U_LOG_W("leia_bg_capture_linux: timed out waiting for portal Response");
	return NULL;
}

/*!
 * Locate the a{sv} `results` iterator inside a Response signal (2nd arg).
 * Returns true and sets @p out_dict to the array iterator on success.
 */
static bool
response_results_iter(DBusMessage *msg, DBusMessageIter *out_dict)
{
	DBusMessageIter it;
	if (!dbus_message_iter_init(msg, &it)) {
		return false;
	}
	// skip the leading response code (u)
	if (!dbus_message_iter_next(&it)) {
		return false;
	}
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
		return false;
	}
	dbus_message_iter_recurse(&it, out_dict);
	return true;
}

/*!
 * Scan an a{sv} dict for a string-valued key. Returns a strdup (caller frees)
 * or NULL if absent.
 */
static char *
dict_get_string(DBusMessageIter dict, const char *want_key)
{
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry;
		dbus_message_iter_recurse(&dict, &entry);
		const char *key = NULL;
		dbus_message_iter_get_basic(&entry, &key);
		if (key != NULL && strcmp(key, want_key) == 0) {
			dbus_message_iter_next(&entry);
			if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
				DBusMessageIter var;
				dbus_message_iter_recurse(&entry, &var);
				if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
					const char *val = NULL;
					dbus_message_iter_get_basic(&var, &val);
					return val ? strdup(val) : NULL;
				}
			}
			return NULL;
		}
		dbus_message_iter_next(&dict);
	}
	return NULL;
}

/*!
 * Parse the `streams` a(ua{sv}) result: first stream's node id + position/size.
 * Returns true on success.
 */
static bool
parse_streams(struct leia_bg_capture_linux *c, DBusMessageIter dict)
{
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry;
		dbus_message_iter_recurse(&dict, &entry);
		const char *key = NULL;
		dbus_message_iter_get_basic(&entry, &key);
		if (key != NULL && strcmp(key, "streams") == 0) {
			dbus_message_iter_next(&entry);
			if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) {
				return false;
			}
			DBusMessageIter var;
			dbus_message_iter_recurse(&entry, &var);
			if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_ARRAY) {
				return false;
			}
			DBusMessageIter streams;
			dbus_message_iter_recurse(&var, &streams);
			if (dbus_message_iter_get_arg_type(&streams) != DBUS_TYPE_STRUCT) {
				U_LOG_W("leia_bg_capture_linux: no streams in Start() result");
				return false;
			}
			// First stream: (u node_id, a{sv} props)
			DBusMessageIter s;
			dbus_message_iter_recurse(&streams, &s);
			if (dbus_message_iter_get_arg_type(&s) != DBUS_TYPE_UINT32) {
				return false;
			}
			dbus_message_iter_get_basic(&s, &c->node_id);
			dbus_message_iter_next(&s);
			if (dbus_message_iter_get_arg_type(&s) == DBUS_TYPE_ARRAY) {
				DBusMessageIter props;
				dbus_message_iter_recurse(&s, &props);
				while (dbus_message_iter_get_arg_type(&props) ==
				       DBUS_TYPE_DICT_ENTRY) {
					DBusMessageIter pe;
					dbus_message_iter_recurse(&props, &pe);
					const char *pk = NULL;
					dbus_message_iter_get_basic(&pe, &pk);
					dbus_message_iter_next(&pe);
					if (pk != NULL &&
					    dbus_message_iter_get_arg_type(&pe) == DBUS_TYPE_VARIANT) {
						DBusMessageIter pv;
						dbus_message_iter_recurse(&pe, &pv);
						if (dbus_message_iter_get_arg_type(&pv) ==
						    DBUS_TYPE_STRUCT) {
							DBusMessageIter tup;
							dbus_message_iter_recurse(&pv, &tup);
							int32_t a = 0, b = 0;
							dbus_message_iter_get_basic(&tup, &a);
							dbus_message_iter_next(&tup);
							dbus_message_iter_get_basic(&tup, &b);
							if (strcmp(pk, "position") == 0) {
								c->mon_x = a;
								c->mon_y = b;
							} else if (strcmp(pk, "size") == 0) {
								c->mon_w = a;
								c->mon_h = b;
							}
						}
					}
					dbus_message_iter_next(&props);
				}
			}
			return true;
		}
		dbus_message_iter_next(&dict);
	}
	U_LOG_W("leia_bg_capture_linux: Start() result had no 'streams'");
	return false;
}

/*!
 * Send a portal method-call message that we've already populated, read the
 * synchronous reply carrying the Request object path, then block on that
 * request's Response signal. Returns the Response signal message (caller unrefs)
 * and asserts a zero response code, or NULL on any failure.
 */
static DBusMessage *
finish_portal_call(struct leia_bg_capture_linux *c, DBusMessage *call,
                   const char *handle_token)
{
	DBusError err;
	dbus_error_init(&err);

	DBusMessage *reply = dbus_connection_send_with_reply_and_block(c->dbus, call,
	                                                               5000, &err);
	dbus_message_unref(call);
	if (dbus_error_is_set(&err) || reply == NULL) {
		U_LOG_W("leia_bg_capture_linux: portal call failed: %s",
		        dbus_error_is_set(&err) ? err.message : "no reply");
		dbus_error_free(&err);
		if (reply) {
			dbus_message_unref(reply);
		}
		return NULL;
	}
	// Reply carries the request object path (o). We recompute the expected
	// path from the handle token to build the match; both must agree, but we
	// trust the deterministic form for the signal filter.
	dbus_message_unref(reply);

	char *req_path = expected_request_path(c, handle_token);
	if (req_path == NULL) {
		return NULL;
	}
	uint32_t code = 1;
	DBusMessage *resp = wait_for_response(c, req_path, &code);
	free(req_path);
	if (resp == NULL) {
		return NULL;
	}
	if (code != 0) {
		U_LOG_W("leia_bg_capture_linux: portal request denied/failed (code %u)", code);
		dbus_message_unref(resp);
		return NULL;
	}
	return resp;
}

static bool
portal_handshake(struct leia_bg_capture_linux *c)
{
	DBusError err;
	dbus_error_init(&err);

	c->dbus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (dbus_error_is_set(&err) || c->dbus == NULL) {
		U_LOG_W("leia_bg_capture_linux: no session bus: %s",
		        dbus_error_is_set(&err) ? err.message : "(null)");
		dbus_error_free(&err);
		return false;
	}
	// We manage the connection's lifetime; don't let dbus exit the process.
	dbus_connection_set_exit_on_disconnect(c->dbus, FALSE);

	// Derive the sender token: unique name after ':' with '.' → '_'.
	const char *unique = dbus_bus_get_unique_name(c->dbus);
	if (unique == NULL) {
		U_LOG_W("leia_bg_capture_linux: no unique bus name");
		return false;
	}
	const char *p = unique;
	if (*p == ':') {
		p++;
	}
	c->sender_token = strdup(p);
	if (c->sender_token == NULL) {
		return false;
	}
	for (char *q = c->sender_token; *q; q++) {
		if (*q == '.') {
			*q = '_';
		}
	}

	// Match Request::Response signals.
	dbus_bus_add_match(c->dbus,
	                   "type='signal',interface='" PORTAL_REQUEST_IFACE
	                   "',member='Response'",
	                   &err);
	if (dbus_error_is_set(&err)) {
		U_LOG_W("leia_bg_capture_linux: add_match failed: %s", err.message);
		dbus_error_free(&err);
		return false;
	}
	dbus_connection_flush(c->dbus);
	c->match_added = true;

	char htok[32];
	char stok[32];

	// --- CreateSession ---
	{
		make_unique_token(c, htok, sizeof htok);
		make_unique_token(c, stok, sizeof stok);
		DBusMessage *call = dbus_message_new_method_call(
		    PORTAL_BUS, PORTAL_OBJ, PORTAL_SCREENCAST_IFACE, "CreateSession");
		if (call == NULL) {
			return false;
		}
		DBusMessageIter args, dict;
		dbus_message_iter_init_append(call, &args);
		dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
		append_string_variant(&dict, "handle_token", htok);
		append_string_variant(&dict, "session_handle_token", stok);
		dbus_message_iter_close_container(&args, &dict);

		DBusMessage *resp = finish_portal_call(c, call, htok);
		if (resp == NULL) {
			return false;
		}
		DBusMessageIter results;
		if (!response_results_iter(resp, &results)) {
			U_LOG_W("leia_bg_capture_linux: CreateSession: no results");
			dbus_message_unref(resp);
			return false;
		}
		c->session_handle = dict_get_string(results, "session_handle");
		dbus_message_unref(resp);
		if (c->session_handle == NULL) {
			U_LOG_W("leia_bg_capture_linux: CreateSession: no session_handle");
			return false;
		}
	}

	// --- SelectSources ---
	{
		make_unique_token(c, htok, sizeof htok);
		DBusMessage *call = dbus_message_new_method_call(
		    PORTAL_BUS, PORTAL_OBJ, PORTAL_SCREENCAST_IFACE, "SelectSources");
		if (call == NULL) {
			return false;
		}
		DBusMessageIter args, dict;
		dbus_message_iter_init_append(call, &args);
		const char *sh = c->session_handle;
		dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
		dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
		append_string_variant(&dict, "handle_token", htok);
		append_uint32_variant(&dict, "types", 1u);      // MONITOR
		append_bool_variant(&dict, "multiple", FALSE);
		append_uint32_variant(&dict, "cursor_mode", 2u); // embedded
		dbus_message_iter_close_container(&args, &dict);

		DBusMessage *resp = finish_portal_call(c, call, htok);
		if (resp == NULL) {
			return false;
		}
		dbus_message_unref(resp);
	}

	// --- Start ---
	{
		make_unique_token(c, htok, sizeof htok);
		DBusMessage *call = dbus_message_new_method_call(
		    PORTAL_BUS, PORTAL_OBJ, PORTAL_SCREENCAST_IFACE, "Start");
		if (call == NULL) {
			return false;
		}
		DBusMessageIter args, dict;
		dbus_message_iter_init_append(call, &args);
		const char *sh = c->session_handle;
		const char *parent = "";
		dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
		dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
		dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
		append_string_variant(&dict, "handle_token", htok);
		dbus_message_iter_close_container(&args, &dict);

		DBusMessage *resp = finish_portal_call(c, call, htok);
		if (resp == NULL) {
			return false;
		}
		DBusMessageIter results;
		if (!response_results_iter(resp, &results)) {
			U_LOG_W("leia_bg_capture_linux: Start: no results");
			dbus_message_unref(resp);
			return false;
		}
		bool ok = parse_streams(c, results);
		dbus_message_unref(resp);
		if (!ok) {
			return false;
		}
	}

	// --- OpenPipeWireRemote ---
	{
		DBusMessage *call = dbus_message_new_method_call(
		    PORTAL_BUS, PORTAL_OBJ, PORTAL_SCREENCAST_IFACE, "OpenPipeWireRemote");
		if (call == NULL) {
			return false;
		}
		DBusMessageIter args, dict;
		dbus_message_iter_init_append(call, &args);
		const char *sh = c->session_handle;
		dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
		dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
		dbus_message_iter_close_container(&args, &dict);

		DBusMessage *reply = dbus_connection_send_with_reply_and_block(
		    c->dbus, call, 5000, &err);
		dbus_message_unref(call);
		if (dbus_error_is_set(&err) || reply == NULL) {
			U_LOG_W("leia_bg_capture_linux: OpenPipeWireRemote failed: %s",
			        dbus_error_is_set(&err) ? err.message : "no reply");
			dbus_error_free(&err);
			if (reply) {
				dbus_message_unref(reply);
			}
			return false;
		}
		DBusMessageIter it;
		if (!dbus_message_iter_init(reply, &it) ||
		    dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UNIX_FD) {
			U_LOG_W("leia_bg_capture_linux: OpenPipeWireRemote: no fd");
			dbus_message_unref(reply);
			return false;
		}
		int fd = -1;
		dbus_message_iter_get_basic(&it, &fd);
		c->pw_fd = fd; // owned by us; dbus dup'd it into the message
		dbus_message_unref(reply);
		if (c->pw_fd < 0) {
			U_LOG_W("leia_bg_capture_linux: OpenPipeWireRemote: bad fd");
			return false;
		}
	}

	// If the portal did not report geometry, fall back to origin 0,0; size
	// is filled in later from the negotiated video format.
	U_LOG_I("leia_bg_capture_linux: portal ok — node=%u monitor=%d,%d %dx%d",
	        c->node_id, c->mon_x, c->mon_y, c->mon_w, c->mon_h);
	return true;
}

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

	// spa_video_info_raw carries the negotiated DRM modifier directly in
	// .modifier (this SPA has no "modifier present" flag). We force LINEAR in
	// the advertised format (see build_format_params), so this is LINEAR (0)
	// today; honour whatever was negotiated for forward-compat.
	c->modifier = info.modifier; // DRM_FORMAT_MOD_LINEAR == 0

	// If the portal never gave us a monitor size, adopt the negotiated one.
	if (c->mon_w <= 0 || c->mon_h <= 0) {
		c->mon_w = (int32_t)c->width;
		c->mon_h = (int32_t)c->height;
	}

	c->have_format = true;
	U_LOG_I("leia_bg_capture_linux: format %ux%u vkfmt=%d mod=0x%llx",
	        c->width, c->height, (int)c->vk_format,
	        (unsigned long long)c->modifier);

	// Reply with buffer params: request dma-buf-capable buffers.
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *params[1];
	params[0] = spa_pod_builder_add_object(
	    &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
	    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, DXR_MAX_BUFFERS),
	    SPA_PARAM_BUFFERS_dataType,
	    SPA_POD_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) |
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

	if (d->type != SPA_DATA_DmaBuf) {
		// Fallback data types are not imported into Vulkan; log & skip.
		// TODO: mmap MemFd/MemPtr into a staging upload if a compositor ever
		// refuses dma-buf. For bring-up we require dma-buf.
		U_LOG_W("leia_bg_capture_linux: buffer type %u is not dma-buf; skipping",
		        d->type);
		buffer->user_data = (void *)(uintptr_t)DXR_MAX_BUFFERS;
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
		// Publish the stable slot index; the VkImage import is fixed for the
		// buffer's lifetime, so the render thread can sample it after requeue.
		atomic_store(&c->current_buffer, (int)slot);
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

	// Canonical variadic object build (stable across SPA 0.2). First cut FORCES
	// DRM_FORMAT_MOD_LINEAR so the dma-buf import is correct-by-construction
	// (LINEAR always imports right; the compositor blits if its native buffer is
	// tiled). TODO(hardware bring-up): negotiate the driver-tiled modifier set
	// via the DONT_FIXATE two-pass dance to drop that blit on the Arc box.
	if (n < max_params) {
		params[n++] = spa_pod_builder_add_object(
		    b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,          //
		    SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),   //
		    SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), //
		    SPA_FORMAT_VIDEO_format,
		    SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA,
		                           SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRx,
		                           SPA_VIDEO_FORMAT_RGBx),
		    SPA_FORMAT_VIDEO_modifier, SPA_POD_Long(DRM_FORMAT_MOD_LINEAR), //
		    SPA_FORMAT_VIDEO_size,
		    SPA_POD_CHOICE_RANGE_Rectangle(&size_def, &size_min, &size_max), //
		    SPA_FORMAT_VIDEO_framerate,
		    SPA_POD_CHOICE_RANGE_Fraction(&rate_def, &rate_min, &rate_max));
	}

	// Plain (MemFd/MemPtr) fallback — no modifier property. Used when the
	// compositor won't produce a LINEAR dma-buf; add_buffer log-and-skips these
	// until an mmap upload path exists (bring-up requires dma-buf).
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
		    SPA_POD_CHOICE_RANGE_Fraction(&rate_def, &rate_min, &rate_max));
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
	// pw_context_connect_fd takes ownership of pw_fd on success.
	c->core = pw_context_connect_fd(c->context, c->pw_fd, NULL, 0);
	if (c->core == NULL) {
		U_LOG_W("leia_bg_capture_linux: pw_context_connect_fd failed");
		goto unlock;
	}
	c->pw_fd = -1; // consumed by pw

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

	// dma-buf: do NOT map buffers (we import); AUTOCONNECT to the node.
	int r = pw_stream_connect(c->stream, PW_DIRECTION_INPUT, c->node_id,
	                          PW_STREAM_FLAG_AUTOCONNECT, params, (uint32_t)n_params);
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
leia_bg_capture_linux_create(struct vk_bundle *vk, int32_t window_screen_left,
                             int32_t window_screen_top)
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
	c->window_screen_left = window_screen_left;
	c->window_screen_top = window_screen_top;
	c->pw_fd = -1;
	c->vk_format = VK_FORMAT_UNDEFINED;
	c->token_seq = 1;
	atomic_store(&c->current_buffer, -1);

	// vkGetMemoryFdPropertiesKHR is not in the dispatch table.
	c->getMemoryFdProperties = (PFN_vkGetMemoryFdPropertiesKHR)
	    vk->vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdPropertiesKHR");
	if (c->getMemoryFdProperties == NULL) {
		U_LOG_W("leia_bg_capture_linux: vkGetMemoryFdPropertiesKHR unavailable");
		goto fail;
	}

	if (!portal_handshake(c)) {
		U_LOG_W("leia_bg_capture_linux: portal handshake failed");
		goto fail;
	}

	if (!pipewire_start(c)) {
		U_LOG_W("leia_bg_capture_linux: PipeWire start failed");
		goto fail;
	}

	U_LOG_I("leia_bg_capture_linux: capture started (window anchor %d,%d)",
	        window_screen_left, window_screen_top);
	return c;

fail:
	leia_bg_capture_linux_destroy(c);
	return NULL;
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

bool
leia_bg_capture_linux_poll(struct leia_bg_capture_linux *c, VkCommandBuffer cmd,
                           float out_bg_uv_origin[2], float out_bg_uv_extent[2])
{
	if (c == NULL) {
		return false;
	}

	int idx = atomic_load(&c->current_buffer);
	if (idx < 0 || idx >= DXR_MAX_BUFFERS || !c->buffers[idx].imported) {
		return false; // no frame yet — caller passes the raw atlas through
	}

	// If the window is not on the captured monitor, decline (raw pass-through).
	if (c->mon_w > 0 && c->mon_h > 0) {
		if (c->window_screen_left < c->mon_x ||
		    c->window_screen_top < c->mon_y ||
		    c->window_screen_left >= c->mon_x + c->mon_w ||
		    c->window_screen_top >= c->mon_y + c->mon_h) {
			return false;
		}
	}

	c->current_view = c->buffers[idx].view;

	// ACQUIRE barrier: the dma-buf is written out-of-band by the compositor, so
	// we take ownership from the FOREIGN queue and transition UNDEFINED ->
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
	    .image = c->buffers[idx].image,
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

	// TODO: compute the per-window background sub-rect once the window size is
	// threaded into this module. The correct mapping is:
	//   origin = ((win_left - mon_x)/mon_w, (win_top - mon_y)/mon_h)
	//   extent = (win_w/mon_w, win_h/mon_h)
	// For now we map the full captured monitor (window size unknown here).
	if (out_bg_uv_origin) {
		out_bg_uv_origin[0] = 0.0f;
		out_bg_uv_origin[1] = 0.0f;
	}
	if (out_bg_uv_extent) {
		out_bg_uv_extent[0] = 1.0f;
		out_bg_uv_extent[1] = 1.0f;
	}
	return true;
}

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

	// Close a not-yet-consumed PipeWire fd.
	if (c->pw_fd >= 0) {
		close(c->pw_fd);
		c->pw_fd = -1;
	}

	// D-Bus teardown.
	if (c->dbus != NULL) {
		if (c->match_added) {
			DBusError err;
			dbus_error_init(&err);
			dbus_bus_remove_match(c->dbus,
			                      "type='signal',interface='" PORTAL_REQUEST_IFACE
			                      "',member='Response'",
			                      &err);
			dbus_error_free(&err);
			c->match_added = false;
		}
		dbus_connection_close(c->dbus);
		dbus_connection_unref(c->dbus);
		c->dbus = NULL;
	}

	free(c->session_handle);
	free(c->sender_token);
	free(c);
}

#else // !DXR_LEIA_HAVE_PIPEWIRE

struct leia_bg_capture_linux *
leia_bg_capture_linux_create(struct vk_bundle *vk, int32_t window_screen_left, int32_t window_screen_top)
{
	(void)vk; (void)window_screen_left; (void)window_screen_top;
	U_LOG_W("leia_bg_capture_linux: built without libpipewire-0.3 / dbus-1 — desktop "
	        "capture unavailable; transparency uses the 2D-under backdrop only (runtime#757)");
	return NULL;
}
VkImageView leia_bg_capture_linux_get_view(struct leia_bg_capture_linux *c) { (void)c; return VK_NULL_HANDLE; }
void leia_bg_capture_linux_get_size(struct leia_bg_capture_linux *c, uint32_t *out_width, uint32_t *out_height)
{ (void)c; if (out_width) *out_width = 0; if (out_height) *out_height = 0; }
bool leia_bg_capture_linux_poll(struct leia_bg_capture_linux *c, VkCommandBuffer cmd, float o[2], float e[2])
{ (void)c; (void)cmd; (void)o; (void)e; return false; }
void leia_bg_capture_linux_destroy(struct leia_bg_capture_linux *c) { (void)c; }

#endif // DXR_LEIA_HAVE_PIPEWIRE
