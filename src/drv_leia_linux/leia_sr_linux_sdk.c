// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Track B implementation of the Linux weaver-backend seam
 *         (leia_sr_linux.h) over the real LeiaSR srSDK C API.
 *
 * SPIKE status: pinned to the PROTOTYPE package George shared on 2026-07-06
 * (leiasr-prototype-sdk.zip) — srSDK API 1.0.0 (SR_CURRENT_API_VERSION),
 * libLeiaSR_runtime.so BuildID fcf21021eeb277bac06fdb0e484bd4a8f31ad36b.
 * Re-pin to the merged sr-sdk-v* tag (LeiaInc/LeiaSR#53) before anything
 * ships. Selected with -DDXR_LEIA_LINUX_WEAVER=sdk (+ SRSDK_ROOT); the
 * default build stays on leia_sr_stub.c, so CI never needs the SDK.
 *
 * Design notes (contract §8 reconciliation drives every workaround here):
 *
 *  - ONE process-lifetime SR context (instance + eye tracker + system
 *    monitor + display + lens). Forced by the SDK: sense callbacks must be
 *    registered BEFORE srInitialize (no late attach), the seam's display
 *    query is callable pre-create, and srDestroyInstance joins SDK threads
 *    unboundedly — so the context outlives DP create/destroy cycles and is
 *    torn down at .so unload.
 *
 *  - The weave drives the SDK through the "framebuffer = 0, render pass
 *    already begun" path: we own a single-color-attachment VkRenderPass
 *    (returned from leiasr_lnx_get_render_pass), begin it with the caller's
 *    framebuffer (or an internally cached one), and let srWeaverWeave record
 *    its draw inside. Render-pass compatibility VERIFIED against the SDK
 *    source (LeiaSR v2-vulkan-weaver, vkweaver.cpp RenderPassCache): its
 *    pipeline pass is single color attachment @ outputFormat, samples 1,
 *    loadOp LOAD, COLOR_ATTACHMENT_OPTIMAL in/out, no depth, one subpass —
 *    identical shape to ours, so the passes are compatible by Vulkan §8.2.
 *    In fb=0 mode the weaver skips its own Begin/EndRenderPass entirely and
 *    just binds pipeline + draws. Escape hatch kept: DXR_LEIA_SR_FB_SDK=1
 *    hands the caller framebuffer to the SDK (it then begins its own pass).
 *
 *  - Tracking state is event-edge only in srSDK 1.0.0 (USER_FOUND/USER_LOST
 *    via the system monitor) — latched into atomics; is_tracking flips at
 *    raw face-loss (earlier than R-T4's grace-period preference). The weaver
 *    itself independently drops to a blit when its predicted eye pair
 *    collapses below 1 mm separation (vkweaver.cpp weave(): isTracking =
 *    eyeSeparation > 1) — the MANAGED loss lifecycle in action.
 *
 *  - srWeaverSetInputTextureVulkan's width/height are PER-VIEW — vendor-
 *    confirmed on LeiaSR#53 ("The size is for a single view, not SBS").
 *    The 1.0.0 implementation stores and never reads them (shader samples
 *    the SBS view with computed UVs), so this is contract hygiene for
 *    future SDK versions, not a behavior change.
 *
 *  - Windowless (window = 0) weaving is real: the legacy constructor maps
 *    window==NULL to constructedWithoutWindow=true, whose canWeaveInternal
 *    path ALWAYS weaves (given correction textures). On Linux the window
 *    screen-rect helper returns (0,0) unconditionally, so the lens phase
 *    anchors at the viewport offset in panel coordinates — exactly the
 *    display-scoped convention the DP feeds us.
 *
 * Also implements the two probe entry points from ../drv_leia/leia_interface.h
 * (`leiasr_probe_display` / `leiasr_get_probe_results`) that the shared
 * leia_device.c consumes — here they are a REAL probe (SR runtime reachable +
 * srDisplayIsValid), unlike the stub's canned cache.
 *
 * @author David Fattal
 * @ingroup drv_leia_linux
 */

#include "leia_sr_linux.h"

#include "leia_interface.h"

#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_logging.h"

#include <sr/sr.h>
#include <sr/sr_vk.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

DEBUG_GET_ONCE_BOOL_OPTION(sr_fb_sdk, "DXR_LEIA_SR_FB_SDK", false)

#define SDK_HALF_IPD_MM 31.5f /* nominal 63 mm IPD — R-T2 fallback pair */

/*! Log an SrResult once per call site. */
#define LOG_SR_ONCE(what, res)                                                                                         \
	do {                                                                                                           \
		static bool _logged;                                                                                   \
		if (!_logged) {                                                                                        \
			U_LOG_W("leia_sr_sdk: %s failed: %s", what, srResultToString(res));                            \
			_logged = true;                                                                                \
		}                                                                                                      \
	} while (0)


/*
 *
 * Process-lifetime SR context (singleton).
 *
 */

enum sr_ctx_state
{
	SR_CTX_UNINIT = 0,
	SR_CTX_READY,
	SR_CTX_FAILED,
};

struct sr_ctx
{
	enum sr_ctx_state state;
	int64_t last_fail_ns; //!< monotonic; ~1 s cooldown before re-attempting

	SrInstance instance;
	SrEyeTracker tracker; //!< may be NULL (no camera) — non-fatal
	SrSystemMonitor monitor;
	SrDisplay display;
	SrLens lens; //!< may be NULL — non-fatal, event atomic backs 3D state

	/* Latched system-monitor state (SDK thread → render thread). */
	atomic_bool user_present;
	atomic_bool device_ready;
	atomic_bool lens_on;
	atomic_bool display_info_dirty;
	atomic_uint_least64_t last_eye_time_us;

	/* Cached display query (guarded by g_ctx_lock). */
	bool display_info_valid;
	struct leiasr_lnx_display_info display_info;
};

static pthread_mutex_t g_ctx_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sr_ctx g_ctx = {
    .state = SR_CTX_UNINIT,
    .device_ready = true, /* assume ready until the monitor says otherwise */
};

static void SR_CALL
sr_ctx_on_eye_pair(const SrEyePair *pair, void *user_data)
{
	(void)user_data;
	/* SDK worker thread — atomic stores only. Positions always come from
	 * srWeaverGetPredictedEyePositions (R-T1 same-pair rule); this callback
	 * only tracks sample freshness for the timestamp out-param. */
	atomic_store(&g_ctx.last_eye_time_us, pair->timeUs);
}

static void SR_CALL
sr_ctx_on_system_event(const SrSystemEvent *event, void *user_data)
{
	(void)user_data;
	/* SDK worker thread — atomic stores + logging only (the message pointer
	 * dies when this returns; U_LOG formats it immediately). */
	switch (event->eventType) {
	case SR_EVENT_TYPE_USER_FOUND: atomic_store(&g_ctx.user_present, true); break;
	case SR_EVENT_TYPE_USER_LOST: atomic_store(&g_ctx.user_present, false); break;
	case SR_EVENT_TYPE_DEVICE_READY:
	case SR_EVENT_TYPE_SR_RESTORED: atomic_store(&g_ctx.device_ready, true); break;
	case SR_EVENT_TYPE_DEVICE_DISCONNECTED:
	case SR_EVENT_TYPE_SR_UNAVAILABLE: atomic_store(&g_ctx.device_ready, false); break;
	case SR_EVENT_TYPE_LENS_ON: atomic_store(&g_ctx.lens_on, true); break;
	case SR_EVENT_TYPE_LENS_OFF: atomic_store(&g_ctx.lens_on, false); break;
	case SR_EVENT_TYPE_DISPLAY_CONNECTED:
	case SR_EVENT_TYPE_DISPLAY_NOT_CONNECTED: atomic_store(&g_ctx.display_info_dirty, true); break;
	default: break;
	}
	U_LOG_I("leia_sr_sdk: system event %d%s%s", (int)event->eventType, event->message != NULL ? ": " : "",
	        event->message != NULL ? event->message : "");
}

static void SR_CALL
sr_ctx_on_sdk_log(SrLogLevel level, const char *message, void *user_data)
{
	(void)level;
	(void)user_data;
	U_LOG_I("srSDK: %s", message != NULL ? message : "");
}

static void
sr_ctx_teardown_locked(void)
{
	if (g_ctx.monitor != NULL) {
		srDestroySystemMonitor(g_ctx.monitor);
		g_ctx.monitor = NULL;
	}
	if (g_ctx.tracker != NULL) {
		srDestroyEyeTracker(g_ctx.tracker);
		g_ctx.tracker = NULL;
	}
	if (g_ctx.lens != NULL) {
		srDestroyLens(g_ctx.lens);
		g_ctx.lens = NULL;
	}
	if (g_ctx.display != NULL) {
		srDestroyDisplay(g_ctx.display);
		g_ctx.display = NULL;
	}
	if (g_ctx.instance != NULL) {
		/* Blocks joining SDK threads — unbounded if the service died
		 * (contract §8, R-W10 carried ask). Only reached at .so unload
		 * or init failure, never per-session. */
		srDestroyInstance(g_ctx.instance);
		g_ctx.instance = NULL;
	}
}

/*!
 * Lazily bring up the process-wide SR context, retrying srCreateInstance for
 * up to @p retry_budget_s while the SR runtime is unreachable (R-W1).
 * Returns the resulting seam code; SUCCESS ⟹ g_ctx.state == SR_CTX_READY.
 */
static enum leiasr_lnx_result
sr_ctx_ensure(double retry_budget_s)
{
	pthread_mutex_lock(&g_ctx_lock);
	if (g_ctx.state == SR_CTX_READY) {
		pthread_mutex_unlock(&g_ctx_lock);
		return LEIASR_LNX_SUCCESS;
	}
	/* Cooldown: headless boxes call the display query per frame — don't
	 * hammer srCreateInstance when a zero-budget attempt just failed. */
	if (g_ctx.state == SR_CTX_FAILED && retry_budget_s <= 0.0 &&
	    os_monotonic_get_ns() - g_ctx.last_fail_ns < 1000000000L) {
		pthread_mutex_unlock(&g_ctx_lock);
		return LEIASR_LNX_ERROR_SERVICE_UNAVAILABLE;
	}

	enum leiasr_lnx_result out = LEIASR_LNX_ERROR_FAILED;
	const int64_t start_ns = os_monotonic_get_ns();
	const int64_t budget_ns = (int64_t)(retry_budget_s * 1e9);

	SrResult res;
	for (;;) {
		SrInstanceCreateInfo ci = SrInstanceCreateInfo(.applicationName = "DisplayXR-LeiaSR");
		res = srCreateInstance(&ci, &g_ctx.instance);
		if (SR_SUCCEEDED(res)) {
			break;
		}
		if (res != SR_ERROR_RUNTIME_UNAVAILABLE || os_monotonic_get_ns() - start_ns >= budget_ns) {
			if (res == SR_ERROR_RUNTIME_UNAVAILABLE) {
				U_LOG_W("leia_sr_sdk: SR runtime unavailable after %.1f s (%s; loader: %s)",
				        retry_budget_s, srResultToString(res), srGetLastLoaderError());
				out = LEIASR_LNX_ERROR_SERVICE_UNAVAILABLE;
			} else {
				U_LOG_W("leia_sr_sdk: srCreateInstance failed: %s", srResultToString(res));
			}
			goto fail;
		}
		os_nanosleep(250 * 1000 * 1000); /* 250 ms between connect attempts (R-W1) */
	}

	srSetLogCallback(g_ctx.instance, sr_ctx_on_sdk_log, NULL);

	/* Senses + callbacks BEFORE srInitialize (SDK lifecycle rule). */
	SrEyeTrackerCreateInfo tci = SrEyeTrackerCreateInfo(.enablePrediction = SR_TRUE);
	res = srCreateEyeTracker(g_ctx.instance, &tci, &g_ctx.tracker);
	if (SR_FAILED(res)) {
		/* No camera is non-fatal: the weaver still weaves, is_tracking
		 * stays false, poses fall back to nominal (R-T2). */
		U_LOG_W("leia_sr_sdk: srCreateEyeTracker failed (%s) — untracked weaving", srResultToString(res));
		g_ctx.tracker = NULL;
	} else {
		srEyeTrackerAddCallback(g_ctx.tracker, sr_ctx_on_eye_pair, NULL);
	}

	SrSystemMonitorCreateInfo mci = SrSystemMonitorCreateInfo();
	res = srCreateSystemMonitor(g_ctx.instance, &mci, &g_ctx.monitor);
	if (SR_SUCCEEDED(res)) {
		srSystemMonitorAddCallback(g_ctx.monitor, sr_ctx_on_system_event, NULL);
	} else {
		U_LOG_W("leia_sr_sdk: srCreateSystemMonitor failed (%s) — no tracking-state events",
		        srResultToString(res));
		g_ctx.monitor = NULL;
	}

	res = srInitialize(g_ctx.instance);
	if (SR_FAILED(res)) {
		U_LOG_W("leia_sr_sdk: srInitialize failed: %s", srResultToString(res));
		out = res == SR_ERROR_RUNTIME_UNAVAILABLE ? LEIASR_LNX_ERROR_SERVICE_UNAVAILABLE
		                                          : LEIASR_LNX_ERROR_FAILED;
		goto fail;
	}

	SrDisplayCreateInfo dci = SrDisplayCreateInfo(.window = 0); /* primary SR display */
	res = srCreateDisplay(g_ctx.instance, &dci, &g_ctx.display);
	if (SR_FAILED(res)) {
		U_LOG_W("leia_sr_sdk: srCreateDisplay failed: %s", srResultToString(res));
		g_ctx.display = NULL;
	}

	SrLensCreateInfo lci = SrLensCreateInfo();
	res = srCreateLens(g_ctx.instance, &lci, &g_ctx.lens);
	if (SR_FAILED(res)) {
		U_LOG_I("leia_sr_sdk: srCreateLens unavailable (%s) — 3D state from lens events only",
		        srResultToString(res));
		g_ctx.lens = NULL;
	}

	char version[64] = {0};
	srGetRuntimeVersion(g_ctx.instance, version, sizeof(version));
	U_LOG_W("leia_sr_sdk: REAL srSDK weaver backend active (runtime %s, API 1.0.0 prototype pin)", version);

	g_ctx.state = SR_CTX_READY;
	atomic_store(&g_ctx.display_info_dirty, true);
	pthread_mutex_unlock(&g_ctx_lock);
	return LEIASR_LNX_SUCCESS;

fail:
	sr_ctx_teardown_locked();
	g_ctx.state = SR_CTX_FAILED;
	g_ctx.last_fail_ns = os_monotonic_get_ns();
	pthread_mutex_unlock(&g_ctx_lock);
	return out;
}

__attribute__((destructor)) static void
sr_ctx_shutdown(void)
{
	pthread_mutex_lock(&g_ctx_lock);
	if (g_ctx.state == SR_CTX_READY) {
		sr_ctx_teardown_locked();
		g_ctx.state = SR_CTX_UNINIT;
	}
	pthread_mutex_unlock(&g_ctx_lock);
}

/*! Refresh the cached display query from the SDK. Caller holds g_ctx_lock. */
static bool
sr_ctx_refresh_display_info_locked(void)
{
	if (g_ctx.display == NULL) {
		g_ctx.display_info_valid = false;
		return false;
	}

	SrBool32 valid = SR_FALSE;
	if (SR_FAILED(srDisplayIsValid(g_ctx.display, &valid)) || valid == SR_FALSE) {
		g_ctx.display_info_valid = false;
		return false;
	}

	struct leiasr_lnx_display_info info = {.valid = true};

	float w_cm = 0.0f, h_cm = 0.0f;
	int32_t px_w = 0, px_h = 0, rec_w = 0, rec_h = 0;
	SrRecti rect = {0};
	float nx = 0.0f, ny = 0.0f, nz = 0.0f;

	if (SR_FAILED(srDisplayGetPhysicalSize(g_ctx.display, &w_cm, &h_cm)) ||
	    SR_FAILED(srDisplayGetPhysicalResolution(g_ctx.display, &px_w, &px_h)) ||
	    SR_FAILED(srDisplayGetLocation(g_ctx.display, &rect)) ||
	    SR_FAILED(srDisplayGetRecommendedTextureSize(g_ctx.display, &rec_w, &rec_h)) ||
	    SR_FAILED(srDisplayGetDefaultViewingPosition(g_ctx.display, &nx, &ny, &nz))) {
		/* R-D4: headless-soft — a display that vanished mid-query. */
		g_ctx.display_info_valid = false;
		return false;
	}

	info.width_m = w_cm / 100.0f;
	info.height_m = h_cm / 100.0f;
	info.pixel_width = (uint32_t)px_w;
	info.pixel_height = (uint32_t)px_h;
	info.screen_left = (int32_t)rect.left;
	info.screen_top = (int32_t)rect.top;
	/* Per-view: srDisplayGetRecommendedTextureSize wraps the same legacy
	 * getRecommendedViewsTextureWidth/Height getter the Windows arm already
	 * consumes and logs as "per eye" (leia_sr_d3d11.cpp) — settled. */
	info.recommended_view_width = (uint32_t)rec_w;
	info.recommended_view_height = (uint32_t)rec_h;
	/* No refresh getter in srSDK 1.0.0 (contract §8 R-D1 carried ask). */
	info.refresh_mhz = 60000;
	info.nominal_viewer_x_m = nx / 1000.0f;
	info.nominal_viewer_y_m = ny / 1000.0f;
	info.nominal_viewer_z_m = nz / 1000.0f;

	static bool logged;
	if (!logged) {
		U_LOG_I("leia_sr_sdk: display %dx%d px, %.1fx%.1f cm, at (%d,%d), recommended %dx%d "
		        "per view, nominal viewer Z %.0f mm, refresh HARDCODED 60 Hz",
		        px_w, px_h, w_cm, h_cm, (int)rect.left, (int)rect.top, rec_w, rec_h, nz);
		logged = true;
	}

	g_ctx.display_info = info;
	g_ctx.display_info_valid = true;
	atomic_store(&g_ctx.display_info_dirty, false);
	return true;
}


/*
 *
 * Probe entry points (leia_interface.h) — real probe, unlike the stub.
 *
 */

static struct leiasr_probe_result g_probe_cache;
static bool g_probe_cached = false;

bool
leiasr_probe_display(double timeout_seconds)
{
	if (sr_ctx_ensure(timeout_seconds) != LEIASR_LNX_SUCCESS) {
		g_probe_cache = (struct leiasr_probe_result){.hw_found = false};
		g_probe_cached = true;
		return false;
	}

	pthread_mutex_lock(&g_ctx_lock);
	bool found = g_ctx.display_info_valid || sr_ctx_refresh_display_info_locked();
	const struct leiasr_lnx_display_info *di = &g_ctx.display_info;
	g_probe_cache = (struct leiasr_probe_result){
	    .hw_found = found,
	    .pixel_w = found ? di->pixel_width : 0,
	    .pixel_h = found ? di->pixel_height : 0,
	    .refresh_hz = found ? di->refresh_mhz / 1000.0f : 0.0f,
	    .display_w_m = found ? di->width_m : 0.0f,
	    .display_h_m = found ? di->height_m : 0.0f,
	    .nominal_z_m = found ? di->nominal_viewer_z_m : 0.0f,
	};
	g_probe_cached = true;
	pthread_mutex_unlock(&g_ctx_lock);
	return found;
}

bool
leiasr_get_probe_results(struct leiasr_probe_result *out)
{
	if (!g_probe_cached || out == NULL) {
		return false;
	}
	*out = g_probe_cache;
	return true;
}


/*
 *
 * Per-DP weaver state.
 *
 */

#define FB_CACHE_SIZE 8

struct sdk_fb_cache_entry
{
	VkImage image;
	uint32_t width, height;
	VkFormat format;
	VkImageView view;
	VkFramebuffer fb;
};

struct leiasr_lnx
{
	struct leiasr_lnx_create_info info;
	SrWeaver weaver;

	VkRenderPass render_pass; //!< backend-owned, single color attachment (loadOp LOAD)
	VkFormat render_pass_format;

	/* Internal view+fb cache for the output->framebuffer == NULL case,
	 * flushed by leiasr_lnx_output_invalidated (device idle guaranteed). */
	struct sdk_fb_cache_entry fb_cache[FB_CACHE_SIZE];
	uint32_t fb_cache_next;

	struct leiasr_lnx_eye_pair_mm last_good_pair;
	bool have_last_good;
};

static VkResult
sdk_create_render_pass(struct leiasr_lnx *lnx, VkFormat format)
{
	/* loadOp LOAD + COLOR_ATTACHMENT_OPTIMAL in/out: the weave lands inside
	 * a viewport of a target whose other regions (mixed 2D/3D canvas) must
	 * survive, and the DP-side convention keeps targets in attachment
	 * layout around process_atlas. */
	VkAttachmentDescription att = {
	    .format = format,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference att_ref = {
	    .attachment = 0,
	    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription subpass = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &att_ref,
	};
	VkSubpassDependency deps[2] = {
	    {
	        .srcSubpass = VK_SUBPASS_EXTERNAL,
	        .dstSubpass = 0,
	        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    },
	    {
	        .srcSubpass = 0,
	        .dstSubpass = VK_SUBPASS_EXTERNAL,
	        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
	    },
	};
	VkRenderPassCreateInfo rp_info = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &att,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	    .dependencyCount = 2,
	    .pDependencies = deps,
	};
	lnx->render_pass_format = format;
	return vkCreateRenderPass(lnx->info.device, &rp_info, NULL, &lnx->render_pass);
}

static void
sdk_fb_cache_flush(struct leiasr_lnx *lnx)
{
	for (uint32_t i = 0; i < FB_CACHE_SIZE; i++) {
		struct sdk_fb_cache_entry *e = &lnx->fb_cache[i];
		if (e->fb != VK_NULL_HANDLE) {
			vkDestroyFramebuffer(lnx->info.device, e->fb, NULL);
		}
		if (e->view != VK_NULL_HANDLE) {
			vkDestroyImageView(lnx->info.device, e->view, NULL);
		}
		*e = (struct sdk_fb_cache_entry){0};
	}
	lnx->fb_cache_next = 0;
}

static VkFramebuffer
sdk_fb_cache_get(struct leiasr_lnx *lnx, const struct leiasr_lnx_weave_output *output)
{
	for (uint32_t i = 0; i < FB_CACHE_SIZE; i++) {
		struct sdk_fb_cache_entry *e = &lnx->fb_cache[i];
		if (e->fb != VK_NULL_HANDLE && e->image == output->image && e->width == output->width &&
		    e->height == output->height && e->format == output->format) {
			return e->fb;
		}
	}

	struct sdk_fb_cache_entry *e = &lnx->fb_cache[lnx->fb_cache_next];
	lnx->fb_cache_next = (lnx->fb_cache_next + 1) % FB_CACHE_SIZE;
	if (e->fb != VK_NULL_HANDLE) {
		vkDestroyFramebuffer(lnx->info.device, e->fb, NULL);
		e->fb = VK_NULL_HANDLE;
	}
	if (e->view != VK_NULL_HANDLE) {
		vkDestroyImageView(lnx->info.device, e->view, NULL);
		e->view = VK_NULL_HANDLE;
	}

	VkImageViewCreateInfo view_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = output->image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = output->format,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	if (vkCreateImageView(lnx->info.device, &view_info, NULL, &e->view) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	VkFramebufferCreateInfo fb_info = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = lnx->render_pass,
	    .attachmentCount = 1,
	    .pAttachments = &e->view,
	    .width = output->width,
	    .height = output->height,
	    .layers = 1,
	};
	if (vkCreateFramebuffer(lnx->info.device, &fb_info, NULL, &e->fb) != VK_SUCCESS) {
		vkDestroyImageView(lnx->info.device, e->view, NULL);
		e->view = VK_NULL_HANDLE;
		return VK_NULL_HANDLE;
	}
	e->image = output->image;
	e->width = output->width;
	e->height = output->height;
	e->format = output->format;
	return e->fb;
}

/*!
 * Passthrough SBS blit — same body as the Track A stub. Used when the grid
 * isn't the 2×1 srSDK 1.0.0 supports (no tile-grid param — contract §8 R-W4).
 */
static void
sdk_passthrough_blit(VkCommandBuffer cmd_buffer,
                     const struct leiasr_lnx_weave_input *input,
                     const struct leiasr_lnx_weave_output *output,
                     VkRect2D viewport)
{
	const int32_t src_w = (int32_t)(input->view_width * input->tile_columns);
	const int32_t src_h = (int32_t)(input->view_height * input->tile_rows);

	VkImageMemoryBarrier pre[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
	        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = input->atlas_image,
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
	        .image = output->image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	};
	vkCmdPipelineBarrier(cmd_buffer,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre);

	const int32_t src_y0 = input->y_flip ? src_h : 0;
	const int32_t src_y1 = input->y_flip ? 0 : src_h;
	VkImageBlit blit = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .srcOffsets = {{0, src_y0, 0}, {src_w, src_y1, 1}},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffsets = {{viewport.offset.x, viewport.offset.y, 0},
	                   {viewport.offset.x + (int32_t)viewport.extent.width,
	                    viewport.offset.y + (int32_t)viewport.extent.height, 1}},
	};
	vkCmdBlitImage(cmd_buffer, input->atlas_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output->image,
	               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

	VkImageMemoryBarrier post[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = input->atlas_image,
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
	        .image = output->image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	};
	vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
	                     0, NULL, 0, NULL, 2, post);
}


/*
 *
 * Surface (a) — Vulkan weaver.
 *
 */

enum leiasr_lnx_result
leiasr_lnx_create(const struct leiasr_lnx_create_info *info, struct leiasr_lnx **out_lnx)
{
	if (info == NULL || out_lnx == NULL) {
		return LEIASR_LNX_ERROR_FAILED;
	}

	enum leiasr_lnx_result res = sr_ctx_ensure(info->retry_budget_s);
	if (res != LEIASR_LNX_SUCCESS) {
		return res;
	}

	SrRuntimeCapabilities caps = SrRuntimeCapabilities();
	if (SR_FAILED(srGetRuntimeCapabilities(g_ctx.instance, &caps)) ||
	    (caps.weaverBackends & SR_WEAVER_BACKEND_VULKAN_BIT) == 0) {
		U_LOG_W("leia_sr_sdk: srSDK runtime reports no Vulkan weaver backend (weaverBackends=0x%llx)",
		        (unsigned long long)caps.weaverBackends);
		return LEIASR_LNX_ERROR_FAILED;
	}

	struct leiasr_lnx *lnx = calloc(1, sizeof(*lnx));
	if (lnx == NULL) {
		return LEIASR_LNX_ERROR_FAILED;
	}
	lnx->info = *info;

	if (info->x11_connection != NULL) {
		U_LOG_I("leia_sr_sdk: x11_connection has no srSDK 1.0.0 counterpart — ignored "
		        "(the SDK opens its own X connection)");
	}

	/* srSDK takes no queue-family index — the command pool implies it. */
	SrWeaverCreateInfoVulkan wci = SrWeaverCreateInfoVulkan(
	    .device = (SrVkDevice)info->device, .physicalDevice = (SrVkPhysicalDevice)info->physical_device,
	    .graphicsQueue = (SrVkQueue)info->graphics_queue, .commandPool = (SrVkCommandPool)info->command_pool,
	    .window = (SrNativeWindowHandle)(uintptr_t)info->x11_window);
	SrResult sres = srCreateWeaverVulkan(g_ctx.instance, &wci, &lnx->weaver);
	if (SR_FAILED(sres)) {
		U_LOG_W("leia_sr_sdk: srCreateWeaverVulkan failed: %s", srResultToString(sres));
		free(lnx);
		return LEIASR_LNX_ERROR_FAILED;
	}

	VkFormat rp_format = info->target_format != VK_FORMAT_UNDEFINED ? info->target_format
	                                                                : VK_FORMAT_B8G8R8A8_UNORM;
	if (sdk_create_render_pass(lnx, rp_format) != VK_SUCCESS) {
		U_LOG_W("leia_sr_sdk: render pass creation failed (format %d)", rp_format);
		srDestroyWeaver(lnx->weaver);
		free(lnx);
		return LEIASR_LNX_ERROR_FAILED;
	}

	/* The compositor manages gamma; the weave must not double-convert. */
	srWeaverSetShaderSRGBConversion(lnx->weaver, SR_FALSE, SR_FALSE);

	U_LOG_I("leia_sr_sdk: Vulkan weaver created (window=0x%lx%s, target format %d)",
	        (unsigned long)(uintptr_t)info->x11_window,
	        info->x11_window == NULL ? " = windowless/display-scoped" : "", rp_format);
	*out_lnx = lnx;
	return LEIASR_LNX_SUCCESS;
}

void
leiasr_lnx_destroy(struct leiasr_lnx *lnx)
{
	if (lnx == NULL) {
		return;
	}
	/* Weaver-scoped teardown only (R-W10): the process-lifetime SR context
	 * stays up (see file header) and is torn down at .so unload. */
	if (lnx->weaver != NULL) {
		srDestroyWeaver(lnx->weaver);
	}
	sdk_fb_cache_flush(lnx);
	if (lnx->render_pass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(lnx->info.device, lnx->render_pass, NULL);
	}
	free(lnx);
}

void
leiasr_lnx_weave(struct leiasr_lnx *lnx,
                 VkCommandBuffer cmd_buffer,
                 const struct leiasr_lnx_weave_input *input,
                 const struct leiasr_lnx_weave_output *output,
                 VkRect2D viewport,
                 struct leiasr_lnx_phase_origin phase_origin)
{
	if (lnx == NULL || cmd_buffer == VK_NULL_HANDLE || input == NULL || output == NULL ||
	    input->atlas_view == VK_NULL_HANDLE || output->image == VK_NULL_HANDLE) {
		U_LOG_W("leia_sr_sdk: weave called without weavable input/output — skipping");
		return;
	}

	/* srSDK 1.0.0 carries no tile grid — 2×1 SBS only (contract §8 R-W4). */
	if (input->tile_columns != 2 || input->tile_rows != 1) {
		static bool logged;
		if (!logged) {
			U_LOG_W("leia_sr_sdk: %ux%u grid unsupported by srSDK 1.0.0 (2x1 only) — "
			        "passthrough blit fallback",
			        input->tile_columns, input->tile_rows);
			logged = true;
		}
		sdk_passthrough_blit(cmd_buffer, input, output, viewport);
		return;
	}

	if (input->y_flip) {
		static bool logged;
		if (!logged) {
			U_LOG_W("leia_sr_sdk: y_flip requested but srSDK 1.0.0 has no flip toggle — ignored");
			logged = true;
		}
	}

	/* No phase-origin API in srSDK 1.0.0 (runtime#85 gap, contract §8
	 * R-W7). Display-scoped weaving is correct BY CONSTRUCTION: verified in
	 * the SDK source that on Linux the windowless screen-rect is (0,0), so
	 * the weave's phase offset = our viewport offset in panel coordinates.
	 * Anything else (window-scoped) can't weave with 1.0.0. */
	if (phase_origin.x != viewport.offset.x || phase_origin.y != viewport.offset.y) {
		static bool logged;
		if (!logged) {
			U_LOG_W("leia_sr_sdk: phase origin (%d,%d) != viewport offset (%d,%d) — srSDK 1.0.0 "
			        "cannot express decoupled phase; weaving with viewport phase",
			        phase_origin.x, phase_origin.y, viewport.offset.x, viewport.offset.y);
			logged = true;
		}
	}

	SrResult res = srWeaverSetCommandBufferVulkan(lnx->weaver, (SrVkCommandBuffer)cmd_buffer);
	if (SR_FAILED(res)) {
		LOG_SR_ONCE("srWeaverSetCommandBufferVulkan", res);
		return;
	}

	/* PER-VIEW dims — vendor-confirmed contract (LeiaSR#53: "single view,
	 * not SBS"). Unread by the 1.0.0 implementation (verified in
	 * vkweaver.cpp), so contract hygiene for future SDKs. */
	const int32_t in_w = (int32_t)input->view_width;
	const int32_t in_h = (int32_t)input->view_height;
	res = srWeaverSetInputTextureVulkan(lnx->weaver, (SrVkImageView)input->atlas_view, in_w, in_h,
	                                    (SrVkFormat)input->view_format);
	if (SR_FAILED(res)) {
		LOG_SR_ONCE("srWeaverSetInputTextureVulkan", res);
		return;
	}

	const bool fb_to_sdk = debug_get_bool_option_sr_fb_sdk();

	if (fb_to_sdk) {
		/* Escape hatch: hand the caller framebuffer to the SDK and let it
		 * begin its own render pass (framebuffer must be compatible with
		 * the SDK's internal pass — the assumption under test). */
		VkFramebuffer fb = output->framebuffer != VK_NULL_HANDLE ? output->framebuffer
		                                                         : sdk_fb_cache_get(lnx, output);
		res = srWeaverSetOutputFrameBufferVulkan(lnx->weaver, (SrVkFramebuffer)fb, (int32_t)output->width,
		                                         (int32_t)output->height, (SrVkFormat)output->format);
		if (SR_FAILED(res)) {
			LOG_SR_ONCE("srWeaverSetOutputFrameBufferVulkan(fb)", res);
			return;
		}
	} else {
		/* Default: begin OUR render pass (loadOp LOAD, layouts under
		 * plug-in control) and use the SDK's documented "framebuffer = 0,
		 * render pass already started" mode. */
		VkFramebuffer fb = output->framebuffer != VK_NULL_HANDLE ? output->framebuffer
		                                                         : sdk_fb_cache_get(lnx, output);
		if (fb == VK_NULL_HANDLE) {
			U_LOG_W("leia_sr_sdk: no framebuffer for weave target — skipping frame");
			return;
		}
		VkRenderPassBeginInfo begin = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		    .renderPass = lnx->render_pass,
		    .framebuffer = fb,
		    .renderArea = {.offset = {0, 0}, .extent = {output->width, output->height}},
		};
		vkCmdBeginRenderPass(cmd_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);

		res = srWeaverSetOutputFrameBufferVulkan(lnx->weaver, 0, (int32_t)output->width,
		                                         (int32_t)output->height, (SrVkFormat)output->format);
		if (SR_FAILED(res)) {
			LOG_SR_ONCE("srWeaverSetOutputFrameBufferVulkan(0)", res);
			vkCmdEndRenderPass(cmd_buffer);
			return;
		}
	}

	const int32_t l = viewport.offset.x;
	const int32_t t = viewport.offset.y;
	const int32_t r = viewport.offset.x + (int32_t)viewport.extent.width;
	const int32_t b = viewport.offset.y + (int32_t)viewport.extent.height;
	srWeaverSetViewportVulkan(lnx->weaver, l, t, r, b);
	srWeaverSetScissorRectVulkan(lnx->weaver, l, t, r, b);

	res = srWeaverWeave(lnx->weaver);
	if (SR_FAILED(res)) {
		LOG_SR_ONCE("srWeaverWeave", res);
	}

	if (!fb_to_sdk) {
		vkCmdEndRenderPass(cmd_buffer);
	}
}

VkRenderPass
leiasr_lnx_get_render_pass(struct leiasr_lnx *lnx)
{
	return lnx != NULL ? lnx->render_pass : VK_NULL_HANDLE;
}

void
leiasr_lnx_output_invalidated(struct leiasr_lnx *lnx)
{
	if (lnx == NULL) {
		return;
	}
	/* Device idle guaranteed by the caller. No SDK-side invalidation call
	 * exists (contract §8 R-W8) — we re-bind every Set* per frame and flush
	 * only our own view/fb cache. */
	sdk_fb_cache_flush(lnx);
}

void
leiasr_lnx_set_latency_us(struct leiasr_lnx *lnx, uint64_t latency_us)
{
	if (lnx == NULL || lnx->weaver == NULL) {
		return;
	}
	SrResult res = srWeaverSetLatency(lnx->weaver, latency_us);
	if (SR_FAILED(res)) {
		LOG_SR_ONCE("srWeaverSetLatency", res);
	}
}


/*
 *
 * Surface (b) — eye tracking.
 *
 */

bool
leiasr_lnx_get_predicted_eyes(struct leiasr_lnx *lnx,
                              struct leiasr_lnx_eye_pair_mm *out_pair,
                              bool *out_is_tracking,
                              int64_t *out_timestamp_ns)
{
	if (lnx == NULL || out_pair == NULL) {
		return false;
	}

	/* R-T1: the weaver's own predicted pair — the one the weave consumes —
	 * keeps app Kooima and interlacing in agreement. A MANAGED collapse
	 * animation is a legitimate pair and is passed through untouched;
	 * substitution happens only on hard failure or all-zeros (R-T2). */
	SrPoint3f l = {0}, r = {0};
	SrResult res = lnx->weaver != NULL ? srWeaverGetPredictedEyePositions(lnx->weaver, &l, &r)
	                                   : SR_ERROR_HANDLE_INVALID;

	bool plausible = SR_SUCCEEDED(res) &&
	                 !(l.x == 0.0f && l.y == 0.0f && l.z == 0.0f && r.x == 0.0f && r.y == 0.0f && r.z == 0.0f);
	if (plausible) {
		out_pair->left_mm[0] = l.x;
		out_pair->left_mm[1] = l.y;
		out_pair->left_mm[2] = l.z;
		out_pair->right_mm[0] = r.x;
		out_pair->right_mm[1] = r.y;
		out_pair->right_mm[2] = r.z;
		lnx->last_good_pair = *out_pair;
		lnx->have_last_good = true;
	} else if (lnx->have_last_good) {
		*out_pair = lnx->last_good_pair;
	} else {
		/* Nominal pair from the display's design-centre viewing position
		 * (or the seam's default 450 mm when even that is unavailable). */
		float nz_mm = 450.0f;
		pthread_mutex_lock(&g_ctx_lock);
		if (g_ctx.display_info_valid) {
			nz_mm = g_ctx.display_info.nominal_viewer_z_m * 1000.0f;
		}
		pthread_mutex_unlock(&g_ctx_lock);
		out_pair->left_mm[0] = -SDK_HALF_IPD_MM;
		out_pair->left_mm[1] = 0.0f;
		out_pair->left_mm[2] = nz_mm;
		out_pair->right_mm[0] = SDK_HALF_IPD_MM;
		out_pair->right_mm[1] = 0.0f;
		out_pair->right_mm[2] = nz_mm;
	}

	if (out_is_tracking != NULL) {
		/* Event-latched (contract §8 R-T3): flips at raw USER_LOST, i.e.
		 * earlier than R-T4's grace-period preference — srSDK 1.0.0
		 * exposes no grace state. */
		*out_is_tracking = atomic_load(&g_ctx.user_present) && atomic_load(&g_ctx.device_ready) &&
		                   g_ctx.tracker != NULL;
	}
	if (out_timestamp_ns != NULL) {
		uint64_t t_us = atomic_load(&g_ctx.last_eye_time_us);
		/* SrEyePair.timeUs epoch is "unspecified" (contract §8 R-T3 ask);
		 * fall back to now when no sample has arrived. */
		*out_timestamp_ns = t_us != 0 ? (int64_t)t_us * 1000 : (int64_t)os_monotonic_get_ns();
	}
	return true;
}

bool
leiasr_lnx_set_eye_tracking_mode(struct leiasr_lnx *lnx, enum leiasr_lnx_tracking_mode mode)
{
	(void)lnx;
	if (mode == LEIASR_LNX_TRACKING_MANAGED) {
		return true; /* srSDK-native behavior — nothing to switch */
	}
	static bool logged;
	if (!logged) {
		U_LOG_W("leia_sr_sdk: MANUAL tracking mode unsupported — srSDK 1.0.0 has no stand-down "
		        "toggle (contract §8 R-T5); staying MANAGED");
		logged = true;
	}
	return false;
}


/*
 *
 * Surface (c) — display & calibration.
 *
 */

bool
leiasr_lnx_query_display_info(struct leiasr_lnx_display_info *out_info)
{
	if (out_info == NULL) {
		return false;
	}
	/* Static / pre-create (R-D1): single-attempt context bring-up with a
	 * cooldown — headless boxes must fail soft and fast (R-D4). */
	if (sr_ctx_ensure(0.0) != LEIASR_LNX_SUCCESS) {
		return false;
	}

	pthread_mutex_lock(&g_ctx_lock);
	bool ok = g_ctx.display_info_valid && !atomic_load(&g_ctx.display_info_dirty);
	if (!ok) {
		ok = sr_ctx_refresh_display_info_locked();
	}
	if (ok) {
		*out_info = g_ctx.display_info;
	}
	pthread_mutex_unlock(&g_ctx_lock);
	return ok;
}

bool
leiasr_lnx_request_display_mode(struct leiasr_lnx *lnx, bool enable_3d)
{
	(void)lnx;
	if (g_ctx.lens == NULL) {
		static bool logged;
		if (!logged) {
			U_LOG_W("leia_sr_sdk: request_display_mode(%s) with no lens handle — ignored",
			        enable_3d ? "3D" : "2D");
			logged = true;
		}
		return false;
	}
	SrResult res = enable_3d ? srLensEnable(g_ctx.lens) : srLensDisable(g_ctx.lens);
	if (SR_FAILED(res)) {
		LOG_SR_ONCE("srLensEnable/Disable", res);
		return false;
	}
	return true;
}

bool
leiasr_lnx_get_hardware_3d_state(struct leiasr_lnx *lnx, bool *out_is_3d)
{
	(void)lnx;
	if (out_is_3d == NULL) {
		return false;
	}
	if (g_ctx.lens != NULL) {
		SrBool32 enabled = SR_FALSE;
		if (SR_SUCCEEDED(srLensIsEnabled(g_ctx.lens, &enabled))) {
			*out_is_3d = enabled == SR_TRUE;
			return true;
		}
	}
	/* Fall back to the LENS_ON/OFF event atomic. */
	*out_is_3d = atomic_load(&g_ctx.lens_on);
	return true;
}
