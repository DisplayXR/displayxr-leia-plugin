// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Chooses, once per process, whether this plug-in drives SR through the
 *         legacy C++ interfaces (v1) or the v2 C99 API.
 * @ingroup drv_leia
 *
 * ## Why a selector rather than a compile-time switch
 *
 * The v2 C99 API (`SrInstance` / `SrWeaver`, `modules/srSDK`) is the direction
 * of travel: its C ABI plus an append-only dispatch table removes the whole
 * problem `leia_vk_weaver_select.cpp` exists to work around — see that file's
 * header for how a same-name virtual overload silently shifted an ABI. But the
 * v2 runtime (`LeiaSR_runtime.dll`) is only present on newer SR Platforms, and
 * users install the Platform themselves, so the plug-in must keep working on
 * machines that do not have it. Hence: both paths compiled in, one chosen.
 *
 * ## The rule this file exists to enforce
 *
 * **Resolve once, log once, and make both paths reachable on demand.**
 *
 * A dual-path design is how you get two implementations that quietly disagree.
 * That failure already cost this project days: a phase snap that silently
 * returned its input was indistinguishable from one that had nothing to correct
 * (LeiaInc/LeiaSR#163). So:
 *
 * - the decision is made in ONE place, not ad-hoc per call site;
 * - it is logged loudly the first time, with the reason, so "which path am I
 *   on?" is answerable from a log rather than inferred;
 * - `DXR_LEIA_SR_API` forces either path, so both are testable **on the same
 *   binary** instead of only reachable by having the right Platform installed.
 *   Every arm's verification runs `=v1` and `=v2` and compares.
 *
 * ## Availability
 *
 * `DXR_LEIA_HAS_SR_V2` is defined by CMake only when the v2 SDK headers are
 * present. Without it this whole layer compiles down to "always v1", so the
 * tree still builds against the pinned C++-only SDK.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//! Which SR API family a call site should use.
enum leia_sr_api
{
	//! Legacy C++ interfaces: `SR::SRContext`, `SR::IDX11Weaver1`, ...
	LEIA_SR_API_V1 = 0,
	//! v2 C99 API: `SrInstance`, `SrWeaver`, `srXxx()`.
	LEIA_SR_API_V2 = 1,
};

/*!
 * The API family this process uses. Resolved on first call (thread-safe) and
 * constant thereafter — callers may cache it.
 *
 * Returns @ref LEIA_SR_API_V1 whenever v2 is unavailable, unusable, or forced
 * off, so a caller that ignores the distinction still gets a working path.
 */
enum leia_sr_api
leia_sr_api_selected(void);

/*!
 * Human-readable reason the current API was chosen, e.g.
 * `"v2 runtime 1.37.0+1431 available"` or `"DXR_LEIA_SR_API=v1 (forced)"`.
 * Never NULL. For diagnostics and bug reports — do not branch on the text.
 */
const char *
leia_sr_api_reason(void);

#ifdef __cplusplus
}
#endif
