// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Leia GL display processor: wraps SR SDK GL weaver
 *         as an @ref xrt_display_processor_gl.
 *
 * The display processor owns the leiasr_gl handle — it creates it
 * via the factory function and destroys it on cleanup.
 *
 * The SR SDK weaver expects side-by-side (SBS) stereo input. The Leia
 * device defines its 3D mode as tile_columns=2, tile_rows=1, so the
 * compositor always delivers SBS. The compositor crop-blit guarantees
 * the atlas texture dimensions match exactly 2*view_width x view_height.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_gl.h"
#include "leia_sr_gl.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"

// GL types and functions — use glad via ogl_api.h (provides all GL symbols on Windows)
#include "xrt/xrt_windows.h"
#include "ogl/ogl_api.h"

// Compose-under-bg transparency: WGC desktop capture (shared with the
// D3D11/D3D12/VK DPs) imported into GL via WGL_NV_DX_interop2.
#include "leia_bg_capture_win.h"
#include <d3d11_4.h>

#include <cstdlib>
#include <cstring>

// WGL_NV_DX_interop2 entry points — not declared by ogl_api.h. Mirrors the
// runtime's comp_gl_compositor.cpp typedef block. This is the GL analogue of
// the D3D11/D3D12/VK native shared-texture open: it brings the WGC desktop
// (a D3D11 BGRA8 SHARED_NTHANDLE texture) into GL so the compose pass can
// sample it. See project_leia_transparency_model.
typedef HANDLE(WINAPI *PFN_wglDXOpenDeviceNV)(void *dxDevice);
typedef BOOL(WINAPI *PFN_wglDXCloseDeviceNV)(HANDLE hDevice);
typedef HANDLE(WINAPI *PFN_wglDXRegisterObjectNV)(
    HANDLE hDevice, void *dxObject, GLuint name, GLenum type, GLenum access);
typedef BOOL(WINAPI *PFN_wglDXUnregisterObjectNV)(HANDLE hDevice, HANDLE hObject);
typedef BOOL(WINAPI *PFN_wglDXLockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL(WINAPI *PFN_wglDXUnlockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);
#ifndef WGL_ACCESS_READ_ONLY_NV
#define WGL_ACCESS_READ_ONLY_NV 0x0000
#endif


// Default chroma key when the app didn't supply one (set_chroma_key key=0).
// Magenta — matches the D3D11/D3D12/VK DPs' kDefaultChromaKey for cross-API parity.
// Layout 0x00BBGGRR.
static constexpr uint32_t kDefaultChromaKey = 0x00FF00FF;

// Fullscreen-triangle vertex shader (3 vertices, no VBO).
static const char *kCkVertSrc = R"(#version 330 core
out vec2 v_uv;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Pre-weave fill shader: alpha=0 -> chroma key, alpha=1 -> unchanged.
// Output alpha forced to 1 so the SR weaver receives opaque RGB.
static const char *kCkFillFragSrc = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D src;
uniform vec3 chroma_rgb;
void main() {
    vec4 c = texture(src, v_uv);
    frag = vec4(mix(chroma_rgb, c.rgb, c.a), 1.0);
}
)";

// Post-weave strip shader: chroma-match -> alpha=0, else alpha=1, RGB
// premultiplied for DWM/DComp's premultiplied-alpha blend mode.
static const char *kCkStripFragSrc = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D src;
uniform vec3 chroma_rgb;
void main() {
    vec3 c = texture(src, v_uv).rgb;
    vec3 d = abs(c - chroma_rgb);
    bool match = max(max(d.r, d.g), d.b) < (1.0 / 512.0);
    float a = match ? 0.0 : 1.0;
    frag = vec4(c * a, a);
}
)";

// Compose-under-bg pre-weave shader (GLSL port of compose_under_bg_ps_source
// in the D3D11 DP). Composites the WGC-captured desktop UNDER the RGBA atlas
// tiles, outputting opaque RGB the SR weaver can consume — so transparency is
// resolved pre-weave in per-view RGB (the sound path; the post-weave alpha-gate
// then punches flat-transparent regions back to the live desktop). No chroma
// sentinel is ever emitted.
//
//   out = lerp(backdrop-over-desktop, atlas.rgb, atlas.a),  out.a = 1
static const char *kComposeFragSrc = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D atlas;
uniform sampler2D bg;        // WGC desktop (D3D11 BGRA8 via WGL interop)
uniform sampler2D backdrop;  // #491 part 3 — runtime's flattened 2D-under layer
uniform vec2 bg_uv_origin;   // window TL on monitor, normalized
uniform vec2 bg_uv_extent;   // window size on monitor, normalized
uniform ivec2 tile_count;    // (tile_columns, tile_rows)
uniform int has_backdrop;    // 1 ⟹ composite backdrop over desktop
void main() {
    vec4 a = texture(atlas, v_uv);
    vec2 tile_local = fract(v_uv * vec2(tile_count));
    // The WGC desktop is a top-left-origin D3D11 BGRA8 texture imported via
    // WGL_NV_DX_interop2: its interop texcoord t.y=0 is the MONITOR TOP, while
    // the GL render target is bottom-up (v_uv.y=0 = screen bottom). So the
    // window-local vertical fraction must be flipped (1.0 - tile_local.y) BEFORE
    // mapping through origin/extent — NOT after — otherwise the sample lands on
    // the wrong monitor band (off by the window's offset), not behind the window.
    // NO channel swizzle. WGL_NV_DX_interop2 presents the BGRA8 D3D11 texture
    // to GL with the channel order already resolved, so texture() returns RGBA
    // directly — the .bgr swizzle this line used to carry was a second swap on
    // top of that, and it inverted red/blue in the composited desktop. It only
    // showed on a SATURATED background (cyan came out yellow); greys and
    // near-neutral desktops are unchanged by an R/B swap, which is why it
    // survived. The D3D11 DP's equivalent shader samples straight, and D3D11
    // output was verified correct on the same desktop.
    vec2 bg_uv = vec2(bg_uv_origin.x + tile_local.x * bg_uv_extent.x,
                      bg_uv_origin.y + (1.0 - tile_local.y) * bg_uv_extent.y);
    vec3 b = texture(bg, bg_uv).rgb;
    if (has_backdrop != 0) {
        // Backdrop is a GL-pipeline texture (bottom-up, like the atlas) → sample
        // at the unflipped tile_local.
        vec4 bd = texture(backdrop, tile_local);
        b = bd.rgb + (1.0 - bd.a) * b;
    }
    frag = vec4(mix(b, a.rgb, a.a), 1.0);
}
)";

// Post-weave alpha-gate shader (GLSL port of alpha_gate_ps_source in the D3D11
// DP). Replaces the chroma-key strip when compose-under-bg is active. For each
// screen pixel, tests "fully transparent in ALL views" by sampling the original
// atlas at the same tile-local UV across every tile: all-transparent → α=0 (DWM
// blends the LIVE desktop, no captured-bg lag); otherwise → woven RGB at α=1.
// With a 2D-under backdrop present, transparent pixels emit the backdrop
// premultiplied instead of punching fully through.
static const char *kAlphaGateFragSrc = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D backbuffer; // woven output copy (ck_strip_tex)
uniform sampler2D atlas;
uniform sampler2D backdrop;   // #491 part 3
uniform ivec2 tile_count;
uniform int has_backdrop;
uniform vec2 canvas_uv_origin; // canvas sub-rect origin on window, normalized
uniform vec2 canvas_uv_extent; // canvas sub-rect size on window, normalized
void main() {
    bool all_transparent = true;
    for (int ty = 0; ty < tile_count.y; ty++) {
        for (int tx = 0; tx < tile_count.x; tx++) {
            vec2 uv_at_tile = (vec2(float(tx), float(ty)) + v_uv) / vec2(tile_count);
            if (texture(atlas, uv_at_tile).a > 0.0) {
                all_transparent = false;
            }
        }
    }
    vec2 bb_uv = canvas_uv_origin + v_uv * canvas_uv_extent;
    if (!all_transparent) {
        frag = vec4(texture(backbuffer, bb_uv).rgb, 1.0); // woven 3D: opaque
        return;
    }
    if (has_backdrop != 0) {
        vec4 bd = texture(backdrop, bb_uv);
        frag = vec4(bd.rgb, bd.a); // premultiplied over live desktop
        return;
    }
    frag = vec4(0.0, 0.0, 0.0, 0.0); // live desktop
}
)";

/*!
 * Implementation struct wrapping leiasr_gl as xrt_display_processor_gl.
 */
struct leia_display_processor_gl_impl
{
	struct xrt_display_processor_gl base;
	struct leiasr_gl *leiasr; //!< Owned — destroyed in leia_dp_gl_destroy.

	GLuint read_fbo;     //!< Cached read FBO for 2D blit path.
	uint32_t view_count; //!< Active mode view count (1=2D, 2=stereo).

	//
	// Chroma-key transparency support (lazy-initialized on first frame).
	//
	bool ck_enabled;
	uint32_t ck_color;            //!< Effective key (kDefaultChromaKey if app passed 0).
	bool ck_inited;
	GLuint ck_program_fill;       //!< Pre-weave fill program.
	GLuint ck_program_strip;      //!< Post-weave strip program.
	GLint ck_fill_chroma_loc;
	GLint ck_strip_chroma_loc;
	GLint ck_fill_src_loc;
	GLint ck_strip_src_loc;

	GLuint ck_fill_fbo;           //!< FBO with ck_fill_tex attached.
	GLuint ck_fill_tex;           //!< RGBA8 fill target.
	uint32_t ck_fill_w, ck_fill_h;

	GLuint ck_strip_tex;          //!< RGBA8 sampling target for strip pass.
	GLuint ck_strip_blit_fbo;     //!< Read FBO with ck_strip_tex attached (target of glBlitFramebuffer).
	uint32_t ck_strip_w, ck_strip_h;

	GLuint ck_vao;                //!< Empty VAO required by core profile draws.

	//! HWND retained from the factory — needed for leia_bg_capture_create().
	HWND hwnd;

	//
	// Compose-under-bg transparency (preferred path; chroma-key above is the
	// fallback when WGC / WGL interop is unavailable). Lazy-initialized on the
	// first process_atlas (GL context guaranteed current there). Mirrors the
	// D3D11 DP, but the WGC desktop is a D3D11 texture so it is imported into GL
	// via WGL_NV_DX_interop2 (the GL analogue of the D3D11 OpenSharedResource).
	//
	bool compose_requested;             //!< set by set_chroma_key (defers interop).
	bool compose_init_tried;            //!< one-shot guard for compose_try_init_gl.
	bool bg_compose_enabled;            //!< true once interop is live.
	struct leia_bg_capture *bg_capture; //!< Owned; NULL ⟹ fell back to chroma-key.

	//! DP-owned D3D11 device for the WGL interop + cross-device fence Wait
	//! (ADR-019: the plug-in cannot reuse the runtime's D3D11 device).
	ID3D11Device *dx_device;
	ID3D11DeviceContext *dx_context;
	HANDLE dx_interop_device;           //!< wglDXOpenDeviceNV(dx_device).

	ID3D11Texture2D *bg_shared_tex;     //!< Opened from bg_capture on dx_device (BGRA8).
	//! Private device-local BGRA8 texture, CopyResource'd from bg_shared_tex
	//! each frame. WGL_NV_DX_interop2 cannot register a resource opened from a
	//! cross-process shared handle, so we register THIS (a plain device-local
	//! texture) and copy the shared desktop into it on dx_device.
	ID3D11Texture2D *bg_local_tex;
	uint32_t bg_w, bg_h;                //!< Captured desktop (monitor) dimensions.
	GLuint bg_gl_tex;                   //!< GL name registered to bg_local_tex.
	HANDLE bg_interop_obj;              //!< wglDXRegisterObjectNV handle.
	ID3D11Fence *bg_fence;              //!< Opened from bg_capture on dx_device.

	PFN_wglDXOpenDeviceNV       pfn_wglDXOpenDeviceNV;
	PFN_wglDXCloseDeviceNV      pfn_wglDXCloseDeviceNV;
	PFN_wglDXRegisterObjectNV   pfn_wglDXRegisterObjectNV;
	PFN_wglDXUnregisterObjectNV pfn_wglDXUnregisterObjectNV;
	PFN_wglDXLockObjectsNV      pfn_wglDXLockObjectsNV;
	PFN_wglDXUnlockObjectsNV    pfn_wglDXUnlockObjectsNV;

	//! Compose + alpha-gate programs (reuse ck_fill_tex/fbo + ck_strip_tex as
	//! intermediates, exactly as the D3D11 DP reuses ck_fill/ck_strip).
	GLuint compose_program;
	GLuint alpha_gate_program;
	// compose uniform locations
	GLint co_atlas_loc, co_bg_loc, co_backdrop_loc;
	GLint co_bg_origin_loc, co_bg_extent_loc, co_tile_loc, co_has_backdrop_loc;
	// alpha-gate uniform locations
	GLint ag_backbuffer_loc, ag_atlas_loc, ag_backdrop_loc;
	GLint ag_tile_loc, ag_has_backdrop_loc, ag_canvas_origin_loc, ag_canvas_extent_loc;

	//! #491 part 3 — the runtime's flattened 2D-under backdrop (set via
	//! set_background_2d). STORED ONLY on GL: the compose pass + alpha-gate carry
	//! the backdrop shader support, but actually SAMPLING the runtime's backdrop
	//! GL texture from this plug-in mid-frame faults / hangs the NV driver in the
	//! Local2D+transparent path (the runtime writes it the same frame; sample →
	//! crash, blit → GPU hang). The correct fix is runtime-side compositing (like
	//! gl_composite_local_2d does for the OVER layers) — a separate follow-up. So
	//! the GL 2D-under backdrop is a no-op store for now. 0 ⟹ no backdrop.
	GLuint backdrop_tex; //!< NOT owned (compositor-owned). 0 ⟹ no backdrop.
	uint32_t backdrop_w, backdrop_h;

	//! #68 — set by set_shared_texture_present (compositor's has_shared_texture).
	bool shared_texture_present;

	//
	// Display-zones (#613 / ADR-027) — 1×1 single-zone panel, GL port of the
	// D3D11 DP zone vtable. The published R8 wish mask is reduced to one bit
	// (any non-zero ⟹ 3D) and edge-triggers the SR lens hint. The mask is a GL
	// texture name (R8), so the readback is an FBO-attach + glReadPixels (the GL
	// analogue of D3D11's CopyResource-to-staging + Map).
	//
	// #215: that readback is NON-BLOCKING. glReadPixels into CLIENT memory is
	// an implicit full-pipeline sync on the runtime's context, and the runtime
	// calls publish from the compositor frame path — which cost ~180 ms per
	// real mask change on the D3D11 arm. So the read now targets a
	// GL_PIXEL_PACK_BUFFER (@ref zone_pbo), is fenced (@ref zone_fence), and is
	// polled with a zero-timeout glClientWaitSync on the runtime's per-frame
	// republish of the same generation (the runtime republishes every frame
	// with the same seq — only the screen anchor differs — so no timer is
	// needed); the previous generation's verdict is carried until the new one
	// lands.
	//
	// "A read is in flight" is derived from the FENCE (@ref zone_fence != NULL),
	// never from a bool: the PBO is that read's DESTINATION, so issuing a
	// second read under a running one would stomp the buffer the driver is
	// still filling — and a fence-derived rule is the one thing
	// @ref leia_dp_gl_clear_local_zone_mask cannot forget.
	//
	bool zone_active;        //!< A mask is currently published (drives lens authority).
	bool zone_want_3d;       //!< Latest evaluated verdict (any non-zero mask pixel).
	bool zone_hint_3d;       //!< Last lens hint we issued (edge-trigger memo).
	bool zone_eval_valid;    //!< zone_eval_seq holds a real evaluation.
	uint64_t zone_eval_seq;  //!< Generation of the last content evaluation.

	GLuint zone_read_fbo;    //!< Dedicated FBO for attaching the mask tex to read it.
	//! Reused CPU readback buffer (R8, zone_buf_w*zone_buf_h). #215: still in
	//! use — it is the BLOCKING fallback's destination (kill-switch, or a
	//! driver that never exposed the PBO/sync entry points).
	uint8_t *zone_buf;
	uint32_t zone_buf_w, zone_buf_h;

	GLuint zone_pbo;   //!< #215 GL_PIXEL_PACK_BUFFER the async read targets. 0 ⟹ not created yet.
	GLsync zone_fence; //!< #215 gates the async read. Non-NULL ⟹ a read is IN FLIGHT.
	uint32_t zone_pbo_w, zone_pbo_h; //!< Dims @ref zone_pbo was sized (glBufferData) for.
	uint64_t zone_readback_seq;      //!< Generation the issued read belongs to.
	//! Dims the in-flight read was ISSUED with. The scan uses THESE, not the
	//! publish call's — the mask can be resized while a read is in flight, and
	//! the PBO's contents belong to the issued one.
	uint32_t zone_readback_w, zone_readback_h;
	LONGLONG zone_readback_t0_qpc; //!< QPC at issue — reported once when it lands.
	uint32_t zone_readback_polls;  //!< Poll attempts for this generation.
	//! #215: an issue happened whose verdict has not been consumed yet. Set at
	//! issue, cleared when LANDED is consumed or on FAILED. NOT the in-flight
	//! flag — that one is fence-derived (see above).
	bool zone_readback_armed;
	//! #215 kill-switch: DXR_LEIA_ASYNC_ZONE_PUBLISH=0 reverts to the blocking
	//! glReadPixels (and, via leiasr_gl_set_async_lens, the synchronous lens
	//! hint). Default true — set explicitly at create, since the impl is
	//! calloc'd and zero would mean "blocking".
	bool async_zone_publish;
	bool zone_first_logged;   //!< one-shot: first publish reached the DP
	bool zone_refused_logged; //!< one-shot: a publish was refused at the guard
};

static inline struct leia_display_processor_gl_impl *
leia_dp_gl(struct xrt_display_processor_gl *xdp)
{
	return (struct leia_display_processor_gl_impl *)xdp;
}


/*
 *
 * Chroma-key fill/strip helpers (transparency support).
 *
 * Lazy-allocated on first frame the pass runs. ck_should_run() gates the
 * whole flow — when false (the common case) none of these execute and
 * process_atlas behaves identically to the pre-transparency path.
 *
 */

static bool
ck_should_run(struct leia_display_processor_gl_impl *ldp)
{
	return ldp->ck_enabled && ldp->ck_color != 0;
}

static void
ck_unpack_chroma_rgb(uint32_t color, float out_rgb[3])
{
	// 0x00BBGGRR layout matches D3D11/D3D12/VK DPs.
	uint8_t r = (uint8_t)((color >> 0) & 0xff);
	uint8_t g = (uint8_t)((color >> 8) & 0xff);
	uint8_t b = (uint8_t)((color >> 16) & 0xff);
	out_rgb[0] = (float)r / 255.0f;
	out_rgb[1] = (float)g / 255.0f;
	out_rgb[2] = (float)b / 255.0f;
}

static GLuint
ck_compile_shader(GLenum stage, const char *src)
{
	GLuint sh = glCreateShader(stage);
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	GLint status = GL_FALSE;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
	if (status != GL_TRUE) {
		char log[1024] = {0};
		GLsizei n = 0;
		glGetShaderInfoLog(sh, sizeof(log) - 1, &n, log);
		U_LOG_E("Leia GL DP: ck shader compile failed: %s", log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static GLuint
ck_link_program(GLuint vs, GLuint fs)
{
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	GLint status = GL_FALSE;
	glGetProgramiv(prog, GL_LINK_STATUS, &status);
	if (status != GL_TRUE) {
		char log[1024] = {0};
		GLsizei n = 0;
		glGetProgramInfoLog(prog, sizeof(log) - 1, &n, log);
		U_LOG_E("Leia GL DP: ck program link failed: %s", log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static bool
ck_init_pipeline(struct leia_display_processor_gl_impl *ldp)
{
	if (ldp->ck_inited) return true;

	GLuint vs = ck_compile_shader(GL_VERTEX_SHADER, kCkVertSrc);
	if (vs == 0) return false;
	GLuint fs_fill = ck_compile_shader(GL_FRAGMENT_SHADER, kCkFillFragSrc);
	if (fs_fill == 0) { glDeleteShader(vs); return false; }
	GLuint fs_strip = ck_compile_shader(GL_FRAGMENT_SHADER, kCkStripFragSrc);
	if (fs_strip == 0) { glDeleteShader(fs_fill); glDeleteShader(vs); return false; }

	ldp->ck_program_fill = ck_link_program(vs, fs_fill);
	ldp->ck_program_strip = ck_link_program(vs, fs_strip);

	glDeleteShader(fs_strip);
	glDeleteShader(fs_fill);
	glDeleteShader(vs);

	if (ldp->ck_program_fill == 0 || ldp->ck_program_strip == 0) {
		if (ldp->ck_program_fill) glDeleteProgram(ldp->ck_program_fill);
		if (ldp->ck_program_strip) glDeleteProgram(ldp->ck_program_strip);
		ldp->ck_program_fill = 0;
		ldp->ck_program_strip = 0;
		return false;
	}

	ldp->ck_fill_chroma_loc  = glGetUniformLocation(ldp->ck_program_fill,  "chroma_rgb");
	ldp->ck_strip_chroma_loc = glGetUniformLocation(ldp->ck_program_strip, "chroma_rgb");
	ldp->ck_fill_src_loc     = glGetUniformLocation(ldp->ck_program_fill,  "src");
	ldp->ck_strip_src_loc    = glGetUniformLocation(ldp->ck_program_strip, "src");

	// Empty VAO required by 3.3+ core profile.
	glGenVertexArrays(1, &ldp->ck_vao);

	ldp->ck_inited = true;
	U_LOG_W("Leia GL DP: chroma-key programs initialized (key=0x%06x)",
	        ldp->ck_color & 0x00FFFFFFu);
	return true;
}

// Recreate ck_fill_tex + ck_fill_fbo at (w, h). Returns false on failure.
static bool
ck_ensure_fill_target(struct leia_display_processor_gl_impl *ldp, uint32_t w, uint32_t h)
{
	if (ldp->ck_fill_tex != 0 && ldp->ck_fill_w == w && ldp->ck_fill_h == h) {
		return true;
	}

	if (ldp->ck_fill_fbo != 0) glDeleteFramebuffers(1, &ldp->ck_fill_fbo);
	if (ldp->ck_fill_tex != 0) glDeleteTextures(1, &ldp->ck_fill_tex);
	ldp->ck_fill_fbo = 0;
	ldp->ck_fill_tex = 0;

	glGenTextures(1, &ldp->ck_fill_tex);
	glBindTexture(GL_TEXTURE_2D, ldp->ck_fill_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	// Strip pass uses GL_NEAREST too, but we sample the fill texture in the
	// weaver's input path — linear is fine there since fill produces opaque RGB.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &ldp->ck_fill_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, ldp->ck_fill_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                        GL_TEXTURE_2D, ldp->ck_fill_tex, 0);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		U_LOG_E("Leia GL DP: ck fill FBO incomplete: 0x%x", (unsigned)st);
		return false;
	}

	ldp->ck_fill_w = w;
	ldp->ck_fill_h = h;
	return true;
}

// Recreate ck_strip_tex + ck_strip_blit_fbo at (w, h). Returns false on failure.
static bool
ck_ensure_strip_source(struct leia_display_processor_gl_impl *ldp, uint32_t w, uint32_t h)
{
	if (ldp->ck_strip_tex != 0 && ldp->ck_strip_w == w && ldp->ck_strip_h == h) {
		return true;
	}

	if (ldp->ck_strip_blit_fbo != 0) glDeleteFramebuffers(1, &ldp->ck_strip_blit_fbo);
	if (ldp->ck_strip_tex != 0) glDeleteTextures(1, &ldp->ck_strip_tex);
	ldp->ck_strip_blit_fbo = 0;
	ldp->ck_strip_tex = 0;

	glGenTextures(1, &ldp->ck_strip_tex);
	glBindTexture(GL_TEXTURE_2D, ldp->ck_strip_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &ldp->ck_strip_blit_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, ldp->ck_strip_blit_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                        GL_TEXTURE_2D, ldp->ck_strip_tex, 0);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		U_LOG_E("Leia GL DP: ck strip blit FBO incomplete: 0x%x", (unsigned)st);
		return false;
	}

	ldp->ck_strip_w = w;
	ldp->ck_strip_h = h;
	return true;
}

// Pre-weave fill: render atlas RGBA -> ck_fill_tex with alpha=0 pixels filled
// by the chroma key. Returns the GL texture id to feed the weaver as input,
// or 0 on failure.
static GLuint
ck_run_pre_weave_fill(struct leia_display_processor_gl_impl *ldp,
                      GLuint atlas_texture,
                      uint32_t atlas_w, uint32_t atlas_h)
{
	if (!ck_ensure_fill_target(ldp, atlas_w, atlas_h)) return 0;

	// Save state we'll trample.
	GLint prev_fbo = 0, prev_program = 0, prev_vao = 0;
	GLint prev_active_tex = 0, prev_tex2d = 0;
	GLint prev_viewport[4] = {0, 0, 0, 0};
	GLboolean prev_blend = GL_FALSE, prev_depth = GL_FALSE,
	          prev_cull = GL_FALSE, prev_scissor = GL_FALSE;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex2d);
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	prev_blend = glIsEnabled(GL_BLEND);
	prev_depth = glIsEnabled(GL_DEPTH_TEST);
	prev_cull  = glIsEnabled(GL_CULL_FACE);
	prev_scissor = glIsEnabled(GL_SCISSOR_TEST);

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ldp->ck_fill_fbo);
	glViewport(0, 0, (GLsizei)atlas_w, (GLsizei)atlas_h);

	glBindVertexArray(ldp->ck_vao);
	glUseProgram(ldp->ck_program_fill);

	float chroma[3] = {0.0f, 0.0f, 0.0f};
	ck_unpack_chroma_rgb(ldp->ck_color, chroma);
	if (ldp->ck_fill_chroma_loc >= 0) {
		glUniform3fv(ldp->ck_fill_chroma_loc, 1, chroma);
	}
	if (ldp->ck_fill_src_loc >= 0) {
		glUniform1i(ldp->ck_fill_src_loc, 0);
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, atlas_texture);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	// Restore state.
	glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2d);
	glActiveTexture((GLenum)prev_active_tex);
	glUseProgram((GLuint)prev_program);
	glBindVertexArray((GLuint)prev_vao);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_fbo);
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
	if (prev_blend)   glEnable(GL_BLEND);
	if (prev_depth)   glEnable(GL_DEPTH_TEST);
	if (prev_cull)    glEnable(GL_CULL_FACE);
	if (prev_scissor) glEnable(GL_SCISSOR_TEST);

	return ldp->ck_fill_tex;
}

// Post-weave strip: copy the woven default framebuffer to ck_strip_tex via
// glBlitFramebuffer, then render the strip pass back into the default
// framebuffer with chroma-matching pixels set to alpha=0 (premultiplied).
//
// On entry: GL_DRAW_FRAMEBUFFER_BINDING is the target framebuffer (FBO 0
//           for the default framebuffer; or a DComp interop FBO).
// On exit:  same binding restored.
static void
ck_run_post_weave_strip(struct leia_display_processor_gl_impl *ldp,
                        uint32_t target_w, uint32_t target_h)
{
	if (!ck_ensure_strip_source(ldp, target_w, target_h)) return;

	// Save state.
	GLint prev_draw_fbo = 0, prev_read_fbo = 0, prev_program = 0, prev_vao = 0;
	GLint prev_active_tex = 0, prev_tex2d = 0;
	GLint prev_viewport[4] = {0, 0, 0, 0};
	GLboolean prev_blend = GL_FALSE, prev_depth = GL_FALSE,
	          prev_cull = GL_FALSE, prev_scissor = GL_FALSE;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex2d);
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	prev_blend = glIsEnabled(GL_BLEND);
	prev_depth = glIsEnabled(GL_DEPTH_TEST);
	prev_cull  = glIsEnabled(GL_CULL_FACE);
	prev_scissor = glIsEnabled(GL_SCISSOR_TEST);

	// Step 1: blit current draw FBO -> ck_strip_blit_fbo. Read FBO is the
	// previous draw FBO (default framebuffer for the WGL path; DComp
	// interop FBO for the transparent path). Use NEAREST to avoid linear
	// blending across the chroma boundary.
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_draw_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ldp->ck_strip_blit_fbo);
	glBlitFramebuffer(0, 0, (GLsizei)target_w, (GLsizei)target_h,
	                  0, 0, (GLsizei)target_w, (GLsizei)target_h,
	                  GL_COLOR_BUFFER_BIT, GL_NEAREST);

	// Step 2: render strip into the original target FBO sampling ck_strip_tex.
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);
	glViewport(0, 0, (GLsizei)target_w, (GLsizei)target_h);

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);

	glBindVertexArray(ldp->ck_vao);
	glUseProgram(ldp->ck_program_strip);

	float chroma[3] = {0.0f, 0.0f, 0.0f};
	ck_unpack_chroma_rgb(ldp->ck_color, chroma);
	if (ldp->ck_strip_chroma_loc >= 0) {
		glUniform3fv(ldp->ck_strip_chroma_loc, 1, chroma);
	}
	if (ldp->ck_strip_src_loc >= 0) {
		glUniform1i(ldp->ck_strip_src_loc, 0);
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ldp->ck_strip_tex);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	// Restore state.
	glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2d);
	glActiveTexture((GLenum)prev_active_tex);
	glUseProgram((GLuint)prev_program);
	glBindVertexArray((GLuint)prev_vao);
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
	if (prev_blend)   glEnable(GL_BLEND);
	if (prev_depth)   glEnable(GL_DEPTH_TEST);
	if (prev_cull)    glEnable(GL_CULL_FACE);
	if (prev_scissor) glEnable(GL_SCISSOR_TEST);
}

static void
ck_release_resources(struct leia_display_processor_gl_impl *ldp)
{
	if (ldp->ck_fill_fbo != 0)       glDeleteFramebuffers(1, &ldp->ck_fill_fbo);
	if (ldp->ck_fill_tex != 0)       glDeleteTextures(1, &ldp->ck_fill_tex);
	if (ldp->ck_strip_blit_fbo != 0) glDeleteFramebuffers(1, &ldp->ck_strip_blit_fbo);
	if (ldp->ck_strip_tex != 0)      glDeleteTextures(1, &ldp->ck_strip_tex);
	if (ldp->ck_program_fill != 0)   glDeleteProgram(ldp->ck_program_fill);
	if (ldp->ck_program_strip != 0)  glDeleteProgram(ldp->ck_program_strip);
	if (ldp->ck_vao != 0)            glDeleteVertexArrays(1, &ldp->ck_vao);
	ldp->ck_fill_fbo = 0;
	ldp->ck_fill_tex = 0;
	ldp->ck_strip_blit_fbo = 0;
	ldp->ck_strip_tex = 0;
	ldp->ck_program_fill = 0;
	ldp->ck_program_strip = 0;
	ldp->ck_vao = 0;
	ldp->ck_inited = false;
}


/*
 *
 * Compose-under-bg + alpha-gate (preferred transparency path).
 *
 * Mirrors the D3D11 DP's compose_run_pre_weave / alpha_gate_run_post_weave,
 * but the WGC desktop is a D3D11 texture imported into GL via
 * WGL_NV_DX_interop2. Reuses ck_fill_tex/fbo (compose intermediate) and
 * ck_strip_tex/blit_fbo (alpha-gate woven-output copy). When the WGC / interop
 * bring-up fails, bg_compose_enabled stays false and the chroma-key path above
 * runs instead. See project_leia_transparency_model.
 *
 */

static bool
compose_init_programs(struct leia_display_processor_gl_impl *ldp)
{
	if (ldp->compose_program != 0 && ldp->alpha_gate_program != 0) {
		return true;
	}

	GLuint vs = ck_compile_shader(GL_VERTEX_SHADER, kCkVertSrc);
	if (vs == 0) return false;
	GLuint fs_co = ck_compile_shader(GL_FRAGMENT_SHADER, kComposeFragSrc);
	GLuint fs_ag = ck_compile_shader(GL_FRAGMENT_SHADER, kAlphaGateFragSrc);
	if (fs_co == 0 || fs_ag == 0) {
		if (fs_co) glDeleteShader(fs_co);
		if (fs_ag) glDeleteShader(fs_ag);
		glDeleteShader(vs);
		return false;
	}

	ldp->compose_program = ck_link_program(vs, fs_co);
	ldp->alpha_gate_program = ck_link_program(vs, fs_ag);

	glDeleteShader(fs_ag);
	glDeleteShader(fs_co);
	glDeleteShader(vs);

	if (ldp->compose_program == 0 || ldp->alpha_gate_program == 0) {
		if (ldp->compose_program) glDeleteProgram(ldp->compose_program);
		if (ldp->alpha_gate_program) glDeleteProgram(ldp->alpha_gate_program);
		ldp->compose_program = 0;
		ldp->alpha_gate_program = 0;
		return false;
	}

	ldp->co_atlas_loc        = glGetUniformLocation(ldp->compose_program, "atlas");
	ldp->co_bg_loc           = glGetUniformLocation(ldp->compose_program, "bg");
	ldp->co_backdrop_loc     = glGetUniformLocation(ldp->compose_program, "backdrop");
	ldp->co_bg_origin_loc    = glGetUniformLocation(ldp->compose_program, "bg_uv_origin");
	ldp->co_bg_extent_loc    = glGetUniformLocation(ldp->compose_program, "bg_uv_extent");
	ldp->co_tile_loc         = glGetUniformLocation(ldp->compose_program, "tile_count");
	ldp->co_has_backdrop_loc = glGetUniformLocation(ldp->compose_program, "has_backdrop");

	ldp->ag_backbuffer_loc    = glGetUniformLocation(ldp->alpha_gate_program, "backbuffer");
	ldp->ag_atlas_loc         = glGetUniformLocation(ldp->alpha_gate_program, "atlas");
	ldp->ag_backdrop_loc      = glGetUniformLocation(ldp->alpha_gate_program, "backdrop");
	ldp->ag_tile_loc          = glGetUniformLocation(ldp->alpha_gate_program, "tile_count");
	ldp->ag_has_backdrop_loc  = glGetUniformLocation(ldp->alpha_gate_program, "has_backdrop");
	ldp->ag_canvas_origin_loc = glGetUniformLocation(ldp->alpha_gate_program, "canvas_uv_origin");
	ldp->ag_canvas_extent_loc = glGetUniformLocation(ldp->alpha_gate_program, "canvas_uv_extent");

	// Empty VAO required by core-profile draws (shared with the ck path).
	if (ldp->ck_vao == 0) {
		glGenVertexArrays(1, &ldp->ck_vao);
	}
	return true;
}

static void
compose_release_resources_gl(struct leia_display_processor_gl_impl *ldp)
{
	// Unregister the interop object BEFORE closing the interop device.
	if (ldp->bg_interop_obj != nullptr && ldp->dx_interop_device != nullptr &&
	    ldp->pfn_wglDXUnregisterObjectNV != nullptr) {
		ldp->pfn_wglDXUnregisterObjectNV(ldp->dx_interop_device, ldp->bg_interop_obj);
	}
	ldp->bg_interop_obj = nullptr;
	if (ldp->bg_gl_tex != 0) {
		glDeleteTextures(1, &ldp->bg_gl_tex);
		ldp->bg_gl_tex = 0;
	}
	if (ldp->dx_interop_device != nullptr && ldp->pfn_wglDXCloseDeviceNV != nullptr) {
		ldp->pfn_wglDXCloseDeviceNV(ldp->dx_interop_device);
	}
	ldp->dx_interop_device = nullptr;

	if (ldp->bg_fence != nullptr)      { ldp->bg_fence->Release();      ldp->bg_fence = nullptr; }
	if (ldp->bg_local_tex != nullptr)  { ldp->bg_local_tex->Release();  ldp->bg_local_tex = nullptr; }
	if (ldp->bg_shared_tex != nullptr) { ldp->bg_shared_tex->Release(); ldp->bg_shared_tex = nullptr; }
	if (ldp->dx_context != nullptr)    { ldp->dx_context->Release();    ldp->dx_context = nullptr; }
	if (ldp->dx_device != nullptr)     { ldp->dx_device->Release();     ldp->dx_device = nullptr; }

	if (ldp->bg_capture != nullptr) {
		leia_bg_capture_destroy(ldp->bg_capture);
		ldp->bg_capture = nullptr;
	}

	if (ldp->compose_program != 0)    { glDeleteProgram(ldp->compose_program);    ldp->compose_program = 0; }
	if (ldp->alpha_gate_program != 0) { glDeleteProgram(ldp->alpha_gate_program); ldp->alpha_gate_program = 0; }

	ldp->bg_compose_enabled = false;
}

// Lazy compose-under-bg bring-up. Runs once (compose_init_tried guard) on the
// first process_atlas where compose was requested — the GL context is current
// there, which the wglDX* / D3D interop calls require. On ANY failure it leaves
// bg_compose_enabled false so the chroma-key fallback runs.
static void
compose_try_init_gl(struct leia_display_processor_gl_impl *ldp)
{
	if (ldp->compose_init_tried || !ldp->compose_requested) {
		return;
	}
	ldp->compose_init_tried = true; // one-shot regardless of outcome

	if (ldp->hwnd == nullptr) {
		U_LOG_W("Leia GL DP: no HWND for WGC — transparency = chroma-key");
		return;
	}

	// WGL_NV_DX_interop2 entry points (GL context is current here).
	ldp->pfn_wglDXOpenDeviceNV       = (PFN_wglDXOpenDeviceNV)wglGetProcAddress("wglDXOpenDeviceNV");
	ldp->pfn_wglDXCloseDeviceNV      = (PFN_wglDXCloseDeviceNV)wglGetProcAddress("wglDXCloseDeviceNV");
	ldp->pfn_wglDXRegisterObjectNV   = (PFN_wglDXRegisterObjectNV)wglGetProcAddress("wglDXRegisterObjectNV");
	ldp->pfn_wglDXUnregisterObjectNV = (PFN_wglDXUnregisterObjectNV)wglGetProcAddress("wglDXUnregisterObjectNV");
	ldp->pfn_wglDXLockObjectsNV      = (PFN_wglDXLockObjectsNV)wglGetProcAddress("wglDXLockObjectsNV");
	ldp->pfn_wglDXUnlockObjectsNV    = (PFN_wglDXUnlockObjectsNV)wglGetProcAddress("wglDXUnlockObjectsNV");
	if (ldp->pfn_wglDXOpenDeviceNV == nullptr || ldp->pfn_wglDXRegisterObjectNV == nullptr ||
	    ldp->pfn_wglDXLockObjectsNV == nullptr || ldp->pfn_wglDXUnlockObjectsNV == nullptr ||
	    ldp->pfn_wglDXCloseDeviceNV == nullptr || ldp->pfn_wglDXUnregisterObjectNV == nullptr) {
		U_LOG_W("Leia GL DP: WGL_NV_DX_interop2 unavailable — transparency = chroma-key");
		return;
	}

	if (!compose_init_programs(ldp)) {
		U_LOG_W("Leia GL DP: compose program init failed — transparency = chroma-key");
		return;
	}

	// DP-owned D3D11 device for the interop + cross-device fence Wait.
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
	                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
	                               D3D11_SDK_VERSION, &ldp->dx_device, &fl, &ldp->dx_context);
	if (FAILED(hr) || ldp->dx_device == nullptr) {
		U_LOG_W("Leia GL DP: D3D11CreateDevice failed (0x%08x) — transparency = chroma-key", (unsigned)hr);
		compose_release_resources_gl(ldp);
		return;
	}

	// WGC capture (honors LEIA_DP_DISABLE_BG_CAPTURE; NULL ⟹ chroma-key).
	// Producer must be on dx_device's adapter — shared textures do not cross
	// adapters and the default D3D11 device follows GpuPreference (#819).
	uint64_t bg_luid = 0;
	{
		IDXGIDevice *dxgi_device = nullptr;
		if (SUCCEEDED(ldp->dx_device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
			IDXGIAdapter *adapter = nullptr;
			if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
				DXGI_ADAPTER_DESC desc;
				if (SUCCEEDED(adapter->GetDesc(&desc))) {
					bg_luid = ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32) |
					          (uint64_t)(uint32_t)desc.AdapterLuid.LowPart;
				}
				adapter->Release();
			}
			dxgi_device->Release();
		}
	}
	ldp->bg_capture = leia_bg_capture_create(ldp->hwnd, bg_luid);
	if (ldp->bg_capture == nullptr) {
		U_LOG_W("Leia GL DP: WGC capture unavailable — transparency = chroma-key");
		compose_release_resources_gl(ldp);
		return;
	}

	// Open the shared desktop texture (the SRV is unused on the GL path).
	ID3D11ShaderResourceView *throwaway_srv = nullptr;
	hr = leia_bg_capture_open_d3d11(ldp->bg_capture, ldp->dx_device, &ldp->bg_shared_tex, &throwaway_srv);
	if (throwaway_srv != nullptr) {
		throwaway_srv->Release();
	}
	if (FAILED(hr) || ldp->bg_shared_tex == nullptr) {
		U_LOG_W("Leia GL DP: bg texture open failed (0x%08x) — transparency = chroma-key", (unsigned)hr);
		compose_release_resources_gl(ldp);
		return;
	}

	// Producer's shared fence for cross-device sync.
	hr = leia_bg_capture_open_fence_d3d11(ldp->bg_capture, ldp->dx_device, &ldp->bg_fence);
	if (FAILED(hr) || ldp->bg_fence == nullptr) {
		U_LOG_W("Leia GL DP: bg fence open failed (0x%08x) — transparency = chroma-key", (unsigned)hr);
		compose_release_resources_gl(ldp);
		return;
	}

	// Private device-local copy target. WGL_NV_DX_interop2 refuses to register a
	// resource opened from a cross-process shared handle (the WGC staging tex is
	// SHARED_NTHANDLE), so we register this plain device-local texture instead
	// and CopyResource the shared desktop into it each frame on dx_device.
	leia_bg_capture_get_size(ldp->bg_capture, &ldp->bg_w, &ldp->bg_h);
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = ldp->bg_w;
	td.Height = ldp->bg_h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // matches the WGC BGRA8 staging tex
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	hr = ldp->dx_device->CreateTexture2D(&td, nullptr, &ldp->bg_local_tex);
	if (FAILED(hr) || ldp->bg_local_tex == nullptr) {
		U_LOG_W("Leia GL DP: local bg texture create failed (0x%08x) — transparency = chroma-key",
		        (unsigned)hr);
		compose_release_resources_gl(ldp);
		return;
	}

	// Bridge the private D3D11 texture into GL.
	ldp->dx_interop_device = ldp->pfn_wglDXOpenDeviceNV(ldp->dx_device);
	if (ldp->dx_interop_device == nullptr) {
		U_LOG_W("Leia GL DP: wglDXOpenDeviceNV failed — transparency = chroma-key");
		compose_release_resources_gl(ldp);
		return;
	}
	glGenTextures(1, &ldp->bg_gl_tex);
	ldp->bg_interop_obj = ldp->pfn_wglDXRegisterObjectNV(ldp->dx_interop_device, ldp->bg_local_tex,
	                                                     ldp->bg_gl_tex, GL_TEXTURE_2D,
	                                                     WGL_ACCESS_READ_ONLY_NV);
	if (ldp->bg_interop_obj == nullptr) {
		U_LOG_W("Leia GL DP: wglDXRegisterObjectNV failed — transparency = chroma-key");
		compose_release_resources_gl(ldp);
		return;
	}

	ldp->bg_compose_enabled = true;
	ldp->ck_enabled = false; // prefer compose; never run both
	U_LOG_W("Leia GL DP: transparency = compose-under-bg (WGC)");
}

// Pre-weave compose: composite the captured desktop (+ optional backdrop) under
// the atlas tiles into ck_fill_tex (opaque RGB). Returns the GL texture to feed
// the weaver, or the raw atlas if no captured frame is ready this frame.
static GLuint
compose_run_pre_weave_gl(struct leia_display_processor_gl_impl *ldp,
                         GLuint atlas_texture,
                         uint32_t atlas_w, uint32_t atlas_h,
                         uint32_t tile_columns, uint32_t tile_rows,
                         uint32_t target_width, uint32_t target_height,
                         int32_t canvas_offset_x, int32_t canvas_offset_y,
                         uint32_t canvas_width, uint32_t canvas_height)
{
	if (!ck_ensure_fill_target(ldp, atlas_w, atlas_h)) {
		return atlas_texture;
	}

	float bg_origin[2] = {0.0f, 0.0f};
	float bg_extent[2] = {0.0f, 0.0f};
	uint64_t fence_val = 0;
	if (!leia_bg_capture_poll(ldp->bg_capture, bg_origin, bg_extent, &fence_val)) {
		// No captured frame yet / window crossed monitors. Weaver runs on the
		// raw atlas — transparent stays black this frame, recovers next.
		return atlas_texture;
	}

	// (#131) Canvas sub-rect: the weaver downscales the full atlas into the
	// sub-rect viewport, so remap the bg window UVs to the sub-rect fraction —
	// the desktop then lands 1:1 behind the sub-rect after the downscale.
	// Verbatim from the D3D11 DP.
	//
	// (#68) The remap would magnify the captured desktop for a TEXTURE-ZONES app
	// (window == canvas). The skip is gated on `shared_texture_present &&
	// zone_active` (matching the D3D11 DP): a texture app self-presenting only its
	// canvas needs the desktop 1:1 (skip), but a texture-SURROUND app (window ==
	// full target, no active zone) still needs the remap. Handle apps have
	// shared_texture_present=false → remap kept. (#613 wired the GL zones path.)
	bool skip_bg_remap = ldp->shared_texture_present && ldp->zone_active;
	if (!skip_bg_remap && canvas_width > 0 && canvas_height > 0 && target_width > 0 && target_height > 0) {
		float fx = (float)canvas_offset_x / (float)target_width;
		float fy = (float)canvas_offset_y / (float)target_height;
		float fw = (float)canvas_width  / (float)target_width;
		float fh = (float)canvas_height / (float)target_height;
		bg_origin[0] += fx * bg_extent[0];
		bg_origin[1] += fy * bg_extent[1];
		bg_extent[0] *= fw;
		bg_extent[1] *= fh;
	}

	// Cross-device sync + copy into our interop texture, all on OUR D3D11 queue:
	//   1. Wait on the producer's shared fence (producer wrote on WGC's device).
	//   2. CopyResource shared desktop → bg_local_tex (the registered interop tex).
	//   3. Flush so it is submitted before the GL lock serializes against it.
	// wglDXLockObjectsNV then makes GL wait for this copy to complete.
	if (ldp->dx_context != nullptr) {
		if (ldp->bg_fence != nullptr) {
			ID3D11DeviceContext4 *ctx4 = nullptr;
			if (SUCCEEDED(ldp->dx_context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4))) {
				ctx4->Wait(ldp->bg_fence, fence_val);
				ctx4->Release();
			}
		}
		ldp->dx_context->CopyResource(ldp->bg_local_tex, ldp->bg_shared_tex);
		ldp->dx_context->Flush();
	}

	if (!ldp->pfn_wglDXLockObjectsNV(ldp->dx_interop_device, 1, &ldp->bg_interop_obj)) {
		U_LOG_W("Leia GL DP: wglDXLockObjectsNV failed — passing raw atlas");
		return atlas_texture;
	}

	// Sampler params must be set while the object is locked (NV_DX_interop: GL
	// ops on the object are undefined while unlocked). Default min-filter needs
	// mips, so this is required to sample the bg at all.
	glBindTexture(GL_TEXTURE_2D, ldp->bg_gl_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	// Save GL state we trample (mirror ck_run_pre_weave_fill, plus 3 TUs).
	GLint prev_fbo = 0, prev_program = 0, prev_vao = 0, prev_active_tex = 0;
	GLint prev_tex0 = 0, prev_tex1 = 0, prev_tex2 = 0;
	GLint prev_viewport[4] = {0, 0, 0, 0};
	GLboolean prev_blend = glIsEnabled(GL_BLEND), prev_depth = glIsEnabled(GL_DEPTH_TEST),
	          prev_cull = glIsEnabled(GL_CULL_FACE), prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
	glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
	glActiveTexture(GL_TEXTURE1); glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex1);
	glActiveTexture(GL_TEXTURE2); glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex2);
	glGetIntegerv(GL_VIEWPORT, prev_viewport);

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ldp->ck_fill_fbo);
	glViewport(0, 0, (GLsizei)atlas_w, (GLsizei)atlas_h);
	glBindVertexArray(ldp->ck_vao);
	glUseProgram(ldp->compose_program);

	// GL 2D-under backdrop deferred: sampling the runtime's backdrop texture from
	// this plug-in faults/hangs the NV driver in the Local2D+transparent path
	// (needs runtime-side compositing — separate follow-up). Shader support stays
	// wired (has_backdrop=0 → dummy TU2, never sampled) so it's ready when that
	// runtime change lands.
	bool have_backdrop = false;
	if (ldp->co_atlas_loc >= 0)     glUniform1i(ldp->co_atlas_loc, 0);
	if (ldp->co_bg_loc >= 0)        glUniform1i(ldp->co_bg_loc, 1);
	if (ldp->co_backdrop_loc >= 0)  glUniform1i(ldp->co_backdrop_loc, 2);
	if (ldp->co_bg_origin_loc >= 0) glUniform2f(ldp->co_bg_origin_loc, bg_origin[0], bg_origin[1]);
	if (ldp->co_bg_extent_loc >= 0) glUniform2f(ldp->co_bg_extent_loc, bg_extent[0], bg_extent[1]);
	if (ldp->co_tile_loc >= 0)      glUniform2i(ldp->co_tile_loc, (GLint)tile_columns, (GLint)tile_rows);
	if (ldp->co_has_backdrop_loc >= 0) glUniform1i(ldp->co_has_backdrop_loc, have_backdrop ? 1 : 0);

	GLuint backdrop = ldp->bg_gl_tex; // dummy (GL backdrop deferred → has_backdrop always 0)
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, atlas_texture);
	glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, ldp->bg_gl_tex);
	glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, backdrop);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	// Restore textures + state.
	glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2);
	glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex1);
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex0);
	glActiveTexture((GLenum)prev_active_tex);
	glUseProgram((GLuint)prev_program);
	glBindVertexArray((GLuint)prev_vao);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_fbo);
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
	if (prev_blend)   glEnable(GL_BLEND);
	if (prev_depth)   glEnable(GL_DEPTH_TEST);
	if (prev_cull)    glEnable(GL_CULL_FACE);
	if (prev_scissor) glEnable(GL_SCISSOR_TEST);

	// Ensure GL fetches of the locked object complete before we release it.
	glFlush();
	ldp->pfn_wglDXUnlockObjectsNV(ldp->dx_interop_device, 1, &ldp->bg_interop_obj);

	return ldp->ck_fill_tex;
}

// Post-weave alpha-gate: copy the woven draw FBO to ck_strip_tex, then render
// the alpha-gate pass back into it. Pixels where ALL views' atlas α==0 become
// α=0 (live desktop); others keep the woven RGB at α=1. Replaces the chroma-key
// strip when compose is active. Mirrors alpha_gate_run_post_weave (D3D11).
//
// On entry/exit: GL_DRAW_FRAMEBUFFER_BINDING is the woven target FBO.
static void
alpha_gate_run_post_weave_gl(struct leia_display_processor_gl_impl *ldp,
                             GLuint atlas_texture,
                             uint32_t tile_columns, uint32_t tile_rows,
                             uint32_t target_width, uint32_t target_height,
                             int32_t canvas_offset_x, int32_t canvas_offset_y,
                             uint32_t canvas_width, uint32_t canvas_height)
{
	if (!ck_ensure_strip_source(ldp, target_width, target_height)) {
		return;
	}

	// Save state.
	GLint prev_draw_fbo = 0, prev_read_fbo = 0, prev_program = 0, prev_vao = 0, prev_active_tex = 0;
	GLint prev_tex0 = 0, prev_tex1 = 0, prev_tex2 = 0;
	GLint prev_viewport[4] = {0, 0, 0, 0}, prev_scissor[4] = {0, 0, 0, 0};
	GLboolean prev_blend = glIsEnabled(GL_BLEND), prev_depth = glIsEnabled(GL_DEPTH_TEST),
	          prev_cull = glIsEnabled(GL_CULL_FACE), prev_scissor_en = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
	glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
	glActiveTexture(GL_TEXTURE1); glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex1);
	glActiveTexture(GL_TEXTURE2); glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex2);
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	glGetIntegerv(GL_SCISSOR_BOX, prev_scissor);

	// Step 1: blit the woven draw FBO -> ck_strip_tex (NEAREST), same as the ck
	// strip pass. Read FBO is the previous draw FBO (window FBO, or DComp transit).
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_draw_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ldp->ck_strip_blit_fbo);
	glBlitFramebuffer(0, 0, (GLsizei)target_width, (GLsizei)target_height,
	                  0, 0, (GLsizei)target_width, (GLsizei)target_height,
	                  GL_COLOR_BUFFER_BIT, GL_NEAREST);

	// Canvas sub-rect (default = full target → identity). NOTE: GL viewport /
	// scissor are bottom-left origin; the full-canvas (handle-app) case is
	// identity and correct. The sub-rect (texture-app) mapping mirrors D3D11's
	// top-left convention and is unverified on GL (no GL texture app yet).
	int32_t  vp_x = 0, vp_y = 0;
	uint32_t vp_w = target_width, vp_h = target_height;
	float cu_ox = 0.0f, cu_oy = 0.0f, cu_ex = 1.0f, cu_ey = 1.0f;
	if (canvas_width > 0 && canvas_height > 0 && target_width > 0 && target_height > 0) {
		vp_x = canvas_offset_x;
		vp_y = canvas_offset_y;
		vp_w = canvas_width;
		vp_h = canvas_height;
		cu_ox = (float)canvas_offset_x / (float)target_width;
		cu_oy = (float)canvas_offset_y / (float)target_height;
		cu_ex = (float)canvas_width  / (float)target_width;
		cu_ey = (float)canvas_height / (float)target_height;
	}

	// Step 2: render the alpha-gate into the woven FBO sampling ck_strip_tex.
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);
	glViewport(vp_x, vp_y, (GLsizei)vp_w, (GLsizei)vp_h);
	glEnable(GL_SCISSOR_TEST);
	glScissor(vp_x, vp_y, (GLsizei)vp_w, (GLsizei)vp_h);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glBindVertexArray(ldp->ck_vao);
	glUseProgram(ldp->alpha_gate_program);

	bool have_backdrop = false; // GL 2D-under backdrop deferred (see compose pass)
	if (ldp->ag_backbuffer_loc >= 0)    glUniform1i(ldp->ag_backbuffer_loc, 0);
	if (ldp->ag_atlas_loc >= 0)         glUniform1i(ldp->ag_atlas_loc, 1);
	if (ldp->ag_backdrop_loc >= 0)      glUniform1i(ldp->ag_backdrop_loc, 2);
	if (ldp->ag_tile_loc >= 0)          glUniform2i(ldp->ag_tile_loc, (GLint)tile_columns, (GLint)tile_rows);
	if (ldp->ag_has_backdrop_loc >= 0)  glUniform1i(ldp->ag_has_backdrop_loc, have_backdrop ? 1 : 0);
	if (ldp->ag_canvas_origin_loc >= 0) glUniform2f(ldp->ag_canvas_origin_loc, cu_ox, cu_oy);
	if (ldp->ag_canvas_extent_loc >= 0) glUniform2f(ldp->ag_canvas_extent_loc, cu_ex, cu_ey);

	GLuint backdrop = ldp->ck_strip_tex; // dummy (GL backdrop deferred → has_backdrop always 0)
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, ldp->ck_strip_tex);
	glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, atlas_texture);
	glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, backdrop);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	// Restore textures + state.
	glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2);
	glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex1);
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex0);
	glActiveTexture((GLenum)prev_active_tex);
	glUseProgram((GLuint)prev_program);
	glBindVertexArray((GLuint)prev_vao);
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
	glScissor(prev_scissor[0], prev_scissor[1], prev_scissor[2], prev_scissor[3]);
	if (!prev_scissor_en) glDisable(GL_SCISSOR_TEST);
	if (prev_blend)   glEnable(GL_BLEND);
	if (prev_depth)   glEnable(GL_DEPTH_TEST);
	if (prev_cull)    glEnable(GL_CULL_FACE);
}


/*
 *
 * xrt_display_processor_gl interface methods.
 *
 */

static void
leia_dp_gl_process_atlas(struct xrt_display_processor_gl *xdp,
                           uint32_t atlas_texture,
                           uint32_t view_width,
                           uint32_t view_height,
                           uint32_t tile_columns,
                           uint32_t tile_rows,
                           uint32_t format,
                           uint32_t target_width,
                           uint32_t target_height,
                           int32_t canvas_offset_x,
                           int32_t canvas_offset_y,
                           uint32_t canvas_width,
                           uint32_t canvas_height)
{
	// TODO(#85): Pass canvas_offset_x/y to the vendor weaver for interlacing
	// phase correction once the Leia SR SDK supports sub-rect offset. (The
	// compose/alpha-gate passes below already consume the canvas sub-rect.)

	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);

	// ADR-021: `format` is the real atlas GL internal format (unchanged). This GL
	// variant declares no color capability and no set_atlas_encoding (Model-A
	// passthrough only); wiring EITHER + the weaver sRGB control is a follow-up.

	// runtime#542: atlas processing follows the CONTENT, not the lens. The
	// runtime hands us the grid it packed; a multi-view atlas weaves, a
	// single-view atlas flat-blits — regardless of the hardware state set
	// via request_display_mode. view_count also feeds the eye-position
	// centering below.
	ldp->view_count = (tile_columns * tile_rows > 1) ? tile_columns * tile_rows : 1;

	// Single-view content: bypass weaver, blit atlas content directly via glBlitFramebuffer
	if (ldp->view_count == 1) {
		// Lazily create the read FBO
		if (ldp->read_fbo == 0) {
			glGenFramebuffers(1, &ldp->read_fbo);
		}
		// Bind atlas to a temporary read FBO
		glBindFramebuffer(GL_READ_FRAMEBUFFER, ldp->read_fbo);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, atlas_texture, 0);
		// Blit content region (single view) to full draw framebuffer
		glBlitFramebuffer(0, 0, (GLint)view_width, (GLint)view_height,
		                  0, 0, (GLint)target_width, (GLint)target_height,
		                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
		// Restore read framebuffer
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		return;
	}

	// Atlas is guaranteed content-sized SBS (2*view_width x view_height)
	// by compositor crop-blit. Pass directly to weaver.

	// Lazy compose-under-bg bring-up (one-shot; no-op once settled or if not
	// requested). On success it disables ck_enabled so only one path runs.
	compose_try_init_gl(ldp);

	GLuint weaver_input = atlas_texture;
	uint32_t atlas_w = view_width * tile_columns;
	uint32_t atlas_h = view_height * tile_rows;

	// Preferred: compose-under-bg (opaque RGB to the weaver, transparency
	// recovered by the post-weave alpha-gate). Fallback: chroma-key fill/strip.
	bool compose_active = ldp->bg_compose_enabled;
	bool ck_active = false;
	if (compose_active) {
		weaver_input = compose_run_pre_weave_gl(ldp, atlas_texture, atlas_w, atlas_h,
		                                        tile_columns, tile_rows, target_width, target_height,
		                                        canvas_offset_x, canvas_offset_y, canvas_width,
		                                        canvas_height);
	} else {
		ck_active = ck_should_run(ldp);
		if (ck_active) {
			if (ck_init_pipeline(ldp)) {
				GLuint filled = ck_run_pre_weave_fill(ldp, atlas_texture, atlas_w, atlas_h);
				if (filled != 0) {
					weaver_input = filled;
				} else {
					ck_active = false;
				}
			} else {
				ck_active = false;
			}
		}
	}

	leiasr_gl_set_input_texture(ldp->leiasr, weaver_input, view_width, view_height, format);

	// Restore target viewport — SR weaver reads glViewport at weave() time,
	// but set_input_texture may have reset it to input dimensions.
	glViewport(0, 0, target_width, target_height);

	// Perform weaving to the currently bound framebuffer
	leiasr_gl_weave(ldp->leiasr);

	if (compose_active) {
		alpha_gate_run_post_weave_gl(ldp, atlas_texture, tile_columns, tile_rows, target_width,
		                             target_height, canvas_offset_x, canvas_offset_y, canvas_width,
		                             canvas_height);
	} else if (ck_active) {
		ck_run_post_weave_strip(ldp, target_width, target_height);
	}
}

static bool
leia_dp_gl_is_alpha_native(struct xrt_display_processor_gl *xdp)
{
	(void)xdp;
	// SR GL weaver outputs opaque RGB; transparency requires the
	// chroma-key fill+strip trick implemented in this DP.
	return false;
}

// #573 — the sole transparency enable (chroma-key removed). The leia GL DP only
// ever runs IN-PROCESS, where the runtime presents (its own DComp transparent
// present) and the DP does compose-under-bg. ck_color is kept purely as the α==0
// sentinel for the compose shader — there is no chroma-key fill/strip pass.
static void
leia_dp_gl_set_transparent_background(struct xrt_display_processor_gl *xdp, bool enabled, bool client_presents)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	// Sentinel for α==0 atlas pixels in the compose-under-bg shader (internal).
	ldp->ck_color = kDefaultChromaKey;
	ldp->ck_enabled = false; // no chroma-key strip/fill pass (#573)

	// Client-present mode: the runtime's transparent present blends the live
	// desktop into the holes, so the DP must NOT compose-under-bg.
	if (enabled && client_presents) {
		ldp->compose_requested = false;
		U_LOG_W("Leia GL DP: transparency = client-present (alpha-gate only, no WGC)");
		return;
	}

	// In-process path: request compose-under-bg. The actual WGC + WGL interop
	// bring-up is deferred to the first process_atlas (compose_try_init_gl),
	// where the GL context is guaranteed current.
	ldp->compose_requested = enabled;
	if (enabled) {
		U_LOG_W("Leia GL DP: transparency = compose-under-bg (WGC, deferred interop init)");
	} else {
		U_LOG_I("Leia GL DP: transparency disabled");
	}
}

// #68 — record whether the app self-presents only its shared-texture canvas to
// its own window (texture app) vs the runtime presenting the full weave target
// (handle app). When set, the compose-under-bg desktop-UV remap is skipped (the
// window IS the canvas, so the captured desktop already lands 1:1) — otherwise
// the remap magnifies the captured desktop (#68). The skip is gated on
// `shared_texture_present && zone_active` (#613 wired the GL zones path) — see the
// gate in compose_run_pre_weave_gl.
static void
leia_dp_gl_set_shared_texture_present(struct xrt_display_processor_gl *xdp, bool enabled)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	ldp->shared_texture_present = enabled;
	U_LOG_W("Leia GL DP: shared-texture present = %d (#68)", enabled);
}

// #491 part 3 — store the runtime's flattened 2D-under backdrop (a GL texture on
// the compositor's context). STORED ONLY on GL: the compose/alpha-gate carry the
// shader support, but sampling this runtime-owned texture from the plug-in
// mid-frame faults/hangs the NV driver in the Local2D+transparent path (the
// runtime writes it the same frame). GL 2D-under compositing therefore awaits a
// runtime-side change (composite the under-layer in gl_composite_local_2d, as it
// already does for the over-layers) — tracked follow-up. NULL ⟹ clear.
static void
leia_dp_gl_set_background_2d(struct xrt_display_processor_gl *xdp,
                             uint32_t background_tex,
                             uint32_t width,
                             uint32_t height)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	ldp->backdrop_tex = background_tex;
	ldp->backdrop_w = width;
	ldp->backdrop_h = height;
}

static bool
leia_dp_gl_get_predicted_eye_positions(struct xrt_display_processor_gl *xdp,
                                        struct xrt_eye_positions *out_eye_pos)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	float left[3], right[3];
	if (!leiasr_gl_get_predicted_eye_positions(ldp->leiasr, left, right)) {
		return false;
	}
	out_eye_pos->eyes[0].x = left[0];
	out_eye_pos->eyes[0].y = left[1];
	out_eye_pos->eyes[0].z = left[2];
	out_eye_pos->eyes[1].x = right[0];
	out_eye_pos->eyes[1].y = right[1];
	out_eye_pos->eyes[1].z = right[2];
	out_eye_pos->count = 2;
	out_eye_pos->valid = true;
	// Tracking-loss heuristic (#29 / runtime #441): in MANAGED mode the SR
	// SDK keeps serving animated positions through its grace period and
	// collapses the eye pair toward a single point as it falls back to 2D —
	// so near-zero inter-eye distance ⇔ tracking lost, with the flip timing
	// aligned to the actual 2D switch as the eye-tracking-modes spec
	// recommends. Same heuristic as leiasr_get_predicted_eye_positions()
	// (leia_sr.cpp); positions here are meters, so 1e-6 ⇔ 1 mm (real pair
	// ~63 mm apart). MUST be computed on the raw pair, before any 2D-mode
	// midpoint average.
	{
		const float dx = right[0] - left[0];
		const float dy = right[1] - left[1];
		const float dz = right[2] - left[2];
		out_eye_pos->is_tracking = (dx * dx + dy * dy + dz * dz) > 1e-6f;
	}
	// In 2D mode, average L/R to a single midpoint eye.
	if (ldp->view_count == 1 && out_eye_pos->count >= 2) {
		out_eye_pos->eyes[0].x = (out_eye_pos->eyes[0].x + out_eye_pos->eyes[1].x) * 0.5f;
		out_eye_pos->eyes[0].y = (out_eye_pos->eyes[0].y + out_eye_pos->eyes[1].y) * 0.5f;
		out_eye_pos->eyes[0].z = (out_eye_pos->eyes[0].z + out_eye_pos->eyes[1].z) * 0.5f;
		out_eye_pos->count = 1;
	}
	return true;
}

static bool
leia_dp_gl_get_window_metrics(struct xrt_display_processor_gl *xdp,
                               struct xrt_window_metrics *out_metrics)
{
	(void)xdp;
	(void)out_metrics;
	return false;
}

/*
 *
 * Display-zones vtable (#613 / ADR-027) — GL port of the D3D11 DP zone path.
 * Single-zone (1×1) panel: the published R8 wish mask is reduced to one bit
 * (any non-zero ⟹ 3D) and edge-triggers the SR lens hint. The mask arrives as a
 * GL texture name, so the readback attaches it to a dedicated FBO and reads it
 * with glReadPixels (the GL analogue of D3D11's CopyResource-to-staging + Map).
 * The mask is a HARDWARE-LENS signal only (ADR-028) — it never gates content.
 *
 */

static bool
leia_dp_gl_get_local_zone_caps(struct xrt_display_processor_gl *xdp, struct xrt_dp_local_zone_caps *out_caps)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	if (out_caps == nullptr || out_caps->struct_size < XRT_DP_LOCAL_ZONE_CAPS_SIZE_V1) {
		// The V1 shape is the floor — reject only callers older than the zone
		// api itself (mirrors the D3D11 DP).
		return false;
	}
	// Zones are driven through the per-client SR lens hint — needs the hint
	// channel to exist (same gate as request_display_mode support).
	if (!leiasr_gl_supports_display_mode_switch(ldp->leiasr)) {
		return false;
	}
	out_caps->supported = 1;
	out_caps->zone_grid_width = 1; // single-zone panel: union collapses to global on/off
	out_caps->zone_grid_height = 1;
	out_caps->max_mask_width = 0; // no preference — content is reduced to one bit
	out_caps->max_mask_height = 0;
	out_caps->max_update_hz = 0; // edge-triggered internally (verdict changes only)
	if (out_caps->struct_size >= sizeof(struct xrt_dp_local_zone_caps)) {
		out_caps->wish_fractional = 0; // binary panel state, any-nonzero quantization
		out_caps->switch_granularity = (uint32_t)XRT_DP_SWITCH_GRANULARITY_UNKNOWN;
		memset(out_caps->reserved, 0, sizeof(out_caps->reserved));
	}
	return true;
}

//! #215: verdict of one readback poll.
enum zone_rb_state
{
	ZONE_RB_PENDING = 0, //!< The read is still executing — no verdict yet.
	ZONE_RB_LANDED,      //!< Complete and scanned; *out_any holds the verdict.
	ZONE_RB_FAILED,      //!< Wait/map failed — caller must NOT flip the lens.
};

// #215: are the PBO + fence entry points the non-blocking path needs actually
// resolved? glad declares all of them, but leaves a pointer NULL when the
// context the DP was created on is below the GL version that exports it (the
// PBO half is GL 2.1/ARB_pixel_buffer_object, the sync half GL 3.2/ARB_sync).
// A NULL here is not an error — it just means this box takes the blocking
// fallback, which is the pre-#215 behaviour.
static bool
leia_dp_gl_zone_pbo_supported(void)
{
	return glGenBuffers != NULL && glBindBuffer != NULL && glBufferData != NULL &&
	       glMapBufferRange != NULL && glUnmapBuffer != NULL && glFenceSync != NULL &&
	       glClientWaitSync != NULL && glDeleteSync != NULL;
}

// #215: BLOCKING readback — the pre-#215 glReadPixels-into-client-memory path,
// unchanged. Two callers reach it: DXR_LEIA_ASYNC_ZONE_PUBLISH=0 (the
// kill-switch), and a driver where leia_dp_gl_zone_pbo_supported() is false.
// glReadPixels into client memory is an implicit full-pipeline sync on the
// runtime's context: the caller's thread pays for the GPU to catch up.
static bool
leia_dp_gl_zone_readback_blocking(struct leia_display_processor_gl_impl *ldp,
                                  uint32_t mask_tex,
                                  uint32_t mask_width,
                                  uint32_t mask_height,
                                  bool *out_any)
{
	const size_t need = (size_t)mask_width * mask_height;
	if (ldp->zone_buf == nullptr || ldp->zone_buf_w != mask_width || ldp->zone_buf_h != mask_height) {
		uint8_t *nb = (uint8_t *)realloc(ldp->zone_buf, need);
		if (nb == nullptr) {
			return false; // can't evaluate — don't flip the lens blindly
		}
		ldp->zone_buf = nb;
		ldp->zone_buf_w = mask_width;
		ldp->zone_buf_h = mask_height;
	}
	if (ldp->zone_read_fbo == 0) {
		glGenFramebuffers(1, &ldp->zone_read_fbo);
	}

	// Attach the R8 mask to our read FBO and glReadPixels it. Restore the
	// prior read-FBO binding so we don't perturb the runtime's GL state
	// (publish runs on the runtime's context right after present composite).
	GLint prev_read_fbo = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, ldp->zone_read_fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mask_tex, 0);
	if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
		U_LOG_W("SR GL zone: mask FBO incomplete — can't evaluate");
		return false; // mirror D3D11: don't flip on a failed readback
	}
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	GLint prev_pack = 4;
	glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, (GLsizei)mask_width, (GLsizei)mask_height, GL_RED, GL_UNSIGNED_BYTE, ldp->zone_buf);
	glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);

	bool any = false;
	for (size_t i = 0; i < need; i++) {
		if (ldp->zone_buf[i] != 0) {
			any = true;
			break;
		}
	}
	*out_any = any;
	return true;
}

// GPU→CPU readback of the R8 wish mask into a PIXEL_PACK buffer — ISSUE half
// (#215). Records the read and a fence, flushes, and returns WITHOUT waiting;
// the verdict is picked up by leia_dp_gl_zone_readback_poll on this or a later
// publish of the same generation. Returns false on any failure (caller must NOT
// flip the lens on an unevaluated mask).
//
// PRECONDITION: no read in flight (ldp->zone_fence == NULL). The PBO is the
// READ's DESTINATION — issuing a second read into it while the driver is still
// filling it for the first corrupts both, and glBufferData on a resize would
// orphan the storage the in-flight read is writing.
//
// Everything this touches on the runtime's context is saved and restored: the
// pixel-pack binding, the read-framebuffer binding and the pack alignment.
static bool
leia_dp_gl_zone_readback_issue(struct leia_display_processor_gl_impl *ldp,
                               uint32_t mask_tex,
                               uint32_t mask_width,
                               uint32_t mask_height,
                               uint64_t seq)
{
	if (ldp->zone_pbo == 0) {
		glGenBuffers(1, &ldp->zone_pbo);
		if (ldp->zone_pbo == 0) {
			return false;
		}
	}
	if (ldp->zone_read_fbo == 0) {
		glGenFramebuffers(1, &ldp->zone_read_fbo);
	}

	GLint prev_pbo = 0, prev_read_fbo = 0, prev_pack = 4;
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pbo);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
	glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack);

	// (Re)size the destination on a dims change only. Safe here and nowhere
	// else: the precondition says nothing is reading into it.
	if (ldp->zone_pbo_w != mask_width || ldp->zone_pbo_h != mask_height) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, ldp->zone_pbo);
		glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)((size_t)mask_width * mask_height), NULL,
		             GL_STREAM_READ);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pbo);
		ldp->zone_pbo_w = mask_width;
		ldp->zone_pbo_h = mask_height;
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, ldp->zone_read_fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mask_tex, 0);
	if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
		U_LOG_W("SR GL zone: mask FBO incomplete (gen %llu) — can't evaluate",
		        (unsigned long long)seq);
		return false; // mirror D3D11: don't flip on a failed readback
	}
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);

	// With a buffer bound to GL_PIXEL_PACK_BUFFER the last argument is an
	// OFFSET into it, not a client pointer — that is what makes the read
	// asynchronous.
	glBindBuffer(GL_PIXEL_PACK_BUFFER, ldp->zone_pbo);
	glReadPixels(0, 0, (GLsizei)mask_width, (GLsizei)mask_height, GL_RED, GL_UNSIGNED_BYTE, (void *)0);
	ldp->zone_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	// The fence AND the read have to be SUBMITTED for a zero-timeout poll to
	// ever succeed — a command sitting in the driver's unflushed buffer can
	// never signal, so the poll would return PENDING forever. Same rationale as
	// the D3D11 arm's Flush after the staging copy. (glClientWaitSync's
	// GL_SYNC_FLUSH_COMMANDS_BIT would do it too, but only on a BLOCKING wait
	// — which is exactly the thing this path exists to avoid.)
	glFlush();

	glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pbo);
	glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);

	if (ldp->zone_fence == nullptr) {
		return false; // no fence ⇒ nothing could ever poll this read out
	}

	// Issued — no wait. From here until the poll consumes it,
	// ldp->zone_fence != NULL reports "in flight".
	ldp->zone_readback_seq = seq;
	ldp->zone_readback_w = mask_width;
	ldp->zone_readback_h = mask_height;
	LARGE_INTEGER t0 = {};
	QueryPerformanceCounter(&t0);
	ldp->zone_readback_t0_qpc = t0.QuadPart;
	ldp->zone_readback_polls = 0;
	ldp->zone_readback_armed = true;
	return true;
}

// #215: POLL half — the CPU side of the read leia_dp_gl_zone_readback_issue put
// on the context. PENDING while the fence has not signalled (unless @p blocking,
// which waits up to 2 s). Once complete: map the PBO + the 1×1 OR-collapse scan
// (any non-zero pixel ⟹ 3D) + unmap, and *out_any is the verdict.
//
// The scan uses zone_readback_w/h — the dims the read was ISSUED with — because
// the mask can be resized while a read is in flight. Rows are tightly packed:
// the issue set GL_PACK_ALIGNMENT to 1, so the stride is exactly the width.
//
// Every terminal outcome (LANDED and FAILED alike) deletes the fence and NULLs
// it, which is what drops "in flight" back to false.
static enum zone_rb_state
leia_dp_gl_zone_readback_poll(struct leia_display_processor_gl_impl *ldp, bool blocking, bool *out_any)
{
	if (ldp->zone_fence == nullptr || ldp->zone_pbo == 0) {
		return ZONE_RB_FAILED;
	}

	const GLenum r = glClientWaitSync(ldp->zone_fence, blocking ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
	                                  blocking ? 2000000000ull : 0);
	if (r == GL_TIMEOUT_EXPIRED && !blocking) {
		return ZONE_RB_PENDING; // still executing — poll again next republish
	}
	if (r == GL_TIMEOUT_EXPIRED || r == GL_WAIT_FAILED) {
		// A 2 s wait that expires is a hung/lost context, not a slow frame.
		glDeleteSync(ldp->zone_fence);
		ldp->zone_fence = nullptr;
		return ZONE_RB_FAILED;
	}

	// GL_ALREADY_SIGNALED / GL_CONDITION_SATISFIED — the read has landed.
	GLint prev_pbo = 0;
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, ldp->zone_pbo);
	const size_t need = (size_t)ldp->zone_readback_w * ldp->zone_readback_h;
	const uint8_t *p =
	    (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)need, GL_MAP_READ_BIT);
	enum zone_rb_state state = ZONE_RB_FAILED;
	if (p != nullptr) {
		bool any = false;
		for (size_t i = 0; i < need; i++) {
			if (p[i] != 0) {
				any = true;
				break;
			}
		}
		glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		*out_any = any;
		state = ZONE_RB_LANDED;
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pbo);

	glDeleteSync(ldp->zone_fence);
	ldp->zone_fence = nullptr;
	return state;
}

static bool
leia_dp_gl_publish_local_zone_mask(struct xrt_display_processor_gl *xdp,
                                   uint32_t mask_tex,
                                   uint32_t mask_width,
                                   uint32_t mask_height,
                                   int32_t screen_x,
                                   int32_t screen_y,
                                   uint32_t screen_w,
                                   uint32_t screen_h,
                                   uint64_t seq)
{
	// 1×1 grid: the screen anchor can't change the verdict — only content
	// matters, and content only changes per generation (seq).
	(void)screen_x;
	(void)screen_y;
	(void)screen_w;
	(void)screen_h;

	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	if (mask_tex == 0 || mask_width == 0 || mask_height == 0) {
		// One-shot: a publish that is refused here is invisible to the runtime
		// (it does not log a false return), and "never evaluated" then reads
		// like "never called".
		if (!ldp->zone_refused_logged) {
			ldp->zone_refused_logged = true;
			U_LOG_W("SR GL zone: publish REFUSED (mask %u %ux%u) — lens hint untouched", mask_tex,
			        mask_width, mask_height);
		}
		return false;
	}

	// #215: the non-blocking path needs BOTH the kill-switch armed and the
	// PBO/sync entry points present; either missing takes the blocking one.
	const bool async = ldp->async_zone_publish && leia_dp_gl_zone_pbo_supported();

	if (!ldp->zone_first_logged) {
		ldp->zone_first_logged = true;
		U_LOG_W("SR GL zone: first publish (mask %ux%u, seq %llu, %s)", mask_width, mask_height,
		        (unsigned long long)seq, async ? "non-blocking" : "blocking");
	}

	// Content evaluation, once per generation: any non-zero mask pixel ⟹ 3D
	// (an all-zero mask — Tier-1 enable3D=FALSE — must collapse to 2D).
	const bool need_eval = !ldp->zone_eval_valid || seq != ldp->zone_eval_seq;
	if (need_eval && !async) {
		LARGE_INTEGER t0 = {}, t1 = {}, freq = {};
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&t0);
		bool any = false;
		if (!leia_dp_gl_zone_readback_blocking(ldp, mask_tex, mask_width, mask_height, &any)) {
			return false; // can't evaluate — don't flip the lens blindly
		}
		QueryPerformanceCounter(&t1);
		const double ms = freq.QuadPart > 0
		                      ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart
		                      : 0.0;
		ldp->zone_want_3d = any;
		ldp->zone_eval_seq = seq;
		ldp->zone_eval_valid = true;
		// Once per generation — not per frame.
		U_LOG_W("SR GL zone: generation %llu evaluated → %s (mask %ux%u, 1x1 collapse; "
		        "readback %.1f ms over 1 poll [blocking])",
		        (unsigned long long)seq, any ? "3D" : "2D", mask_width, mask_height, ms);
	} else if (need_eval) {
		// #215: issue once, then poll on the runtime's per-frame republish of
		// the same generation. In-flight is read off the FENCE, never a bool —
		// see the zone_* block comment.
		if (!ldp->zone_readback_armed) {
			if (ldp->zone_fence != nullptr) {
				// A previous generation's read is still executing (a clear
				// disarmed us mid-flight): the PBO is its destination and
				// cannot be re-targeted. Carry on and poll it out below.
			} else if (!leia_dp_gl_zone_readback_issue(ldp, mask_tex, mask_width, mask_height, seq)) {
				if (!ldp->zone_refused_logged) {
					ldp->zone_refused_logged = true;
					U_LOG_W("SR GL zone: readback ISSUE failed for generation %llu — lens hint "
					        "untouched",
					        (unsigned long long)seq);
				}
				return false; // can't evaluate — don't flip the lens blindly
			}
		}

		if (ldp->zone_readback_armed) {
			ldp->zone_readback_polls++;
			bool any = false;
			switch (leia_dp_gl_zone_readback_poll(ldp, /*blocking=*/false, &any)) {
			case ZONE_RB_LANDED: {
				ldp->zone_want_3d = any;
				ldp->zone_eval_seq = ldp->zone_readback_seq;
				ldp->zone_eval_valid = true;
				ldp->zone_readback_armed = false;

				LARGE_INTEGER t1 = {}, freq = {};
				QueryPerformanceCounter(&t1);
				QueryPerformanceFrequency(&freq);
				const double ms = freq.QuadPart > 0
				                      ? (double)(t1.QuadPart - ldp->zone_readback_t0_qpc) * 1000.0 /
				                            (double)freq.QuadPart
				                      : 0.0;
				// Once per generation — not per frame.
				U_LOG_W("SR GL zone: generation %llu evaluated → %s (mask %ux%u, 1x1 collapse; "
				        "readback %.1f ms over %u poll%s)",
				        (unsigned long long)ldp->zone_readback_seq, any ? "3D" : "2D",
				        ldp->zone_readback_w, ldp->zone_readback_h, ms, ldp->zone_readback_polls,
				        ldp->zone_readback_polls == 1 ? "" : "s");
				// If the runtime moved on to a newer generation while this one
				// was in flight, the next publish's seq gate re-issues for it
				// (zone_eval_seq != seq). Do NOT issue here — nothing has
				// consumed the frame this poll ran on yet.
				break;
			}
			case ZONE_RB_PENDING:
				// REQUEST API — acceptance, not completion. With a previous
				// verdict we carry it (falling through to the edge below, which
				// will not fire on an unchanged verdict); on the very first
				// publish there is nothing to drive the lens with yet, so leave
				// zone_active untouched and let the next republish poll again.
				if (!ldp->zone_eval_valid) {
					return true;
				}
				break;
			case ZONE_RB_FAILED:
				ldp->zone_readback_armed = false;
				return false;
			}
		} else if (!ldp->zone_eval_valid) {
			// In flight from a previous generation and no verdict ever landed.
			return true;
		}
	}

	// Edge-triggered lens hint: flip only when the verdict changes (or on the
	// first publish), so per-frame republish costs nothing SR-side.
	if (!ldp->zone_active || ldp->zone_hint_3d != ldp->zone_want_3d) {
		if (!leiasr_gl_request_display_mode(ldp->leiasr, ldp->zone_want_3d)) {
			return false;
		}
		ldp->zone_hint_3d = ldp->zone_want_3d;
	}
	ldp->zone_active = true;
	return true;
}

static bool
leia_dp_gl_clear_local_zone_mask(struct xrt_display_processor_gl *xdp)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	// End of zone authority for this client — hand the lens back to the MODE
	// authority: restore the hint to what the active rendering mode implies
	// (view_count ≥ 2 ⟹ a 3D mode is active and the compositor snaps back to
	// the canvas weave, which needs a 3D panel; view_count 1 ⟹ 2D mode).
	// Blindly disabling here would strand a 3D-mode canvas weave on a 2D panel.
	if (ldp->zone_active) {
		bool mode_wants_3d = ldp->view_count >= 2;
		if (ldp->zone_hint_3d != mode_wants_3d) {
			leiasr_gl_request_display_mode(ldp->leiasr, mode_wants_3d);
		}
		ldp->zone_hint_3d = false;
		U_LOG_W("SR GL zone: cleared — lens handed back to mode authority (%s)",
		        mode_wants_3d ? "3D" : "2D");
	}
	ldp->zone_active = false;
	ldp->zone_eval_valid = false;
	// #215: forget the issued readback's verdict — the next publish starts a
	// fresh generation. The READ itself is not forgotten: in-flight is derived
	// from the fence, so the next issue still refuses to re-target the PBO
	// under a read the driver is still filling.
	ldp->zone_readback_armed = false;
	return true;
}

static bool
leia_dp_gl_request_display_mode(struct xrt_display_processor_gl *xdp, bool enable_3d)
{
	// runtime#542: HARDWARE only — drive the SR lens hint and nothing else.
	// Atlas processing (weave vs flat blit) follows the CONTENT: view_count
	// tracks the per-frame atlas grid in process_atlas, so a hardware
	// override (xrRequestDisplayModeDXR) leaves the weave running and the
	// panel shows the woven atlas flat — the app-authored transition state.
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	return leiasr_gl_request_display_mode(ldp->leiasr, enable_3d);
}

static bool
leia_dp_gl_get_hardware_3d_state(struct xrt_display_processor_gl *xdp, bool *out_is_3d)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	return leiasr_gl_get_hardware_3d_state(ldp->leiasr, out_is_3d);
}

static bool
leia_dp_gl_get_display_dimensions(struct xrt_display_processor_gl *xdp,
                                   float *out_width_m,
                                   float *out_height_m)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	struct leiasr_display_dimensions dims = {};
	if (!leiasr_gl_get_display_dimensions(ldp->leiasr, &dims) || !dims.valid) {
		return false;
	}
	*out_width_m = dims.width_m;
	*out_height_m = dims.height_m;
	return true;
}

static bool
leia_dp_gl_get_display_pixel_info(struct xrt_display_processor_gl *xdp,
                                   uint32_t *out_pixel_width,
                                   uint32_t *out_pixel_height,
                                   int32_t *out_screen_left,
                                   int32_t *out_screen_top)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	float w_m, h_m; // unused but required by API
	return leiasr_gl_get_display_pixel_info(ldp->leiasr, out_pixel_width, out_pixel_height, out_screen_left,
	                                        out_screen_top, &w_m, &h_m);
}

static void
leia_dp_gl_destroy(struct xrt_display_processor_gl *xdp)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);

	compose_release_resources_gl(ldp);
	ck_release_resources(ldp);

	if (ldp->read_fbo != 0) {
		glDeleteFramebuffers(1, &ldp->read_fbo);
	}
	// #613 zone readback machinery.
	// #215: with the readback made non-blocking, a read can still be executing
	// here — and its destination, zone_pbo, is about to go away. Drain the
	// fence first. Bounded (200 ms), not infinite: teardown must not wedge, and
	// the only way this times out is a hung/lost context, where the objects
	// below are moot anyway. A non-NULL fence proves the entry points resolved,
	// so no pointer test is needed; the GL calls assume a current context, the
	// same assumption the glDelete* calls around them have always made.
	if (ldp->zone_fence != nullptr) {
		const GLenum r = glClientWaitSync(ldp->zone_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 200000000ull);
		if (r == GL_TIMEOUT_EXPIRED || r == GL_WAIT_FAILED) {
			U_LOG_W("Leia GL DP: zone readback still in flight at destroy — releasing anyway");
		}
		glDeleteSync(ldp->zone_fence);
		ldp->zone_fence = nullptr;
	}
	if (ldp->zone_pbo != 0) {
		glDeleteBuffers(1, &ldp->zone_pbo);
		ldp->zone_pbo = 0;
	}
	if (ldp->zone_read_fbo != 0) {
		glDeleteFramebuffers(1, &ldp->zone_read_fbo);
	}
	if (ldp->zone_buf != NULL) {
		free(ldp->zone_buf);
		ldp->zone_buf = NULL;
	}

	if (ldp->leiasr != NULL) {
		leiasr_gl_destroy(&ldp->leiasr);
	}
	free(ldp);
}


/*
 *
 * Helper to populate vtable entries on an impl struct.
 *
 */

#ifdef XRT_DP_GL_HAS_BACKEND_STATE
/*!
 * #158: report the SR backend's health to the runtime.
 *
 * Detection only on this arm (no in-place reconnect — see the poll's doc): a
 * restarted SR platform is reported STALE, and the runtime's remedy is to
 * recreate the display processor. Computed on demand; non-blocking.
 */
static bool
leia_dp_gl_get_backend_state(struct xrt_display_processor_gl *xdp, uint32_t *out_state)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	if (ldp == NULL || out_state == NULL) {
		return false;
	}
	*out_state = leiasr_gl_poll_backend_state(ldp->leiasr);
	return true;
}
#endif

/*
 * #224 / runtime#1363 — rear depth budget background source. Pure forward to
 * the shared WGC module (the preview is produced once there, per capture
 * generation); no policy in vendor code. NULL bg_capture ⟹ no source.
 */
#if defined(XRT_DP_GL_HAS_BACKGROUND_PREVIEW) && defined(LEIA_BG_CAPTURE_HAS_PREVIEW)
static bool
leia_dp_gl_get_background_preview(struct xrt_display_processor_gl *xdp,
                                  struct xrt_dp_background_preview *out_preview)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	if (ldp == NULL) {
		return false;
	}
	return leia_bg_capture_get_preview(ldp->bg_capture, out_preview);
}
#endif

static void
leia_dp_gl_init_vtable(struct leia_display_processor_gl_impl *ldp)
{
	// ADR-020 rule 1: advertise the vtable size (caller calloc'd the struct
	// so reserved_0 is already zero).
	ldp->base.struct_size = static_cast<uint32_t>(sizeof(struct xrt_display_processor_gl));
	ldp->base.process_atlas = leia_dp_gl_process_atlas;
	ldp->base.get_predicted_eye_positions = leia_dp_gl_get_predicted_eye_positions;
	ldp->base.get_window_metrics = leia_dp_gl_get_window_metrics;
	ldp->base.request_display_mode = leia_dp_gl_request_display_mode;
	ldp->base.get_hardware_3d_state = leia_dp_gl_get_hardware_3d_state;
	ldp->base.get_display_dimensions = leia_dp_gl_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_gl_get_display_pixel_info;
	ldp->base.is_alpha_native = leia_dp_gl_is_alpha_native;
	ldp->base.set_transparent_background = leia_dp_gl_set_transparent_background; // #573
	ldp->base.set_background_2d = leia_dp_gl_set_background_2d; // #491 part 3 (no-op store; GL composite deferred)
	ldp->base.set_shared_texture_present = leia_dp_gl_set_shared_texture_present; // #68
	// #613 / ADR-027 — display-zones vtable (slots 13/14/15, GL port of D3D11).
	ldp->base.get_local_zone_caps = leia_dp_gl_get_local_zone_caps;
	ldp->base.publish_local_zone_mask = leia_dp_gl_publish_local_zone_mask;
	ldp->base.clear_local_zone_mask = leia_dp_gl_clear_local_zone_mask;
	ldp->base.destroy = leia_dp_gl_destroy;
#ifdef XRT_DP_GL_HAS_BACKEND_STATE
	ldp->base.get_backend_state = leia_dp_gl_get_backend_state; // #158 SR restart detection
#endif
#if defined(XRT_DP_GL_HAS_BACKGROUND_PREVIEW) && defined(LEIA_BG_CAPTURE_HAS_PREVIEW)
	ldp->base.get_background_preview = leia_dp_gl_get_background_preview; // #224 rear depth budget (slot 21)
#endif
}


/*
 *
 * GLAD function-pointer loading for the plug-in module.
 *
 * aux_ogl is a static lib, so DisplayXR-LeiaSR.dll has its OWN copy of the
 * glad_gl* function-pointer table — the runtime's load (comp_gl_compositor)
 * does NOT populate ours. Without this, the first GL call in process_atlas
 * (e.g. glViewport) dereferences a null pointer and crashes. Mirrors the
 * runtime's loader (comp_gl_compositor.cpp); runs once, on the GL context the
 * compositor made current before invoking the factory.
 *
 */

//! GLAD loader: wglGetProcAddress first, fall back to opengl32.dll exports.
static GLADapiproc
leia_gl_get_proc_addr(void *userptr, const char *name)
{
	GLADapiproc ret = (GLADapiproc)wglGetProcAddress(name);
	if (ret == NULL) {
		ret = (GLADapiproc)GetProcAddress((HMODULE)userptr, name);
	}
	return ret;
}

static bool
leia_gl_ensure_glad_loaded(void)
{
	static bool loaded = false;
	if (loaded) {
		return true;
	}

	// Kept loaded for the process lifetime (no FreeLibrary) so the function
	// pointers stay valid.
	HMODULE opengl_dll = LoadLibraryW(L"opengl32.dll");
	if (opengl_dll == NULL) {
		U_LOG_E("Leia GL DP: failed to load opengl32.dll");
		return false;
	}

	int gl_result = gladLoadGLUserPtr(leia_gl_get_proc_addr, opengl_dll);
	if (gl_result == 0) {
		U_LOG_E("Leia GL DP: gladLoadGLUserPtr failed (no current GL context?)");
		return false;
	}

	loaded = true;
	U_LOG_W("Leia GL DP: GLAD loaded for plug-in module (GL %d.%d)",
	        GLAD_VERSION_MAJOR(gl_result), GLAD_VERSION_MINOR(gl_result));
	return true;
}


/*
 *
 * Factory function — matches xrt_dp_factory_gl_fn_t signature.
 *
 */

extern "C" xrt_result_t
leia_dp_factory_gl(void *window_handle,
                    struct xrt_display_processor_gl **out_xdp)
{
	// Populate this module's GLAD table before any GL call (see above). The
	// compositor has made the GL context current ahead of this call.
	if (!leia_gl_ensure_glad_loaded()) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	// Create weaver — view dimensions are set per-frame via setInputViewTexture,
	// so we pass 0,0 here.
	struct leiasr_gl *weaver = NULL;
	xrt_result_t ret = leiasr_gl_create(5.0, window_handle, 0, 0, &weaver);
	if (ret != XRT_SUCCESS || weaver == NULL) {
		U_LOG_W("Failed to create SR GL weaver");
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor_gl_impl *ldp =
	    (struct leia_display_processor_gl_impl *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		leiasr_gl_destroy(&weaver);
		return XRT_ERROR_ALLOCATION;
	}

	leia_dp_gl_init_vtable(ldp);
	ldp->leiasr = weaver;
	ldp->view_count = 2;
	ldp->hwnd = (HWND)window_handle; // retained for WGC compose-under-bg

	// #215: the zone publish slot used to block the runtime's compositor
	// thread for ~180 ms per real mask change — a synchronous GPU readback
	// plus a synchronous SR-service round trip for the lens hint. Both are
	// non-blocking by default; DXR_LEIA_ASYNC_ZONE_PUBLISH=0 reverts both for
	// bisecting. Set after ldp->leiasr exists — the lens half lives there.
	// GLAD is already loaded at this point (top of this factory), so the PBO
	// half's availability is knowable here rather than at first publish.
	const char *zone_env = std::getenv("DXR_LEIA_ASYNC_ZONE_PUBLISH");
	ldp->async_zone_publish = !(zone_env != NULL && zone_env[0] == '0');
	leiasr_gl_set_async_lens(ldp->leiasr, ldp->async_zone_publish);
	U_LOG_W("Leia GL zone publish: %s (#215; DXR_LEIA_ASYNC_ZONE_PUBLISH=0 reverts to the "
	        "blocking readback + synchronous lens hint; pbo=%s)",
	        ldp->async_zone_publish ? "non-blocking readback + async lens hint"
	                                : "BLOCKING (kill-switch)",
	        leia_dp_gl_zone_pbo_supported() ? "available" : "MISSING — blocking readback");

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR GL display processor (factory, owns weaver)");

	return XRT_SUCCESS;
}
