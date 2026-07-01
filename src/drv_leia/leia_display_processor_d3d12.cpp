// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Leia D3D12 display processor: wraps SR SDK D3D12 weaver
 *         as an @ref xrt_display_processor_d3d12.
 *
 * The display processor owns the leiasr_d3d12 handle — it creates it
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

#include "leia_display_processor_d3d12.h"
#include "leia_sr_d3d12.h"
#include "leia_bg_capture_win.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <cstdlib>
#include <cstring>


// Fullscreen quad vertex shader (4 vertices, triangle strip via SV_VertexID)
static const char *blit_vs_source = R"(
struct VS_OUTPUT {
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

VS_OUTPUT main(uint id : SV_VertexID) {
	VS_OUTPUT o;
	o.uv = float2(id & 1, id >> 1);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
)";

// Passthrough pixel shader: samples first tile from atlas, stretches to fill target
static const char *blit_ps_source = R"(
Texture2D atlas_tex : register(t0);
SamplerState samp : register(s0);

cbuffer BlitParams : register(b0) {
	float u_scale;
	float v_scale;
	float pad0;
	float pad1;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	return atlas_tex.Sample(samp, float2(uv.x * u_scale, uv.y * v_scale));
}
)";

/*
 * Chroma-key shaders — same algorithm as the D3D11 DP, retargeted to D3D12.
 * The fill pass replaces alpha=0 atlas pixels with the chroma key so the SR
 * weaver (opaque RGB only) can run. The strip pass examines the woven back
 * buffer and rewrites RGB-matching pixels to alpha=0 (with RGB premultiplied
 * for DWM's premultiplied alpha mode).
 *
 * Both shaders share a fullscreen-triangle VS (3 verts via SV_VertexID).
 * Root signature: b0 = 32-bit constants (chroma_rgb + pad), t0 = SRV
 * descriptor table, s0 = static sampler (point filter).
 */
static const char *ck_vs_source = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// Pre-weave fill: atlas RGBA → opaque RGB with alpha=0 regions filled
// by chroma_rgb. lerp(key, src.rgb, src.a) is no-op for alpha=1 (legacy
// app-pre-filled flow) and full key for alpha=0 (true-alpha flow).
static const char *ck_fill_ps_source = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
cbuffer Constants : register(b0) { float3 chroma_rgb; float pad; };
float4 main(VSOut i) : SV_Target {
    float4 c = src.Sample(samp, i.uv);
    float3 rgb = lerp(chroma_rgb, c.rgb, c.a);
    return float4(rgb, 1.0);
}
)";

// Post-weave strip: woven RGB → alpha=0 where RGB exact-matches chroma_rgb,
// alpha=1 elsewhere with RGB premultiplied so DWM's
//     src.rgb + (1-alpha)*dst.rgb
// blend doesn't add the matched chroma color to the desktop and saturate.
static const char *ck_strip_ps_source = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
cbuffer Constants : register(b0) { float3 chroma_rgb; float pad; };
float4 main(VSOut i) : SV_Target {
    float3 c = src.Sample(samp, i.uv).rgb;
    float3 d = abs(c - chroma_rgb);
    bool match = max(max(d.r, d.g), d.b) < (1.0/512.0);
    float a = match ? 0.0 : 1.0;
    return float4(c * a, a);
}
)";

/*
 * Default chroma key when the app didn't supply one (set_chroma_key key=0).
 * Magenta — matches the D3D11 DP's kDefaultChromaKey for cross-API parity.
 * 0x00BBGGRR layout: R=0xFF, G=0x00, B=0xFF.
 */
static constexpr uint32_t kDefaultChromaKey = 0x00FF00FF;

/*
 * Compose-under-bg pre-weave shader (preferred path when WGC is available).
 *
 * Reads the app's RGBA atlas + the captured desktop region behind the window,
 * composes them per-tile and outputs opaque RGB the SR weaver consumes:
 *
 *   out = lerp(bg, atlas.rgb, atlas.a),  out.a = 1
 *
 * Same VS as the ck pipeline (single fullscreen triangle).
 */
/*
 * Post-weave alpha-gate (compose-mode replacement for the chroma-key strip).
 * Samples the woven back-buffer (in copy form via ck_strip_tex) and the
 * original atlas, then for each screen pixel tests whether ALL tiles' atlas
 * α==0 at the matching tile-local UV. Pixels passing the test get α=0 (DWM
 * blends live desktop); others keep α=1.
 *
 * Screen UV == tile-local UV when target = (tile_columns × view_w,
 * tile_rows × view_h), which is the canvas-fills-target case.
 */
static const char *alpha_gate_ps_source = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2D<float4> backbuffer : register(t0);
Texture2D<float4> atlas      : register(t1);
// #491 part 3 — runtime's flattened 2D-under backdrop (premultiplied). A real
// under-layer: where the atlas is transparent but the backdrop covers, emit the
// backdrop premultiplied with its own alpha (DWM composites it over the LIVE
// desktop) instead of punching to full transparency.
Texture2D<float4> backdrop   : register(t2);
SamplerState samp            : register(s0);
cbuffer Constants : register(b0) {
    uint2 tile_count;
    uint  has_backdrop;       // #491 part 3 — 1 ⟹ a 2D-under backdrop is present
    uint  pad;
    float2 canvas_uv_origin;  // canvas sub-rect origin on the window, normalized
    float2 canvas_uv_extent;  // canvas sub-rect size on the window, normalized
};
float4 main(VSOut i) : SV_Target {
    // (#131) The gate runs only over the canvas sub-rect viewport, so i.uv (0..1)
    // maps directly to the woven tiger's tile-local atlas UV. The back buffer is
    // full-window → map i.uv into the window via the canvas rect. Default (no
    // sub-rect) origin=(0,0) extent=(1,1) → identity (original behavior).
    bool all_transparent = true;
    for (uint ty = 0; ty < tile_count.y; ty++) {
        for (uint tx = 0; tx < tile_count.x; tx++) {
            float2 uv_at_tile = (float2(tx, ty) + i.uv) / float2(tile_count);
            if (atlas.SampleLevel(samp, uv_at_tile, 0).a > 0.0) {
                all_transparent = false;
            }
        }
    }
    float2 bb_uv = canvas_uv_origin + i.uv * canvas_uv_extent;
    if (!all_transparent) {
        // Woven 3D content (already over backdrop-over-desktop): opaque.
        return float4(backbuffer.Sample(samp, bb_uv).rgb, 1.0);
    }
    // #491 part 3 — atlas transparent: emit the backdrop premultiplied with its
    // own alpha (DWM adds the live desktop) instead of punching fully through.
    if (has_backdrop != 0) {
        float4 bd = backdrop.Sample(samp, bb_uv);
        return float4(bd.rgb, bd.a);
    }
    return float4(0.0, 0.0, 0.0, 0.0); // live desktop
}
)";

static const char *compose_under_bg_ps_source = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2D<float4> atlas : register(t0);
Texture2D<float4> bg    : register(t1);
// #491 part 3 — runtime's flattened 2D-under backdrop (premultiplied,
// window-client-area). Composited OVER the captured desktop when has_backdrop.
Texture2D<float4> backdrop : register(t2);
SamplerState samp       : register(s0);
cbuffer Constants : register(b0) {
    float2 bg_uv_origin;
    float2 bg_uv_extent;
    uint2  tile_count;
    uint   has_backdrop;  // #491 part 3 — 1 ⟹ composite backdrop over desktop
    uint   pad0;
    float3 chroma_rgb;    // sentinel for fully-transparent atlas pixels
    float  pad1;
};
float4 main(VSOut i) : SV_Target {
    // Plain compose-with-bg. Transparency holes are produced by the
    // post-weave alpha-gate pass — this shader never emits a chroma sentinel.
    float4 a = atlas.Sample(samp, i.uv);
    float2 tile_local = frac(i.uv * float2(tile_count));
    float2 bg_uv = bg_uv_origin + tile_local * bg_uv_extent;
    float3 b = bg.SampleLevel(samp, bg_uv, 0).rgb;
    // #491 part 3 — flat z=0 backdrop sampled at window-local tile_local;
    // premultiplied "over" the desktop before the atlas-over.
    if (has_backdrop != 0) {
        float4 bd = backdrop.Sample(samp, tile_local);
        b = bd.rgb + (1.0 - bd.a) * b;
    }
    return float4(lerp(b, a.rgb, a.a), 1.0);
}
)";


/*!
 * Implementation struct wrapping leiasr_d3d12 as xrt_display_processor_d3d12.
 */
struct leia_display_processor_d3d12_impl
{
	struct xrt_display_processor_d3d12 base;
	struct leiasr_d3d12 *leiasr; //!< Owned — destroyed in leia_dp_d3d12_destroy.

	ID3D12Device *device;              //!< Cached device reference (not owned, for blit init).
	HWND hwnd;                         //!< Native window handle from factory, used by bg-capture for self-exclusion + window-on-monitor rect.

	//! @name 2D blit pipeline resources (passthrough stretch-blit)
	//! @{
	ID3D12RootSignature *blit_root_sig;
	ID3D12PipelineState *blit_pso;
	ID3D12DescriptorHeap *blit_srv_heap; //!< Shader-visible, 1 SRV
	DXGI_FORMAT blit_output_format;
	//! @}

	uint32_t view_count; //!< Active mode view count (1=2D, 2=stereo).

	//! @name Chroma-key transparency support (lazy-allocated on first frame)
	//!
	//! When @ref ck_enabled and @ref ck_color != 0, process_atlas() does:
	//!   1. Pre-weave fill: atlas RGBA → ck_fill_tex (alpha=0 → chroma_rgb,
	//!      output alpha=1) so the SR weaver receives opaque RGB.
	//!   2. Pass ck_fill_tex (resource pointer) to the weaver instead of the
	//!      original atlas via leiasr_d3d12_set_input_texture.
	//!   3. Post-weave strip: copy back buffer → ck_strip_tex, then run strip
	//!      shader back to back-buffer RTV (chroma match → alpha=0, RGB
	//!      premultiplied for DWM).
	//! @{
	bool ck_enabled;
	uint32_t ck_color;                       //!< 0x00BBGGRR; effective key.
	ID3D12RootSignature *ck_root_sig;        //!< Shared by fill + strip PSOs.
	ID3D12PipelineState *ck_fill_pso;
	ID3D12PipelineState *ck_strip_pso;
	ID3D12DescriptorHeap *ck_srv_heap_fill;  //!< Shader-visible, 1 SRV (atlas).
	ID3D12DescriptorHeap *ck_srv_heap_strip; //!< Shader-visible, 1 SRV (strip_tex).
	ID3D12DescriptorHeap *ck_rtv_heap_fill;  //!< Non-shader-visible, 1 RTV (fill_tex).
	// Pre-weave fill target — RT-bindable + SRV-readable; the weaver samples
	// this resource directly via leiasr_d3d12_set_input_texture.
	ID3D12Resource *ck_fill_tex;
	uint32_t ck_fill_w, ck_fill_h;
	D3D12_RESOURCE_STATES ck_fill_state;
	// Post-weave strip source — copy of the back buffer for the strip pass
	// to sample.
	ID3D12Resource *ck_strip_tex;
	uint32_t ck_strip_w, ck_strip_h;
	uint32_t ck_strip_fmt; //!< DXGI format of ck_strip_tex — MUST match the back-buffer for CopyResource (#610: texture apps use BGRA, not RGBA).
	D3D12_RESOURCE_STATES ck_strip_state;
	//! @}

	//! @name Compose-under-bg transparency support (preferred over chroma-key)
	//!
	//! Reuses ck_fill_tex/ck_rtv_heap_fill as the intermediate target. Own
	//! root sig + PSO + descriptor heap (2 SRV slots: atlas at 0, bg at 1).
	//! @{
	ID3D12CommandQueue *command_queue;   //!< Saved from factory; needed for fence Wait().
	struct leia_bg_capture *bg_capture;  //!< Owned; NULL → fall back to chroma-key.
	bool bg_compose_enabled;             //!< Active when the new path is in use.
	ID3D12Resource *bg_shared_tex;       //!< Opened from bg_capture's shared NT handle.
	ID3D12Fence *bg_fence;               //!< Opened from bg_capture's shared fence handle.
	ID3D12RootSignature *compose_root_sig;
	ID3D12PipelineState *compose_pso;
	ID3D12DescriptorHeap *compose_srv_heap; //!< Shader-visible, 2 entries (atlas, bg).
	UINT cbv_srv_desc_size;              //!< Cached for offset arithmetic in compose heap.

	// Post-weave alpha-gate (replaces ck_strip when compose is active).
	// Reuses compose_root_sig (12 32-bit constants, 3-SRV table, linear sampler).
	ID3D12PipelineState *alpha_gate_pso;
	ID3D12DescriptorHeap *alpha_gate_srv_heap; //!< Shader-visible, 3 entries (backbuffer, atlas, backdrop).

	//! #491 part 3 — the runtime's flattened 2D-under backdrop for the next
	//! process_atlas (set via set_background_2d; same D3D12 device as the
	//! compositor → the runtime's ID3D12Resource is sampled directly via an SRV
	//! we create into heap slot 2). The compose pass composites
	//! `backdrop over captured-desktop`; the alpha-gate preserves it over the
	//! live desktop. NULL ⟹ no backdrop (desktop-only).
	ID3D12Resource *backdrop_resource; //!< NOT owned (compositor-owned).
	uint32_t backdrop_w, backdrop_h;

	//! #68 — set by set_shared_texture_present (compositor's has_shared_texture).
	//! Skips the compose bg-UV remap for a self-presenting texture-zones app.
	bool shared_texture_present;
	//! @}

	//! @name #224 / ADR-027 local 2D/3D zones — 1×1 leg (D3D12 port of D3D11)
	//! The runtime publishes the XR_EXT_local_3d_zone wish mask per frame; on this
	//! single-zone panel the OR-collapse is "any non-zero mask pixel ⟹ this client
	//! hints 3D", driven onto the per-client SR lens hint
	//! (leiasr_d3d12_request_display_mode). Content is evaluated once per mask
	//! GENERATION (the publish seq bumps only on submit/re-raster). The mask is a
	//! PHYSICAL lens signal ONLY (ADR-028) — it is NEVER sampled to gate or
	//! composite content; the zone path deliberately does NOT touch view_count.
	//!
	//! Unlike D3D11 (which gets an immediate context + SRV and reads back via
	//! ctx->Map), the D3D12 publish slot hands us a bare ID3D12Resource* with no
	//! context — so the verdict needs an OWNED readback pipeline (own allocator +
	//! list + READBACK buffer + fence) run on @ref command_queue. It is
	//! edge-triggered (once per generation), so the synchronous fence wait is
	//! cheap.
	//! @{
	uint64_t zone_eval_seq;  //!< Generation last content-evaluated.
	bool zone_eval_valid;    //!< zone_eval_seq/zone_want_3d are valid.
	bool zone_want_3d;       //!< Verdict for zone_eval_seq (any non-zero).
	bool zone_hint_3d;       //!< Lens-hint state the zone path last set.
	bool zone_active;        //!< A publish is live (not cleared). Also gates the
	                         //!< #68 bg-UV remap-skip in compose_run_pre_weave.
	ID3D12CommandAllocator *zone_cmd_alloc;    //!< Owned readback allocator.
	ID3D12GraphicsCommandList *zone_cmd_list;  //!< Owned readback list.
	ID3D12Resource *zone_readback;             //!< HEAP_TYPE_READBACK buffer (mask-sized).
	uint32_t zone_readback_w, zone_readback_h; //!< Dims the readback buffer was sized for.
	uint32_t zone_readback_pitch;              //!< Aligned row pitch of the readback footprint.
	ID3D12Fence *zone_fence;                   //!< Owned; gates the synchronous readback.
	HANDLE zone_fence_event;                   //!< Owned; CloseHandle in destroy.
	uint64_t zone_fence_value;                 //!< Monotonic fence signal counter.
	//! @}

	//! One-shot guard for a lost device. The per-frame resource creates (the
	//! alpha-gate's ck_ensure_strip_source, the chroma-key ck_ensure_fill_target)
	//! would otherwise log an identical error EVERY frame once the D3D12 device
	//! is removed (e.g. a GPU fault / TDR upstream), burying the real fault.
	//! Set true the first time a removal is reported (with GetDeviceRemovedReason).
	bool device_removed_reported;
};

static inline struct leia_display_processor_d3d12_impl *
leia_dp_d3d12(struct xrt_display_processor_d3d12 *xdp)
{
	return (struct leia_display_processor_d3d12_impl *)xdp;
}

// On a failed D3D12 resource create, detect a lost device and report the real
// fault (GetDeviceRemovedReason) ONCE. Returns true when the device is removed —
// the caller should then SKIP its own per-frame error log, since the device is
// dead and every subsequent create fails identically (DXGI_ERROR_DEVICE_REMOVED
// on the symptom, not the cause). Returns false for an ordinary device-alive
// failure, where the caller logs its specific error normally.
static bool
leia_dp_d3d12_device_removed(struct leia_display_processor_d3d12_impl *ldp)
{
	if (ldp->device == nullptr) {
		return false;
	}
	HRESULT reason = ldp->device->GetDeviceRemovedReason();
	if (reason == S_OK) {
		return false; // device still alive — an ordinary create failure.
	}
	if (!ldp->device_removed_reported) {
		ldp->device_removed_reported = true;
		U_LOG_E("Leia D3D12 DP: device removed — GetDeviceRemovedReason=0x%08x. "
		        "Suppressing further per-frame resource-create errors.",
		        (unsigned)reason);
	}
	return true;
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
ck_should_run(struct leia_display_processor_d3d12_impl *ldp)
{
	return ldp->ck_enabled && ldp->ck_color != 0;
}

// Compile a single shader source via D3DCompile. Returns owning blob (caller releases).
static ID3DBlob *
ck_compile_shader(const char *src, const char *entry, const char *target)
{
	ID3DBlob *blob = nullptr;
	ID3DBlob *err = nullptr;
	HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
	                        entry, target, 0, 0, &blob, &err);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: ck shader compile (%s) failed: 0x%08x %s",
		        target, (unsigned)hr,
		        err ? (const char *)err->GetBufferPointer() : "");
		if (err) err->Release();
		return nullptr;
	}
	if (err) err->Release();
	return blob;
}

// Build the shared root signature: 32-bit constants for chroma_rgb (b0) +
// SRV descriptor table (t0) + static point sampler (s0).
static bool
ck_build_root_sig(struct leia_display_processor_d3d12_impl *ldp)
{
	D3D12_DESCRIPTOR_RANGE srv_range = {};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = 1;
	srv_range.BaseShaderRegister = 0;
	srv_range.OffsetInDescriptorsFromTableStart = 0;

	D3D12_ROOT_PARAMETER root_params[2] = {};
	root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	root_params[0].Constants.ShaderRegister = 0;
	root_params[0].Constants.RegisterSpace = 0;
	root_params[0].Constants.Num32BitValues = 4;
	root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_params[1].DescriptorTable.NumDescriptorRanges = 1;
	root_params[1].DescriptorTable.pDescriptorRanges = &srv_range;
	root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	// Point filter — strip's RGB exact-equality test would smear with linear.
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
	rs_desc.NumParameters = 2;
	rs_desc.pParameters = root_params;
	rs_desc.NumStaticSamplers = 1;
	rs_desc.pStaticSamplers = &sampler;
	rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

	ID3DBlob *rs_blob = nullptr;
	ID3DBlob *err = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                          &rs_blob, &err);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: ck root sig serialize failed: 0x%08x %s",
		        (unsigned)hr, err ? (const char *)err->GetBufferPointer() : "");
		if (err) err->Release();
		return false;
	}
	if (err) err->Release();

	hr = ldp->device->CreateRootSignature(0, rs_blob->GetBufferPointer(),
	                                       rs_blob->GetBufferSize(),
	                                       __uuidof(ID3D12RootSignature),
	                                       reinterpret_cast<void **>(&ldp->ck_root_sig));
	rs_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: ck root sig create failed: 0x%08x", (unsigned)hr);
		return false;
	}
	return true;
}

// Build a chroma-key PSO for the given pixel-shader source. The RTV format
// must match the render target the pass writes to: the fill pass renders to
// ck_fill_tex (always R8G8B8A8_UNORM), but the strip pass renders to the
// back buffer, which is BGRA for _texture apps (#610) — pass the back-buffer
// (swap-chain) format for the strip PSO.
static bool
ck_build_pso(struct leia_display_processor_d3d12_impl *ldp,
             const char *ps_source,
             DXGI_FORMAT rtv_format,
             ID3D12PipelineState **out_pso)
{
	ID3DBlob *vs_blob = ck_compile_shader(ck_vs_source, "main", "vs_5_0");
	if (vs_blob == nullptr) return false;
	ID3DBlob *ps_blob = ck_compile_shader(ps_source, "main", "ps_5_0");
	if (ps_blob == nullptr) {
		vs_blob->Release();
		return false;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
	pso_desc.pRootSignature = ldp->ck_root_sig;
	pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
	pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
	pso_desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
	pso_desc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
	pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pso_desc.SampleMask = UINT_MAX;
	pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso_desc.RasterizerState.DepthClipEnable = TRUE;
	pso_desc.DepthStencilState.DepthEnable = FALSE;
	pso_desc.DepthStencilState.StencilEnable = FALSE;
	pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso_desc.NumRenderTargets = 1;
	pso_desc.RTVFormats[0] = rtv_format;
	pso_desc.SampleDesc.Count = 1;

	HRESULT hr = ldp->device->CreateGraphicsPipelineState(
	    &pso_desc, __uuidof(ID3D12PipelineState),
	    reinterpret_cast<void **>(out_pso));
	vs_blob->Release();
	ps_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: ck PSO create failed: 0x%08x", (unsigned)hr);
		return false;
	}
	return true;
}

// Lazy init root sig + 2 PSOs + 2 shader-visible SRV heaps + 1 RTV heap.
static bool
ck_init_pipeline(struct leia_display_processor_d3d12_impl *ldp)
{
	if (ldp->ck_fill_pso != nullptr && ldp->ck_strip_pso != nullptr) {
		return true;
	}
	if (ldp->device == nullptr) {
		return false;
	}

	if (ldp->ck_root_sig == nullptr && !ck_build_root_sig(ldp)) {
		return false;
	}
	// Fill PSO renders to ck_fill_tex (always RGBA); strip PSO renders to the
	// back buffer (BGRA for _texture apps — #610). Mirror the alpha-gate PSO:
	// use the swap-chain format the compositor set via set_output_format.
	DXGI_FORMAT strip_rtv_fmt = ldp->blit_output_format != DXGI_FORMAT_UNKNOWN
	                                ? ldp->blit_output_format
	                                : DXGI_FORMAT_R8G8B8A8_UNORM;
	if (ldp->ck_fill_pso == nullptr &&
	    !ck_build_pso(ldp, ck_fill_ps_source, DXGI_FORMAT_R8G8B8A8_UNORM, &ldp->ck_fill_pso)) {
		return false;
	}
	if (ldp->ck_strip_pso == nullptr &&
	    !ck_build_pso(ldp, ck_strip_ps_source, strip_rtv_fmt, &ldp->ck_strip_pso)) {
		return false;
	}

	if (ldp->ck_srv_heap_fill == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = 1;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		HRESULT hr = ldp->device->CreateDescriptorHeap(
		    &hd, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&ldp->ck_srv_heap_fill));
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: ck fill SRV heap create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}
	if (ldp->ck_srv_heap_strip == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = 1;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		HRESULT hr = ldp->device->CreateDescriptorHeap(
		    &hd, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&ldp->ck_srv_heap_strip));
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: ck strip SRV heap create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}
	if (ldp->ck_rtv_heap_fill == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 1;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		HRESULT hr = ldp->device->CreateDescriptorHeap(
		    &hd, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&ldp->ck_rtv_heap_fill));
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: ck fill RTV heap create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}

	U_LOG_W("Leia D3D12 DP: chroma-key pipeline ready (key=0x%08X)", ldp->ck_color);
	return true;
}

// Allocate ck_fill_tex sized to the atlas. State at exit: PIXEL_SHADER_RESOURCE
// (so the weaver, which samples it next, sees it ready). Recreates RTV + SRV
// descriptors on every (re)alloc.
static bool
ck_ensure_fill_target(struct leia_display_processor_d3d12_impl *ldp,
                       uint32_t w, uint32_t h)
{
	if (ldp->ck_fill_tex != nullptr && ldp->ck_fill_w == w && ldp->ck_fill_h == h) {
		return true;
	}
	if (ldp->ck_fill_tex != nullptr) {
		ldp->ck_fill_tex->Release();
		ldp->ck_fill_tex = nullptr;
	}

	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = w;
	td.Height = h;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	HRESULT hr = ldp->device->CreateCommittedResource(
	    &heap_props, D3D12_HEAP_FLAG_NONE, &td,
	    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
	    __uuidof(ID3D12Resource),
	    reinterpret_cast<void **>(&ldp->ck_fill_tex));
	if (FAILED(hr)) {
		if (!leia_dp_d3d12_device_removed(ldp)) {
			U_LOG_E("Leia D3D12 DP: ck fill tex create (%ux%u) failed: 0x%08x",
			        w, h, (unsigned)hr);
		}
		return false;
	}
	ldp->ck_fill_w = w;
	ldp->ck_fill_h = h;
	ldp->ck_fill_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
	rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	ldp->device->CreateRenderTargetView(
	    ldp->ck_fill_tex, &rtv_desc,
	    ldp->ck_rtv_heap_fill->GetCPUDescriptorHandleForHeapStart());
	return true;
}

// Allocate ck_strip_tex sized to the back buffer. State at exit:
// PIXEL_SHADER_RESOURCE.
//
// #610: the strip source is a CopyResource of the back buffer, which requires
// an IDENTICAL format. _handle apps render to an RGBA back buffer, but
// _texture apps' shared texture is BGRA (B8G8R8A8_UNORM, DXGI 87) — a hardcoded
// RGBA strip tex makes CopyResource fail silently → strip tex stays black → the
// alpha-gate's keep-path resamples black, blacking all opaque woven content
// (e.g. the opaque Zone A in a texture+zones frame). Match the actual
// back-buffer format. Sampling stays correct: the SRV maps channels to RGBA in
// the shader regardless of byte order, and we read-then-write the same format.
static bool
ck_ensure_strip_source(struct leia_display_processor_d3d12_impl *ldp,
                        uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	if (ldp->ck_strip_tex != nullptr && ldp->ck_strip_w == w && ldp->ck_strip_h == h &&
	    ldp->ck_strip_fmt == (uint32_t)fmt) {
		return true;
	}
	if (ldp->ck_strip_tex != nullptr) {
		ldp->ck_strip_tex->Release();
		ldp->ck_strip_tex = nullptr;
	}

	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = w;
	td.Height = h;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	td.Flags = D3D12_RESOURCE_FLAG_NONE;

	HRESULT hr = ldp->device->CreateCommittedResource(
	    &heap_props, D3D12_HEAP_FLAG_NONE, &td,
	    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
	    __uuidof(ID3D12Resource),
	    reinterpret_cast<void **>(&ldp->ck_strip_tex));
	if (FAILED(hr)) {
		if (!leia_dp_d3d12_device_removed(ldp)) {
			U_LOG_E("Leia D3D12 DP: ck strip tex create (%ux%u) failed: 0x%08x",
			        w, h, (unsigned)hr);
		}
		return false;
	}
	ldp->ck_strip_w = w;
	ldp->ck_strip_h = h;
	ldp->ck_strip_fmt = (uint32_t)fmt;
	ldp->ck_strip_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = fmt;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv_desc.Texture2D.MipLevels = 1;
	ldp->device->CreateShaderResourceView(
	    ldp->ck_strip_tex, &srv_desc,
	    ldp->ck_srv_heap_strip->GetCPUDescriptorHandleForHeapStart());
	return true;
}

// Pack ck_color (0x00BBGGRR) → 4 root constants (R, G, B, pad).
static void
ck_root_constants(struct leia_display_processor_d3d12_impl *ldp, float out[4])
{
	uint32_t k = ldp->ck_color;
	out[0] = ((k >>  0) & 0xFF) / 255.0f; // R
	out[1] = ((k >>  8) & 0xFF) / 255.0f; // G
	out[2] = ((k >> 16) & 0xFF) / 255.0f; // B
	out[3] = 0.0f;
}

/*
 * Pre-weave fill: read RGBA atlas, write opaque RGB to ck_fill_tex with
 * alpha=0 regions filled by chroma_rgb. Returns the resource the weaver
 * should sample (ck_fill_tex on success, original atlas on fallback).
 *
 * The caller (process_atlas) has already set viewport+scissor and bound
 * the back-buffer RTV. This pass switches the RT binding to ck_fill_tex,
 * draws, then restores the caller's RTV via prev_rtv so the subsequent
 * weaver call writes to the right back buffer (D3D11's analog uses
 * OMGetRenderTargets to save+restore; D3D12 has no Getter on command
 * lists so prev_rtv is passed in explicitly).
 */
static ID3D12Resource *
ck_run_pre_weave_fill(struct leia_display_processor_d3d12_impl *ldp,
                       ID3D12GraphicsCommandList *cmd,
                       ID3D12Resource *atlas_resource,
                       uint32_t atlas_w, uint32_t atlas_h,
                       DXGI_FORMAT atlas_format,
                       D3D12_CPU_DESCRIPTOR_HANDLE prev_rtv)
{
	if (!ck_init_pipeline(ldp) || !ck_ensure_fill_target(ldp, atlas_w, atlas_h)) {
		return atlas_resource;
	}

	// Create SRV on the atlas resource in the fill heap (slot 0).
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = atlas_format;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv_desc.Texture2D.MipLevels = 1;
	ldp->device->CreateShaderResourceView(
	    atlas_resource, &srv_desc,
	    ldp->ck_srv_heap_fill->GetCPUDescriptorHandleForHeapStart());

	// fill_tex PIXEL_SHADER_RESOURCE → RENDER_TARGET
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = ldp->ck_fill_tex;
	barrier.Transition.StateBefore = ldp->ck_fill_state;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->ResourceBarrier(1, &barrier);
	ldp->ck_fill_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

	// Bind PSO + root sig + descriptor heap + chroma constants.
	cmd->SetPipelineState(ldp->ck_fill_pso);
	cmd->SetGraphicsRootSignature(ldp->ck_root_sig);
	ID3D12DescriptorHeap *heaps[] = {ldp->ck_srv_heap_fill};
	cmd->SetDescriptorHeaps(1, heaps);
	float root_consts[4];
	ck_root_constants(ldp, root_consts);
	cmd->SetGraphicsRoot32BitConstants(0, 4, root_consts, 0);
	cmd->SetGraphicsRootDescriptorTable(
	    1, ldp->ck_srv_heap_fill->GetGPUDescriptorHandleForHeapStart());

	D3D12_VIEWPORT vp = {0.0f, 0.0f, (float)atlas_w, (float)atlas_h, 0.0f, 1.0f};
	D3D12_RECT scissor = {0, 0, (LONG)atlas_w, (LONG)atlas_h};
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &scissor);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv =
	    ldp->ck_rtv_heap_fill->GetCPUDescriptorHandleForHeapStart();
	cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 0, nullptr);
	cmd->IASetIndexBuffer(nullptr);
	cmd->DrawInstanced(3, 1, 0, 0);

	// fill_tex RENDER_TARGET → PIXEL_SHADER_RESOURCE (so weaver can sample).
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(1, &barrier);
	ldp->ck_fill_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// Restore the back-buffer RTV so the subsequent weave writes to the
	// right target. D3D11's analog uses OMGetRenderTargets to save/restore;
	// D3D12 has no Get on the command list so the caller passes prev_rtv in.
	cmd->OMSetRenderTargets(1, &prev_rtv, FALSE, nullptr);

	return ldp->ck_fill_tex;
}

/*
 * Post-weave strip: copy back buffer → ck_strip_tex, then sample strip_tex
 * back to back-buffer RTV with alpha=0 where RGB matches chroma_rgb. Caller
 * passes back_buffer in RENDER_TARGET state; we transition through
 * COPY_SOURCE and back to RENDER_TARGET.
 */
static void
ck_run_post_weave_strip(struct leia_display_processor_d3d12_impl *ldp,
                         ID3D12GraphicsCommandList *cmd,
                         ID3D12Resource *back_buffer,
                         D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv,
                         uint32_t bb_w, uint32_t bb_h)
{
	// #610: ck_strip_tex must match the back-buffer format (BGRA for _texture
	// apps) or CopyResource fails silently. The strip PSO RTV format is set
	// to the same swap-chain format in ck_init_pipeline.
	DXGI_FORMAT bb_fmt = back_buffer->GetDesc().Format;
	if (!ck_init_pipeline(ldp) || !ck_ensure_strip_source(ldp, bb_w, bb_h, bb_fmt)) {
		return;
	}

	// back_buffer RENDER_TARGET → COPY_SOURCE; strip_tex
	// PIXEL_SHADER_RESOURCE → COPY_DEST.
	D3D12_RESOURCE_BARRIER barriers[2] = {};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition.pResource = back_buffer;
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition.pResource = ldp->ck_strip_tex;
	barriers[1].Transition.StateBefore = ldp->ck_strip_state;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->ResourceBarrier(2, barriers);
	ldp->ck_strip_state = D3D12_RESOURCE_STATE_COPY_DEST;

	cmd->CopyResource(ldp->ck_strip_tex, back_buffer);

	// back_buffer COPY_SOURCE → RENDER_TARGET; strip_tex COPY_DEST →
	// PIXEL_SHADER_RESOURCE.
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(2, barriers);
	ldp->ck_strip_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// Bind strip PSO + descriptors + chroma constants.
	cmd->SetPipelineState(ldp->ck_strip_pso);
	cmd->SetGraphicsRootSignature(ldp->ck_root_sig);
	ID3D12DescriptorHeap *heaps[] = {ldp->ck_srv_heap_strip};
	cmd->SetDescriptorHeaps(1, heaps);
	float root_consts[4];
	ck_root_constants(ldp, root_consts);
	cmd->SetGraphicsRoot32BitConstants(0, 4, root_consts, 0);
	cmd->SetGraphicsRootDescriptorTable(
	    1, ldp->ck_srv_heap_strip->GetGPUDescriptorHandleForHeapStart());

	D3D12_VIEWPORT vp = {0.0f, 0.0f, (float)bb_w, (float)bb_h, 0.0f, 1.0f};
	D3D12_RECT scissor = {0, 0, (LONG)bb_w, (LONG)bb_h};
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &scissor);

	cmd->OMSetRenderTargets(1, &back_buffer_rtv, FALSE, nullptr);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 0, nullptr);
	cmd->IASetIndexBuffer(nullptr);
	cmd->DrawInstanced(3, 1, 0, 0);
}

static void
ck_release_resources(struct leia_display_processor_d3d12_impl *ldp)
{
	if (ldp->ck_fill_tex)        { ldp->ck_fill_tex->Release();        ldp->ck_fill_tex = nullptr; }
	if (ldp->ck_strip_tex)       { ldp->ck_strip_tex->Release();       ldp->ck_strip_tex = nullptr; }
	if (ldp->ck_rtv_heap_fill)   { ldp->ck_rtv_heap_fill->Release();   ldp->ck_rtv_heap_fill = nullptr; }
	if (ldp->ck_srv_heap_strip)  { ldp->ck_srv_heap_strip->Release();  ldp->ck_srv_heap_strip = nullptr; }
	if (ldp->ck_srv_heap_fill)   { ldp->ck_srv_heap_fill->Release();   ldp->ck_srv_heap_fill = nullptr; }
	if (ldp->ck_strip_pso)       { ldp->ck_strip_pso->Release();       ldp->ck_strip_pso = nullptr; }
	if (ldp->ck_fill_pso)        { ldp->ck_fill_pso->Release();        ldp->ck_fill_pso = nullptr; }
	if (ldp->ck_root_sig)        { ldp->ck_root_sig->Release();        ldp->ck_root_sig = nullptr; }
}


/*
 *
 * Compose-under-bg pipeline (preferred path when WGC is available).
 * Reuses ck_fill_tex / ck_rtv_heap_fill as the intermediate render target.
 *
 */

static bool
compose_should_run(struct leia_display_processor_d3d12_impl *ldp)
{
	return ldp->bg_compose_enabled && ldp->bg_capture != nullptr && ldp->bg_shared_tex != nullptr;
}

// Root signature: 12 32-bit constants (bg_uv_origin xy + bg_uv_extent xy +
// tile_count xy + has_backdrop + pad + chroma_rgb + pad = 48 bytes) at b0 +
// 3-SRV descriptor table (t0,t1,t2 — #491 part 3 added the backdrop at t2) +
// static linear sampler at s0. Shared by the compose + alpha-gate PSOs.
static bool
compose_build_root_sig(struct leia_display_processor_d3d12_impl *ldp)
{
	D3D12_DESCRIPTOR_RANGE srv_range = {};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = 3;
	srv_range.BaseShaderRegister = 0;
	srv_range.OffsetInDescriptorsFromTableStart = 0;

	D3D12_ROOT_PARAMETER params[2] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 12;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &srv_range;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters = 2;
	desc.pParameters = params;
	desc.NumStaticSamplers = 1;
	desc.pStaticSamplers = &sampler;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

	ID3DBlob *blob = nullptr;
	ID3DBlob *err = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: compose root sig serialize failed: 0x%08x %s",
		        (unsigned)hr, err ? (const char *)err->GetBufferPointer() : "");
		if (err) err->Release();
		return false;
	}
	if (err) err->Release();
	hr = ldp->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                       __uuidof(ID3D12RootSignature),
	                                       reinterpret_cast<void **>(&ldp->compose_root_sig));
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: compose root sig create failed: 0x%08x", (unsigned)hr);
		return false;
	}
	return true;
}

static bool
compose_init_pipeline(struct leia_display_processor_d3d12_impl *ldp)
{
	if (ldp->compose_pso != nullptr && ldp->compose_srv_heap != nullptr) {
		return true;
	}
	if (ldp->device == nullptr) {
		return false;
	}
	if (ldp->compose_root_sig == nullptr && !compose_build_root_sig(ldp)) {
		return false;
	}
	if (ldp->compose_pso == nullptr) {
		ID3DBlob *vs_blob = ck_compile_shader(ck_vs_source, "main", "vs_5_0");
		if (vs_blob == nullptr) return false;
		ID3DBlob *ps_blob = ck_compile_shader(compose_under_bg_ps_source, "main", "ps_5_0");
		if (ps_blob == nullptr) { vs_blob->Release(); return false; }

		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
		pd.pRootSignature = ldp->compose_root_sig;
		pd.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
		pd.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
		pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pd.SampleMask = UINT_MAX;
		pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pd.RasterizerState.DepthClipEnable = TRUE;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 1;
		pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		pd.SampleDesc.Count = 1;
		HRESULT hr = ldp->device->CreateGraphicsPipelineState(
		    &pd, __uuidof(ID3D12PipelineState),
		    reinterpret_cast<void **>(&ldp->compose_pso));
		vs_blob->Release();
		ps_blob->Release();
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: compose PSO create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}
	// We reuse ck_fill_tex / ck_rtv_heap_fill as the intermediate target. In
	// the chroma-key path those are created inside ck_init_pipeline; in the
	// compose path we never call ck_init_pipeline, so the RTV heap must be
	// created here. (Without this, ck_ensure_fill_target null-derefs
	// ldp->ck_rtv_heap_fill on the first compose frame.)
	if (ldp->ck_rtv_heap_fill == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 1;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		HRESULT hr = ldp->device->CreateDescriptorHeap(
		    &hd, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&ldp->ck_rtv_heap_fill));
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: compose fill RTV heap create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}
	// Same reason: ck_ensure_strip_source writes the strip SRV into
	// ck_srv_heap_strip. The alpha-gate path uses its own srv heap but
	// ck_ensure_strip_source still writes the legacy heap. Create it here
	// so the ck path doesn't null-deref on the compose-mode first frame.
	if (ldp->ck_srv_heap_strip == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = 1;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		HRESULT hr = ldp->device->CreateDescriptorHeap(
		    &hd, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&ldp->ck_srv_heap_strip));
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: compose strip SRV heap create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}

	if (ldp->compose_srv_heap == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = 3; // #491 part 3 — slot 2 = backdrop (t2)
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		HRESULT hr = ldp->device->CreateDescriptorHeap(
		    &hd, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&ldp->compose_srv_heap));
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: compose SRV heap create failed: 0x%08x", (unsigned)hr);
			return false;
		}
		ldp->cbv_srv_desc_size = ldp->device->GetDescriptorHandleIncrementSize(
		    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		// Slot 1: bg SRV (stable across frames — bg_shared_tex doesn't change).
		D3D12_SHADER_RESOURCE_VIEW_DESC bg_srv = {};
		bg_srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		bg_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		bg_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		bg_srv.Texture2D.MipLevels = 1;
		D3D12_CPU_DESCRIPTOR_HANDLE bg_cpu =
		    ldp->compose_srv_heap->GetCPUDescriptorHandleForHeapStart();
		bg_cpu.ptr += ldp->cbv_srv_desc_size;
		ldp->device->CreateShaderResourceView(ldp->bg_shared_tex, &bg_srv, bg_cpu);
	}

	// Alpha-gate PSO (reuses compose_root_sig). Same fullscreen-tri VS,
	// alpha_gate_ps_source for the PS. Different render-target — back buffer
	// can be either DXGI_FORMAT_R8G8B8A8_UNORM or _B8G8R8A8_UNORM; we build
	// the PSO for the swap-chain format the compositor set via set_output_format.
	if (ldp->alpha_gate_pso == nullptr) {
		ID3DBlob *vs_blob = ck_compile_shader(ck_vs_source, "main", "vs_5_0");
		if (vs_blob == nullptr) return false;
		ID3DBlob *ps_blob = ck_compile_shader(alpha_gate_ps_source, "main", "ps_5_0");
		if (ps_blob == nullptr) { vs_blob->Release(); return false; }

		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
		pd.pRootSignature = ldp->compose_root_sig;
		pd.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
		pd.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
		pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pd.SampleMask = UINT_MAX;
		pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pd.RasterizerState.DepthClipEnable = TRUE;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 1;
		// Use the swap-chain format the strip pipeline uses (ck_strip_target_format
		// equivalent — set via set_output_format → blit_output_format).
		pd.RTVFormats[0] = ldp->blit_output_format != DXGI_FORMAT_UNKNOWN
		                       ? ldp->blit_output_format
		                       : DXGI_FORMAT_R8G8B8A8_UNORM;
		pd.SampleDesc.Count = 1;
		HRESULT hr = ldp->device->CreateGraphicsPipelineState(
		    &pd, __uuidof(ID3D12PipelineState),
		    reinterpret_cast<void **>(&ldp->alpha_gate_pso));
		vs_blob->Release();
		ps_blob->Release();
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: alpha-gate PSO create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}
	if (ldp->alpha_gate_srv_heap == nullptr) {
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = 3; // #491 part 3 — slot 2 = backdrop (t2)
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		HRESULT hr = ldp->device->CreateDescriptorHeap(
		    &hd, __uuidof(ID3D12DescriptorHeap),
		    reinterpret_cast<void **>(&ldp->alpha_gate_srv_heap));
		if (FAILED(hr)) {
			U_LOG_E("Leia D3D12 DP: alpha-gate SRV heap create failed: 0x%08x", (unsigned)hr);
			return false;
		}
	}

	U_LOG_W("Leia D3D12 DP: compose-under-bg + alpha-gate pipelines ready");
	return true;
}

/*
 * Pre-weave compose-under-bg. Captures the latest desktop frame, composes
 * it under the atlas per tile, writes opaque RGB into ck_fill_tex. Returns
 * ck_fill_tex on success (weaver samples it), original atlas on fallback.
 *
 * Caller passes prev_rtv (current back-buffer RTV) to restore after we
 * change the binding to ck_fill_tex.
 */
static ID3D12Resource *
compose_run_pre_weave(struct leia_display_processor_d3d12_impl *ldp,
                      ID3D12GraphicsCommandList *cmd,
                      ID3D12Resource *atlas_resource,
                      uint32_t atlas_w, uint32_t atlas_h,
                      DXGI_FORMAT atlas_format,
                      uint32_t tile_columns, uint32_t tile_rows,
                      D3D12_CPU_DESCRIPTOR_HANDLE prev_rtv,
                      uint32_t target_width, uint32_t target_height,
                      int32_t canvas_offset_x, int32_t canvas_offset_y,
                      uint32_t canvas_width, uint32_t canvas_height)
{
	if (!compose_init_pipeline(ldp) || !ck_ensure_fill_target(ldp, atlas_w, atlas_h)) {
		return atlas_resource;
	}

	float bg_origin[2] = {0.0f, 0.0f};
	float bg_extent[2] = {0.0f, 0.0f};
	uint64_t fence_value = 0;
	bool have_bg = leia_bg_capture_poll(ldp->bg_capture, bg_origin, bg_extent, &fence_value);
	if (!have_bg) {
		return atlas_resource;
	}

	// (#131) Canvas sub-rect: the atlas is full-size, but the weaver downscales
	// it into the sub-rect viewport at weave time — so the desktop we composite
	// under it here gets downscaled along with the foreground. Remap the
	// background window UVs to the sub-rect's fraction of the window, so the
	// captured desktop lands 1:1 behind the sub-rect after the weave downscale
	// (instead of the whole-window desktop shrunk into the sub-rect). No sub-rect
	// (canvas_w/h == 0) leaves the full-window mapping unchanged.
	// NOTE: offsets assume bg UVs are top-left origin (bg_origin = window TL);
	// if the desktop appears vertically misplaced on device, negate the fy term.
	//
	// #68: a `_texture` app self-presents its shared texture's canvas to its own
	// window — for a TEXTURE-ZONES frame the canvas IS the whole window, so the
	// window does NOT show the whole panel-sized target and this canvas/target
	// remap would magnify the captured desktop (#68). Gated on
	// `shared_texture_present && zone_active` (mirrors D3D11): the zones gate is
	// what keeps the remap for a texture-SURROUND app (no zone mask published →
	// zone_active false → window == full target, remap NEEDED), and skips it only
	// for a self-presenting texture-ZONES app where the canvas fills the window.
	bool skip_bg_remap = ldp->shared_texture_present && ldp->zone_active;
	if (!skip_bg_remap && canvas_width > 0 && canvas_height > 0 &&
	    target_width > 0 && target_height > 0) {
		float fx = (float)canvas_offset_x / (float)target_width;
		float fy = (float)canvas_offset_y / (float)target_height;
		float fw = (float)canvas_width  / (float)target_width;
		float fh = (float)canvas_height / (float)target_height;
		bg_origin[0] += fx * bg_extent[0];
		bg_origin[1] += fy * bg_extent[1];
		bg_extent[0] *= fw;
		bg_extent[1] *= fh;
	}

	// Order the consumer's GPU work after the producer's signal. Queue Wait
	// gates all subsequently-executed cmd lists on this queue.
	if (ldp->bg_fence != nullptr && ldp->command_queue != nullptr && fence_value > 0) {
		ldp->command_queue->Wait(ldp->bg_fence, fence_value);
	}

	// Slot 0: atlas SRV (refreshed each frame — atlas resource may change).
	D3D12_SHADER_RESOURCE_VIEW_DESC atlas_srv = {};
	atlas_srv.Format = atlas_format;
	atlas_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	atlas_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	atlas_srv.Texture2D.MipLevels = 1;
	ldp->device->CreateShaderResourceView(
	    atlas_resource, &atlas_srv,
	    ldp->compose_srv_heap->GetCPUDescriptorHandleForHeapStart());

	// #491 part 3 — slot 2: the 2D-under backdrop (R8G8B8A8_UNORM, premultiplied).
	// Same device as the compositor → the runtime's resource is sampled directly.
	// When absent, a dummy SRV (bg) keeps the descriptor valid; has_backdrop gates
	// the sample so the dummy is never read.
	{
		ID3D12Resource *bd_res =
		    (ldp->backdrop_resource != nullptr) ? ldp->backdrop_resource : ldp->bg_shared_tex;
		D3D12_SHADER_RESOURCE_VIEW_DESC bd_srv = {};
		bd_srv.Format = (ldp->backdrop_resource != nullptr) ? DXGI_FORMAT_R8G8B8A8_UNORM
		                                                    : DXGI_FORMAT_B8G8R8A8_UNORM;
		bd_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		bd_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		bd_srv.Texture2D.MipLevels = 1;
		D3D12_CPU_DESCRIPTOR_HANDLE bd_cpu =
		    ldp->compose_srv_heap->GetCPUDescriptorHandleForHeapStart();
		bd_cpu.ptr += 2 * ldp->cbv_srv_desc_size;
		ldp->device->CreateShaderResourceView(bd_res, &bd_srv, bd_cpu);
	}

	// ck_fill_tex PIXEL_SHADER_RESOURCE → RENDER_TARGET
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = ldp->ck_fill_tex;
	barrier.Transition.StateBefore = ldp->ck_fill_state;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->ResourceBarrier(1, &barrier);
	ldp->ck_fill_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

	cmd->SetPipelineState(ldp->compose_pso);
	cmd->SetGraphicsRootSignature(ldp->compose_root_sig);
	ID3D12DescriptorHeap *heaps[] = {ldp->compose_srv_heap};
	cmd->SetDescriptorHeaps(1, heaps);
	uint32_t consts[12];
	memcpy(&consts[0], &bg_origin[0], sizeof(float));
	memcpy(&consts[1], &bg_origin[1], sizeof(float));
	memcpy(&consts[2], &bg_extent[0], sizeof(float));
	memcpy(&consts[3], &bg_extent[1], sizeof(float));
	consts[4] = tile_columns;
	consts[5] = tile_rows;
	consts[6] = (ldp->backdrop_resource != nullptr) ? 1u : 0u; // #491 part 3 has_backdrop
	consts[7] = 0;
	if (ldp->backdrop_resource != nullptr) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("Leia D3D12 DP #491 part3: compositing 2D-under backdrop %ux%u over desktop",
			        ldp->backdrop_w, ldp->backdrop_h);
		}
	}
	// chroma_rgb (same key as the strip pass). ck_color stays set in compose
	// mode — see set_chroma_key. Atlas α==0 pixels are emitted as this sentinel
	// so the post-weave strip can rewrite them to α=0 → live desktop via DWM.
	float chroma_r = ((ldp->ck_color >>  0) & 0xFF) / 255.0f;
	float chroma_g = ((ldp->ck_color >>  8) & 0xFF) / 255.0f;
	float chroma_b = ((ldp->ck_color >> 16) & 0xFF) / 255.0f;
	memcpy(&consts[8],  &chroma_r, sizeof(float));
	memcpy(&consts[9],  &chroma_g, sizeof(float));
	memcpy(&consts[10], &chroma_b, sizeof(float));
	consts[11] = 0;
	cmd->SetGraphicsRoot32BitConstants(0, 12, consts, 0);
	cmd->SetGraphicsRootDescriptorTable(
	    1, ldp->compose_srv_heap->GetGPUDescriptorHandleForHeapStart());

	D3D12_VIEWPORT vp = {0.0f, 0.0f, (float)atlas_w, (float)atlas_h, 0.0f, 1.0f};
	D3D12_RECT scissor = {0, 0, (LONG)atlas_w, (LONG)atlas_h};
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &scissor);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv =
	    ldp->ck_rtv_heap_fill->GetCPUDescriptorHandleForHeapStart();
	cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 0, nullptr);
	cmd->IASetIndexBuffer(nullptr);
	cmd->DrawInstanced(3, 1, 0, 0);

	// ck_fill_tex RENDER_TARGET → PIXEL_SHADER_RESOURCE (weaver will sample).
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(1, &barrier);
	ldp->ck_fill_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	cmd->OMSetRenderTargets(1, &prev_rtv, FALSE, nullptr);
	return ldp->ck_fill_tex;
}

/*
 * Post-weave alpha-gate. Transitions back buffer through COPY_SOURCE to
 * fill ck_strip_tex, then samples (ck_strip_tex, atlas) and writes back to
 * the back buffer with per-pixel α derived from the "all views α==0" mask.
 * Replaces ck_run_post_weave_strip when compose-under-bg is the active
 * transparency path — no chroma keying involved.
 */
static void
alpha_gate_run_post_weave(struct leia_display_processor_d3d12_impl *ldp,
                          ID3D12GraphicsCommandList *cmd,
                          ID3D12Resource *back_buffer,
                          D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv,
                          ID3D12Resource *atlas_resource,
                          DXGI_FORMAT atlas_format,
                          uint32_t bb_w, uint32_t bb_h,
                          uint32_t tile_columns, uint32_t tile_rows,
                          uint32_t target_width, uint32_t target_height,
                          int32_t canvas_offset_x, int32_t canvas_offset_y,
                          uint32_t canvas_width, uint32_t canvas_height)
{
	if (atlas_resource == nullptr || ldp->alpha_gate_pso == nullptr) {
		return;
	}
	// #610: ck_strip_tex (the CopyResource of the back buffer) must match the
	// back-buffer format — BGRA for _texture apps, else CopyResource fails
	// silently and the gate resamples black over all opaque woven content.
	DXGI_FORMAT bb_fmt = back_buffer->GetDesc().Format;
	if (!ck_ensure_strip_source(ldp, bb_w, bb_h, bb_fmt)) {
		return;
	}

	// Refresh the alpha-gate SRV heap per-frame: slot 0 = back-buffer copy
	// (ck_strip_tex), slot 1 = atlas. ck_strip_tex doesn't change, but its
	// content does; SRV is stable. Atlas SRV must be (re)created since the
	// resource may change per-frame.
	D3D12_CPU_DESCRIPTOR_HANDLE cpu_base =
	    ldp->alpha_gate_srv_heap->GetCPUDescriptorHandleForHeapStart();
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = (DXGI_FORMAT)ldp->ck_strip_fmt; // #610: matches the back-buffer copy (BGRA for _texture apps)
		sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.Texture2D.MipLevels = 1;
		ldp->device->CreateShaderResourceView(ldp->ck_strip_tex, &sd, cpu_base);
	}
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = atlas_format;
		sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.Texture2D.MipLevels = 1;
		D3D12_CPU_DESCRIPTOR_HANDLE atlas_cpu = cpu_base;
		atlas_cpu.ptr += ldp->cbv_srv_desc_size;
		ldp->device->CreateShaderResourceView(atlas_resource, &sd, atlas_cpu);
	}
	// #491 part 3 — slot 2: the 2D-under backdrop (premultiplied). In
	// atlas-transparent regions the gate emits this over the live desktop instead
	// of punching to full transparency. Dummy = ck_strip_tex keeps slot valid
	// when absent; has_backdrop gates the sample.
	{
		ID3D12Resource *bd_res =
		    (ldp->backdrop_resource != nullptr) ? ldp->backdrop_resource : ldp->ck_strip_tex;
		D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
		// backdrop is RGBA (runtime's flattened 2D); the ck_strip_tex dummy
		// (no-backdrop case) must match its own format — BGRA for _texture
		// apps (#610), else the SRV format mismatches the resource.
		sd.Format = (ldp->backdrop_resource != nullptr) ? DXGI_FORMAT_R8G8B8A8_UNORM
		                                                : (DXGI_FORMAT)ldp->ck_strip_fmt;
		sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.Texture2D.MipLevels = 1;
		D3D12_CPU_DESCRIPTOR_HANDLE bd_cpu = cpu_base;
		bd_cpu.ptr += 2 * ldp->cbv_srv_desc_size;
		ldp->device->CreateShaderResourceView(bd_res, &sd, bd_cpu);
	}

	// back_buffer RENDER_TARGET → COPY_SOURCE; ck_strip_tex → COPY_DEST.
	D3D12_RESOURCE_BARRIER barriers[2] = {};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition.pResource = back_buffer;
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition.pResource = ldp->ck_strip_tex;
	barriers[1].Transition.StateBefore = ldp->ck_strip_state;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->ResourceBarrier(2, barriers);
	ldp->ck_strip_state = D3D12_RESOURCE_STATE_COPY_DEST;

	cmd->CopyResource(ldp->ck_strip_tex, back_buffer);

	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(2, barriers);
	ldp->ck_strip_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	cmd->SetPipelineState(ldp->alpha_gate_pso);
	cmd->SetGraphicsRootSignature(ldp->compose_root_sig);
	ID3D12DescriptorHeap *heaps[] = {ldp->alpha_gate_srv_heap};
	cmd->SetDescriptorHeaps(1, heaps);
	// (#131) Restrict the gate to the canvas sub-rect (where the woven 3D lives)
	// so i.uv maps directly to the atlas tile-local UV — otherwise it samples the
	// atlas at full-window scale and carves the shrunk tiger (cheek/arm wedges).
	// The surround region outside the canvas is filled by the surround blit.
	int32_t  vp_x = 0, vp_y = 0;
	uint32_t vp_w = bb_w, vp_h = bb_h;
	float cu_ox = 0.0f, cu_oy = 0.0f, cu_ex = 1.0f, cu_ey = 1.0f;
	if (canvas_width > 0 && canvas_height > 0 &&
	    target_width > 0 && target_height > 0) {
		vp_x = canvas_offset_x;
		vp_y = canvas_offset_y;
		vp_w = canvas_width;
		vp_h = canvas_height;
		cu_ox = (float)canvas_offset_x / (float)target_width;
		cu_oy = (float)canvas_offset_y / (float)target_height;
		cu_ex = (float)canvas_width  / (float)target_width;
		cu_ey = (float)canvas_height / (float)target_height;
	}

	// Root constants layout matches the cbuffer: tile_count.xy (0,1),
	// has_backdrop (2) + pad (3), canvas_uv_origin.xy (4,5),
	// canvas_uv_extent.xy (6,7). Remaining slots unused.
	uint32_t consts[12] = {tile_columns, tile_rows,
	                       (ldp->backdrop_resource != nullptr) ? 1u : 0u, 0}; // #491 part 3
	memcpy(&consts[4], &cu_ox, sizeof(float));
	memcpy(&consts[5], &cu_oy, sizeof(float));
	memcpy(&consts[6], &cu_ex, sizeof(float));
	memcpy(&consts[7], &cu_ey, sizeof(float));
	cmd->SetGraphicsRoot32BitConstants(0, 12, consts, 0);
	cmd->SetGraphicsRootDescriptorTable(
	    1, ldp->alpha_gate_srv_heap->GetGPUDescriptorHandleForHeapStart());

	D3D12_VIEWPORT vp = {(float)vp_x, (float)vp_y, (float)vp_w, (float)vp_h, 0.0f, 1.0f};
	D3D12_RECT scissor = {vp_x, vp_y, vp_x + (LONG)vp_w, vp_y + (LONG)vp_h};
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &scissor);

	cmd->OMSetRenderTargets(1, &back_buffer_rtv, FALSE, nullptr);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 0, nullptr);
	cmd->IASetIndexBuffer(nullptr);
	cmd->DrawInstanced(3, 1, 0, 0);
}

static void
compose_release_resources(struct leia_display_processor_d3d12_impl *ldp)
{
	if (ldp->alpha_gate_srv_heap) { ldp->alpha_gate_srv_heap->Release(); ldp->alpha_gate_srv_heap = nullptr; }
	if (ldp->alpha_gate_pso)      { ldp->alpha_gate_pso->Release();      ldp->alpha_gate_pso = nullptr; }
	if (ldp->compose_srv_heap)    { ldp->compose_srv_heap->Release();    ldp->compose_srv_heap = nullptr; }
	if (ldp->compose_pso)         { ldp->compose_pso->Release();         ldp->compose_pso = nullptr; }
	if (ldp->compose_root_sig)    { ldp->compose_root_sig->Release();    ldp->compose_root_sig = nullptr; }
	if (ldp->bg_fence)            { ldp->bg_fence->Release();            ldp->bg_fence = nullptr; }
	if (ldp->bg_shared_tex)       { ldp->bg_shared_tex->Release();       ldp->bg_shared_tex = nullptr; }
	if (ldp->bg_capture)          { leia_bg_capture_destroy(ldp->bg_capture); ldp->bg_capture = nullptr; }
	ldp->bg_compose_enabled = false;
}


/*
 *
 * xrt_display_processor_d3d12 interface methods.
 *
 */

static void
leia_dp_d3d12_process_atlas(struct xrt_display_processor_d3d12 *xdp,
                             void *d3d12_command_list,
                             void *atlas_texture_resource,
                             uint64_t atlas_srv_gpu_handle,
                             uint64_t target_rtv_cpu_handle,
                             void *target_resource,
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
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);

	// ADR-021: `format` is the real atlas DXGI format (unchanged). This D3D12
	// variant declares no color capability and no set_atlas_encoding (Model-A
	// passthrough only); wiring EITHER + the weaver sRGB control here is a
	// tracked follow-up.

	// runtime#542: atlas processing follows the CONTENT, not the lens. The
	// runtime hands us the grid it packed; a multi-view atlas weaves, a
	// single-view atlas flat-blits — regardless of the hardware state set
	// via request_display_mode. view_count also feeds the eye-position
	// centering below.
	ldp->view_count = (tile_columns * tile_rows > 1) ? tile_columns * tile_rows : 1;

	// Compute effective viewport: canvas sub-rect when set, else full target.
	// The SR SDK weaver uses viewport offset in its phase calculation:
	//   xOffset = window_WeavingX + vpX
	//   yOffset = window_WeavingY + vpY
	int32_t vp_x = 0;
	int32_t vp_y = 0;
	uint32_t vp_w = target_width;
	uint32_t vp_h = target_height;
	if (canvas_width > 0 && canvas_height > 0) {
		vp_x = canvas_offset_x;
		vp_y = canvas_offset_y;
		vp_w = canvas_width;
		vp_h = canvas_height;
	}

	// 2D mode: passthrough stretch-blit (first tile fills target)
	if (ldp->view_count == 1) {
		if (ldp->blit_pso == NULL || ldp->blit_root_sig == NULL ||
		    ldp->blit_srv_heap == NULL || atlas_texture_resource == NULL) {
			return;
		}

		ID3D12GraphicsCommandList *cmd = static_cast<ID3D12GraphicsCommandList *>(d3d12_command_list);
		ID3D12Resource *original_atlas_res = static_cast<ID3D12Resource *>(atlas_texture_resource);
		ID3D12Resource *atlas_res = original_atlas_res;

		// Compose-under-bg: pre-composite captured desktop under the atlas,
		// then stretch-blit the opaque result. Replaces the post-weave strip
		// path for 2D mode when WGC capture is active.
		if (compose_should_run(ldp)) {
			uint32_t atlas_w = tile_columns * view_width;
			uint32_t atlas_h = tile_rows * view_height;
			D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv;
			bb_rtv.ptr = static_cast<SIZE_T>(target_rtv_cpu_handle);
			ID3D12Resource *composed = compose_run_pre_weave(
			    ldp, cmd, atlas_res, atlas_w, atlas_h,
			    static_cast<DXGI_FORMAT>(format), tile_columns, tile_rows, bb_rtv,
			    target_width, target_height,
			    canvas_offset_x, canvas_offset_y, canvas_width, canvas_height);
			if (composed != nullptr) {
				atlas_res = composed;
			}
		}

		// Create SRV for the (possibly pre-composed) atlas resource in our
		// shader-visible heap. The compose path produces an opaque RGBA8 result
		// in ck_fill_tex; the original-atlas format may differ (e.g. SRGB).
		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = compose_should_run(ldp) ? DXGI_FORMAT_R8G8B8A8_UNORM
		                                          : static_cast<DXGI_FORMAT>(format);
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv_desc.Texture2D.MipLevels = 1;
		ldp->device->CreateShaderResourceView(
		    atlas_res, &srv_desc,
		    ldp->blit_srv_heap->GetCPUDescriptorHandleForHeapStart());

		// Set descriptor heap, root sig, PSO
		ID3D12DescriptorHeap *heaps[] = {ldp->blit_srv_heap};
		cmd->SetDescriptorHeaps(1, heaps);
		cmd->SetGraphicsRootSignature(ldp->blit_root_sig);
		cmd->SetPipelineState(ldp->blit_pso);

		// Set render target
		D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
		rtv_handle.ptr = static_cast<SIZE_T>(target_rtv_cpu_handle);
		cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

		// Set viewport and scissor to canvas sub-rect (or full target)
		D3D12_VIEWPORT viewport = {};
		viewport.TopLeftX = static_cast<float>(vp_x);
		viewport.TopLeftY = static_cast<float>(vp_y);
		viewport.Width = static_cast<float>(vp_w);
		viewport.Height = static_cast<float>(vp_h);
		viewport.MaxDepth = 1.0f;
		cmd->RSSetViewports(1, &viewport);

		D3D12_RECT scissor = {};
		scissor.left = static_cast<LONG>(vp_x);
		scissor.top = static_cast<LONG>(vp_y);
		scissor.right = static_cast<LONG>(vp_x) + static_cast<LONG>(vp_w);
		scissor.bottom = static_cast<LONG>(vp_y) + static_cast<LONG>(vp_h);
		cmd->RSSetScissorRects(1, &scissor);

		// Set SRV descriptor table
		cmd->SetGraphicsRootDescriptorTable(
		    0, ldp->blit_srv_heap->GetGPUDescriptorHandleForHeapStart());

		// Atlas is guaranteed content-sized by compositor crop-blit.
		// In 2D mode, content occupies min(target, atlas) of the atlas.
		uint32_t atlas_w = tile_columns * view_width;
		uint32_t atlas_h = tile_rows * view_height;
		uint32_t content_w = (target_width < atlas_w) ? target_width : atlas_w;
		uint32_t content_h = (target_height < atlas_h) ? target_height : atlas_h;
		float u_scale = (atlas_w > 0) ? (float)content_w / (float)atlas_w : 1.0f;
		float v_scale = (atlas_h > 0) ? (float)content_h / (float)atlas_h : 1.0f;
		uint32_t constants[4];
		memcpy(&constants[0], &u_scale, sizeof(float));
		memcpy(&constants[1], &v_scale, sizeof(float));
		constants[2] = 0;
		constants[3] = 0;
		cmd->SetGraphicsRoot32BitConstants(1, 4, constants, 0);

		// Draw fullscreen quad
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		cmd->IASetVertexBuffers(0, 0, nullptr);
		cmd->DrawInstanced(4, 1, 0, 0);

		// Post-weave transparency pass:
		//   - compose path: alpha-gate samples the ORIGINAL atlas (not the
		//     composed result) to derive the screen-space "all views α==0"
		//     mask and zeroes alpha on those pixels → DWM blends the LIVE
		//     desktop (no captured-bg lag, no magenta fringe at silhouettes).
		//   - chroma-key fallback: legacy strip.
		if (target_resource != NULL) {
			D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv;
			bb_rtv.ptr = static_cast<SIZE_T>(target_rtv_cpu_handle);
			if (compose_should_run(ldp)) {
				alpha_gate_run_post_weave(
				    ldp, cmd,
				    static_cast<ID3D12Resource *>(target_resource), bb_rtv,
				    original_atlas_res, static_cast<DXGI_FORMAT>(format),
				    target_width, target_height,
				    tile_columns, tile_rows,
				    target_width, target_height,
				    canvas_offset_x, canvas_offset_y, canvas_width, canvas_height);
			} else if (ck_should_run(ldp)) {
				ck_run_post_weave_strip(
				    ldp, cmd,
				    static_cast<ID3D12Resource *>(target_resource),
				    bb_rtv, target_width, target_height);
			}
		}
		return;
	}

	(void)atlas_srv_gpu_handle;

	ID3D12GraphicsCommandList *cmd_3d =
	    static_cast<ID3D12GraphicsCommandList *>(d3d12_command_list);

	// Atlas is guaranteed content-sized SBS (2*view_width x view_height)
	// by compositor crop-blit.
	//
	// Two transparency paths feed the SR weaver opaque RGB:
	//   1. Compose-under-bg (preferred): pre-composite captured desktop under
	//      each per-view tile so the weaver consumes opaque RGB with the
	//      desktop already integrated. No post-weave pass. Quality-correct
	//      on AA edges and semi-transparent pixels.
	//   2. Chroma-key (fallback): replace alpha=0 with key color before
	//      weaver, strip back to alpha=0 after. Hard edges, used when WGC
	//      is unavailable.
	ID3D12Resource *original_atlas_3d = static_cast<ID3D12Resource *>(atlas_texture_resource);
	ID3D12Resource *weaver_input = original_atlas_3d;
	uint32_t atlas_w = tile_columns * view_width;
	uint32_t atlas_h = tile_rows * view_height;
	D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv;
	bb_rtv.ptr = static_cast<SIZE_T>(target_rtv_cpu_handle);
	if (compose_should_run(ldp) && weaver_input != NULL) {
		weaver_input = compose_run_pre_weave(
		    ldp, cmd_3d, weaver_input, atlas_w, atlas_h,
		    static_cast<DXGI_FORMAT>(format), tile_columns, tile_rows, bb_rtv,
		    target_width, target_height,
		    canvas_offset_x, canvas_offset_y, canvas_width, canvas_height);
	} else if (ck_should_run(ldp) && weaver_input != NULL) {
		weaver_input = ck_run_pre_weave_fill(
		    ldp, cmd_3d, weaver_input, atlas_w, atlas_h,
		    static_cast<DXGI_FORMAT>(format), bb_rtv);
	}

	if (weaver_input != NULL) {
		leiasr_d3d12_set_input_texture(ldp->leiasr, weaver_input,
		                               view_width, view_height, format);
	}

	// vp_x/vp_y/vp_w/vp_h carry the canvas sub-rect. leiasr_d3d12_weave
	// applies them via RSSetViewports/RSSetScissorRects on the cmd list —
	// the weaver's setViewport/setScissorRect alone do NOT scope the draw.
	// See gotcha at leiasr_d3d12_weave().
	leiasr_d3d12_weave(ldp->leiasr, d3d12_command_list, vp_x, vp_y, vp_w, vp_h);

	// Post-weave transparency pass:
	//   - compose path: alpha-gate samples the ORIGINAL atlas (not the
	//     composed result) to derive the screen-space "all views α==0" mask
	//     and zeroes alpha on those pixels — DWM blends the LIVE desktop in
	//     large flat transparent regions (no lag, no magenta fringe).
	//   - chroma-key fallback: legacy strip.
	if (target_resource != NULL) {
		D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv_post;
		bb_rtv_post.ptr = static_cast<SIZE_T>(target_rtv_cpu_handle);
		if (compose_should_run(ldp) && original_atlas_3d != NULL) {
			alpha_gate_run_post_weave(
			    ldp, cmd_3d,
			    static_cast<ID3D12Resource *>(target_resource), bb_rtv_post,
			    original_atlas_3d, static_cast<DXGI_FORMAT>(format),
			    target_width, target_height,
			    tile_columns, tile_rows,
			    target_width, target_height,
			    canvas_offset_x, canvas_offset_y, canvas_width, canvas_height);
		} else if (ck_should_run(ldp)) {
			ck_run_post_weave_strip(
			    ldp, cmd_3d,
			    static_cast<ID3D12Resource *>(target_resource),
			    bb_rtv_post, target_width, target_height);
		}
	}
}

static void
leia_dp_d3d12_ensure_blit_pso(struct leia_display_processor_d3d12_impl *ldp, DXGI_FORMAT fmt)
{
	if (ldp->blit_root_sig == NULL || ldp->device == NULL) {
		return;
	}
	if (ldp->blit_pso != NULL && ldp->blit_output_format == fmt) {
		return;
	}

	if (ldp->blit_pso != NULL) {
		ldp->blit_pso->Release();
		ldp->blit_pso = NULL;
	}

	// Compile shaders
	ID3DBlob *vs_blob = NULL;
	ID3DBlob *ps_blob = NULL;
	ID3DBlob *error_blob = NULL;

	HRESULT hr = D3DCompile(blit_vs_source, strlen(blit_vs_source), NULL, NULL, NULL,
	                        "main", "vs_5_0", 0, 0, &vs_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob) { error_blob->Release(); }
		U_LOG_E("Leia D3D12 DP: blit VS compile failed: 0x%08x", (unsigned)hr);
		return;
	}

	hr = D3DCompile(blit_ps_source, strlen(blit_ps_source), NULL, NULL, NULL,
	                "main", "ps_5_0", 0, 0, &ps_blob, &error_blob);
	if (FAILED(hr)) {
		vs_blob->Release();
		if (error_blob) { error_blob->Release(); }
		U_LOG_E("Leia D3D12 DP: blit PS compile failed: 0x%08x", (unsigned)hr);
		return;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
	pso_desc.pRootSignature = ldp->blit_root_sig;
	pso_desc.VS.pShaderBytecode = vs_blob->GetBufferPointer();
	pso_desc.VS.BytecodeLength = vs_blob->GetBufferSize();
	pso_desc.PS.pShaderBytecode = ps_blob->GetBufferPointer();
	pso_desc.PS.BytecodeLength = ps_blob->GetBufferSize();
	pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso_desc.RasterizerState.DepthClipEnable = TRUE;
	pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso_desc.NumRenderTargets = 1;
	pso_desc.RTVFormats[0] = fmt;
	pso_desc.SampleDesc.Count = 1;
	pso_desc.SampleMask = UINT_MAX;

	hr = ldp->device->CreateGraphicsPipelineState(
	    &pso_desc, __uuidof(ID3D12PipelineState),
	    reinterpret_cast<void **>(&ldp->blit_pso));

	vs_blob->Release();
	ps_blob->Release();

	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: blit PSO creation failed: 0x%08x", (unsigned)hr);
		return;
	}

	ldp->blit_output_format = fmt;
	U_LOG_I("Leia D3D12 DP: created 2D blit PSO for format %u", (unsigned)fmt);
}

static void
leia_dp_d3d12_set_output_format(struct xrt_display_processor_d3d12 *xdp, uint32_t format)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	leiasr_d3d12_set_output_format(ldp->leiasr, format);

	// Create/recreate blit PSO to match the output format
	leia_dp_d3d12_ensure_blit_pso(ldp, static_cast<DXGI_FORMAT>(format));
}

static bool
leia_dp_d3d12_get_predicted_eye_positions(struct xrt_display_processor_d3d12 *xdp,
                                          struct xrt_eye_positions *out_eye_pos)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	float left[3], right[3];
	if (!leiasr_d3d12_get_predicted_eye_positions(ldp->leiasr, left, right)) {
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

/*
 *
 * #224 / ADR-027 local 2D/3D zones — 1×1 leg (D3D12 port of the D3D11 slot).
 *
 * The mask is the WISH = a physical 2D/3D lens signal only (ADR-028). It is
 * NEVER sampled to decide content; it only drives request_display_mode.
 *
 */

static bool
leia_dp_d3d12_get_local_zone_caps(struct xrt_display_processor_d3d12 *xdp, struct xrt_dp_local_zone_caps *out_caps)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	if (out_caps == nullptr || out_caps->struct_size < XRT_DP_LOCAL_ZONE_CAPS_SIZE_V1) {
		// The V1 shape is the floor — reject only callers older than the zone
		// api itself. The V1 fields are written always; appended fields only
		// when the caller's struct_size covers them (ADR-027 append).
		return false;
	}
	// Zones are driven through the per-client SR lens hint — needs the hint
	// channel to exist (same gate as request_display_mode support).
	if (!leiasr_d3d12_supports_display_mode_switch(ldp->leiasr)) {
		return false;
	}
	out_caps->supported = 1;
	out_caps->zone_grid_width = 1; // single-zone panel: union collapses to global on/off
	out_caps->zone_grid_height = 1;
	out_caps->max_mask_width = 0; // no preference — content is reduced to one bit
	out_caps->max_mask_height = 0;
	out_caps->max_update_hz = 0; // edge-triggered internally (verdict changes only)
	if (out_caps->struct_size >= sizeof(struct xrt_dp_local_zone_caps)) {
		// SR weaver drives a binary panel state — the wish is consumed by the
		// conformant any-nonzero quantization, not driven fractionally.
		out_caps->wish_fractional = 0;
		// Advisory only; no verified hardware claim (ADR-027 flags the
		// lenticular column-band hypothesis as unverified).
		out_caps->switch_granularity = (uint32_t)XRT_DP_SWITCH_GRANULARITY_UNKNOWN;
		memset(out_caps->reserved, 0, sizeof(out_caps->reserved));
	}
	return true;
}

// GPU→CPU readback of the R8 wish mask on the DP's OWN queue (D3D12 has no
// immediate context like D3D11). Edge-triggered — runs once per mask generation,
// so the synchronous fence wait is cheap. Returns false on any failure (caller
// must NOT flip the lens on an unevaluated mask). On success *out_any reports the
// 1×1 OR-collapse verdict (any non-zero pixel ⟹ 3D). The mask is borrowed and
// arrives in PIXEL_SHADER_RESOURCE state (compositor contract) — restored before
// return.
static bool
leia_dp_d3d12_zone_readback(struct leia_display_processor_d3d12_impl *ldp,
                            ID3D12Resource *mask,
                            uint32_t mask_width,
                            uint32_t mask_height,
                            bool *out_any)
{
	ID3D12Device *dev = ldp->device;

	// Lazy one-time allocator + list + fence + event.
	if (ldp->zone_cmd_alloc == nullptr) {
		if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		                                       IID_PPV_ARGS(&ldp->zone_cmd_alloc)))) {
			return false;
		}
	}
	if (ldp->zone_cmd_list == nullptr) {
		if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ldp->zone_cmd_alloc,
		                                  nullptr, IID_PPV_ARGS(&ldp->zone_cmd_list)))) {
			return false;
		}
		ldp->zone_cmd_list->Close(); // created open; we Reset() before each use
	}
	if (ldp->zone_fence == nullptr) {
		if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ldp->zone_fence)))) {
			return false;
		}
		ldp->zone_fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
		if (ldp->zone_fence_event == nullptr) {
			return false;
		}
		ldp->zone_fence_value = 0;
	}

	// The mask footprint — R8_UNORM rows aligned to 256 in the readback buffer.
	D3D12_RESOURCE_DESC md = mask->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
	UINT num_rows = 0;
	UINT64 row_bytes = 0, total_bytes = 0;
	dev->GetCopyableFootprints(&md, 0, 1, 0, &fp, &num_rows, &row_bytes, &total_bytes);

	// (Re)allocate the readback buffer when the mask size changes.
	if (ldp->zone_readback == nullptr || ldp->zone_readback_w != mask_width ||
	    ldp->zone_readback_h != mask_height) {
		if (ldp->zone_readback != nullptr) {
			ldp->zone_readback->Release();
			ldp->zone_readback = nullptr;
		}
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = total_bytes;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.Format = DXGI_FORMAT_UNKNOWN;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
		                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		                                        IID_PPV_ARGS(&ldp->zone_readback)))) {
			ldp->zone_readback = nullptr;
			return false; // can't evaluate — don't flip the lens blindly
		}
		ldp->zone_readback_w = mask_width;
		ldp->zone_readback_h = mask_height;
		ldp->zone_readback_pitch = fp.Footprint.RowPitch;
	}

	// Record: mask PIXEL_SHADER_RESOURCE → COPY_SOURCE, copy into readback, back.
	if (FAILED(ldp->zone_cmd_alloc->Reset()) || FAILED(ldp->zone_cmd_list->Reset(ldp->zone_cmd_alloc, nullptr))) {
		return false;
	}

	D3D12_RESOURCE_BARRIER to_src = {};
	to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	to_src.Transition.pResource = mask;
	to_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	to_src.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	to_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	ldp->zone_cmd_list->ResourceBarrier(1, &to_src);

	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = ldp->zone_readback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = fp;
	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = mask;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;
	ldp->zone_cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	D3D12_RESOURCE_BARRIER to_psr = to_src;
	to_psr.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	to_psr.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	ldp->zone_cmd_list->ResourceBarrier(1, &to_psr);

	if (FAILED(ldp->zone_cmd_list->Close())) {
		return false;
	}
	ID3D12CommandList *lists[] = {ldp->zone_cmd_list};
	ldp->command_queue->ExecuteCommandLists(1, lists);
	const uint64_t signal = ++ldp->zone_fence_value;
	if (FAILED(ldp->command_queue->Signal(ldp->zone_fence, signal))) {
		return false;
	}
	if (ldp->zone_fence->GetCompletedValue() < signal) {
		if (FAILED(ldp->zone_fence->SetEventOnCompletion(signal, ldp->zone_fence_event))) {
			return false;
		}
		WaitForSingleObject(ldp->zone_fence_event, INFINITE);
	}

	// Map + any-nonzero over rows (pitch is the footprint's aligned RowPitch).
	void *mapped = nullptr;
	D3D12_RANGE read_range = {0, (SIZE_T)total_bytes};
	if (FAILED(ldp->zone_readback->Map(0, &read_range, &mapped)) || mapped == nullptr) {
		return false;
	}
	bool any = false;
	for (uint32_t y = 0; y < mask_height && !any; y++) {
		const uint8_t *row = static_cast<const uint8_t *>(mapped) + (size_t)y * fp.Footprint.RowPitch;
		for (uint32_t x = 0; x < mask_width; x++) {
			if (row[x] != 0) {
				any = true;
				break;
			}
		}
	}
	D3D12_RANGE no_write = {0, 0};
	ldp->zone_readback->Unmap(0, &no_write);

	*out_any = any;
	return true;
}

static bool
leia_dp_d3d12_publish_local_zone_mask(struct xrt_display_processor_d3d12 *xdp,
                                      void *mask_resource,
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

	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	ID3D12Resource *mask = static_cast<ID3D12Resource *>(mask_resource);
	if (mask == nullptr || mask_width == 0 || mask_height == 0 || ldp->device == nullptr ||
	    ldp->command_queue == nullptr) {
		return false;
	}

	// Content evaluation, once per generation: any non-zero mask pixel ⟹ 3D
	// (an all-zero mask — Tier-1 enable3D=FALSE — must collapse to 2D).
	if (!ldp->zone_eval_valid || seq != ldp->zone_eval_seq) {
		bool any = false;
		if (!leia_dp_d3d12_zone_readback(ldp, mask, mask_width, mask_height, &any)) {
			return false; // can't evaluate — don't flip the lens blindly
		}
		ldp->zone_want_3d = any;
		ldp->zone_eval_seq = seq;
		ldp->zone_eval_valid = true;
		U_LOG_W("SR D3D12 zone: generation %llu evaluated → %s (mask %ux%u, 1x1 collapse)",
		        (unsigned long long)seq, any ? "3D" : "2D", mask_width, mask_height);
	}

	// Edge-triggered lens hint: flip only when the verdict changes (or on the
	// first publish), so per-frame republish costs nothing SR-side.
	if (!ldp->zone_active || ldp->zone_hint_3d != ldp->zone_want_3d) {
		if (!leiasr_d3d12_request_display_mode(ldp->leiasr, ldp->zone_want_3d)) {
			return false;
		}
		ldp->zone_hint_3d = ldp->zone_want_3d;
	}
	ldp->zone_active = true;
	return true;
}

static bool
leia_dp_d3d12_clear_local_zone_mask(struct xrt_display_processor_d3d12 *xdp)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	// End of zone authority for this client — hand the lens back to the MODE
	// authority: restore the hint to what the active rendering mode implies
	// (view_count ≥ 2 ⟹ a 3D mode is active and the compositor snaps back to the
	// canvas weave, which needs a 3D panel; view_count 1 ⟹ 2D mode). Blindly
	// disabling here would strand a 3D-mode canvas weave on a 2D panel.
	if (ldp->zone_active) {
		bool mode_wants_3d = ldp->view_count >= 2;
		if (ldp->zone_hint_3d != mode_wants_3d) {
			leiasr_d3d12_request_display_mode(ldp->leiasr, mode_wants_3d);
		}
		ldp->zone_hint_3d = false;
		U_LOG_W("SR D3D12 zone: cleared — lens handed back to mode authority (%s)",
		        mode_wants_3d ? "3D" : "2D");
	}
	ldp->zone_active = false;
	ldp->zone_eval_valid = false;
	return true;
}

static bool
leia_dp_d3d12_request_display_mode(struct xrt_display_processor_d3d12 *xdp, bool enable_3d)
{
	// runtime#542: HARDWARE only — drive the SR lens hint and nothing else.
	// Atlas processing (weave vs flat blit) follows the CONTENT: view_count
	// tracks the per-frame atlas grid in process_atlas, so a hardware
	// override (xrRequestDisplayModeEXT) leaves the weave running and the
	// panel shows the woven atlas flat — the app-authored transition state.
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	return leiasr_d3d12_request_display_mode(ldp->leiasr, enable_3d);
}

static bool
leia_dp_d3d12_get_hardware_3d_state(struct xrt_display_processor_d3d12 *xdp, bool *out_is_3d)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	return leiasr_d3d12_get_hardware_3d_state(ldp->leiasr, out_is_3d);
}

static bool
leia_dp_d3d12_get_display_dimensions(struct xrt_display_processor_d3d12 *xdp,
                                     float *out_width_m,
                                     float *out_height_m)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	struct leiasr_display_dimensions dims = {};
	if (!leiasr_d3d12_get_display_dimensions(ldp->leiasr, &dims) || !dims.valid) {
		return false;
	}
	*out_width_m = dims.width_m;
	*out_height_m = dims.height_m;
	return true;
}

static bool
leia_dp_d3d12_get_display_pixel_info(struct xrt_display_processor_d3d12 *xdp,
                                     uint32_t *out_pixel_width,
                                     uint32_t *out_pixel_height,
                                     int32_t *out_screen_left,
                                     int32_t *out_screen_top)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	float w_m, h_m; // unused but required by API
	return leiasr_d3d12_get_display_pixel_info(ldp->leiasr, out_pixel_width, out_pixel_height,
	                                           out_screen_left, out_screen_top, &w_m, &h_m);
}

static bool
leia_dp_d3d12_is_alpha_native(struct xrt_display_processor_d3d12 *xdp)
{
	(void)xdp;
	// SR SDK D3D12 weaver interlaces into opaque RGB — alpha is destroyed.
	// Transparency is recovered via the chroma-key trick (see set_chroma_key).
	return false;
}

// #573 — the sole transparency enable (chroma-key removed). The leia D3D12 DP
// only ever runs IN-PROCESS (IPC D3D12 clients route through the D3D11 service
// DP), where the runtime presents opaque and the DP owns see-through via WGC
// compose-under-bg (client_presents=false). compose-under-bg uses ldp->ck_color
// purely as the α==0 sentinel (see process_atlas), so it stays set to the
// internal default — there is no chroma-key fill/strip pass anymore.
static void
leia_dp_d3d12_set_transparent_background(struct xrt_display_processor_d3d12 *xdp, bool enabled, bool client_presents)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);

	// Sentinel for α==0 atlas pixels in the compose-under-bg shader (NOT a
	// user-facing chroma key — kept internal).
	ldp->ck_color = kDefaultChromaKey;
	ldp->ck_enabled = false; // no chroma-key strip/fill pass (#573)

	// Client-present mode: the runtime owns a transparent present and blends the
	// live desktop into the holes, so the DP must NOT compose-under-bg.
	if (enabled && client_presents) {
		if (ldp->bg_compose_enabled) {
			compose_release_resources(ldp);
			ldp->bg_compose_enabled = false;
		}
		U_LOG_W("Leia D3D12 DP: transparency = client-present (alpha-gate only, no WGC)");
		return;
	}

	// In-process path: WGC desktop capture + per-tile compose-under-bg.
	if (enabled && !ldp->bg_compose_enabled && ldp->hwnd != nullptr) {
		ldp->bg_capture = leia_bg_capture_create(ldp->hwnd);
		if (ldp->bg_capture != nullptr && ldp->device != nullptr) {
			HRESULT hr = leia_bg_capture_open_d3d12(
			    ldp->bg_capture, ldp->device, &ldp->bg_shared_tex);
			if (SUCCEEDED(hr)) {
				hr = leia_bg_capture_open_fence_d3d12(
				    ldp->bg_capture, ldp->device, &ldp->bg_fence);
			}
			if (SUCCEEDED(hr)) {
				ldp->bg_compose_enabled = true;
				U_LOG_W("Leia D3D12 DP: transparency = compose-under-bg (WGC)");
			} else {
				U_LOG_W("Leia D3D12 DP: WGC import failed (0x%08x) — staying opaque",
				        (unsigned)hr);
				compose_release_resources(ldp);
			}
		}
	}
}

// #68 — the app self-presents its shared texture's canvas to its own window
// (vs the runtime presenting the full target). Drives the compose bg-UV
// remap-skip in compose_run_pre_weave (see @ref shared_texture_present).
static void
leia_dp_d3d12_set_shared_texture_present(struct xrt_display_processor_d3d12 *xdp, bool enabled)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	ldp->shared_texture_present = enabled;
	// #68 — combined with @ref zone_active in compose_run_pre_weave: the bg-UV
	// remap is skipped only for a self-presenting texture-ZONES frame (a published
	// zone mask), and kept for a texture-SURROUND app.
	U_LOG_W("Leia D3D12 DP: shared-texture present = %d (#68 bg-UV remap %s in zones frames)",
	        enabled, enabled ? "SKIPPED" : "applied");
}

// #491 part 3 — store the runtime's flattened 2D-under backdrop for the next
// process_atlas. Same D3D12 device as the compositor → the resource is sampled
// directly via an SRV we create into heap slot 2 (no open/import, unlike the
// WGC desktop). NULL ⟹ clear (desktop-only).
static void
leia_dp_d3d12_set_background_2d(struct xrt_display_processor_d3d12 *xdp,
                                void *background_resource,
                                uint32_t width,
                                uint32_t height)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);
	ldp->backdrop_resource = static_cast<ID3D12Resource *>(background_resource);
	ldp->backdrop_w = width;
	ldp->backdrop_h = height;
}

static void
leia_dp_d3d12_destroy(struct xrt_display_processor_d3d12 *xdp)
{
	struct leia_display_processor_d3d12_impl *ldp = leia_dp_d3d12(xdp);

	compose_release_resources(ldp);
	ck_release_resources(ldp);

	// #224 zone readback machinery.
	if (ldp->zone_readback != NULL) {
		ldp->zone_readback->Release();
	}
	if (ldp->zone_cmd_list != NULL) {
		ldp->zone_cmd_list->Release();
	}
	if (ldp->zone_cmd_alloc != NULL) {
		ldp->zone_cmd_alloc->Release();
	}
	if (ldp->zone_fence != NULL) {
		ldp->zone_fence->Release();
	}
	if (ldp->zone_fence_event != NULL) {
		CloseHandle(ldp->zone_fence_event);
	}

	if (ldp->blit_pso != NULL) {
		ldp->blit_pso->Release();
	}
	if (ldp->blit_root_sig != NULL) {
		ldp->blit_root_sig->Release();
	}
	if (ldp->blit_srv_heap != NULL) {
		ldp->blit_srv_heap->Release();
	}

	if (ldp->leiasr != NULL) {
		leiasr_d3d12_destroy(&ldp->leiasr);
	}
	free(ldp);
}


/*
 *
 * Helper: create blit root signature and SRV heap for 2D passthrough mode.
 *
 */

static bool
leia_dp_d3d12_init_blit(struct leia_display_processor_d3d12_impl *ldp)
{
	if (ldp->device == NULL) {
		return false;
	}

	// Create root signature: 1 SRV descriptor table (t0) + 4 root constants (b0) + 1 static sampler (s0)
	D3D12_DESCRIPTOR_RANGE srv_range = {};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = 1;
	srv_range.BaseShaderRegister = 0;
	srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER root_params[2] = {};
	root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_params[0].DescriptorTable.NumDescriptorRanges = 1;
	root_params[0].DescriptorTable.pDescriptorRanges = &srv_range;
	root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	root_params[1].Constants.ShaderRegister = 0;
	root_params[1].Constants.Num32BitValues = 4;
	root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
	rs_desc.NumParameters = 2;
	rs_desc.pParameters = root_params;
	rs_desc.NumStaticSamplers = 1;
	rs_desc.pStaticSamplers = &sampler;
	rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ID3DBlob *sig_blob = NULL;
	ID3DBlob *error_blob = NULL;
	HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
	                                          &sig_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob) { error_blob->Release(); }
		U_LOG_E("Leia D3D12 DP: blit root sig serialize failed: 0x%08x", (unsigned)hr);
		return false;
	}

	hr = ldp->device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
	                                       __uuidof(ID3D12RootSignature),
	                                       reinterpret_cast<void **>(&ldp->blit_root_sig));
	sig_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: blit root sig creation failed: 0x%08x", (unsigned)hr);
		return false;
	}

	// Create shader-visible SRV heap (1 descriptor)
	D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
	heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heap_desc.NumDescriptors = 1;
	heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	hr = ldp->device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
	                                        reinterpret_cast<void **>(&ldp->blit_srv_heap));
	if (FAILED(hr)) {
		U_LOG_E("Leia D3D12 DP: blit SRV heap creation failed: 0x%08x", (unsigned)hr);
		return false;
	}

	// PSO is created lazily in set_output_format when the format is known
	U_LOG_I("Leia D3D12 DP: initialized 2D blit root signature and SRV heap");
	return true;
}


/*
 *
 * Factory function — matches xrt_dp_factory_d3d12_fn_t signature.
 *
 */

extern "C" xrt_result_t
leia_dp_factory_d3d12(void *d3d12_device,
                      void *d3d12_command_queue,
                      void *window_handle,
                      struct xrt_display_processor_d3d12 **out_xdp)
{
	// Create weaver — view dimensions are set per-frame via setInputViewTexture,
	// so we pass 0,0 here.
	struct leiasr_d3d12 *weaver = NULL;
	xrt_result_t ret = leiasr_d3d12_create(5.0, d3d12_device, d3d12_command_queue,
	                                       window_handle, 0, 0, &weaver);
	if (ret != XRT_SUCCESS || weaver == NULL) {
		U_LOG_W("Failed to create SR D3D12 weaver");
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor_d3d12_impl *ldp =
	    (struct leia_display_processor_d3d12_impl *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		leiasr_d3d12_destroy(&weaver);
		return XRT_ERROR_ALLOCATION;
	}

	// ADR-020 rule 1: advertise the vtable size (calloc zeroed reserved_0).
	ldp->base.struct_size = static_cast<uint32_t>(sizeof(struct xrt_display_processor_d3d12));
	ldp->base.process_atlas = leia_dp_d3d12_process_atlas;
	ldp->base.set_output_format = leia_dp_d3d12_set_output_format;
	ldp->base.get_predicted_eye_positions = leia_dp_d3d12_get_predicted_eye_positions;
	ldp->base.get_window_metrics = NULL;
	ldp->base.request_display_mode = leia_dp_d3d12_request_display_mode;
	ldp->base.get_hardware_3d_state = leia_dp_d3d12_get_hardware_3d_state;
	ldp->base.get_display_dimensions = leia_dp_d3d12_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_d3d12_get_display_pixel_info;
	ldp->base.is_alpha_native = leia_dp_d3d12_is_alpha_native;
	ldp->base.set_background_2d = leia_dp_d3d12_set_background_2d; // #491 part 3
	ldp->base.set_transparent_background = leia_dp_d3d12_set_transparent_background; // #573
	ldp->base.set_shared_texture_present = leia_dp_d3d12_set_shared_texture_present; // #68
	// #224 / ADR-027 local 2D/3D zones — 1×1 leg (runtime gates on struct_size).
	ldp->base.get_local_zone_caps = leia_dp_d3d12_get_local_zone_caps;
	ldp->base.publish_local_zone_mask = leia_dp_d3d12_publish_local_zone_mask;
	ldp->base.clear_local_zone_mask = leia_dp_d3d12_clear_local_zone_mask;
	ldp->base.destroy = leia_dp_d3d12_destroy;
	ldp->leiasr = weaver;
	ldp->device = static_cast<ID3D12Device *>(d3d12_device);
	ldp->hwnd = static_cast<HWND>(window_handle);
	ldp->command_queue = static_cast<ID3D12CommandQueue *>(d3d12_command_queue);
	ldp->view_count = 2;

	// Init blit root signature and SRV heap for 2D passthrough mode
	if (!leia_dp_d3d12_init_blit(ldp)) {
		U_LOG_W("Leia D3D12 DP: blit init failed — 2D mode will be unavailable");
	}

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR D3D12 display processor (factory, owns weaver)");

	return XRT_SUCCESS;
}
