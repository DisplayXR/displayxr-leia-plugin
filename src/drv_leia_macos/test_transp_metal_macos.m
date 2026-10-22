// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  GPU test of the macOS transparency passes (leia_transp_msl_macos.h):
 *         the compose-under colour contract and the alpha-gate punch rule, on
 *         a real Metal device. The macOS counterpart of
 *         drv_leia_linux/tests/test_compose_color_linux.c. No SR SDK, no panel.
 *
 * Compose (2x1 atlas of 4x2 views, BGRA8Unorm = ENCODED bytes, premultiplied):
 *   1. an opaque atlas texel comes out VERBATIM;
 *   2. a fully transparent texel comes out as the background;
 *   3. a partly transparent texel is `C + B * (1 - A)` in LINEAR light;
 *   4. every tile samples the SAME window-local background texel.
 * Gate (woven scratch + the same atlas):
 *   5. (0,0,0,0) only where EVERY view is transparent;
 *   6. the woven colour at alpha 1 wherever ANY view is opaque.
 *
 * Exit 0 = pass, 1 = a contract broke, 77 = no Metal device (skip).
 */

#import <Metal/Metal.h>

#include "leia_transp_msl_macos.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define VW 4u
#define VH 2u

static int g_fail;

static double
srgb_to_linear(double c)
{
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double
linear_to_srgb(double c)
{
	c = c < 0 ? 0 : (c > 1 ? 1 : c);
	return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

static id<MTLTexture>
make_tex(id<MTLDevice> dev, uint32_t w, uint32_t h, const uint8_t *bgra)
{
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                                             width:w
	                                                                            height:h
	                                                                         mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
	d.storageMode = MTLStorageModeShared;
	id<MTLTexture> t = [dev newTextureWithDescriptor:d];
	if (bgra != NULL) {
		[t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:bgra bytesPerRow:w * 4];
	}
	return t;
}

static id<MTLRenderPipelineState>
pso(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *fs)
{
	MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
	desc.vertexFunction = [lib newFunctionWithName:@"leia_tri_vs"];
	desc.fragmentFunction = [lib newFunctionWithName:fs];
	desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	NSError *err = nil;
	id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:desc error:&err];
	if (p == nil) {
		fprintf(stderr, "pipeline %s: %s\n", fs.UTF8String, err.localizedDescription.UTF8String);
		exit(1);
	}
	return p;
}

static void
put(uint8_t *img, uint32_t w, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	uint8_t *px = img + (y * w + x) * 4;
	px[0] = b;
	px[1] = g;
	px[2] = r;
	px[3] = a;
}

static void
expect(const char *what, const uint8_t *px, int r, int g, int b, int a, int tol)
{
	const int got[4] = {px[2], px[1], px[0], px[3]};
	const int want[4] = {r, g, b, a};
	for (int i = 0; i < 4; i++) {
		if (abs(got[i] - want[i]) > tol) {
			printf("FAIL %s: got (%d,%d,%d,%d) want (%d,%d,%d,%d) ±%d\n", what, got[0], got[1], got[2], got[3], r,
			       g, b, a, tol);
			g_fail = 1;
			return;
		}
	}
	printf("ok   %s: (%d,%d,%d,%d)\n", what, got[0], got[1], got[2], got[3]);
}

int
main(void)
{
	@autoreleasepool {
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		if (dev == nil) {
			printf("SKIP: no Metal device\n");
			return 77;
		}
		NSError *err = nil;
		id<MTLLibrary> lib = [dev newLibraryWithSource:k_transp_msl options:nil error:&err];
		if (lib == nil) {
			fprintf(stderr, "MSL compile failed: %s\n", err.localizedDescription.UTF8String);
			return 1;
		}
		id<MTLRenderPipelineState> compose = pso(dev, lib, @"leia_compose_fs");
		id<MTLRenderPipelineState> gate = pso(dev, lib, @"leia_gate_fs");
		id<MTLCommandQueue> q = [dev newCommandQueue];

		// Background: window-local, one distinct grey per column (x * 50 + 40).
		uint8_t bg[VW * VH * 4];
		for (uint32_t y = 0; y < VH; y++) {
			for (uint32_t x = 0; x < VW; x++) {
				uint8_t v = (uint8_t)(x * 50 + 40);
				put(bg, VW, x, y, v, v, v, 255);
			}
		}
		// Atlas 2x1 of 4x2 views. Row 0 = the compose cases; row 1 = gate cases.
		uint8_t atlas[2 * VW * VH * 4] = {0};
		const uint32_t AW = 2 * VW;
		// Row 0, view 0: x0 opaque, x1 transparent, x2 half (premultiplied), x3 transparent.
		put(atlas, AW, 0, 0, 10, 200, 30, 255);
		put(atlas, AW, 1, 0, 0, 0, 0, 0);
		put(atlas, AW, 2, 0, 64, 64, 64, 128);
		put(atlas, AW, 3, 0, 0, 0, 0, 0);
		// Row 0, view 1: x0 transparent (same column as view 0's opaque texel).
		put(atlas, AW, VW + 0, 0, 0, 0, 0, 0);
		// Row 1 (gate): column 0 transparent in both views; column 1 opaque in
		// view 0 only; column 2 opaque in both; column 3 opaque in view 1 only.
		put(atlas, AW, 1, 1, 255, 0, 0, 255);
		put(atlas, AW, 2, 1, 255, 0, 0, 255);
		put(atlas, AW, VW + 2, 1, 0, 255, 0, 255);
		put(atlas, AW, VW + 3, 1, 0, 255, 0, 255);

		id<MTLTexture> atlas_tex = make_tex(dev, AW, VH, atlas);
		id<MTLTexture> bg_tex = make_tex(dev, VW, VH, bg);
		id<MTLTexture> fill = make_tex(dev, AW, VH, NULL);

		MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
		sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
		sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
		id<MTLSamplerState> samp = [dev newSamplerStateWithDescriptor:sd];

		// --- Compose ---
		struct
		{
			float bg_origin[2], bg_extent[2];
			uint32_t tiles[2], atlas_linear, pad;
		} cp = {{0, 0}, {1, 1}, {2, 1}, 0, 0};
		id<MTLCommandBuffer> cb = [q commandBuffer];
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = fill;
		pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
		[enc setRenderPipelineState:compose];
		[enc setFragmentTexture:atlas_tex atIndex:0];
		[enc setFragmentTexture:bg_tex atIndex:1];
		[enc setFragmentSamplerState:samp atIndex:0];
		[enc setFragmentBytes:&cp length:sizeof(cp) atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		uint8_t out[2 * VW * VH * 4];
		[fill getBytes:out bytesPerRow:AW * 4 fromRegion:MTLRegionMake2D(0, 0, AW, VH) mipmapLevel:0];
#define PX(x, y) (out + ((y) * AW + (x)) * 4)
		expect("1 opaque texel verbatim", PX(0, 0), 10, 200, 30, 255, 0);
		expect("2 transparent texel = background (col 1)", PX(1, 0), 90, 90, 90, 255, 1);
		{
			double c = srgb_to_linear(64 / 255.0), b = srgb_to_linear(140 / 255.0);
			int want = (int)lround(linear_to_srgb(c + b * (1.0 - 128 / 255.0)) * 255.0);
			expect("3 premultiplied half over background, linear light", PX(2, 0), want, want, want, 255, 1);
		}
		expect("4 view 1 col 0 samples the same window-local background", PX(VW + 0, 0), 40, 40, 40, 255, 1);
		expect("4 view 1 col 3 samples the same window-local background", PX(VW + 3, 0), 190, 190, 190, 255, 1);

		// --- Gate --- woven scratch = a flat colour; the target is view-sized.
		uint8_t woven_px[VW * VH * 4];
		for (uint32_t i = 0; i < VW * VH; i++) {
			put(woven_px, VW, i % VW, i / VW, 100, 110, 120, 255);
		}
		id<MTLTexture> woven = make_tex(dev, VW, VH, woven_px);
		id<MTLTexture> target = make_tex(dev, VW, VH, NULL);
		struct
		{
			uint32_t tiles[2];
			float tile_px[2];
		} gp = {{2, 1}, {VW, VH}};
		cb = [q commandBuffer];
		pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = target;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
		enc = [cb renderCommandEncoderWithDescriptor:pass];
		[enc setRenderPipelineState:gate];
		[enc setFragmentTexture:woven atIndex:0];
		[enc setFragmentTexture:atlas_tex atIndex:1];
		[enc setFragmentBytes:&gp length:sizeof(gp) atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		uint8_t g[VW * VH * 4];
		[target getBytes:g bytesPerRow:VW * 4 fromRegion:MTLRegionMake2D(0, 0, VW, VH) mipmapLevel:0];
#define GX(x, y) (g + ((y) * VW + (x)) * 4)
		expect("5 every view transparent -> punched", GX(0, 1), 0, 0, 0, 0, 0);
		expect("6 view 0 opaque only -> woven, alpha 1", GX(1, 1), 100, 110, 120, 255, 0);
		expect("6 both views opaque -> woven, alpha 1", GX(2, 1), 100, 110, 120, 255, 0);
		expect("6 view 1 opaque only -> woven, alpha 1", GX(3, 1), 100, 110, 120, 255, 0);
		// Row 0: col 0 is opaque in view 0 -> kept; col 1 transparent in both -> punched.
		expect("6 row 0 col 0 (view 0 opaque) -> woven", GX(0, 0), 100, 110, 120, 255, 0);
		expect("5 row 0 col 1 (both transparent) -> punched", GX(1, 0), 0, 0, 0, 0, 0);
	}
	printf(g_fail ? "FAILED\n" : "PASS\n");
	return g_fail;
}
