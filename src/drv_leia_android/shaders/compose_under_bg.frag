// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0

// Pre-weave compose-under-bg: composite the background UNDER each per-view
// atlas tile, outputting opaque RGB the weaver can consume. Preserves AA edges
// and genuinely semi-transparent (0<a<1) pixels.
//
//   out = mix(bg, atlas.rgb, atlas.a),  out.a = 1
//
// The background is at z=0 (display plane), so the same region is sampled into
// every tile; per-eye parallax comes from the atlas content, not the
// background.
//
// ANDROID COPY (runtime#1073 T0). Byte-identical to ../drv_leia/shaders/
// compose_under_bg.frag — per-backend shader copies are this repo's convention
// (see drv_leia_linux/shaders). Android has no desktop capture, so the DP binds
// the runtime-supplied backdrop (base DP slot 16 `set_background_2d`) to
// binding 1 with bg_uv = (0,0)-(1,1), binds it again to the unused binding 2
// to keep the descriptor set complete, and pushes has_backdrop = 0. Keeping the
// file identical means a T1/T2 producer (MediaProjection / vendor
// captureDisplay) that DOES have a monitor-space sub-rect drops straight in.

#version 450

layout(binding = 0) uniform sampler2D atlas;
layout(binding = 1) uniform sampler2D bg;
// #491 part 3 — the runtime's flattened 2D-under backdrop (premultiplied RGBA,
// window-client-area pixels). When pc.has_backdrop != 0 it is composited OVER
// the captured desktop before the atlas-over, so a flat 2D plane sits behind
// the woven 3D and a semi-transparent backdrop reveals the desktop.
layout(binding = 2) uniform sampler2D backdrop;

layout(push_constant) uniform PC {
	vec2  bg_uv_origin;   // window TL on monitor, normalized
	vec2  bg_uv_extent;   // window size on monitor, normalized
	uvec2 tile_count;     // (tile_columns, tile_rows)
	uint  has_backdrop;   // #491 part 3 — 1 ⟹ composite `backdrop over desktop`
	uint  pad;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	// Plain compose-with-bg. Transparency holes are produced by the
	// post-weave alpha-gate pass — this shader never emits a chroma sentinel.
	vec4 a = texture(atlas, in_uv);
	vec2 tile_local = fract(in_uv * vec2(pc.tile_count));
	vec2 bg_uv = pc.bg_uv_origin + tile_local * pc.bg_uv_extent;
	vec3 b = textureLod(bg, bg_uv, 0.0).rgb;

	// #491 part 3 — the backdrop is a flat z=0 layer covering the window client
	// area, so sample it at the same per-tile window-local UV as the desktop.
	// Premultiplied "over": b' = backdrop.rgb + (1 - backdrop.a) * desktop.
	if (pc.has_backdrop != 0u) {
		vec4 bd = texture(backdrop, tile_local);
		b = bd.rgb + (1.0 - bd.a) * b;
	}

	out_color = vec4(mix(b, a.rgb, a.a), 1.0);
}
