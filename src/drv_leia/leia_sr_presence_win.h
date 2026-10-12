// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Per-display user presence monitor (SR system event 20) for one DP's
 *         SR v2 instance. The rules live in leia_sr_presence.h; this is only
 *         the SDK plumbing: capability gate, system monitor, callback.
 * @ingroup drv_leia
 *
 * Lifecycle, per weaver arm (D3D11 / D3D12 / GL / Vulkan):
 *
 * - **Create** between the weaver create and `leia_sr_v2_initialize`: the SDK
 *   requires senses AND their callbacks to exist before srInitialize starts
 *   them (same window the predict-trace recorder uses). Only on a DP whose
 *   instance is initialised, i.e. one with a tracker of its own; a pinned
 *   (untracked) segment attaches nothing and stays UNKNOWN.
 * - **Read** from any thread via the caller's `std::atomic<int32_t>`; it lives
 *   in the arm's own struct, so a reader never touches the monitor and a
 *   destroy racing a frame cannot be a use-after-free.
 * - **Destroy** before the instance (removes the callback first, then the
 *   monitor), and on the #158 in-place reconnect with the rest of the SDK
 *   objects. The published state goes back to UNKNOWN.
 *
 * Absent capability (SR runtime older than the q4 line, or a plug-in built
 * without the multi-screen caps query), or a box where SR enumerates fewer
 * than two displays = no monitor, UNKNOWN forever: the DP keeps today's
 * eye-pair heuristic exactly.
 */
#pragma once

#ifdef DXR_LEIA_HAS_SR_V2

#include "leia_sr_presence.h"

#include <sr/sr_instance.h>

#include <atomic>
#include <stdint.h>

struct leia_sr_presence_monitor;

/*!
 * Attach a monitor for @p display_id to @p instance (not yet initialised).
 *
 * @param instance          The DP's own SR instance.
 * @param user_presence_cap leia_win_sr_multi_caps::display_user_presence.
 * @param display_id        The DP's display as @p instance names it
 *                          (leia_sr_v2_display_info::display_id); 0 = unknown,
 *                          then only a serial match can ever apply.
 * @param arm               "D3D11" / "D3D12" / "GL" / "VK", for the log.
 * @param out_state         Published leia_sr_presence; set to UNKNOWN here.
 * @return NULL when the capability is absent or the SDK refused (logged once);
 *         the DP then simply has no per-display presence.
 */
struct leia_sr_presence_monitor *
leia_sr_presence_monitor_create(SrInstance instance,
                                bool user_presence_cap,
                                uint64_t display_id,
                                const char *arm,
                                std::atomic<int32_t> *out_state);

/*!
 * Remove the callback, destroy the monitor, publish UNKNOWN, NULL the pointer.
 * Safe on NULL. Must run before the instance is destroyed.
 */
void
leia_sr_presence_monitor_destroy(struct leia_sr_presence_monitor **monitor_ptr);

//! The published state as the enum (relaxed load; one int).
static inline enum leia_sr_presence
leia_sr_presence_load(const std::atomic<int32_t> &state)
{
	return (enum leia_sr_presence)state.load(std::memory_order_relaxed);
}

#endif // DXR_LEIA_HAS_SR_V2
