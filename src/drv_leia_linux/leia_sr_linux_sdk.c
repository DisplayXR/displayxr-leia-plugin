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
 *    loadOp LOAD, COLOR_ATTACHMENT_OPTIMAL in/out, no depth, one subpass, NO
 *    subpass dependencies — identical shape to ours, so the passes are
 *    compatible by Vulkan §8.2. (Ours used to declare two dependencies, which
 *    compatibility does not exempt: VUID-vkCmdDraw-renderPass-02684, #280.)
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
 *  - Lens preference ownership (LeiaSR #266): SRService keeps one lens
 *    preference per SR context, shared by srCreateLens and the weaver, and
 *    the first srLensEnable/Disable makes it ours for the context's life.
 *    We never make that call at startup, leave 3D to the weaver until
 *    something asks for 2D, and re-apply the last call sent to every new
 *    context (leia_lens_owner_linux.h). A context invalidated by an
 *    SRService restart (SR_EVENT_TYPE_CONTEXT_INVALID) is replaced once no
 *    weaver is alive on it; the dead one is abandoned, not destroyed.
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
#include "leia_edid_probe_linux.h"
#include "leia_lens_owner_linux.h"
#include "leia_display_claims_linux.h"

#include "leia_interface.h"

#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_logging.h"

#include <sr/sr.h>
#include <sr/sr_vk.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEBUG_GET_ONCE_BOOL_OPTION(sr_fb_sdk, "DXR_LEIA_SR_FB_SDK", false)
/* 2D under the lens: DXR_LEIA_SR_COMPOSE=0 declines every layer (the runtime
 * then blends it post-weave, as before) — an on-panel A/B, not a setting.
 * DXR_LEIA_SR_COMPOSE_INLINE=1 keeps a composing weave inside OUR render pass,
 * which makes SR fall back to its inline lens kernel (no prefilter passes, no
 * v14 reuse) — the escape hatch should the SDK-owned pass misbehave. */
DEBUG_GET_ONCE_BOOL_OPTION(sr_compose, "DXR_LEIA_SR_COMPOSE", true)
DEBUG_GET_ONCE_BOOL_OPTION(sr_compose_inline, "DXR_LEIA_SR_COMPOSE_INLINE", false)

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

#ifdef DXR_LEIA_LNX_HAVE_SR_SNAP
/*!
 * Result name for logging. Delegates to the SDK's own srResultToString for
 * everything EXCEPT SR_DECLINED, which is named here directly. The staged
 * loader archive does know that code — but it is newer than the code itself,
 * and an archive that predates it names it something else entirely rather than
 * failing to compile. Naming the one code this file added handling for keeps
 * the log honest whichever loader .a ends up in the link line.
 */
static const char *
sdk_sr_result_str(SrResult res)
{
	if (res == SR_DECLINED) {
		return "SR_DECLINED";
	}
	return srResultToString(res);
}
#endif


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
	//! Monotonic time (ns) the last real eye sample arrived, stamped on OUR
	//! clock in the SDK callback; 0 = none yet. See the is_tracking comment.
	_Atomic int64_t last_pair_mono_ns;
	atomic_bool lens_on;
	atomic_bool display_info_dirty;
	//! SR_EVENT_TYPE_DISPLAY_TOPOLOGY_CHANGED latched (new SR API only):
	//! the cached srEnumerateDisplays answer is stale.
	atomic_bool display_topology_dirty;

	/*
	 * Context loss (SR_EVENT_TYPE_CONTEXT_INVALID). The SDK raises it when the
	 * connection to SRService drops (e.g. the service restarted) and does NOT
	 * reconnect the instance: every handle hanging off it is dead, and the
	 * header's instruction is "release all SR handles and re-initialise".
	 * Latched by the SDK thread, acted on by sr_ctx_ensure() once no weaver
	 * is alive on the dead context (see there).
	 */
	atomic_bool context_invalid;
	//! Bumped per created context; SDK callbacks carry it as user_data so a
	//! late callback from an abandoned context cannot write this one's state.
	_Atomic uint32_t generation;
	//! Weavers alive on the current context (guarded by g_ctx_lock). A
	//! context is only replaced while this is 0: a weaver is bound to the
	//! instance that created it.
	uint32_t live_weavers;
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

/*!
 * Lens-preference ownership (LeiaSR #266), guarded by g_ctx_lock. Lives OUTSIDE
 * g_ctx on purpose: it must survive a context being torn down and re-created,
 * because re-applying the last request to the new context is its whole job.
 * The rules are in leia_lens_owner_linux.h.
 */
static struct leia_lens_owner g_lens_owner;

//! True when an SDK callback belongs to the live context (see sr_ctx::generation).
static inline bool
sr_ctx_callback_is_current(void *user_data)
{
	return (uint32_t)(uintptr_t)user_data == atomic_load(&g_ctx.generation);
}

static void SR_CALL
sr_ctx_on_eye_pair(const SrEyePair *pair, void *user_data)
{
	if (!sr_ctx_callback_is_current(user_data)) {
		return; // abandoned context (see sr_ctx_abandon_locked)
	}
	/* SDK worker thread — atomic stores only. Positions always come from
	 * srWeaverGetPredictedEyePositions (R-T1 same-pair rule); this callback
	 * only tracks sample freshness for the timestamp out-param. */
	atomic_store(&g_ctx.last_eye_time_us, pair->timeUs);
	atomic_store(&g_ctx.last_pair_mono_ns, (int64_t)os_monotonic_get_ns());
}

static void SR_CALL
sr_ctx_on_system_event(const SrSystemEvent *event, void *user_data)
{
	if (!sr_ctx_callback_is_current(user_data)) {
		return; // abandoned context (see sr_ctx_abandon_locked)
	}
	/* SDK worker thread — atomic stores + logging only (the message pointer
	 * dies when this returns; U_LOG formats it immediately). */
	switch (event->eventType) {
	case SR_EVENT_TYPE_CONTEXT_INVALID:
		/* Lifecycle, one line per loss: this is the edge that explains a
		 * session that stops tracking and a lens nobody drives any more. */
		if (!atomic_exchange(&g_ctx.context_invalid, true)) {
			U_LOG_W("leia_sr_sdk: SR context invalidated (%s) — the connection to SRService is gone; "
			        "a new context is created once no weaver is alive on this one",
			        event->message != NULL ? event->message : "no message");
		}
		break;
	case SR_EVENT_TYPE_USER_FOUND: atomic_store(&g_ctx.user_present, true); break;
	case SR_EVENT_TYPE_USER_LOST: atomic_store(&g_ctx.user_present, false); break;
	case SR_EVENT_TYPE_DEVICE_READY:
	case SR_EVENT_TYPE_SR_RESTORED: atomic_store(&g_ctx.device_ready, true); break;
	case SR_EVENT_TYPE_DEVICE_DISCONNECTED:
	case SR_EVENT_TYPE_SR_UNAVAILABLE: atomic_store(&g_ctx.device_ready, false); break;
	case SR_EVENT_TYPE_LENS_ON: atomic_store(&g_ctx.lens_on, true); break;
	case SR_EVENT_TYPE_LENS_OFF: atomic_store(&g_ctx.lens_on, false); break;
	case SR_EVENT_TYPE_DISPLAY_CONNECTED:
	case SR_EVENT_TYPE_DISPLAY_NOT_CONNECTED:
		atomic_store(&g_ctx.display_info_dirty, true);
		atomic_store(&g_ctx.display_topology_dirty, true);
		leia_lnx_edid_cache_invalidate(); // a hot-plugged panel re-resolves (review C)
		break;
#ifdef DXR_LEIA_LNX_HAVE_SR_DISPLAY_ENUM
	case SR_EVENT_TYPE_DISPLAY_TOPOLOGY_CHANGED:
		atomic_store(&g_ctx.display_topology_dirty, true);
		leia_lnx_edid_cache_invalidate();
		break;
#endif
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
 * Forget a context whose connection to SRService is gone, WITHOUT destroying
 * its handles. srDestroyInstance joins SDK threads and is unbounded when the
 * service died under it (R-W10) -- exactly the case here -- so one leaked,
 * dead context per SRService restart is the bounded price of not hanging the
 * caller. Its callbacks are fenced off by the generation bump in
 * sr_ctx_ensure(). Caller holds g_ctx_lock and has checked live_weavers == 0.
 */
static void
sr_ctx_abandon_locked(void)
{
	g_ctx.monitor = NULL;
	g_ctx.tracker = NULL;
	g_ctx.lens = NULL;
	g_ctx.display = NULL;
	g_ctx.instance = NULL;
	g_ctx.display_info_valid = false;
	atomic_store(&g_ctx.user_present, false);
	atomic_store(&g_ctx.last_pair_mono_ns, 0);
	atomic_store(&g_ctx.lens_on, false);
	atomic_store(&g_ctx.device_ready, true);
	atomic_store(&g_ctx.context_invalid, false);
}

/*!
 * Send one lens call and record it as sent on success. Caller holds g_ctx_lock
 * and has checked g_ctx.lens != NULL. @p why names the caller in the log.
 */
static bool
sr_ctx_send_lens_locked(enum leia_lens_action action, const char *why)
{
	if (action == LEIA_LENS_ACTION_NONE) {
		return true;
	}
	const bool enable = action == LEIA_LENS_ACTION_ENABLE;
	const bool first_on_ctx = !g_lens_owner.ctx_app_owned;
	SrResult res = enable ? srLensEnable(g_ctx.lens) : srLensDisable(g_ctx.lens);
	if (SR_FAILED(res)) {
		LOG_SR_ONCE("srLensEnable/Disable", res);
		return false;
	}
	leia_lens_owner_commit(&g_lens_owner, action);
	if (first_on_ctx) {
		/* One line per context: from here on the weaver no longer writes
		 * this context's lens preference (LeiaSR #266), so every later
		 * change must come from us. */
		U_LOG_W("leia_sr_sdk: lens %s (%s) — DisplayXR now owns this SR context's lens preference; "
		        "the weaver will no longer turn it on or off",
		        enable ? "ON" : "OFF", why);
	}
	return true;
}

/*!
 * A new context's lens handle exists: re-apply the last request sent on an
 * earlier context, if any (leia_lens_owner_linux.h). Caller holds g_ctx_lock.
 */
static void
sr_ctx_reapply_lens_locked(void)
{
	const enum leia_lens_action action = leia_lens_owner_on_new_context(&g_lens_owner);
	if (action == LEIA_LENS_ACTION_NONE) {
		/* Deliberately NO srLensEnable at startup. The first lens call on a
		 * context takes the preference away from the weaver for the rest of
		 * the context's life (LeiaSR #266), which would lose the weaver's
		 * automatic "lens off when the window leaves the panel" -- the
		 * Windows-parity behaviour an untoggled session should get. The
		 * weaver turns the lens on by itself at its first woven frame. */
		return;
	}
	if (g_ctx.lens == NULL) {
		U_LOG_W("leia_sr_sdk: cannot re-apply lens %s to the new SR context: no lens handle",
		        action == LEIA_LENS_ACTION_ENABLE ? "ON" : "OFF");
		return;
	}
	(void)sr_ctx_send_lens_locked(action, "re-applied to a new SR context");
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
	if (g_ctx.state == SR_CTX_READY && atomic_load(&g_ctx.context_invalid)) {
		if (g_ctx.live_weavers == 0) {
			/* The connection to SRService is gone and nothing is bound to
			 * this context any more: drop it and build a new one below. */
			U_LOG_W("leia_sr_sdk: replacing the invalidated SR context");
			sr_ctx_abandon_locked();
			g_ctx.state = SR_CTX_UNINIT;
		} else {
			/* A live weaver is bound to the dead instance. Replacing the
			 * context under it would need re-creating the weaver and all
			 * the state set on it; that is not done here, so the session
			 * keeps the dead context and the next one recovers. */
			static bool logged;
			if (!logged) {
				U_LOG_W("leia_sr_sdk: SR context invalid but %u weaver(s) still alive on it — "
				        "keeping it until they are destroyed",
				        g_ctx.live_weavers);
				logged = true;
			}
		}
	}
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

	/* New context: fence off callbacks from any abandoned one. */
	const uint32_t gen = atomic_fetch_add(&g_ctx.generation, 1) + 1;
	void *const gen_tag = (void *)(uintptr_t)gen;
	atomic_store(&g_ctx.context_invalid, false);

	/* Senses + callbacks BEFORE srInitialize (SDK lifecycle rule). */
	SrEyeTrackerCreateInfo tci = SrEyeTrackerCreateInfo(.enablePrediction = SR_TRUE);
	res = srCreateEyeTracker(g_ctx.instance, &tci, &g_ctx.tracker);
	if (SR_FAILED(res)) {
		/* No camera is non-fatal: the weaver still weaves, is_tracking
		 * stays false, poses fall back to nominal (R-T2). */
		U_LOG_W("leia_sr_sdk: srCreateEyeTracker failed (%s) — untracked weaving", srResultToString(res));
		g_ctx.tracker = NULL;
	} else {
		srEyeTrackerAddCallback(g_ctx.tracker, sr_ctx_on_eye_pair, gen_tag);
	}

	SrSystemMonitorCreateInfo mci = SrSystemMonitorCreateInfo();
	res = srCreateSystemMonitor(g_ctx.instance, &mci, &g_ctx.monitor);
	if (SR_SUCCEEDED(res)) {
		srSystemMonitorAddCallback(g_ctx.monitor, sr_ctx_on_system_event, gen_tag);
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

	/* Every path that creates a context comes through here -- first
	 * creation, a retry after a failed bring-up, and the replacement of an
	 * invalidated context above -- so this is the one place "the app took
	 * control" is carried over to a new context. */
	sr_ctx_reapply_lens_locked();

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

	/* SRService-Linux does not yet populate per-panel display geometry: on an
	 * unconfigured panel it reports its built-in default (344.2 x 193.6 mm
	 * 15.6" quad — the same constants the Windows probe uses as a last-resort
	 * fallback, leia_sr_probe.cpp) plus a nominal viewer derived from that
	 * default. Accepting it verbatim shrinks the runtime's Kooima screen quad
	 * ~1.7x on a 27" panel — wrong frusta, wrong convergence plane ("cube out
	 * of focus"). Detect the canned default and override the physical size
	 * from the panel's own EDID; re-derive the nominal viewer from the real
	 * height (design position ~ centre, ~1.9x width in front is SR's own
	 * ratio, but plain (0, 0, max(nz_scaled, 0.45)) matches the Windows
	 * fallback semantics and keeps the axis on panel-centre). */
	const bool sr_default_geometry = (w_cm > 34.3f && w_cm < 34.6f && h_cm > 19.2f && h_cm < 19.5f);
	if (sr_default_geometry) {
		float edid_w = 0.0f, edid_h = 0.0f;
		if (leia_lnx_edid_panel_physical_size(&edid_w, &edid_h)) {
			const float scale = edid_w / info.width_m;
			U_LOG_W("leia_sr_sdk: SR reported its DEFAULT display geometry (%.1fx%.1f cm) — "
			        "overriding physical size from EDID: %.3fx%.3f m (nominal viewer scaled %.2fx)",
			        w_cm, h_cm, edid_w, edid_h, scale);
			info.width_m = edid_w;
			info.height_m = edid_h;
			/* The default-derived nominal is for the wrong quad; scale its Z
			 * with the size ratio and drop the bogus Y offset. */
			info.nominal_viewer_x_m = 0.0f;
			info.nominal_viewer_y_m = 0.0f;
			info.nominal_viewer_z_m = info.nominal_viewer_z_m * scale;
		} else {
			U_LOG_W("leia_sr_sdk: SR reported its DEFAULT display geometry (%.1fx%.1f cm) and "
			        "no EDID size available — projection geometry will be wrong on large panels",
			        w_cm, h_cm);
		}
	}

	/* NB: recommended per-view size is passed through as-is (e.g. 0.5x0.5 of
	 * the panel for the 2-view mode — half-size views, tiled 2x1 from the
	 * atlas origin). The weave path hands the srSDK weaver the CONTENT rect
	 * (view_width*cols x view_height*rows) via the crop/flip intermediate in
	 * leiasr_lnx_weave — never the whole worst-case atlas — per the runtime's
	 * crop-before-the-DP rule (ADR-030). */

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

	/* Y-flip intermediate: srSDK 1.0.0's Vulkan weaver has no input-flip
	 * toggle and samples the SBS input with the opposite V convention from
	 * the Windows SR VK weaver, so a Vulkan-rendered (Y-down) atlas weaves
	 * upside-down. When input->y_flip is set we flip-blit the atlas into
	 * this backend-owned intermediate and hand ITS view to the weaver.
	 * Lazily (re)created on size/format change; freed with the backend. */
	VkImage flip_image;
	VkDeviceMemory flip_memory;
	VkImageView flip_view;
	uint32_t flip_w, flip_h;
	VkFormat flip_format;

	/* Gamma / ADR-021 (runtime#1484). Two independent inputs decide the
	 * weave shader's sRGB conversion pair: @p atlas_linear, which the RUNTIME
	 * declares through the base DP's set_atlas_encoding slot, and
	 * @p hw_encodes, the target-format half only we can see. The pair itself
	 * is derived + applied by sdk_apply_srgb_conversion(); srgb_read/
	 * srgb_write/srgb_applied latch what the SDK was last told so the log
	 * fires ON CHANGE ONLY, and srgb_dirty defers the SDK call to the weave
	 * (render thread) — see leiasr_lnx_set_atlas_linear(). */
	bool atlas_linear;
	bool hw_encodes;
	bool srgb_dirty;
	bool srgb_applied;
	SrBool32 srgb_read;
	SrBool32 srgb_write;

	struct leiasr_lnx_eye_pair_mm last_good_pair;
	bool have_last_good;

	/* 2D under the lens (ADR-027 Amendment). The compose ORDER is sticky per
	 * weaver and one weaver lives exactly as long as this struct, so the probe
	 * runs once per instance (compose_probed) and its answer is compose_ok.
	 * compose_strength_* latch the filter strength last APPLIED (sticky in SR
	 * too), so the SDK call is made on change only. */
	bool compose_probed;
	bool compose_ok;
	bool compose_strength_valid;
	float compose_strength_applied;
	bool compose_announced;   //!< first composited weave logged
	bool compose_lost_logged; //!< a layer that reached the weave but could not be composited
};

static void
sdk_flip_cache_flush(struct leiasr_lnx *lnx)
{
	if (lnx->flip_view != VK_NULL_HANDLE) {
		vkDestroyImageView(lnx->info.device, lnx->flip_view, NULL);
	}
	if (lnx->flip_image != VK_NULL_HANDLE) {
		vkDestroyImage(lnx->info.device, lnx->flip_image, NULL);
	}
	if (lnx->flip_memory != VK_NULL_HANDLE) {
		vkFreeMemory(lnx->info.device, lnx->flip_memory, NULL);
	}
	lnx->flip_view = VK_NULL_HANDLE;
	lnx->flip_image = VK_NULL_HANDLE;
	lnx->flip_memory = VK_NULL_HANDLE;
	lnx->flip_w = lnx->flip_h = 0;
	lnx->flip_format = VK_FORMAT_UNDEFINED;
}

/*! Lazily (re)create the flip intermediate for the given atlas dims/format.
 * Returns the sampleable view, or VK_NULL_HANDLE on failure (caller weaves
 * unflipped rather than dropping the frame). */
static VkImageView
sdk_flip_get_view(struct leiasr_lnx *lnx, uint32_t w, uint32_t h, VkFormat format)
{
	if (lnx->flip_view != VK_NULL_HANDLE && lnx->flip_w == w && lnx->flip_h == h &&
	    lnx->flip_format == format) {
		return lnx->flip_view;
	}
	sdk_flip_cache_flush(lnx);

	VkImageCreateInfo img_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = format,
	    .extent = {w, h, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(lnx->info.device, &img_info, NULL, &lnx->flip_image) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(lnx->info.device, lnx->flip_image, &req);
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(lnx->info.physical_device, &props);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((req.memoryTypeBits & (1u << i)) &&
		    (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			type = i;
			break;
		}
	}
	VkMemoryAllocateInfo alloc = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = req.size,
	    .memoryTypeIndex = type,
	};
	if (type == UINT32_MAX ||
	    vkAllocateMemory(lnx->info.device, &alloc, NULL, &lnx->flip_memory) != VK_SUCCESS ||
	    vkBindImageMemory(lnx->info.device, lnx->flip_image, lnx->flip_memory, 0) != VK_SUCCESS) {
		sdk_flip_cache_flush(lnx);
		return VK_NULL_HANDLE;
	}

	VkImageViewCreateInfo view_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = lnx->flip_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = format,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	if (vkCreateImageView(lnx->info.device, &view_info, NULL, &lnx->flip_view) != VK_SUCCESS) {
		sdk_flip_cache_flush(lnx);
		return VK_NULL_HANDLE;
	}
	lnx->flip_w = w;
	lnx->flip_h = h;
	lnx->flip_format = format;
	U_LOG_I("leia_sr_sdk: created y-flip intermediate %ux%u (srSDK weaver has no input flip)", w, h);
	return lnx->flip_view;
}

/*! Record the flip blit: atlas (SHADER_READ) -> intermediate (-> SHADER_READ),
 * vertically mirrored. Whole-image overwrite, so the intermediate's oldLayout
 * is UNDEFINED every frame (legal + skips a redundant transition). */
static void
sdk_record_flip_blit(struct leiasr_lnx *lnx, VkCommandBuffer cmd_buffer,
                     const struct leiasr_lnx_weave_input *input, uint32_t w, uint32_t h)
{
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
	        .srcAccessMask = 0,
	        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = lnx->flip_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	};
	vkCmdPipelineBarrier(cmd_buffer,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre);

	VkImageBlit blit = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .srcOffsets = {{0, (int32_t)h, 0}, {(int32_t)w, 0, 1}}, // mirrored V
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
	};
	vkCmdBlitImage(cmd_buffer, input->atlas_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               lnx->flip_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
	               VK_FILTER_NEAREST);

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
	        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = lnx->flip_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    },
	};
	vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 2, post);
}

/*!
 * Does the WSI/render-pass encode linear→sRGB for us on write?
 *
 * An `*_SRGB` colour attachment makes the HW convert the shader's linear output
 * to sRGB on store; a `*_UNORM` one stores the shader's bytes verbatim. The
 * weave's own output conversion has to complement whichever it is — see the
 * ADR-021 block in leiasr_lnx_create().
 *
 * The compositor only *prefers* B8G8R8A8_UNORM and otherwise takes the
 * surface's first reported format (runtime comp_vk_native_target.cpp), so an
 * `*_SRGB` target is reachable on a driver that doesn't expose the UNORM
 * sibling — this must not be assumed away.
 */
static bool
sdk_format_is_srgb(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
	case VK_FORMAT_R8G8B8_SRGB:
	case VK_FORMAT_B8G8R8_SRGB:
	case VK_FORMAT_R8_SRGB:
	case VK_FORMAT_R8G8_SRGB: return true;
	default: return false;
	}
}

/*!
 * Derive the weave shader's sRGB read/write conversion pair and tell the SDK.
 *
 * Gamma / ADR-021 (runtime#778, this repo #247, runtime#1484). Two independent
 * inputs decide it, one from each side of the seam:
 *
 *   atlas_linear  The **runtime declares** it, out-of-band, through the base
 *                 display-processor slot `set_atlas_encoding` (ADR-021, wired
 *                 in leia_display_processor_linux.c). The plug-in no longer
 *                 GUESSES the atlas's transfer function — removing that guess
 *                 is the whole point of runtime#1484. The default is false =
 *                 display-referred / sRGB-ENCODED, because
 *                 XRT_ATLAS_ENCODING_ENCODED is 0 so an un-negotiated path
 *                 degrades to encoded passthrough and never to a spurious
 *                 "linear" — a runtime that never calls the slot therefore
 *                 lands on exactly the #247 behaviour.
 *   hw_encodes    Whether the *target* format encodes linear→sRGB on store
 *                 (`*_SRGB`) or stores our bytes verbatim (`*_UNORM`) — see
 *                 sdk_format_is_srgb(). This is a fact about the present
 *                 surface that the runtime cannot see, so it stays local.
 *
 * srSDK semantics: read = SR_TRUE decodes sRGB→linear as the shader samples
 * the atlas; write = SR_TRUE encodes linear→sRGB as it writes the target.
 *
 *   atlas     target    read  write  why
 *   ENCODED   *_UNORM   F     F      passthrough, nobody converts (#247's fix)
 *   ENCODED   *_SRGB    T     F      decode so the HW encode doesn't double
 *   LINEAR    *_UNORM   F     T      we encode; the HW won't (#778-era pair)
 *   LINEAR    *_SRGB    F     F      hand the HW linear, it encodes
 *
 *   read  = !atlas_linear && hw_encodes
 *   write =  atlas_linear && !hw_encodes
 *
 * History worth not re-learning: this pair flipped twice on honest on-panel
 * readings six weeks apart — (F,T) for runtime#778's crushed-dark (linear bytes
 * sent to an sRGB display exaggerate a near-neutral clear's channel ratio by
 * ^2.2, which is why it read BLUE rather than merely dark), then back to (F,F)
 * for #247's washed-out. Both readings were real; the bug was that a single
 * *constant* cannot express two different atlases. It is no longer a constant —
 * the runtime says which atlas it sent, so both rows are reachable without
 * anyone flipping anything a third time.
 *
 * DXR_LEIA_SRGB **stays**, as the plug-in-side kill switch / diagnostic
 * override: "RW", each of 0/1, applied LAST so it wins over both inputs above
 * (=01 restores the pre-#247 pair for a rebuild-free on-panel A/B; =00 pins
 * passthrough if a runtime ever declares LINEAR wrongly). It is a diagnostic,
 * not a setting: never document it as one.
 *
 * Called at create and, when the declaration changed, from the weave — so the
 * U_LOG_W below is gated on the applied pair actually changing (first apply
 * counts), never per-frame.
 */
static void
sdk_apply_srgb_conversion(struct leiasr_lnx *lnx)
{
	if (lnx == NULL || lnx->weaver == NULL) {
		return;
	}
	lnx->srgb_dirty = false;

	SrBool32 srgb_read = (!lnx->atlas_linear && lnx->hw_encodes) ? SR_TRUE : SR_FALSE;
	SrBool32 srgb_write = (lnx->atlas_linear && !lnx->hw_encodes) ? SR_TRUE : SR_FALSE;
	const char *reason =
	    lnx->atlas_linear
	        ? (lnx->hw_encodes ? "atlas LINEAR + *_SRGB target: hand the HW linear, it encodes"
	                           : "atlas LINEAR + *_UNORM target: we encode, the HW won't")
	        : (lnx->hw_encodes ? "atlas ENCODED + *_SRGB target: decode so the HW encode doesn't double"
	                           : "atlas ENCODED + *_UNORM target: passthrough, nobody converts");

	const char *srgb_override = getenv("DXR_LEIA_SRGB");
	if (srgb_override != NULL && srgb_override[0] != '\0' && srgb_override[1] != '\0') {
		srgb_read = srgb_override[0] == '1' ? SR_TRUE : SR_FALSE;
		srgb_write = srgb_override[1] == '1' ? SR_TRUE : SR_FALSE;
		reason = "DXR_LEIA_SRGB kill switch / diagnostic override";
	}

	const bool changed = !lnx->srgb_applied || srgb_read != lnx->srgb_read || srgb_write != lnx->srgb_write;

	SrResult res = srWeaverSetShaderSRGBConversion(lnx->weaver, srgb_read, srgb_write);
	if (SR_FAILED(res)) {
		LOG_SR_ONCE("srWeaverSetShaderSRGBConversion", res);
		return;
	}
	lnx->srgb_read = srgb_read;
	lnx->srgb_write = srgb_write;
	lnx->srgb_applied = true;

	if (changed) {
		U_LOG_W("leia_sr_sdk: weave sRGB conversion read=%u write=%u — %s", (unsigned)srgb_read,
		        (unsigned)srgb_write, reason);
	}
}

/*!
 * Issue #280: the ordering the weave render pass's EXTERNAL->0 subpass
 * dependency used to provide, as a barrier recorded before the pass begins.
 * Prior colour-attachment or transfer writes to the target (the runtime's
 * pre-weave transition, a 2D frame's blit, a previous overlay) happen before
 * the weave's LOAD and its blended draw. Layout unchanged: the target is in
 * COLOR_ATTACHMENT_OPTIMAL here (runtime pre-weave barrier), as both passes
 * that can draw into it declare.
 */
static void
sdk_weave_target_barrier(VkCommandBuffer cmd_buffer, VkImage image)
{
	VkImageMemoryBarrier b = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

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
	/* NO subpass dependencies (issue #280). The weaver draws with a pipeline
	 * built against ITS OWN RenderPassCache pass (vkweaver.cpp), which declares
	 * dependencyCount = 0 (the dependency is commented out there). Render-pass
	 * compatibility (Vulkan "Render Pass Compatibility") exempts only layouts
	 * and load/store ops — dependencies must match — so the two dependencies
	 * this pass used to declare made every weave draw
	 * VUID-vkCmdDraw-renderPass-02684 ("dependencyCount 2 vs 0"). The ordering
	 * those dependencies gave is now an explicit barrier in leiasr_lnx_weave
	 * (sdk_weave_target_barrier); consumers after the pass carry their own
	 * COLOR_ATTACHMENT_OUTPUT-sourced barrier. */
	VkRenderPassCreateInfo rp_info = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &att,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	    .dependencyCount = 0,
	    .pDependencies = NULL,
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
 * 2D under the lens (ADR-027 Amendment) — srWeaverSetComposeOrder +
 * srWeaverSetComposeInputsVulkan (LeiaSR ST-5801 / ST-5792), plus the v14
 * cache hint (srWeaverSetComposeLayerUnchanged) and the v15 strength
 * (srWeaverSetComposeFilterStrength).
 *
 * Every one of these goes through the loader trampoline, like
 * srWeaverSnapToPhase: an installed SR runtime that predates a call answers
 * SR_ERROR_FUNCTION_UNSUPPORTED, which is the run-time guard. The build-time
 * guard is CMake's check_symbol_exists against the SAME loader archive (see
 * CMakeLists.txt) — the trampolines dispatch by slot index, so only a loader
 * from the runtime's own tree is safe, exactly as for the snap call.
 *
 */

static void
sdk_compose_log_lost(struct leiasr_lnx *lnx, const char *why)
{
	if (!lnx->compose_lost_logged) {
		lnx->compose_lost_logged = true;
		U_LOG_W("leia_sr_sdk: 2D over-layer reached the weave but was NOT composited (%s) — dropped this frame "
		        "(logged once)",
		        why);
	}
}

bool
leiasr_lnx_compose_available(struct leiasr_lnx *lnx)
{
#ifdef DXR_LEIA_LNX_HAVE_SR_COMPOSE
	if (lnx == NULL || lnx->weaver == NULL) {
		return false;
	}
	if (lnx->compose_probed) {
		return lnx->compose_ok;
	}
	lnx->compose_probed = true;
	if (!debug_get_bool_option_sr_compose()) {
		U_LOG_W("leia_sr_sdk: SR weaver 2D compose DISABLED by DXR_LEIA_SR_COMPOSE=0 — the runtime composites "
		        "the 2D over-layer post-weave");
		return false;
	}
	/* SR_COMPOSE_ORDER_2D_OVER (1), deliberately NOT 2D_OVER_COVERAGE (3) as on
	 * Windows. Order 3 writes the filtered 2D coverage into the output alpha,
	 * i.e. alpha 0 over every woven 3D pixel. The D3D11 DP consumes that alpha
	 * in its alpha gate; nothing here does — the woven output goes back to the
	 * runtime (weave engine) or to a present that treats alpha as alpha — so
	 * order 3 would only make the 3D transparent. Order 1 writes alpha 1.0, as
	 * a weave without compose does. Sticky per weaver: set once here. */
	const SrResult res = srWeaverSetComposeOrder(lnx->weaver, SR_COMPOSE_ORDER_2D_OVER);
	lnx->compose_ok = !SR_FAILED(res);
	U_LOG_W("leia_sr_sdk: SR weaver 2D compose %s (srWeaverSetComposeOrder(2D_OVER) = %s)",
	        lnx->compose_ok ? "ENABLED — the runtime's 2D over-layer is composited and lens-filtered in the weave"
	                        : "UNAVAILABLE — the runtime composites the 2D over-layer post-weave",
	        srResultToString(res));
	return lnx->compose_ok;
#else
	(void)lnx;
	return false;
#endif
}

#ifdef DXR_LEIA_LNX_HAVE_SR_COMPOSE
#ifdef DXR_LEIA_LNX_HAVE_SR_COMPOSE_STRENGTH
/*!
 * XR_DXR_weave v15 — the app's filter strength, sent only when it changes
 * (sticky per weaver in SR; a fresh weaver starts at the runtime default, so a
 * "default" request before anything was applied sends nothing).
 */
static void
sdk_apply_compose_strength(struct leiasr_lnx *lnx, float strength)
{
	const float want = (strength >= 0.0f && strength <= 1.0f) ? strength : SR_COMPOSE_FILTER_STRENGTH_DEFAULT;
	const bool is_default = want < 0.0f;
	if (!lnx->compose_strength_valid && is_default) {
		return;
	}
	if (lnx->compose_strength_valid && lnx->compose_strength_applied == want) {
		return;
	}
	const SrResult res = srWeaverSetComposeFilterStrength(lnx->weaver, want);
	/* Latched either way: a FUNCTION_UNSUPPORTED (SR runtime older than
	 * ST-5799) means its own default applies, which is correct — logged once
	 * per value change, never per frame. */
	lnx->compose_strength_applied = want;
	lnx->compose_strength_valid = true;
	if (SR_FAILED(res)) {
		U_LOG_W("leia_sr_sdk: srWeaverSetComposeFilterStrength(%.2f): %s — the SR runtime's default strength "
		        "applies",
		        (double)want, srResultToString(res));
	} else if (is_default) {
		U_LOG_W("leia_sr_sdk: SR weaver 2D compose: filter strength back to the SR runtime default (XR_DXR_weave "
		        "v15)");
	} else {
		U_LOG_W("leia_sr_sdk: SR weaver 2D compose: filter strength %.2f (XR_DXR_weave v15)", (double)want);
	}
}
#endif

/*!
 * Hand the weaver this weave's 2D layer. Called immediately before
 * srWeaverWeave, after every early return of leiasr_lnx_weave, so a layer the
 * SDK accepted is always consumed by the very next weave (it applies to that
 * weave only, and must not survive into a later one holding a dead view).
 */
static bool
sdk_set_compose_inputs(struct leiasr_lnx *lnx, const struct leiasr_lnx_weave_input *input)
{
#ifdef DXR_LEIA_LNX_HAVE_SR_COMPOSE_STRENGTH
	sdk_apply_compose_strength(lnx, input->compose_strength);
#endif
#ifdef DXR_LEIA_LNX_HAVE_SR_COMPOSE_UNCHANGED
	/* v14: per weave (SR clears it after each weave), so sent every frame. An
	 * SR runtime without it re-filters every frame, which is correct. */
	const SrResult ures =
	    srWeaverSetComposeLayerUnchanged(lnx->weaver, input->compose_unchanged ? SR_TRUE : SR_FALSE);
	if (SR_FAILED(ures)) {
		LOG_SR_ONCE("srWeaverSetComposeLayerUnchanged (the weaver re-filters the layer every frame)", ures);
	}
#endif
	const SrResult res = srWeaverSetComposeInputsVulkan(lnx->weaver, (SrVkImageView)input->compose_view,
	                                                    (SrVkFormat)input->compose_format, 0);
	if (SR_FAILED(res)) {
		LOG_SR_ONCE("srWeaverSetComposeInputsVulkan", res);
		sdk_compose_log_lost(lnx, srResultToString(res));
		return false;
	}
	if (!lnx->compose_announced) {
		lnx->compose_announced = true;
		U_LOG_W("leia_sr_sdk: first 2D over-layer composited in the weave (view format %d, %s)",
		        (int)input->compose_format,
		        debug_get_bool_option_sr_compose_inline()
		            ? "inside our render pass: SR's inline lens kernel (DXR_LEIA_SR_COMPOSE_INLINE=1)"
		            : "SR-owned output pass: prefilter passes + v14 reuse available");
	}
	return true;
}
#endif


/*
 *
 * Surface (a) — Vulkan weaver.
 *
 */

//! Count a weaver alive on the current context (see sr_ctx::live_weavers).
static void
sr_ctx_weaver_pin(void)
{
	pthread_mutex_lock(&g_ctx_lock);
	g_ctx.live_weavers++;
	pthread_mutex_unlock(&g_ctx_lock);
}

static void
sr_ctx_weaver_unpin(void)
{
	pthread_mutex_lock(&g_ctx_lock);
	if (g_ctx.live_weavers > 0) {
		g_ctx.live_weavers--;
	}
	pthread_mutex_unlock(&g_ctx_lock);
}

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

	/* Pin the context for the weaver's lifetime: sr_ctx_ensure only replaces
	 * an invalidated context while no weaver is alive on it. Taken before the
	 * weaver exists so a concurrent replacement cannot slip in between. */
	sr_ctx_weaver_pin();

	SrRuntimeCapabilities caps = SrRuntimeCapabilities();
	if (SR_FAILED(srGetRuntimeCapabilities(g_ctx.instance, &caps)) ||
	    (caps.weaverBackends & SR_WEAVER_BACKEND_VULKAN_BIT) == 0) {
		U_LOG_W("leia_sr_sdk: srSDK runtime reports no Vulkan weaver backend (weaverBackends=0x%llx)",
		        (unsigned long long)caps.weaverBackends);
		sr_ctx_weaver_unpin();
		return LEIASR_LNX_ERROR_FAILED;
	}

	struct leiasr_lnx *lnx = calloc(1, sizeof(*lnx));
	if (lnx == NULL) {
		sr_ctx_weaver_unpin();
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
		sr_ctx_weaver_unpin();
		return LEIASR_LNX_ERROR_FAILED;
	}

	VkFormat rp_format = info->target_format != VK_FORMAT_UNDEFINED ? info->target_format
	                                                                : VK_FORMAT_B8G8R8A8_UNORM;
	if (sdk_create_render_pass(lnx, rp_format) != VK_SUCCESS) {
		U_LOG_W("leia_sr_sdk: render pass creation failed (format %d)", rp_format);
		srDestroyWeaver(lnx->weaver);
		free(lnx);
		sr_ctx_weaver_unpin();
		return LEIASR_LNX_ERROR_FAILED;
	}

	/* Gamma / ADR-021: the target-side half of the conversion pair — the only
	 * half we can see locally, and fixed for the weaver's lifetime because the
	 * render-pass format is. The atlas-side half arrives later, declared by the
	 * runtime through leiasr_lnx_set_atlas_linear(); `lnx` is calloc'd, so we
	 * start on the ADR-021 default (ENCODED) = exactly the #247 behaviour, which
	 * is also where a runtime that never declares anything stays. The derivation,
	 * the truth table and the DXR_LEIA_SRGB kill switch all live in
	 * sdk_apply_srgb_conversion() — read that comment, not this one. */
	lnx->hw_encodes = sdk_format_is_srgb(rp_format);
	sdk_apply_srgb_conversion(lnx);

	U_LOG_I("leia_sr_sdk: Vulkan weaver created (window=0x%lx%s, target format %d%s, "
	        "atlas encoding awaiting the runtime's declaration (ADR-021 default ENCODED), "
	        "weave sRGB read=%u write=%u)",
	        (unsigned long)(uintptr_t)info->x11_window,
	        info->x11_window == NULL ? " = windowless/display-scoped" : "", rp_format,
	        lnx->hw_encodes ? " = *_SRGB, HW encodes on store" : " = *_UNORM, stores verbatim",
	        (unsigned)lnx->srgb_read, (unsigned)lnx->srgb_write);
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
	sdk_flip_cache_flush(lnx);
	if (lnx->render_pass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(lnx->info.device, lnx->render_pass, NULL);
	}
	free(lnx);
	sr_ctx_weaver_unpin();
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
		if (input->compose_view != VK_NULL_HANDLE) {
			sdk_compose_log_lost(lnx, "non-2x1 grid: passthrough blit, no weave");
		}
		sdk_passthrough_blit(cmd_buffer, input, output, viewport);
		return;
	}

	/* srSDK 1.0.0 has no input-flip toggle and its Linux VK weaver samples the
	 * SBS input with the opposite V convention from the Windows SR VK weaver
	 * (which consumes the runtime's top-down atlas directly — see
	 * drv_leia/leia_display_processor.cpp). So when the compositor asks for a
	 * flip, blit the atlas vertically mirrored into a backend-owned
	 * intermediate and weave from THAT. On any allocation failure, weave
	 * unflipped (upside-down beats a dropped frame). */
	VkImageView weave_view = input->atlas_view;
	if (input->y_flip) {
		const uint32_t atlas_w = input->view_width * input->tile_columns;
		const uint32_t atlas_h = input->view_height * input->tile_rows;
		VkImageView fv = sdk_flip_get_view(lnx, atlas_w, atlas_h, input->view_format);
		if (fv != VK_NULL_HANDLE) {
			sdk_record_flip_blit(lnx, cmd_buffer, input, atlas_w, atlas_h);
			weave_view = fv;
		} else {
			static bool logged;
			if (!logged) {
				U_LOG_W("leia_sr_sdk: y_flip intermediate unavailable — weaving unflipped");
				logged = true;
			}
		}
	}

	/* Apply a pending atlas-encoding declaration (ADR-021 / runtime#1484)
	 * before the weave is recorded — here, on the render thread, rather than
	 * wherever the runtime happened to call the DP slot from. Dirty only when
	 * the declaration actually changed, so this is not a per-frame SDK call. */
	if (lnx->srgb_dirty) {
		sdk_apply_srgb_conversion(lnx);
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
	res = srWeaverSetInputTextureVulkan(lnx->weaver, (SrVkImageView)weave_view, in_w, in_h,
	                                    (SrVkFormat)input->view_format);
	if (SR_FAILED(res)) {
		LOG_SR_ONCE("srWeaverSetInputTextureVulkan", res);
		return;
	}

	/* 2D under the lens: a layer for this weave (the DP hands one only after
	 * leiasr_lnx_compose_available said yes). SR filters it for the lens in
	 * prefilter render passes of its own, which it can record only BEFORE the
	 * output pass — i.e. only when it begins that pass itself. So a composing
	 * weave hands the SDK a framebuffer, always OUR cached one: it is built
	 * against our render pass, the same shape as SR's RenderPassCache pass
	 * (one colour attachment, LOAD, COLOR_ATTACHMENT in/out, 0 dependencies),
	 * hence compatible by construction — the caller's framebuffer may come
	 * from a pass with dependencies (#280). Without a framebuffer, or with
	 * DXR_LEIA_SR_COMPOSE_INLINE=1, the weave stays inside our pass and SR uses
	 * its inline kernel (same lens box, no prefilter cache). */
	bool composing = false;
	VkFramebuffer compose_fb = VK_NULL_HANDLE;
	if (input->compose_view != VK_NULL_HANDLE) {
		if (lnx->compose_ok) {
			composing = true;
			if (!debug_get_bool_option_sr_compose_inline()) {
				compose_fb = sdk_fb_cache_get(lnx, output);
			}
		} else {
			sdk_compose_log_lost(lnx, "SR compose unavailable on this weaver");
		}
	}

	const bool fb_to_sdk = debug_get_bool_option_sr_fb_sdk() || compose_fb != VK_NULL_HANDLE;

	/* Outside any render pass, before either path begins one (#280). */
	sdk_weave_target_barrier(cmd_buffer, output->image);

	if (fb_to_sdk) {
		/* Hand the framebuffer to the SDK and let it begin its own render
		 * pass: a composing weave (above), or the DXR_LEIA_SR_FB_SDK escape
		 * hatch (framebuffer must be compatible with the SDK's internal pass
		 * — the assumption under test). */
		VkFramebuffer fb = compose_fb != VK_NULL_HANDLE              ? compose_fb
		                   : output->framebuffer != VK_NULL_HANDLE ? output->framebuffer
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

	/* Windowed weaving (runtime#757 / LeiaSR#85): anchor the interlacing phase to
	 * the app WINDOW's panel-relative origin. The SDK combines it with the
	 * viewport above (phase = presentOrigin + viewportOffset), so we pass the
	 * window term only — the DP already put the canvas offset in the viewport.
	 * (0,0) = display-scoped, exactly the pre-#85 windowless behavior. A runtime
	 * predating this slot returns SR_ERROR_FUNCTION_UNSUPPORTED → we log once and
	 * weave display-scoped. */
	res = srWeaverSetPresentOrigin(lnx->weaver, phase_origin.x, phase_origin.y);
	if (SR_FAILED(res)) {
		LOG_SR_ONCE("srWeaverSetPresentOrigin", res);
	}

#ifdef DXR_LEIA_LNX_HAVE_SR_COMPOSE
	/* Last thing before the weave, after every early return above: the layer
	 * applies to the NEXT srWeaverWeave only and must never be left pending. */
	if (composing) {
		(void)sdk_set_compose_inputs(lnx, input);
	}
#else
	(void)composing;
#endif

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
	sdk_flip_cache_flush(lnx);
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

void
leiasr_lnx_set_atlas_linear(struct leiasr_lnx *lnx, bool atlas_linear)
{
	if (lnx == NULL || lnx->atlas_linear == atlas_linear) {
		return;
	}
	lnx->atlas_linear = atlas_linear;
	/* Store + mark dirty only; leiasr_lnx_weave() does the one SDK call.
	 * srWeaverSetShaderSRGBConversion has no documented thread-safety, and
	 * the weave is the one place we know we are on the compositor's render
	 * thread. Sticky either way — a declaration that repeats its value costs
	 * nothing (early-out above), so nothing here runs per frame. */
	lnx->srgb_dirty = true;
}


/*
 *
 * Drag phase-snap (runtime#1588) — srWeaverSnapToPhase.
 *
 * ONE SDK call, made unconditionally, with no capability probe in front of it.
 * That is the SDK's own design, not an omission: the loader trampoline
 * (sr_loader.c) writes *pX = targetX / *pY = targetY BEFORE anything can fail,
 * and only then null-checks the weaver and the dispatch slot — so a runtime
 * that predates the call returns SR_ERROR_FUNCTION_UNSUPPORTED with the target
 * already written back. Identity comes for free and there is nothing to probe.
 *
 * WHAT THE BUILD MUST GUARANTEE, and why no runtime guard could.  The loader
 * archive has to come from the SAME tree as the runtime it will talk to, and
 * only the build can establish that (SRSDK_ROOT). Hence: link the trampoline,
 * never reimplement the dispatch.
 *
 * Be precise about the reason, because the obvious one is not true today.
 * The two v2 lines do NOT currently disagree about slot meanings: measured
 * header-to-header, this Linux line's 74 slots are a byte-identical prefix of
 * the Windows release-candidate line's 90, with pfnWeaverSetPresentOrigin at
 * slot 74 and pfnWeaverSnapToPhase at slot 75 on both. So a hand-rolled
 * dispatch lookup against a same-line runtime would in fact work right now.
 *
 * The hazard is forward-looking, which is worse, not better: the RC line
 * already occupies fifteen slots past pfnWeaverSnapToPhase, so the next append
 * on THIS line collides with them unless the two are reconciled first. On the
 * day that happens, a hand-rolled index keeps compiling, keeps returning a
 * non-NULL pointer, and silently calls the wrong function — there is no length
 * change and no version bump for a probe to catch. The trampoline is immune
 * because the loader and the runtime move together. Verified with the vendor
 * 2026-09-20; an earlier note here claimed the lines had ALREADY diverged at
 * equal size, which the vendor has since retracted.
 */

bool
leiasr_lnx_snap_to_phase(struct leiasr_lnx *lnx,
                         int32_t origin_x,
                         int32_t origin_y,
                         int32_t target_x,
                         int32_t target_y,
                         int32_t *out_x,
                         int32_t *out_y)
{
	if (out_x == NULL || out_y == NULL) {
		return false;
	}
	*out_x = target_x; /* default on every path below: no-op snap */
	*out_y = target_y;

#ifdef DXR_LEIA_LNX_HAVE_SR_SNAP
	if (lnx == NULL || lnx->weaver == NULL) {
		return false;
	}

	int32_t sx = target_x;
	int32_t sy = target_y;
	const SrResult res = srWeaverSnapToPhase(lnx->weaver, origin_x, origin_y, target_x, target_y, &sx, &sy);

	if (res == SR_DECLINED) {
		/* "Could not snap yet" — typically no viewing distance before the
		 * first tracked frame. A real answer, not a failure, and reported
		 * distinctly on purpose: an unsnapped position returned as success
		 * is indistinguishable from a snap that had nothing to correct.
		 * INFO, not WARN: it is the expected state during warm-up. */
		static bool logged_declined;
		if (!logged_declined) {
			U_LOG_I("leia_sr_sdk: srWeaverSnapToPhase declined (no viewing distance yet) — using the "
			        "raw drag target until tracking settles");
			logged_declined = true;
		}
		return false;
	}

	if (res == SR_ERROR_FUNCTION_UNSUPPORTED) {
		/* The loaded SR runtime has no snap slot — i.e. it is older than the
		 * loader we linked against. On a bring-up box that almost always
		 * means SR_RUNTIME_PATH did not take and the installed runtime was
		 * discovered instead, so name the code: it is the difference between
		 * "this build cannot snap" and "this RUN picked the wrong runtime". */
		static bool logged_unsupported;
		if (!logged_unsupported) {
			U_LOG_W("leia_sr_sdk: srWeaverSnapToPhase reports %s — this SR runtime predates the call "
			        "(check SR_RUNTIME_PATH); window drags will not phase-snap",
			        sdk_sr_result_str(res));
			logged_unsupported = true;
		}
		return false;
	}

	if (res == SR_ERROR_FEATURE_NOT_SUPPORTED) {
		/* The weaver cannot report the display orientation, without which the
		 * snap cannot be correct — the runtime declines rather than guessing
		 * landscape. Distinct from FUNCTION_UNSUPPORTED: the call exists, the
		 * display does not support it. */
		static bool logged_feature;
		if (!logged_feature) {
			U_LOG_W("leia_sr_sdk: srWeaverSnapToPhase reports %s — the weaver cannot report the display "
			        "orientation; window drags will not phase-snap",
			        sdk_sr_result_str(res));
			logged_feature = true;
		}
		return false;
	}

	if (SR_FAILED(res)) {
		static bool logged_failed;
		if (!logged_failed) {
			U_LOG_W("leia_sr_sdk: srWeaverSnapToPhase failed: %s (%d)", sdk_sr_result_str(res), (int)res);
			logged_failed = true;
		}
		return false;
	}

	/* Log the FIRST success, once. Without it there is no way to tell "the
	 * snap ran and corrected the target" from "the snap was never called and
	 * the drag happened to look right" — the ambiguity that made the Windows
	 * investigation (LeiaInc/LeiaSR#163) expensive. The delta is included so
	 * a zero correction reads as a correction of zero, not as a no-op. */
	static bool logged_live;
	if (!logged_live) {
		U_LOG_W("leia_sr_sdk: srWeaverSnapToPhase LIVE — (%d,%d) -> (%d,%d), delta (%d,%d)", target_x, target_y,
		        sx, sy, sx - target_x, sy - target_y);
		logged_live = true;
	}

	*out_x = sx;
	*out_y = sy;
	return true;
#else
	/* Built against an SDK tree that does not declare (or cannot link)
	 * srWeaverSnapToPhase — only reachable with the explicit
	 * -DDXR_LEIA_LNX_ALLOW_NO_SNAP=ON opt-out (#271). The feature compiles out
	 * to identity, the same answer the trampoline gives on an older runtime.
	 * Not logged per call: the DP says it once at creation, via
	 * leiasr_lnx_has_sr_snap(). See drv_leia_linux/CMakeLists.txt. */
	(void)lnx;
	(void)origin_x;
	(void)origin_y;
	return false;
#endif
}

bool
leiasr_lnx_has_sr_snap(const char **out_reason)
{
#ifdef DXR_LEIA_LNX_HAVE_SR_SNAP
	(void)out_reason;
	return true;
#else
	if (out_reason != NULL) {
		/* Do NOT spell the SR function name here: the release check greps
		 * the .so's strings for it to prove the call is compiled in (#271). */
		*out_reason = "built against an SR SDK without the drag-snap call "
		              "(-DDXR_LEIA_LNX_ALLOW_NO_SNAP=ON; needs srWeaver SnapToPhase + SetPresentOrigin, "
		              "LeiaSR#85)";
	}
	return false;
#endif
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
		/* The SDK announces a viewer only on a TRANSITION (USER_FOUND). A viewer
		 * already in front of the panel when this client connects is never
		 * announced, so the event latch alone reported "not tracking" for the
		 * whole session while real eye samples streamed in — measured on a DS1:
		 * valid, moving eye positions, zero USER_FOUND events. Anything gated on
		 * tracking (the eye-tracking state, apps that hold still until the
		 * tracker locks) then never engaged. So a fresh real sample also counts
		 * as tracking. USER_LOST still clears user_present immediately, and
		 * samples stop when the viewer leaves, so freshness lapses within
		 * kSampleFreshNs rather than latching on. */
		const int64_t kSampleFreshNs = 250 * 1000 * 1000; /* ~8 samples at 30 Hz */
		const int64_t last = atomic_load(&g_ctx.last_pair_mono_ns);
		const bool fresh = last != 0 && ((int64_t)os_monotonic_get_ns() - last) < kSampleFreshNs;
		*out_is_tracking = (atomic_load(&g_ctx.user_present) || fresh) && atomic_load(&g_ctx.device_ready) &&
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

/*
 * Lens ownership (LeiaSR #266) -- read leia_lens_owner_linux.h first.
 *
 * The short version: 3D before we have ever turned the lens off is left to the
 * weaver (no SDK call), the first 2D request takes the context's lens
 * preference away from the weaver for good, and from then on every request is
 * sent. So whoever asks for 2D here -- the app, or the runtime degrading on its
 * behalf -- must ask for the previous state back when the reason clears,
 * because nothing else ever will.
 */
bool
leiasr_lnx_request_display_mode(struct leiasr_lnx *lnx, bool enable_3d)
{
	(void)lnx;
	pthread_mutex_lock(&g_ctx_lock);
	if (g_ctx.lens == NULL) {
		pthread_mutex_unlock(&g_ctx_lock);
		static bool logged;
		if (!logged) {
			U_LOG_W("leia_sr_sdk: request_display_mode(%s) with no lens handle — ignored",
			        enable_3d ? "3D" : "2D");
			logged = true;
		}
		return false;
	}
	const enum leia_lens_action action = leia_lens_owner_on_request(&g_lens_owner, enable_3d);
	if (action == LEIA_LENS_ACTION_NONE) {
		static bool logged_delegated;
		if (!logged_delegated) {
			U_LOG_W("leia_sr_sdk: request_display_mode(3D) left to the weaver — no lens call until "
			        "something asks for 2D (keeps the weaver's off-panel lens release)");
			logged_delegated = true;
		}
	}
	const bool ok = sr_ctx_send_lens_locked(action, enable_3d ? "request_display_mode(3D)"
	                                                          : "request_display_mode(2D)");
	pthread_mutex_unlock(&g_ctx_lock);
	return ok;
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


/*
 *
 * Multi-screen M0 — per-monitor identity (leia_sr_linux.h). Neither entry
 * point creates an SR context: see the seam header for why.
 *
 */

bool
leiasr_lnx_peek_fpc_serial(char *out_serial, size_t cap)
{
	if (out_serial == NULL || cap == 0) {
		return false;
	}
	out_serial[0] = '\0';
	bool ok = false;
	pthread_mutex_lock(&g_ctx_lock);
	if (g_ctx.state == SR_CTX_READY && !atomic_load(&g_ctx.context_invalid) && g_ctx.lens != NULL) {
		uint32_t written = 0;
		const SrResult res = srLensGetSerialNumber(g_ctx.lens, out_serial, (uint32_t)cap, &written);
		if (SR_SUCCEEDED(res)) {
			ok = out_serial[0] != '\0';
		} else {
			LOG_SR_ONCE("srLensGetSerialNumber", res);
			out_serial[0] = '\0';
		}
	}
	pthread_mutex_unlock(&g_ctx_lock);
	return ok;
}

#ifdef DXR_LEIA_LNX_HAVE_SR_DISPLAY_ENUM

/* Cache of the last enumeration, guarded by g_ctx_lock. Keyed by the context
 * generation: a new context (SRService restart) re-enumerates. */
static struct leia_lnx_sr_display g_sr_displays[LEIA_LNX_SR_MAX_DISPLAYS];
static int32_t g_sr_display_count = -1;
static uint32_t g_sr_displays_generation = 0; // 0 = never enumerated
static uint32_t g_sr_caps_logged_generation = 0;

/*! Capability query, once per context: logged for bring-up and gating. A
 *  runtime that predates the structs leaves them at "not supported". */
static void
sr_log_display_caps_locked(uint32_t gen)
{
	if (g_sr_caps_logged_generation == gen) {
		return;
	}
	g_sr_caps_logged_generation = gen;

	SrDisplayBindingCapabilities bind_caps = SrDisplayBindingCapabilities();
	SrWeaverRoutingCapabilities route_caps = SrWeaverRoutingCapabilities();
	SrRuntimeCapabilities caps = SrRuntimeCapabilities();
	route_caps.pNext = &bind_caps;
	caps.pNext = &route_caps;
	const SrResult res = srGetRuntimeCapabilities(g_ctx.instance, &caps);
	if (SR_FAILED(res)) {
		U_LOG_W("leia_sr_sdk: srGetRuntimeCapabilities failed: %s", srResultToString(res));
		return;
	}
	U_LOG_W("leia_sr_sdk: SR multi-display caps: externalRouting=%u displayBinding=%u maxBoundDisplays=%u",
	        (unsigned)route_caps.externalRouting, (unsigned)bind_caps.displayBinding,
	        (unsigned)bind_caps.maxBoundDisplays);
}

static void
sr_display_from_descriptor(const SrDisplayDescriptor *d, struct leia_lnx_sr_display *out)
{
	memset(out, 0, sizeof(*out));
	out->display_id = d->displayId;
	out->fpc_verified = d->confidence == SR_DISPLAY_CONFIDENCE_FPC_VERIFIED;
	if (out->fpc_verified) {
		snprintf(out->serial, sizeof(out->serial), "%.*s", (int)sizeof(d->serial), d->serial);
	}
	snprintf(out->product_code, sizeof(out->product_code), "%.*s", (int)sizeof(d->productCode) - 1, d->productCode);
	out->manufacturer_id = leia_lnx_pnp_to_manufacturer_id(d->edidVendor);
	out->product_id = d->edidProduct;
	out->edid_serial = d->edidSerial;
	snprintf(out->connector, sizeof(out->connector), "%.*s", (int)sizeof(d->connector), d->connector);
	snprintf(out->output_name, sizeof(out->output_name), "%.*s", (int)sizeof(d->outputName), d->outputName);
	out->native_w = d->nativeWidth > 0 ? (uint32_t)d->nativeWidth : 0;
	out->native_h = d->nativeHeight > 0 ? (uint32_t)d->nativeHeight : 0;
	out->refresh_hz = d->refreshHz;
}

int32_t
leia_lnx_sr_enumerate_displays(struct leia_lnx_sr_display *out, uint32_t cap)
{
	int32_t ret = -1;
	pthread_mutex_lock(&g_ctx_lock);
	if (g_ctx.state != SR_CTX_READY || atomic_load(&g_ctx.context_invalid)) {
		goto out; // no live context — never create one just to probe
	}

	const uint32_t gen = atomic_load(&g_ctx.generation);
	sr_log_display_caps_locked(gen);

	const bool dirty = atomic_exchange(&g_ctx.display_topology_dirty, false);
	if (dirty || g_sr_displays_generation != gen) {
		// The cache is marked fresh for this context only once an answer
		// was read (review C): a transient failure must be retried on the
		// next probe, not stick until a topology event.
		g_sr_display_count = -1;
		g_sr_displays_generation = 0;

		uint32_t count = 0;
		SrResult res = srEnumerateDisplays(g_ctx.instance, &count, NULL);
		if (SR_FAILED(res)) {
			// SR_ERROR_FUNCTION_UNSUPPORTED = the installed runtime is older
			// than these headers (e.g. 1.38): the 1.38 path takes over. That
			// answer is final for this context; anything else is retried.
			LOG_SR_ONCE("srEnumerateDisplays (count)", res);
			if (res == SR_ERROR_FUNCTION_UNSUPPORTED) {
				g_sr_displays_generation = gen;
			}
			goto out;
		}
		SrDisplayDescriptor descs[LEIA_LNX_SR_MAX_DISPLAYS];
		if (count > LEIA_LNX_SR_MAX_DISPLAYS) {
			count = LEIA_LNX_SR_MAX_DISPLAYS;
		}
		for (uint32_t i = 0; i < count; i++) {
			descs[i] = SrDisplayDescriptor();
		}
		if (count > 0) {
			res = srEnumerateDisplays(g_ctx.instance, &count, descs);
			if (SR_FAILED(res)) { // SR_INCOMPLETE is a success code: keep the first `count`
				LOG_SR_ONCE("srEnumerateDisplays", res);
				goto out;
			}
		}
		for (uint32_t i = 0; i < count; i++) {
			sr_display_from_descriptor(&descs[i], &g_sr_displays[i]);
			U_LOG_W(
			    "leia_sr_sdk: SR display #%u id=0x%016llx %s connector=%s output=%s edid=%u/%u/%u "
			    "%ux%u %.2f Hz product=%s",
			    i, (unsigned long long)g_sr_displays[i].display_id,
			    g_sr_displays[i].fpc_verified ? "FPC_VERIFIED" : "EDID_ONLY", g_sr_displays[i].connector,
			    g_sr_displays[i].output_name, g_sr_displays[i].manufacturer_id, g_sr_displays[i].product_id,
			    g_sr_displays[i].edid_serial, g_sr_displays[i].native_w, g_sr_displays[i].native_h,
			    g_sr_displays[i].refresh_hz, g_sr_displays[i].product_code);
		}
		g_sr_display_count = (int32_t)count;
		g_sr_displays_generation = gen;
	}

	if (g_sr_display_count >= 0) {
		const uint32_t n = (uint32_t)g_sr_display_count < cap ? (uint32_t)g_sr_display_count : cap;
		if (out != NULL && n > 0) {
			memcpy(out, g_sr_displays, n * sizeof(out[0]));
		}
		ret = (int32_t)n;
	}

out:
	pthread_mutex_unlock(&g_ctx_lock);
	return ret;
}

#else // !DXR_LEIA_LNX_HAVE_SR_DISPLAY_ENUM

int32_t
leia_lnx_sr_enumerate_displays(struct leia_lnx_sr_display *out, uint32_t cap)
{
	// SDK headers/loader predate srEnumerateDisplays (e.g. LeiaSR 1.38):
	// probe_displays falls back to the SR 1.38 evidence (lens serial).
	(void)out;
	(void)cap;
	return -1;
}

#endif // DXR_LEIA_LNX_HAVE_SR_DISPLAY_ENUM
