// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief Push-constant block of shaders/compose_under_bg.frag.
 *
 * One definition shared by the DP (leia_display_processor_linux.c) and the
 * GPU unit test (tests/test_compose_color_linux.c), so the test drives the
 * shader with exactly the layout the plug-in uses. std430 push-constant
 * layout: every member here is 4-byte aligned, vec2 members 8-byte aligned,
 * and the order must match the GLSL block member for member.
 */

#pragma once

#include <stdint.h>

struct leia_compose_push
{
	float bg_uv_origin[2];  //!< window TL on the captured monitor, normalised
	float bg_uv_extent[2];  //!< window size on the captured monitor, normalised
	uint32_t tile_count[2]; //!< (tile_columns, tile_rows)
	uint32_t has_backdrop;  //!< 1 = composite the 2D-under backdrop over the desktop
	uint32_t debug_bg_only; //!< DXR_LEIA_BG_DEBUG=1: output the background only
	uint32_t atlas_linear;  //!< 1 = runtime declared the atlas LINEAR (runtime#1484), else ENCODED
	uint32_t bg_is_atlas_space; //!< 1 = the background is the runtime's 2D-under (atlas encoding), 0 = desktop (sRGB)
};

_Static_assert(sizeof(struct leia_compose_push) == 40, "must match compose_under_bg.frag's push_constant block");
