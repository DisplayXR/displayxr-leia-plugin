// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  MSL for the macOS DP's transparency passes (compose-under + alpha
 *         gate), shared by leia_display_processor_macos.m and the GPU test
 *         tests/test_transp_metal_macos.m so the test runs exactly the
 *         shipped shader.
 * @ingroup drv_leia
 */

#pragma once

#import <Foundation/Foundation.h>

//! Compose-under + alpha gate (see the file header). Ports of
//! drv_leia_linux/shaders/{compose_under_bg,alpha_gate}.frag — keep in step.
static NSString *const k_transp_msl =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut { float4 pos [[position]]; float2 uv; };\n"
     "vertex VOut leia_tri_vs(uint vid [[vertex_id]]) {\n"
     "    VOut o; o.uv = float2((vid << 1) & 2, vid & 2);\n"
     "    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
     "static float3 srgb_to_linear(float3 c) {\n"
     "    return select(pow((c + 0.055) / 1.055, float3(2.4)), c / 12.92, c <= 0.04045); }\n"
     "static float3 linear_to_srgb(float3 c) { c = saturate(c);\n"
     "    return select(1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, c * 12.92, c <= 0.0031308); }\n"
     "struct ComposeP { float2 bg_origin; float2 bg_extent; uint2 tiles; uint atlas_linear; uint pad; };\n"
     "fragment float4 leia_compose_fs(VOut in [[stage_in]],\n"
     "        texture2d<float, access::read> atlas [[texture(0)]], texture2d<float> bg [[texture(1)]],\n"
     "        sampler s [[sampler(0)]], constant ComposeP &p [[buffer(0)]]) {\n"
     "    float4 a = atlas.read(uint2(in.pos.xy));\n"
     "    if (a.a >= 1.0) return float4(a.rgb, 1.0);\n" // opaque: verbatim
     "    float2 tl = fract(in.uv * float2(p.tiles));\n"
     "    float3 b = srgb_to_linear(bg.sample(s, p.bg_origin + tl * p.bg_extent).rgb);\n"
     "    float3 c = p.atlas_linear != 0 ? a.rgb : srgb_to_linear(a.rgb);\n"
     "    float3 o = c + (1.0 - a.a) * b;\n" // premultiplied over
     "    return float4(p.atlas_linear != 0 ? saturate(o) : linear_to_srgb(o), 1.0); }\n"
     "struct GateP { uint2 tiles; float2 tile_px; };\n"
     "fragment float4 leia_gate_fs(VOut in [[stage_in]],\n"
     "        texture2d<float, access::read> woven [[texture(0)]],\n"
     "        texture2d<float, access::read> atlas [[texture(1)]], constant GateP &p [[buffer(0)]]) {\n"
     "    uint2 amax = uint2(atlas.get_width() - 1, atlas.get_height() - 1);\n"
     "    for (uint ty = 0; ty < p.tiles.y; ty++) for (uint tx = 0; tx < p.tiles.x; tx++) {\n"
     "        uint2 px = min(uint2((float2(tx, ty) + saturate(in.uv)) * p.tile_px), amax);\n"
     "        if (atlas.read(px).a > 0.0) return float4(woven.read(uint2(in.pos.xy)).rgb, 1.0); }\n"
     "    return float4(0.0); }\n"; // every view transparent: the live desktop
