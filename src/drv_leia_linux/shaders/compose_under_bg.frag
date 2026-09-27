// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0

// Pre-weave compose-under-bg: composite the captured desktop region UNDER
// each per-view atlas tile, outputting opaque RGB the SR weaver can consume.
// Replaces the chroma-key trick on the Vulkan DP — preserves AA edges and
// genuinely semi-transparent (0<a<1) pixels.
//
// The desktop is at z=0 (display plane), so the same captured region is
// sampled into every tile; per-eye parallax comes from the atlas content,
// not the background.
//
// COLOUR CONTRACT (OpenXR 1.1; regression-tested by
// tests/test_compose_color_linux.c on a real Vulkan device):
//
//  1. An OPAQUE atlas pixel (a == 1) is written VERBATIM — the exact bytes the
//     weaver gets with the capture off. The capture may only change pixels the
//     app left (partly) transparent. Read with texelFetch, not a filtered
//     texture(): the fill is atlas-sized, and a bilinear read at a texel centre
//     is not exact on every GPU (8-bit sub-texel weights can pull 1/256 of a
//     neighbour in — see alpha_gate.frag).
//
//  2. The atlas is PREMULTIPLIED. §10.6.2 "Composition Layer Blending": a layer
//     without XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT is premultiplied,
//     an unpremultiplied one is converted ("LayerColor.RGB *= LayerColor.A"),
//     and layers blend as "CompositeColor = LayerColor + CompositeColor *
//     (1 - LayerColor.A)" — so the runtime's composited atlas is premultiplied
//     either way (and an MSAA resolve of straight colour over the transparent
//     clear is premultiplied by construction). §10.6.4 ALPHA_BLEND then blends
//     that composite with what is behind the display using its alpha. The
//     desktop is that "behind", so:  out = C + desktop * (1 - A).
//     (The previous `mix(desktop, C, A)` treated C as straight and multiplied
//     it by A a second time: every a < 1 pixel came out too dark.)
//
//  3. The blend runs in LINEAR light. §10.1 Swapchain Image Management:
//     "Rendering operations involving composition of submitted layers are
//     assumed to be internally performed by the runtime in linear color
//     space." The runtime DECLARES the atlas encoding (runtime#1484,
//     pc.atlas_linear): ENCODED (today) bytes are sRGB-encoded — decoded here,
//     blended, re-encoded, so the fill stays in the declared encoding and the
//     weaver's sRGB conversion (derived from the same declaration,
//     leia_sr_linux_sdk.c) stays correct. The mutter capture (B8G8R8A8_UNORM,
//     SPA BGRx) holds the desktop's scanout bytes, i.e. sRGB-encoded, and is
//     sampled through a UNORM view (no hardware decode), so it is decoded
//     here too. Vulkan: a *_UNORM view returns the stored value / 255, a *_SRGB
//     view applies the sRGB EOTF on read — neither image involved here is
//     viewed as *_SRGB, so nothing is decoded twice.

#version 450

layout(binding = 0) uniform sampler2D atlas;
layout(binding = 1) uniform sampler2D bg;
// #491 part 3 — the runtime's flattened 2D-under backdrop (premultiplied RGBA,
// window-client-area pixels, in the ATLAS's encoding). When pc.has_backdrop
// != 0 it is composited OVER the captured desktop before the atlas-over, so a
// flat 2D plane sits behind the woven 3D and a semi-transparent backdrop
// reveals the desktop.
layout(binding = 2) uniform sampler2D backdrop;

// Must match struct leia_compose_push (leia_compose_push_linux.h).
layout(push_constant) uniform PC {
	vec2  bg_uv_origin;      // window TL on monitor, normalized
	vec2  bg_uv_extent;      // window size on monitor, normalized
	uvec2 tile_count;        // (tile_columns, tile_rows)
	uint  has_backdrop;      // #491 part 3 — 1 ⟹ composite `backdrop over desktop`
	uint  debug_bg_only;     // DXR_LEIA_BG_DEBUG=1 — output the background only
	uint  atlas_linear;      // 1 ⟹ runtime declared the atlas LINEAR (runtime#1484)
	uint  bg_is_atlas_space; // 1 ⟹ `bg` is the 2D-under backdrop (atlas encoding), 0 ⟹ desktop (sRGB)
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

// IEC 61966-2-1 piecewise sRGB, exact inverses of each other.
vec3 srgb_to_linear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

vec3 linear_to_srgb(vec3 c)
{
	c = clamp(c, 0.0, 1.0);
	return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308)));
}

vec3 atlas_space_to_linear(vec3 c)
{
	return pc.atlas_linear != 0u ? c : srgb_to_linear(c);
}

void main()
{
	// Plain compose-with-bg. Transparency holes are produced by the
	// post-weave alpha-gate pass — this shader never emits a chroma sentinel.
	// The fill is exactly atlas-sized, so the fragment IS the atlas texel.
	vec4 a = texelFetch(atlas, ivec2(gl_FragCoord.xy), 0);
	vec2 tile_local = fract(in_uv * vec2(pc.tile_count));
	vec2 bg_uv = pc.bg_uv_origin + tile_local * pc.bg_uv_extent;
	vec3 b = textureLod(bg, bg_uv, 0.0).rgb;

	// DXR_LEIA_BG_DEBUG=1: output the background ONLY across the whole tile —
	// makes "is the captured desktop actually arriving?" a one-glance visual
	// check on the panel. Raw bytes, like everything else the weaver gets.
	if (pc.debug_bg_only != 0u) {
		out_color = vec4(b, 1.0);
		return;
	}

	// Contract 1: opaque content is untouched by the capture — bit-exact.
	if (a.a >= 1.0) {
		out_color = vec4(a.rgb, 1.0);
		return;
	}

	// Contract 3: everything below is linear light.
	vec3 under = pc.bg_is_atlas_space != 0u ? atlas_space_to_linear(b) : srgb_to_linear(b);

	// #491 part 3 — the backdrop is a flat z=0 layer covering the window client
	// area, so sample it at the same per-tile window-local UV as the desktop.
	// Premultiplied "over": under' = backdrop + (1 - backdrop.a) * desktop.
	if (pc.has_backdrop != 0u) {
		vec4 bd = texture(backdrop, tile_local);
		under = atlas_space_to_linear(bd.rgb) + (1.0 - bd.a) * under;
	}

	// Contract 2: premultiplied atlas over what is behind the display.
	vec3 o = atlas_space_to_linear(a.rgb) + (1.0 - a.a) * under;
	out_color = vec4(pc.atlas_linear != 0u ? clamp(o, 0.0, 1.0) : linear_to_srgb(o), 1.0);
}
