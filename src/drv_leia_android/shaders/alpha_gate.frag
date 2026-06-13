// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0

// Post-weave alpha-gate (Android CNSDK path, #568). The CNSDK interlacer weaves
// into opaque RGB and exposes no per-pixel alpha, so transparency is
// reconstructed here so SurfaceFlinger shows the live screen through the
// transparent regions (the Android in-process model — no WGC capture, unlike
// the Windows DP).
//
// This is the Android-only fork of drv_leia/shaders/alpha_gate.frag: the
// desktop path hides the parallax de-occlusion fringe with WGC
// compose-under-desktop; Android has no WGC, so the de-occlusion band must be
// made see-through instead. The default mode here (mode 1) does that; mode 0 is
// the original Windows-derived all-or-nothing mask, kept for on-device A/B
// (debug.dxr.alphagate).
//
// Screen UV equals tile-local UV when target = (tile_columns × view_w,
// tile_rows × view_h), the canvas-fills-target case (always true on Android).

#version 450

// The woven (CNSDK interlacer output) target, copied to a sampleable image.
layout(binding = 0) uniform sampler2D backbuffer;
// The ORIGINAL multiview atlas handed to the weave (premultiplied RGBA).
layout(binding = 1) uniform sampler2D atlas;
// Unused on Android (no Local2D under-layer; has_backdrop is always 0). Declared
// only to keep the descriptor-set layout identical to the desktop shader's.
layout(binding = 2) uniform sampler2D backdrop;

layout(push_constant) uniform PC {
	uvec2 tile_count;
	uint  has_backdrop;   // always 0 on Android
	uint  mode;           // 0 = legacy all-or-nothing, 1 = woven view-select (#568 de-occlusion fix)
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	vec3 woven = texture(backbuffer, in_uv).rgb;

	if (pc.mode != 0u) {
		// ── Woven per-pixel view-selection (#568 de-occlusion fix) ──
		//
		// The CNSDK interlacer draws each output pixel from ONE view (the
		// lenticular per-subpixel selection), so the woven RGB at this pixel IS
		// that view's sampled color. Recover which view by matching the woven
		// RGB against each tile's atlas RGB, then emit THAT view's alpha — the
		// same per-pixel selection the interlacer applied to RGB, now applied to
		// alpha. In the parallax de-occlusion band one view holds the avatar
		// (α>0) and the other holds transparent background (α==0); the pixels
		// the interlacer drew from the background view get α==0 and the live
		// screen shows through, while avatar pixels keep their α. Outside the
		// band both views agree, so the result matches the simple mask.
		//
		// Matching is done in whatever space CNSDK sampled (works for
		// premultiplied or straight alpha — we read the chosen view's own α). On
		// the rare ambiguity of two views with identical RGB but different α
		// (e.g. fully-opaque black content next to transparent background) the
		// first match wins, i.e. it safe-fails toward KEEPING the pixel opaque —
		// deliberately, to never over-punch the avatar (cf. naive min-alpha,
		// which erodes every one-view silhouette edge).
		float best_d = 1e30;
		float sel_a = 1.0;
		for (uint ty = 0u; ty < pc.tile_count.y; ty++) {
			for (uint tx = 0u; tx < pc.tile_count.x; tx++) {
				vec2 uv_at_tile = (vec2(tx, ty) + in_uv) / vec2(pc.tile_count);
				vec4 s = textureLod(atlas, uv_at_tile, 0.0);
				float d = distance(woven, s.rgb);
				if (d < best_d) {
					best_d = d;
					sel_a = s.a;
				}
			}
		}
		out_color = vec4(woven, sel_a);
		return;
	}

	// ── mode 0: legacy all-or-nothing (pre-#568), kept for A/B ──
	// Opaque if ANY tile has content; transparent only where ALL tiles are
	// α==0. This is what produced the black de-occlusion fringe.
	bool all_transparent = true;
	for (uint ty = 0u; ty < pc.tile_count.y; ty++) {
		for (uint tx = 0u; tx < pc.tile_count.x; tx++) {
			vec2 uv_at_tile = (vec2(tx, ty) + in_uv) / vec2(pc.tile_count);
			if (textureLod(atlas, uv_at_tile, 0.0).a > 0.0) {
				all_transparent = false;
			}
		}
	}
	out_color = all_transparent ? vec4(0.0, 0.0, 0.0, 0.0) : vec4(woven, 1.0);
}
