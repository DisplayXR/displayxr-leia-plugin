// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  macOS Metal display processor over the srSDK Metal weaver
 *         (sr/sr_metal.h, v2 C API).
 *
 * Per frame (process_atlas, on the compositor's render thread):
 *
 *   - clear the target, so pixels outside the canvas are defined;
 *   - 2x1 atlas  -> srSDK Metal weaver. The atlas is the runtime's
 *                   content-sized crop (ADR-030), i.e. exactly the
 *                   side-by-side stereo pair the weaver consumes (each view
 *                   half the panel width — "anamorphic SBS"). Recorded into
 *                   the RUNTIME's command buffer, rendered into the target,
 *                   viewport + scissor = canvas;
 *   - anything else (1x1 2D frame, an N-view grid) -> passthrough blit of
 *     view 0 into the canvas, weaver bypassed — the Linux/Windows 2D path.
 *
 * Window phase: srWeaverSetPresentOrigin gets the app window's content origin
 * relative to the panel's CGDisplayBounds, in panel (backing) pixels, whenever
 * that origin changes (window moves; first frame). Never latched off: an SR
 * runtime without IWeaverPresentOrigin1 answers FEATURE_NOT_SUPPORTED and the
 * next move retries, so a runtime swap is picked up live. Each distinct result
 * code is logged once. A window whose content rect does not touch the panel
 * skips the call (logged once per off-panel episode); a window that STRADDLES
 * the panel sends its origin even when negative (origin + canvas offset is the
 * segment's top-left on the panel). Multi-screen: segment DPs made by
 * create_dp_metal_for_screen get the origin from the runtime
 * (set_present_origin) and forward it. The runtime also sends the APPLIED
 * origin to the session DP while it owns the window's placement (ADR-050);
 * polling yields on every frame the slot was called for, and runs on the
 * rest (e.g. split frames, where the primary DP is not sent one).
 *
 * Colour: macOS colour-matches every window from its layer's colour space to
 * the display profile, which mixes subpixels across views on a lenticular
 * panel. The SR weaver tags `window.contentView.layer` with the display's
 * colour space itself, but only when that layer IS the CAMetalLayer; the
 * runtime adds its CAMetalLayer as a SUBLAYER for views that are not
 * layer-backed by Metal (GL apps). So this DP tags the layer it actually
 * presents into, with CGDisplayCopyColorSpace(the Leia PANEL) — the weave is
 * computed for the panel's subpixels whatever screen the window is on now —
 * falling back to the window's screen only when the panel's CGDirectDisplayID
 * is unknown. The window's screen is re-checked every 250 ms (a screen change
 * is logged and re-tags); the tag is re-applied when the presenting layer or
 * the target display changes, AND whenever the layer's or the window's colour
 * space no longer equals the panel's — something in the app (a VkSurface /
 * swapchain on the same layer, AppKit on a backing change, an app that sets
 * its own colour space) can reset it after the one-shot tag, and a reset tag
 * is exactly the colour fringe the tag exists to prevent. Each reset is
 * logged (throttled) with what the colour space was reset to.
 * DXR_LEIA_MAC_COLORSPACE_TRACE=1 logs the presenting layer, the view's layer
 * and both colour spaces about once a second. DXR_LEIA_MAC_KEEP_COLOR_MATCHING=1
 * skips the tag (A/B), as SR_METAL_KEEP_COLOR_MATCHING=1 does on the SR side.
 *
 * @ingroup drv_leia
 */

#include "leia_display_processor_macos.h"
#include "leia_sr_macos.h"
#include "leia_lens_owner_linux.h" // pure-C lens ownership rules (LeiaSR #266); not Linux-specific

#include "xrt/xrt_display_processor_metal.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <sr/sr.h>
#include <sr/sr_metal.h>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#include <unistd.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEBUG_GET_ONCE_BOOL_OPTION(leia_mac_keep_color_matching, "DXR_LEIA_MAC_KEEP_COLOR_MATCHING", false)
DEBUG_GET_ONCE_BOOL_OPTION(leia_mac_colorspace_trace, "DXR_LEIA_MAC_COLORSPACE_TRACE", false)
//! Period of the throttled "weaver eyes" INFO line (ms). Verification scripts
//! lower it to collect enough samples in a short run; never per frame.
DEBUG_GET_ONCE_NUM_OPTION(leia_mac_eye_log_ms, "DXR_LEIA_MAC_EYE_LOG_MS", 5000)

#define LEIA_MAC_HALF_IPD_MM 31.5f
#define LEIA_MAC_EYE_FRESH_NS (250ll * 1000 * 1000)
#define LEIA_MAC_TAG_CHECK_NS (250ull * 1000 * 1000)
//! After a tag is issued (async, on the main queue), don't judge the layer's
//! colour space until it has had time to land.
#define LEIA_MAC_TAG_SETTLE_NS (500ull * 1000 * 1000)
#define LEIA_MAC_TAG_TRACE_NS (1000ull * 1000 * 1000)
#define LEIA_MAC_MAX_LOGGED_RESULTS 8

static NSString *const k_blit_msl =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut { float4 pos [[position]]; float2 uv; };\n"
     "vertex VOut leia_blit_vs(uint vid [[vertex_id]]) {\n"
     "    VOut o; o.uv = float2((vid << 1) & 2, vid & 2);\n"
     "    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
     "struct Tile { float2 scale; };\n"
     "fragment float4 leia_blit_fs(VOut in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
     "                             sampler s [[sampler(0)]], constant Tile &tile [[buffer(0)]]) {\n"
     "    return t.sample(s, in.uv * tile.scale); }\n";

struct leia_dp_mac
{
	struct xrt_display_processor_metal base; //!< MUST be first

	id<MTLDevice> device;        //!< retained
	id<MTLCommandQueue> queue;   //!< retained
	NSView *view;                //!< retained; NULL for hosted apps
	NSWindow *window;            //!< retained; the view's window at creation

	SrInstance inst;
	SrWeaver weaver;
	SrEyeTracker tracker;
	SrLens lens;
	struct leia_lens_owner lens_owner;

	_Atomic int64_t last_pair_ns; //!< monotonic time of the last raw eye sample (tracking freshness)
	SrPoint3f last_good_l, last_good_r;
	bool have_last_good;

	//! Passthrough (2D) pipeline, built for one target pixel format.
	id<MTLRenderPipelineState> blit_pipeline;
	MTLPixelFormat blit_format;
	id<MTLSamplerState> sampler;

	//! Colour-space tagging state (see file header).
	void *tagged_layer;
	uint32_t tagged_did;    //!< display whose colour space the layer carries
	uint32_t window_did;    //!< screen the window was last seen on (0 = never)
	uint64_t last_tag_check_ns;
	uint64_t last_tag_issued_ns; //!< when the last tag was dispatched (settle window)
	uint64_t last_tag_trace_ns;
	uint64_t retags;             //!< times the tag was found reset and re-applied
	CGColorSpaceRef panel_cs;    //!< cached CGDisplayCopyColorSpace(panel_cs_did); owned
	uint32_t panel_cs_did;

	//! Created by create_dp_metal_for_screen: windowless, one display, and NOT
	//! the first writer to the target this frame (load, never clear; every
	//! write confined to the canvas).
	bool screen_bound;
	//! The SR display a screen-bound DP was made for (srEnumerateDisplays
	//! entry, resolved at creation and owned here — never the process-wide
	//! "active panel" cache, which a topology change rewrites).
	struct leia_mac_display_info screen;
	//! Weaver created with SR_WEAVER_ROUTING_EXTERNAL: it never votes the
	//! lens, so this DP owns it (lens_owner.external_weavers = 1).
	bool external_routing;
	//! The runtime has called set_present_origin at least once (logging only).
	bool runtime_origin;
	//! Frame (ldp->frames numbering) the runtime's set_present_origin was last
	//! called for. Polling yields on that frame only: the runtime sends the
	//! APPLIED origin of a window it is moving (ADR-050) while the view still
	//! reports the pre-move position until the CA transaction commits, but it
	//! does not send it on every path (split frames skip the primary DP).
	uint64_t runtime_origin_frame;

	//! Present-origin state: last origin SENT (any result), so the call is
	//! repeated only when the window moves; result codes already logged.
	bool origin_sent;
	int32_t last_origin_x, last_origin_y;
	bool origin_off_panel;  //!< current episode of "content origin not on the panel" logged
	SrResult logged_results[LEIA_MAC_MAX_LOGGED_RESULTS];
	uint32_t logged_result_count;

	uint64_t frames;
	uint64_t last_eye_log_ns;
	bool weave_logged, blit_logged, grid_logged, size_logged;
};

static inline struct leia_dp_mac *
leia_dp_mac(struct xrt_display_processor_metal *xdp)
{
	return (struct leia_dp_mac *)xdp;
}


/*
 *
 * Eye samples (SR thread).
 *
 */

static void SR_CALL
leia_dp_mac_on_eye_pair(const SrEyePair *pair, void *user_data)
{
	(void)pair;
	struct leia_dp_mac *ldp = (struct leia_dp_mac *)user_data;
	atomic_store(&ldp->last_pair_ns, (int64_t)os_monotonic_get_ns());
}


/*
 *
 * Window geometry helpers.
 *
 */

//! The CAMetalLayer the compositor presents into: the view's own layer, or the
//! sublayer the runtime adds to a non-Metal view (setup_external_window).
static CAMetalLayer *
find_metal_layer(NSView *view)
{
	if (view == nil) {
		return nil;
	}
	CALayer *layer = view.layer;
	if ([layer isKindOfClass:[CAMetalLayer class]]) {
		return (CAMetalLayer *)layer;
	}
	for (CALayer *sub in layer.sublayers) {
		if ([sub isKindOfClass:[CAMetalLayer class]]) {
			return (CAMetalLayer *)sub;
		}
	}
	return nil;
}

//! The view's top-left in CoreGraphics global points (origin = top-left of the
//! primary display, y down) — the space CGDisplayBounds is in.
static bool
view_origin_cg_points(NSView *view, CGFloat *out_x, CGFloat *out_y, CGFloat *out_w, CGFloat *out_h)
{
	NSWindow *win = view.window;
	NSArray<NSScreen *> *screens = [NSScreen screens];
	if (win == nil || screens.count == 0) {
		return false;
	}
	const NSRect in_win = [view convertRect:view.bounds toView:nil];
	const NSRect in_screen = [win convertRectToScreen:in_win]; // AppKit global, y up
	const CGFloat primary_h = screens[0].frame.size.height;   // screens[0] = the menu-bar screen
	*out_x = in_screen.origin.x;
	*out_y = primary_h - (in_screen.origin.y + in_screen.size.height);
	*out_w = in_screen.size.width;
	*out_h = in_screen.size.height;
	return true;
}

//! True the first time @p res is seen (per DP) — for once-per-code logging.
static bool
first_time_result(struct leia_dp_mac *ldp, SrResult res)
{
	for (uint32_t i = 0; i < ldp->logged_result_count; i++) {
		if (ldp->logged_results[i] == res) {
			return false;
		}
	}
	if (ldp->logged_result_count < LEIA_MAC_MAX_LOGGED_RESULTS) {
		ldp->logged_results[ldp->logged_result_count++] = res;
	}
	return true;
}

static void
update_present_origin(struct leia_dp_mac *ldp)
{
	if (ldp->runtime_origin_frame == ldp->frames || ldp->view == nil || ldp->weaver == NULL) {
		return; // the runtime's set_present_origin is authoritative for this frame
	}
	struct leia_mac_display_info info;
	if (!leia_mac_get_display_info(&info) || info.cg_display_id == 0) {
		return;
	}
	CGFloat vx = 0, vy = 0, vw = 0, vh = 0;
	if (!view_origin_cg_points(ldp->view, &vx, &vy, &vw, &vh)) {
		return;
	}
	const CGRect b = CGDisplayBounds(info.cg_display_id);
	const CGFloat rx = vx - b.origin.x;
	const CGFloat ry = vy - b.origin.y;

	// The content rect does not touch the panel = nothing of the window is
	// woven there: skip, log once per episode, send again once it is back.
	// A window that STRADDLES the panel keeps its (possibly negative) origin:
	// the weave adds the canvas offset, and origin + canvas offset is the
	// segment's top-left on the panel (the runtime's multi-screen contract,
	// which computes e.g. -800,300 for a window hanging off the panel's left).
	if (rx + vw <= 0.0 || ry + vh <= 0.0 || rx >= b.size.width || ry >= b.size.height) {
		if (!ldp->origin_off_panel) {
			ldp->origin_off_panel = true;
			U_LOG_W("leia_mac_dp: window content (%.0f, %.0f) pt does not reach the Leia panel "
			        "(display %u, bounds %.0f,%.0f %.0fx%.0f pt) — not calling srWeaverSetPresentOrigin "
			        "until it is on the panel",
			        vx, vy, info.cg_display_id, b.origin.x, b.origin.y, b.size.width, b.size.height);
		}
		ldp->origin_sent = false;
		return;
	}
	if (ldp->origin_off_panel) {
		ldp->origin_off_panel = false;
		U_LOG_W("leia_mac_dp: window content origin is on the Leia panel again (%.0f, %.0f pt panel-relative)",
		        rx, ry);
	}

	const double scale = info.backing_scale > 0.0f ? info.backing_scale : 1.0;
	const int32_t ox = (int32_t)llround(rx * scale);
	const int32_t oy = (int32_t)llround(ry * scale);
	if (ldp->origin_sent && ox == ldp->last_origin_x && oy == ldp->last_origin_y) {
		return; // unchanged since the last call, whatever it answered
	}

	const SrResult res = srWeaverSetPresentOrigin(ldp->weaver, ox, oy);
	ldp->origin_sent = true;
	ldp->last_origin_x = ox;
	ldp->last_origin_y = oy;
	if (first_time_result(ldp, res)) {
		U_LOG_W("leia_mac_dp: srWeaverSetPresentOrigin(%d, %d) panel px -> %s%s", ox, oy,
		        leia_mac_sr_result_str(res),
		        SR_SUCCEEDED(res) ? " (accepted)"
		                          : " — retried on every window move; this code is logged once");
	} else {
		// INFO: only on window moves, never per frame.
		U_LOG_I("leia_mac_dp: present origin (%d, %d) panel px -> %s", ox, oy, leia_mac_sr_result_str(res));
	}
}

static uint32_t
screen_display_id(NSScreen *screen)
{
	return screen != nil ? [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue] : 0;
}

//! Same colour space? CFEqual first; an equivalent space created another way
//! (a copy, an NSColorSpace round trip) compares by its ICC profile.
static bool
colorspace_same(CGColorSpaceRef a, CGColorSpaceRef b)
{
	if (a == b) {
		return true;
	}
	if (a == NULL || b == NULL) {
		return false;
	}
	if (CFEqual(a, b)) {
		return true;
	}
	CFDataRef ia = CGColorSpaceCopyICCData(a);
	CFDataRef ib = CGColorSpaceCopyICCData(b);
	const bool same = ia != NULL && ib != NULL && CFEqual(ia, ib);
	if (ia != NULL) {
		CFRelease(ia);
	}
	if (ib != NULL) {
		CFRelease(ib);
	}
	return same;
}

//! A short printable name for a colour space (its name, else "unnamed"/"none").
static void
colorspace_name(CGColorSpaceRef cs, char *out, size_t out_size)
{
	if (cs == NULL) {
		snprintf(out, out_size, "none");
		return;
	}
	CFStringRef name = CGColorSpaceCopyName(cs);
	if (name == NULL || !CFStringGetCString(name, out, (CFIndex)out_size, kCFStringEncodingUTF8)) {
		snprintf(out, out_size, "unnamed(%p)", (void *)cs);
	}
	if (name != NULL) {
		CFRelease(name);
	}
}

//! Tag the presenting layer (and window) with the Leia panel's colour space so
//! WindowServer applies no colour matching to the woven frame. See header.
static void
update_colorspace_tag(struct leia_dp_mac *ldp)
{
	if (ldp->view == nil || debug_get_bool_option_leia_mac_keep_color_matching()) {
		return;
	}
	const uint64_t now = os_monotonic_get_ns();
	if (ldp->last_tag_check_ns != 0 && now - ldp->last_tag_check_ns < LEIA_MAC_TAG_CHECK_NS) {
		return;
	}
	ldp->last_tag_check_ns = now;

	CAMetalLayer *layer = find_metal_layer(ldp->view);
	NSWindow *win = ldp->view.window;
	const uint32_t win_did = screen_display_id(win.screen);
	if (layer == nil || win == nil) {
		return;
	}
	struct leia_mac_display_info info;
	const uint32_t panel_did = leia_mac_get_display_info(&info) ? info.cg_display_id : 0;

	if (win_did != ldp->window_did) {
		U_LOG_W("leia_mac_dp: window is on display %u%s", win_did,
		        panel_did == 0 ? " (Leia panel display unknown)"
		        : win_did == panel_did ? " (the Leia panel)"
		                               : " (NOT the Leia panel — move it onto the panel to see the weave)");
		ldp->window_did = win_did;
		ldp->origin_sent = false; // re-send the present origin after a screen change
	}

	// The weave targets the panel: its colour space, wherever the window is.
	const uint32_t did = panel_did != 0 ? panel_did : win_did;
	if (did == 0) {
		return;
	}
	if (ldp->panel_cs == NULL || ldp->panel_cs_did != did) {
		if (ldp->panel_cs != NULL) {
			CGColorSpaceRelease(ldp->panel_cs);
		}
		ldp->panel_cs = CGDisplayCopyColorSpace(did);
		ldp->panel_cs_did = did;
	}
	if (ldp->panel_cs == NULL) {
		return;
	}

	// What the layer / window carry NOW (cheap property reads; a reset by the
	// app or AppKit after the one-shot tag shows up here).
	CGColorSpaceRef layer_cs = layer.colorspace;
	CGColorSpaceRef win_cs = win.colorSpace != nil ? win.colorSpace.CGColorSpace : NULL;
	const bool layer_ok = colorspace_same(layer_cs, ldp->panel_cs);
	const bool win_ok = colorspace_same(win_cs, ldp->panel_cs);

	if (debug_get_bool_option_leia_mac_colorspace_trace() &&
	    (ldp->last_tag_trace_ns == 0 || now - ldp->last_tag_trace_ns >= LEIA_MAC_TAG_TRACE_NS)) {
		ldp->last_tag_trace_ns = now;
		char ln[96], wn[96], pn[96];
		colorspace_name(layer_cs, ln, sizeof(ln));
		colorspace_name(win_cs, wn, sizeof(wn));
		colorspace_name(ldp->panel_cs, pn, sizeof(pn));
		U_LOG_W("leia_mac_dp: [cs-trace] presenting layer %p (%s, view.layer sublayers %lu, EDR %d, fmt %lu, "
		        "opaque %d) layer cs=%s%s window cs=%s%s panel(display %u) cs=%s; window on display %u, "
		        "retags %llu",
		        (void *)layer, (CALayer *)layer == ldp->view.layer ? "= view.layer" : "a sublayer of view.layer",
		        (unsigned long)ldp->view.layer.sublayers.count, (int)layer.wantsExtendedDynamicRangeContent,
		        (unsigned long)layer.pixelFormat, (int)layer.opaque, ln, layer_ok ? " [ok]" : " [MISMATCH]", wn,
		        win_ok ? " [ok]" : " [MISMATCH]", did, pn, win_did, (unsigned long long)ldp->retags);
		// 1:1 check: a contentsScale != the window's backing scale (or a
		// drawable != bounds x contentsScale) makes WindowServer RESAMPLE the
		// woven frame, which shows as colour bands.
		const CGSize ds = layer.drawableSize;
		const CGRect lb = layer.bounds;
		const CGFloat ws = win.backingScaleFactor;
		const bool one_to_one = layer.contentsScale == ws && ds.width == lb.size.width * layer.contentsScale &&
		                        ds.height == lb.size.height * layer.contentsScale;
		U_LOG_W("leia_mac_dp: [cs-trace] layer contentsScale %.2f window backing %.2f, bounds %.0fx%.0f pt, "
		        "drawable %.0fx%.0f px, filters mag=%s min=%s -> %s",
		        layer.contentsScale, ws, lb.size.width, lb.size.height, ds.width, ds.height,
		        layer.magnificationFilter.UTF8String, layer.minificationFilter.UTF8String,
		        one_to_one ? "1:1 [ok]" : "NOT 1:1 [RESAMPLED by WindowServer]");
	}

	const bool same_target = (void *)layer == ldp->tagged_layer && did == ldp->tagged_did;
	if (same_target && layer_ok && win_ok) {
		return;
	}
	// A tag is in flight on the main queue: let it land before judging it.
	if (same_target && ldp->last_tag_issued_ns != 0 && now - ldp->last_tag_issued_ns < LEIA_MAC_TAG_SETTLE_NS) {
		return;
	}

	if (same_target) {
		// Same layer, same panel, but the colour space moved: someone reset it.
		ldp->retags++;
		if (ldp->retags <= 3 || (ldp->retags & (ldp->retags - 1)) == 0) {
			char ln[96], wn[96];
			colorspace_name(layer_cs, ln, sizeof(ln));
			colorspace_name(win_cs, wn, sizeof(wn));
			U_LOG_W("leia_mac_dp: colour-space tag was RESET (layer cs=%s%s, window cs=%s%s) — re-applying "
			        "the panel's (display %u); reset #%llu (logged at 1, 2, 3, then powers of two)",
			        ln, layer_ok ? "" : " != panel", wn, win_ok ? "" : " != panel", did,
			        (unsigned long long)ldp->retags);
		}
	}

	ldp->tagged_layer = (void *)layer;
	ldp->tagged_did = did;
	ldp->last_tag_issued_ns = now;

	CGColorSpaceRef cs = CGColorSpaceRetain(ldp->panel_cs);
	[layer retain];
	[win retain];
	dispatch_async(dispatch_get_main_queue(), ^{
		layer.colorspace = cs;
		NSColorSpace *ns = [[NSColorSpace alloc] initWithCGColorSpace:cs];
		win.colorSpace = ns;
		[ns release];
		CGColorSpaceRelease(cs);
		[layer release];
		[win release];
	});
	if (!same_target) {
		U_LOG_W("leia_mac_dp: tagged the presenting CAMetalLayer (%s) + NSWindow with the colour space of "
		        "display %u%s — WindowServer applies no colour matching to the woven frame",
		        (CALayer *)layer == ldp->view.layer ? "the view's layer" : "a runtime-added sublayer", did,
		        did == panel_did ? " (the Leia panel)" : " (window's screen; Leia panel display unknown)");
	}
}


/*
 *
 * 2D / fallback path.
 *
 */

static bool
ensure_blit_pipeline(struct leia_dp_mac *ldp, MTLPixelFormat format)
{
	if (ldp->blit_pipeline != nil && ldp->blit_format == format) {
		return true;
	}
	[ldp->blit_pipeline release];
	ldp->blit_pipeline = nil;

	NSError *err = nil;
	id<MTLLibrary> lib = [ldp->device newLibraryWithSource:k_blit_msl options:nil error:&err];
	if (lib == nil) {
		U_LOG_E("leia_mac_dp: blit shader compile failed: %s", err.localizedDescription.UTF8String);
		return false;
	}
	id<MTLFunction> vs = [lib newFunctionWithName:@"leia_blit_vs"];
	id<MTLFunction> fs = [lib newFunctionWithName:@"leia_blit_fs"];
	MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
	desc.vertexFunction = vs;
	desc.fragmentFunction = fs;
	desc.colorAttachments[0].pixelFormat = format;
	ldp->blit_pipeline = [ldp->device newRenderPipelineStateWithDescriptor:desc error:&err];
	[desc release];
	[vs release];
	[fs release];
	[lib release];
	if (ldp->blit_pipeline == nil) {
		U_LOG_E("leia_mac_dp: blit pipeline failed: %s", err.localizedDescription.UTF8String);
		return false;
	}
	ldp->blit_format = format;

	if (ldp->sampler == nil) {
		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
		sd.minFilter = MTLSamplerMinMagFilterLinear;
		sd.magFilter = MTLSamplerMinMagFilterLinear;
		sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
		sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
		ldp->sampler = [ldp->device newSamplerStateWithDescriptor:sd];
		[sd release];
	}
	return ldp->sampler != nil;
}

//! Clear the whole target; when @p src is non-nil also draw the view-0 tile of
//! a @p cols x @p rows atlas into @p vp.
static void
encode_clear_and_blit(struct leia_dp_mac *ldp,
                      id<MTLCommandBuffer> cmd,
                      id<MTLTexture> target,
                      id<MTLTexture> src,
                      uint32_t cols,
                      uint32_t rows,
                      MTLViewport vp)
{
	// A screen-bound (segment) DP is never the first writer this frame: load
	// the target, and confine the draw to the canvas (viewport AND scissor).
	const bool load = ldp->screen_bound;
	if (load && src == nil) {
		return; // nothing to clear on a segment
	}
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = target;
	pass.colorAttachments[0].loadAction = load ? MTLLoadActionLoad : MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
	id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
	if (src != nil && ensure_blit_pipeline(ldp, target.pixelFormat)) {
		const float scale[2] = {1.0f / (float)(cols ? cols : 1), 1.0f / (float)(rows ? rows : 1)};
		[enc setViewport:vp];
		if (load) {
			const int64_t x0 = vp.originX < 0 ? 0 : (int64_t)vp.originX;
			const int64_t y0 = vp.originY < 0 ? 0 : (int64_t)vp.originY;
			int64_t x1 = (int64_t)(vp.originX + vp.width), y1 = (int64_t)(vp.originY + vp.height);
			x1 = x1 > (int64_t)target.width ? (int64_t)target.width : x1;
			y1 = y1 > (int64_t)target.height ? (int64_t)target.height : y1;
			if (x1 <= x0 || y1 <= y0) {
				[enc endEncoding];
				return;
			}
			[enc setScissorRect:(MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0),
			                                     (NSUInteger)(y1 - y0)}];
		}
		[enc setRenderPipelineState:ldp->blit_pipeline];
		[enc setFragmentTexture:src atIndex:0];
		[enc setFragmentSamplerState:ldp->sampler atIndex:0];
		[enc setFragmentBytes:scale length:sizeof(scale) atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	}
	[enc endEncoding];
}


/*
 *
 * Post-weave capture (debug): touch /tmp/dxr_leia_woven_trigger and the next
 * woven frame's target is written to /tmp/dxr_leia_woven.png — the pixels the
 * weaver produced, before WindowServer composites them (the runtime's
 * /tmp/dxr_atlas_trigger captures the PRE-weave atlas).
 *
 */

#define LEIA_MAC_WOVEN_TRIGGER "/tmp/dxr_leia_woven_trigger"
#define LEIA_MAC_WOVEN_PNG "/tmp/dxr_leia_woven.png"

static void
maybe_capture_woven(struct leia_dp_mac *ldp, id<MTLCommandBuffer> cmd, id<MTLTexture> target)
{
	if ((ldp->frames % 15) != 0 || access(LEIA_MAC_WOVEN_TRIGGER, F_OK) != 0) {
		return;
	}
	unlink(LEIA_MAC_WOVEN_TRIGGER);
	if (target.pixelFormat != MTLPixelFormatBGRA8Unorm && target.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB) {
		U_LOG_W("leia_mac_dp: woven capture skipped: target format %lu", (unsigned long)target.pixelFormat);
		return;
	}
	const NSUInteger w = target.width, h = target.height, row = w * 4;
	id<MTLBuffer> buf = [ldp->device newBufferWithLength:row * h options:MTLResourceStorageModeShared];
	if (buf == nil) {
		return;
	}
	id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
	[blit copyFromTexture:target
	                 sourceSlice:0
	                 sourceLevel:0
	                sourceOrigin:MTLOriginMake(0, 0, 0)
	                  sourceSize:MTLSizeMake(w, h, 1)
	                    toBuffer:buf
	           destinationOffset:0
	      destinationBytesPerRow:row
	    destinationBytesPerImage:row * h];
	[blit endEncoding];
	[cmd addCompletedHandler:^(id<MTLCommandBuffer> cb) {
		(void)cb;
		CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
		CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs,
		                                         kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
		CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
		bool ok = false;
		if (img != NULL) {
			CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)LEIA_MAC_WOVEN_PNG,
			                                                       strlen(LEIA_MAC_WOVEN_PNG), false);
			CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
			if (dst != NULL) {
				CGImageDestinationAddImage(dst, img, NULL);
				ok = CGImageDestinationFinalize(dst);
				CFRelease(dst);
			}
			CFRelease(url);
			CGImageRelease(img);
		}
		if (ctx != NULL) {
			CGContextRelease(ctx);
		}
		CGColorSpaceRelease(cs);
		U_LOG_W("leia_mac_dp: woven capture %lux%lu -> %s: %s", (unsigned long)w, (unsigned long)h,
		        LEIA_MAC_WOVEN_PNG, ok ? "ok" : "FAILED");
		[buf release];
	}];
}


/*
 *
 * Vtable.
 *
 */

static void
leia_dp_mac_process_atlas(struct xrt_display_processor_metal *xdp,
                          void *command_buffer,
                          void *atlas_texture,
                          uint32_t view_width,
                          uint32_t view_height,
                          uint32_t tile_columns,
                          uint32_t tile_rows,
                          uint32_t format,
                          void *target_texture,
                          uint32_t target_width,
                          uint32_t target_height,
                          int32_t canvas_offset_x,
                          int32_t canvas_offset_y,
                          uint32_t canvas_width,
                          uint32_t canvas_height)
{
	struct leia_dp_mac *ldp = leia_dp_mac(xdp);
	id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)command_buffer;
	id<MTLTexture> atlas = (__bridge id<MTLTexture>)atlas_texture;
	id<MTLTexture> target = (__bridge id<MTLTexture>)target_texture;
	if (cmd == nil || atlas == nil || target == nil) {
		return;
	}
	ldp->frames++;

	// Canvas (ADR-010): 0 = the whole target.
	const bool use_canvas = canvas_width > 0 && canvas_height > 0;
	const int32_t cx = use_canvas ? canvas_offset_x : 0;
	const int32_t cy = use_canvas ? canvas_offset_y : 0;
	const uint32_t cw = use_canvas ? canvas_width : target_width;
	const uint32_t ch = use_canvas ? canvas_height : target_height;
	const MTLViewport vp = {(double)cx, (double)cy, (double)cw, (double)ch, 0.0, 1.0};

	update_colorspace_tag(ldp);

	const bool stereo = tile_columns == 2 && tile_rows == 1 && ldp->weaver != NULL;
	if (!stereo) {
		// 2D frame (1x1), an N-view grid, or no weaver: view 0 flat, weaver bypassed.
		if (tile_columns * tile_rows > 1 && !ldp->grid_logged) {
			ldp->grid_logged = true;
			U_LOG_W("leia_mac_dp: %ux%u atlas — the SR Metal weaver takes a 2x1 stereo pair only; "
			        "showing view 0 flat (logged once)",
			        tile_columns, tile_rows);
		}
		if (!ldp->blit_logged) {
			ldp->blit_logged = true;
			U_LOG_W("leia_mac_dp: first passthrough frame: %ux%u grid of %ux%u views -> target %ux%u, "
			        "canvas (%d,%d %ux%u)",
			        tile_columns, tile_rows, view_width, view_height, target_width, target_height, cx, cy, cw,
			        ch);
		}
		encode_clear_and_blit(ldp, cmd, target, atlas, tile_columns, tile_rows, vp);
		return;
	}

	// Defined pixels outside the canvas (SR loads, it does not clear).
	if (use_canvas && (cx != 0 || cy != 0 || cw != target_width || ch != target_height)) {
		encode_clear_and_blit(ldp, cmd, target, nil, 1, 1, vp);
	}

	const uint32_t in_w = view_width * tile_columns;
	const uint32_t in_h = view_height * tile_rows;
	if (!ldp->size_logged && (atlas.width != in_w || atlas.height != in_h)) {
		ldp->size_logged = true;
		U_LOG_W("leia_mac_dp: atlas texture is %lux%lu but the content is %ux%u — the weaver samples the "
		        "whole texture, so this frame is not cropped as ADR-030 promises (logged once)",
		        (unsigned long)atlas.width, (unsigned long)atlas.height, in_w, in_h);
	}

	update_present_origin(ldp);

	SrResult res = srWeaverSetInputTextureMetal(ldp->weaver, (__bridge void *)atlas, in_w, in_h,
	                                            (SrMetalPixelFormat)format);
	if (SR_SUCCEEDED(res)) {
		res = srWeaverSetOutputTextureMetal(ldp->weaver, (__bridge void *)target, target_width,
		                                    target_height, (SrMetalPixelFormat)target.pixelFormat);
	}
	if (SR_SUCCEEDED(res)) {
		res = srWeaverSetCommandBufferMetal(ldp->weaver, (__bridge void *)cmd);
	}
	if (SR_SUCCEEDED(res)) {
		(void)srWeaverSetViewportMetal(ldp->weaver, cx, cy, cx + (int32_t)cw, cy + (int32_t)ch);
		(void)srWeaverSetScissorRectMetal(ldp->weaver, cx, cy, cx + (int32_t)cw, cy + (int32_t)ch);
		res = srWeaverWeave(ldp->weaver);
	}
	if (SR_FAILED(res)) {
		static bool logged;
		if (!logged) {
			logged = true;
			U_LOG_W("leia_mac_dp: weave failed: %s — passthrough this frame (logged once)",
			        leia_mac_sr_result_str(res));
		}
		encode_clear_and_blit(ldp, cmd, target, atlas, tile_columns, tile_rows, vp);
		return;
	}
	maybe_capture_woven(ldp, cmd, target);
	if (!ldp->weave_logged) {
		ldp->weave_logged = true;
		U_LOG_W("leia_mac_dp: first SR weave%s: atlas %ux%u (2x1 of %ux%u, fmt %u) -> target %ux%u (fmt %lu), "
		        "canvas (%d,%d %ux%u)",
		        ldp->screen_bound ? " (SEGMENT DP)" : "", in_w, in_h, view_width, view_height, format, target_width, target_height,
		        (unsigned long)target.pixelFormat, cx, cy, cw, ch);
	}
}

#ifdef XRT_DP_METAL_HAS_PRESENT_ORIGIN
//! Runtime-computed phase origin (multi-screen on macOS): backing px of the
//! content view's top-left relative to THIS display's CGDisplayBounds origin —
//! exactly srWeaverSetPresentOrigin's units. Forwarded as is (only when it
//! changes); from the first call on, update_present_origin stands down.
static void
leia_dp_mac_set_present_origin(struct xrt_display_processor_metal *xdp, int32_t panel_x, int32_t panel_y)
{
	struct leia_dp_mac *ldp = leia_dp_mac(xdp);
	// Called before process_atlas, which increments ldp->frames: tag the
	// frame about to be woven.
	ldp->runtime_origin_frame = ldp->frames + 1;
	if (!ldp->runtime_origin) {
		ldp->runtime_origin = true;
		U_LOG_W("leia_mac_dp: the runtime drives set_present_origin (first: %d, %d) — DP-side origin "
		        "polling yields on every frame it is called for",
		        panel_x, panel_y);
	}
	if (ldp->weaver == NULL ||
	    (ldp->origin_sent && panel_x == ldp->last_origin_x && panel_y == ldp->last_origin_y)) {
		return;
	}
	const SrResult res = srWeaverSetPresentOrigin(ldp->weaver, panel_x, panel_y);
	ldp->origin_sent = true;
	ldp->last_origin_x = panel_x;
	ldp->last_origin_y = panel_y;
	if (first_time_result(ldp, res)) {
		U_LOG_W("leia_mac_dp: srWeaverSetPresentOrigin(%d, %d) panel px (runtime) -> %s", panel_x, panel_y,
		        leia_mac_sr_result_str(res));
	} else {
		U_LOG_I("leia_mac_dp: present origin (%d, %d) panel px (runtime) -> %s", panel_x, panel_y,
		        leia_mac_sr_result_str(res));
	}
}
#endif

#ifdef XRT_DP_METAL_HAS_SNAP_WINDOW_RECT
/*!
 * Window-drag phase lock (ADR-050): a pure query. srWeaverSnapToPhase returns
 * the phase-equivalent position nearest @p target that preserves the phase the
 * window had at @p origin (backing px, top-down; only the displacement
 * matters). SR_SUCCESS -> snapped; SR_DECLINED (no viewing distance yet) or
 * any error -> false, and the runtime keeps the target. Each distinct result
 * code is logged once; results at INFO are per drag step, not per frame.
 */
static bool
leia_dp_mac_snap_window_rect(struct xrt_display_processor_metal *xdp,
                             int32_t origin_x,
                             int32_t origin_y,
                             int32_t target_x,
                             int32_t target_y,
                             int32_t *out_x,
                             int32_t *out_y)
{
	struct leia_dp_mac *ldp = leia_dp_mac(xdp);
	if (ldp->weaver == NULL || out_x == NULL || out_y == NULL) {
		return false;
	}
	int32_t x = target_x, y = target_y;
	const SrResult res = srWeaverSnapToPhase(ldp->weaver, origin_x, origin_y, target_x, target_y, &x, &y);
	if (first_time_result(ldp, res)) {
		U_LOG_W("leia_mac_dp: srWeaverSnapToPhase(origin %d,%d target %d,%d) -> %s (%d,%d)%s", origin_x,
		        origin_y, target_x, target_y, leia_mac_sr_result_str(res), x, y,
		        res == SR_SUCCESS ? "" : " — runtime keeps the target (this code logged once)");
	} else {
		U_LOG_I("leia_mac_dp: snap (%d,%d)->(%d,%d) from (%d,%d): %s", target_x, target_y, x, y, origin_x,
		        origin_y, leia_mac_sr_result_str(res));
	}
	if (res != SR_SUCCESS) {
		return false;
	}
	*out_x = x;
	*out_y = y;
	return true;
}
#endif

static bool
leia_dp_mac_get_predicted_eye_positions(struct xrt_display_processor_metal *xdp, struct xrt_eye_positions *out)
{
	struct leia_dp_mac *ldp = leia_dp_mac(xdp);
	SrPoint3f l = {0}, r = {0};
	const SrResult res =
	    ldp->weaver != NULL ? srWeaverGetPredictedEyePositions(ldp->weaver, &l, &r) : SR_ERROR_HANDLE_INVALID;
	const bool plausible = SR_SUCCEEDED(res) && !(l.x == 0.0f && l.y == 0.0f && l.z == 0.0f && r.x == 0.0f &&
	                                              r.y == 0.0f && r.z == 0.0f);
	if (plausible) {
		ldp->last_good_l = l;
		ldp->last_good_r = r;
		ldp->have_last_good = true;
	} else if (ldp->have_last_good) {
		l = ldp->last_good_l;
		r = ldp->last_good_r;
	} else {
		struct leia_mac_display_info info;
		const float nz = leia_mac_get_display_info(&info) && info.nominal_z_m > 0.0f
		                     ? info.nominal_z_m * 1000.0f
		                     : 600.0f;
		l = (SrPoint3f){-LEIA_MAC_HALF_IPD_MM, 0.0f, nz};
		r = (SrPoint3f){LEIA_MAC_HALF_IPD_MM, 0.0f, nz};
	}

	// SR: millimetres, display-centred, +X right, +Y up, +Z toward the viewer.
	memset(out, 0, sizeof(*out));
	out->eyes[0] = (struct xrt_eye_position){l.x / 1000.0f, l.y / 1000.0f, l.z / 1000.0f};
	out->eyes[1] = (struct xrt_eye_position){r.x / 1000.0f, r.y / 1000.0f, r.z / 1000.0f};
	out->count = 2;
	out->timestamp_ns = (int64_t)os_monotonic_get_ns();
	out->valid = true;
	const int64_t last = atomic_load(&ldp->last_pair_ns);
	out->is_tracking = last != 0 && ((int64_t)os_monotonic_get_ns() - last) < LEIA_MAC_EYE_FRESH_NS;

	// Throttled diagnostic (INFO, every ~5 s): what the weaver itself will weave for.
	const uint64_t now = os_monotonic_get_ns();
	int64_t period_ms = debug_get_num_option_leia_mac_eye_log_ms();
	period_ms = period_ms < 100 ? 100 : period_ms; // floor: never per-frame
	if (now - ldp->last_eye_log_ns > (uint64_t)period_ms * 1000 * 1000) {
		ldp->last_eye_log_ns = now;
		char tag[48] = "";
		if (ldp->screen_bound) {
			snprintf(tag, sizeof(tag), " [segment 0x%016llx]", (unsigned long long)ldp->screen.sr_display_id);
		}
		U_LOG_I("leia_mac_dp:%s weaver eyes L(%.1f %.1f %.1f) R(%.1f %.1f %.1f) mm rc=%s fresh_raw=%d", tag, l.x,
		        l.y, l.z, r.x, r.y, r.z, leia_mac_sr_result_str(res), (int)out->is_tracking);
	}
	return true;
}

static bool
leia_dp_mac_request_display_mode(struct xrt_display_processor_metal *xdp, bool enable_3d)
{
	struct leia_dp_mac *ldp = leia_dp_mac(xdp);
	if (ldp->lens == NULL) {
		return false;
	}
	const enum leia_lens_action action = leia_lens_owner_on_request(&ldp->lens_owner, enable_3d);
	if (action == LEIA_LENS_ACTION_NONE) {
		return true; // 3D before any 2D: the weaver owns the lens and raises it itself.
	}
	const bool enable = action == LEIA_LENS_ACTION_ENABLE;
	const SrResult res = enable ? srLensEnable(ldp->lens) : srLensDisable(ldp->lens);
	if (SR_FAILED(res)) {
		U_LOG_W("leia_mac_dp: srLens%s failed: %s", enable ? "Enable" : "Disable", leia_mac_sr_result_str(res));
		return false;
	}
	leia_lens_owner_commit(&ldp->lens_owner, action);
	U_LOG_W("leia_mac_dp: lens %s (request_display_mode)", enable ? "ON" : "OFF");
	return true;
}

static bool
leia_dp_mac_get_hardware_3d_state(struct xrt_display_processor_metal *xdp, bool *out_is_3d)
{
	struct leia_dp_mac *ldp = leia_dp_mac(xdp);
	if (ldp->lens == NULL || out_is_3d == NULL) {
		return false;
	}
	SrBool32 enabled = SR_FALSE;
	if (SR_FAILED(srLensIsEnabled(ldp->lens, &enabled))) {
		return false;
	}
	*out_is_3d = enabled == SR_TRUE;
	return true;
}

static bool
leia_dp_mac_get_display_dimensions(struct xrt_display_processor_metal *xdp, float *out_w_m, float *out_h_m)
{
	struct leia_dp_mac *ldp = leia_dp_mac(xdp);
	struct leia_mac_display_info info;
	if (ldp->screen_bound) {
		info = ldp->screen;
	} else if (!leia_mac_get_display_info(&info)) {
		return false;
	}
	if (!info.valid || info.width_m <= 0.0f) {
		return false;
	}
	*out_w_m = info.width_m;
	*out_h_m = info.height_m;
	return true;
}

static bool
leia_dp_mac_get_display_pixel_info(struct xrt_display_processor_metal *xdp,
                                   uint32_t *out_px_w,
                                   uint32_t *out_px_h,
                                   int32_t *out_left,
                                   int32_t *out_top)
{
	struct leia_dp_mac *ldp = leia_dp_mac(xdp);
	struct leia_mac_display_info info;
	if (ldp->screen_bound) {
		info = ldp->screen;
	} else if (!leia_mac_get_display_info(&info)) {
		return false;
	}
	if (!info.valid) {
		return false;
	}
	*out_px_w = info.pixel_width;
	*out_px_h = info.pixel_height;
	// Session DP: the Metal compositor's window metrics are screen-relative
	// (its display_screen_left/top are 0), so 0,0. Segment DP: the binding's
	// desktop origin (top-down points), per the for_screen contract.
	*out_left = ldp->screen_bound ? info.screen_left_pt : 0;
	*out_top = ldp->screen_bound ? info.screen_top_pt : 0;
	return true;
}

static void
leia_dp_mac_destroy(struct xrt_display_processor_metal *xdp)
{
	struct leia_dp_mac *ldp = leia_dp_mac(xdp);
	if (ldp->external_routing && ldp->lens != NULL && ldp->lens_owner.last_sent == LEIA_LENS_REQ_3D) {
		// No weaver will ever release a lens an EXTERNAL weaver's owner raised.
		(void)srLensDisable(ldp->lens);
	}
	if (ldp->weaver != NULL) {
		srDestroyWeaver(ldp->weaver);
	}
	if (ldp->tracker != NULL) {
		srDestroyEyeTracker(ldp->tracker);
	}
	if (ldp->lens != NULL) {
		srDestroyLens(ldp->lens);
	}
	if (ldp->inst != NULL) {
		srDestroyInstance(ldp->inst);
	}
	if (ldp->panel_cs != NULL) {
		CGColorSpaceRelease(ldp->panel_cs);
	}
	if (ldp->retags > 0) {
		U_LOG_W("leia_mac_dp: colour-space tag was reset and re-applied %llu time(s) this session",
		        (unsigned long long)ldp->retags);
	}
	[ldp->blit_pipeline release];
	[ldp->sampler release];
	[ldp->window release];
	[ldp->view release];
	[ldp->queue release];
	[ldp->device release];
	U_LOG_W("leia_mac_dp: destroyed after %llu frames", (unsigned long long)ldp->frames);
	free(ldp);
}


/*
 *
 * Factory.
 *
 */

static xrt_result_t
leia_dp_mac_create(void *metal_device,
                   void *command_queue,
                   void *window_handle,
                   const struct leia_mac_display_info *screen,
                   struct xrt_display_processor_metal **out_xdp)
{
	const bool screen_bound = screen != NULL;
	if (out_xdp == NULL || metal_device == NULL || command_queue == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	(void)leia_mac_sr_probe(); // panel geometry + CGDirectDisplayID (cached)

	struct leia_dp_mac *ldp = calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	ldp->screen_bound = screen_bound;
	if (screen_bound) {
		ldp->screen = *screen;
	}
	struct leia_mac_sr_caps sr_caps = {0};
	(void)leia_mac_get_sr_caps(&sr_caps);
	ldp->device = [(__bridge id<MTLDevice>)metal_device retain];
	ldp->queue = [(__bridge id<MTLCommandQueue>)command_queue retain];

	// window_handle is the app's NSView for handle/texture apps (NULL for
	// hosted). SR wants the NSWindow (geometry + window tracking).
	id handle = (__bridge id)window_handle;
	if (handle != nil && [handle isKindOfClass:[NSView class]]) {
		ldp->view = [(NSView *)handle retain];
		ldp->window = [((NSView *)handle).window retain];
	} else if (handle != nil && [handle isKindOfClass:[NSWindow class]]) {
		ldp->window = [(NSWindow *)handle retain];
		ldp->view = [((NSWindow *)handle).contentView retain];
	}

	// Own SR instance: on macOS the weaver + eye callback must exist BEFORE
	// srInitialize (leia_sr_macos.h explains why).
	SrInstanceCreateInfo ci = SrInstanceCreateInfo(.applicationName = "DisplayXR-LeiaSR",
	                                               .networkMode = SR_NETWORK_MODE_CLIENT);
	SrResult res = srCreateInstance(&ci, &ldp->inst);
	if (SR_FAILED(res)) {
		U_LOG_E("leia_mac_dp: srCreateInstance failed: %s (loader: %s)", leia_mac_sr_result_str(res),
		        srGetLastLoaderError() ? srGetLastLoaderError() : "-");
		ldp->inst = NULL;
		goto fail;
	}
	srSetLogCallback(ldp->inst, leia_mac_sr_log_cb, NULL);

	SrRuntimeCapabilities caps = SrRuntimeCapabilities();
	if (SR_SUCCEEDED(srGetRuntimeCapabilities(ldp->inst, &caps)) &&
	    !(caps.weaverBackends & SR_WEAVER_BACKEND_METAL_BIT)) {
		U_LOG_E("leia_mac_dp: the SR runtime has no Metal weaver backend");
		goto fail;
	}

	// Per-screen DP: bind to that SR display. The weaver only when the SR
	// runtime honours binding and the display is FPC verified (an EDID-only
	// one has no calibration and fails with DEVICE_NOT_AVAILABLE); lens and
	// eye tracker only when their per-device capability says a non-active
	// display can be named (macOS today: no — they stay on the active one).
	SrDisplayBindingInfo bind = SrDisplayBindingInfo(.displayId = screen_bound ? ldp->screen.sr_display_id : 0);
	const bool bind_weaver = screen_bound && sr_caps.display_binding && ldp->screen.fpc_verified &&
	                         ldp->screen.sr_display_id != 0;
	const bool bind_tracker = bind_weaver && sr_caps.eye_tracker_per_device;
	const bool bind_lens = bind_weaver && sr_caps.lens_per_device;
	SrWeaverRoutingInfo routing = SrWeaverRoutingInfo(.mode = SR_WEAVER_ROUTING_EXTERNAL,
	                                                  .pNext = bind_weaver ? &bind : NULL);
	ldp->external_routing = screen_bound && sr_caps.external_routing;
	if (screen_bound && !sr_caps.external_routing) {
		U_LOG_W("leia_mac_dp: SR runtime lacks external weaver routing — the windowless segment weaver is "
		        "SDK-routed (phase from set_present_origin only if the runtime honours it)");
	}

	SrEyeTrackerCreateInfo eci = SrEyeTrackerCreateInfo(.enablePrediction = SR_FALSE,
	                                                    .pNext = bind_tracker ? &bind : NULL);
	res = srCreateEyeTracker(ldp->inst, &eci, &ldp->tracker);
	if (SR_SUCCEEDED(res)) {
		srEyeTrackerAddCallback(ldp->tracker, leia_dp_mac_on_eye_pair, ldp);
	} else {
		U_LOG_W("leia_mac_dp: srCreateEyeTracker failed (%s) — is_tracking stays false",
		        leia_mac_sr_result_str(res));
		ldp->tracker = NULL;
	}

	SrWeaverCreateInfoMetal wci = SrWeaverCreateInfoMetal(.device = metal_device, .commandQueue = command_queue,
	                                                      .window = (__bridge void *)ldp->window);
	if (ldp->external_routing) {
		wci.pNext = &routing; // routing -> (binding)
	} else if (bind_weaver) {
		wci.pNext = &bind;
	}
	res = srCreateWeaverMetal(ldp->inst, &wci, &ldp->weaver);
	if (SR_FAILED(res)) {
		U_LOG_E("leia_mac_dp: srCreateWeaverMetal failed: %s", leia_mac_sr_result_str(res));
		ldp->weaver = NULL;
		goto fail;
	}

	res = srInitialize(ldp->inst);
	if (SR_FAILED(res)) {
		U_LOG_E("leia_mac_dp: srInitialize failed: %s", leia_mac_sr_result_str(res));
		goto fail;
	}

	SrLensCreateInfo lci = SrLensCreateInfo(.pNext = bind_lens ? &bind : NULL);
	res = srCreateLens(ldp->inst, &lci, &ldp->lens);
	if (SR_FAILED(res)) {
		U_LOG_W("leia_mac_dp: srCreateLens failed (%s) — no 2D/3D switching", leia_mac_sr_result_str(res));
		ldp->lens = NULL;
	}
	if (ldp->external_routing) {
		// An EXTERNAL weaver never votes the lens: a 3D request must be sent.
		ldp->lens_owner.external_weavers = 1;
	}
	if (screen_bound) {
		U_LOG_W("leia_mac_dp: segment weaver for SR display 0x%016llx (%s): routing %s, weaver %s, eye tracker "
		        "%s, lens %s",
		        (unsigned long long)ldp->screen.sr_display_id, ldp->screen.fpc_verified ? "FPC verified" : "EDID only",
		        ldp->external_routing ? "EXTERNAL" : "SDK", bind_weaver ? "BOUND" : "active display",
		        bind_tracker ? "BOUND" : "active display", bind_lens ? "BOUND" : "active display");
	}

	ldp->base.struct_size = (uint32_t)sizeof(struct xrt_display_processor_metal);
	ldp->base.process_atlas = leia_dp_mac_process_atlas;
	ldp->base.get_predicted_eye_positions = leia_dp_mac_get_predicted_eye_positions;
	ldp->base.request_display_mode = leia_dp_mac_request_display_mode;
	ldp->base.get_hardware_3d_state = leia_dp_mac_get_hardware_3d_state;
	ldp->base.get_display_dimensions = leia_dp_mac_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_mac_get_display_pixel_info;
	ldp->base.destroy = leia_dp_mac_destroy;
#ifdef XRT_DP_METAL_HAS_PRESENT_ORIGIN
	ldp->base.set_present_origin = leia_dp_mac_set_present_origin;
#endif
#ifdef XRT_DP_METAL_HAS_SNAP_WINDOW_RECT
	ldp->base.snap_window_rect = leia_dp_mac_snap_window_rect;
#endif
	// Left NULL, as on the Linux arm's first cut: get_window_metrics (the
	// Metal compositor computes it from the view), is_alpha_native (a weave is
	// opaque), colour capability / encoding (ENCODED default), background,
	// zones, scanout caps, background preview.

	struct leia_mac_display_info info = {0};
	(void)leia_mac_get_display_info(&info);
	U_LOG_W("leia_mac_dp: created SR Metal weaver (%s; window %p, view %p, panel display %u, %ux%u px)",
	        screen_bound ? "SCREEN-BOUND segment DP, windowless" : "session DP", (void *)ldp->window,
	        (void *)ldp->view, info.cg_display_id, info.pixel_width, info.pixel_height);
	if (ldp->window == nil && !screen_bound) {
		U_LOG_W("leia_mac_dp: no window handed to the DP (hosted app) — SR weaves windowless: the "
		        "phase assumes the target sits at the panel origin");
	}

	*out_xdp = &ldp->base;
	return XRT_SUCCESS;

fail:
	ldp->base.destroy = leia_dp_mac_destroy;
	leia_dp_mac_destroy(&ldp->base);
	return XRT_ERROR_DEVICE_CREATION_FAILED;
}

xrt_result_t
leia_mac_dp_factory_metal(void *metal_device,
                          void *command_queue,
                          void *window_handle,
                          struct xrt_display_processor_metal **out_xdp)
{
	return leia_dp_mac_create(metal_device, command_queue, window_handle, NULL, out_xdp);
}

xrt_result_t
leia_mac_dp_factory_metal_for_screen(void *metal_device,
                                     void *command_queue,
                                     const struct leia_mac_display_info *display,
                                     struct xrt_display_processor_metal **out_xdp)
{
	if (display == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	// Windowless (srCreateWeaverMetal window = NULL); the phase arrives
	// through set_present_origin.
	return leia_dp_mac_create(metal_device, command_queue, NULL, display, out_xdp);
}
