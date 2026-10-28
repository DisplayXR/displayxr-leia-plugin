// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The pure decisions behind SR-readiness behaviours of the Windows
 *         arm: when a weaver create retry stops waiting (#468). Plain C, no
 *         SDK or Win32 types, so the rules are host-testable
 *         (tests/test_sr_ready_rules.c); leia_sr_ready.cpp feeds them.
 *
 * ## #468: wake the retry on identification
 *
 * After a long unplug SR takes 10-30 s to identify the panel again. The D3D11
 * async create retries with a 5 s SR spin and then sleeps 5 s, so an
 * identification that lands in the sleep waited up to 5 s on the flat blit.
 * The sleep now ends as soon as SR identifies the panel: the shared-memory
 * count goes 0 -> >= 1, or verified geometry is published (the readiness
 * watcher's fresh-handle query succeeded; this covers SR keeping the count
 * across an unplug, where there is no edge to see).
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *
 * #468: the retry wake.
 *
 */

//! What a weaver create retry saw when it started waiting.
struct leiasr_identify_mark
{
	bool identified;        //!< the SR shared-memory count was >= 1
	uint32_t publish_epoch; //!< verified-geometry publishes so far
};

enum leiasr_identify_wake
{
	LEIASR_IDENTIFY_WAKE_NONE = 0,
	LEIASR_IDENTIFY_WAKE_COUNT = 1,   //!< the shared-memory count went 0 -> >= 1
	LEIASR_IDENTIFY_WAKE_PUBLISH = 2, //!< verified geometry was published
};

/*!
 * Has SR identified the panel since @p mark was taken? A count that was
 * already >= 1 at the mark is no news (a failed attempt on an identified
 * panel waits its full period), so only the edge counts; a publish always
 * does.
 */
static inline enum leiasr_identify_wake
leiasr_identify_woke(const struct leiasr_identify_mark *mark, bool identified_now, uint32_t publish_epoch_now)
{
	if (publish_epoch_now != mark->publish_epoch) {
		return LEIASR_IDENTIFY_WAKE_PUBLISH;
	}
	if (!mark->identified && identified_now) {
		return LEIASR_IDENTIFY_WAKE_COUNT;
	}
	return LEIASR_IDENTIFY_WAKE_NONE;
}

static inline const char *
leiasr_identify_wake_name(enum leiasr_identify_wake w)
{
	switch (w) {
	case LEIASR_IDENTIFY_WAKE_COUNT: return "shared-memory count 0 -> 1";
	case LEIASR_IDENTIFY_WAKE_PUBLISH: return "verified geometry published";
	default: return "none";
	}
}

#ifdef __cplusplus
}
#endif
