// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

// Post-weave alpha-gate: compose-mode replacement for the chroma-key strip.
// Samples the woven back-buffer and the original atlas. For each screen pixel
// at tile-local UV, tests whether EVERY view's atlas has α==0 at the matching
// tile-local position. Pixels passing the test get (0,0,0,0) so DWM blends the
// live desktop. Others keep the woven RGB at α=1.
//
// No chroma keying — captured-bg lag is bypassed in flat transparent regions
// and silhouettes show the smooth lenticular blend the weaver produced from
// captured-bg ↔ atlas-content inputs.
//
// Screen UV equals tile-local UV when target = (tile_columns × view_w,
// tile_rows × view_h), the canvas-fills-target case.

#version 450

layout(binding = 0) uniform sampler2D backbuffer;
layout(binding = 1) uniform sampler2D atlas;
// #491 part 3 — the runtime's flattened 2D-under backdrop (premultiplied RGBA,
// window-client-area pixels = screen/tile-local UV). A real under-layer the
// runtime owns, so it must NOT be punched through to the live desktop: where
// the atlas is transparent but the backdrop covers, the backdrop is emitted
// premultiplied with its own alpha so DWM blends it OVER the live desktop
// (opaque backdrop fully occludes; semi-transparent reveals the desktop).
layout(binding = 2) uniform sampler2D backdrop;

layout(push_constant) uniform PC {
	uvec2 tile_count;
	uint  has_backdrop;   // #491 part 3 — 1 ⟹ a 2D-under backdrop is present
	uint  pad;
	// #602 — the back-buffer copy (ck_strip_image) is allocated at a
	// high-water-mark so content-fit zones stop churning it; only its top-left
	// (w, h) sub-rect holds this frame's copy. Scale screen UV into that
	// sub-rect: (w/alloc_w, h/alloc_h). (1,1) when the image matches exactly.
	vec2  strip_uv_scale;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	bool all_transparent = true;
	for (uint ty = 0u; ty < pc.tile_count.y; ty++) {
		for (uint tx = 0u; tx < pc.tile_count.x; tx++) {
			vec2 uv_at_tile = (vec2(tx, ty) + in_uv) / vec2(pc.tile_count);
			if (textureLod(atlas, uv_at_tile, 0.0).a > 0.0) {
				all_transparent = false;
			}
		}
	}

	if (!all_transparent) {
		// Woven 3D content (over the backdrop-over-desktop baked pre-weave):
		// opaque so DWM shows it as-is. #602 — sample the strip's valid
		// top-left sub-rect (the image may be over-allocated).
		out_color = vec4(texture(backbuffer, in_uv * pc.strip_uv_scale).rgb, 1.0);
		return;
	}

	// Atlas transparent here. #491 part 3 — if a 2D-under backdrop covers this
	// pixel, emit it premultiplied with its own alpha (DWM composites it over
	// the live desktop). Sample at screen UV (= window-local) like the compose
	// pass. Otherwise punch through to the live desktop (today's behavior).
	if (pc.has_backdrop != 0u) {
		vec4 bd = texture(backdrop, in_uv); // premultiplied
		out_color = vec4(bd.rgb, bd.a);
		return;
	}
	out_color = vec4(0.0, 0.0, 0.0, 0.0);   // live desktop
}
