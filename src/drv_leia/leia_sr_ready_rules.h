// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The pure decisions behind two SR-readiness behaviours of the Windows
 *         arm: when a weaver create retry stops waiting (#468), and which
 *         geometry a display processor reports while its weaver is not up yet
 *         (#469). Plain C, no SDK or Win32 types, so the rules are
 *         host-testable (tests/test_sr_ready_rules.c); leia_sr_ready.cpp feeds
 *         them.
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
 * ## #469: report the panel's kept geometry while its weaver waits
 *
 * While the weaver is pending the DP had no geometry to report, so the
 * runtime had no screen and rendered a symmetric default FOV for the whole
 * wait (31 s in a 90 s-unplug run). The geometry SR last verified is kept
 * with the EDID identity of the panel it was verified on; P-c still drops the
 * VERIFIED geometry at an unplug, the kept copy survives it. A pending DP
 * reports, in order:
 *
 * - nothing, while no Leia panel is attached, or (a per-screen DP) while the
 *   attached panel is not at the binding's desktop rect;
 * - the verified geometry, when there is one;
 * - the kept geometry, when the attached panel has the identity it was
 *   verified on (manufacturer, product, desktop size);
 * - nothing otherwise: a panel never verified in this process gets no
 *   invented geometry.
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *
 * #468: the retry wake.
 *
 */

//! What SR had identified when a weaver create attempt started (the retry
//! wait after a failed attempt compares against it, so an identification
//! during the attempt wakes the wait at once).
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
 * Has SR identified the panel since @p mark was taken (just before the create
 * attempt)? A count that was already >= 1 at the mark is no news (a failed
 * attempt on an identified panel waits its full period), so only the edge
 * counts; a publish always does.
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

/*
 *
 * #469: the geometry a pending DP reports.
 *
 */

//! A desktop rect in the space windows are placed in. (0, 0) is a real
//! origin (a primary-display panel), so validity is out of band.
struct leia_desktop_rect
{
	bool valid;
	int32_t left;
	int32_t top;
	uint32_t width;
	uint32_t height;
};

/*!
 * A Leia panel as the EDID table matched it (P-c).
 *
 * Limitation, one panel per process: P-c tracks ONE matched panel (the first
 * EDID-table hit), and a publish pairs SR's verified geometry with it. With
 * two Leia panels of different models attached, the geometry SR verified may
 * be the other panel's, so the kept pair can name the wrong panel and a
 * pending DP would report that geometry for this one. Rare (one Leia panel
 * per machine is the supported setup), so it is left as is. The identity has
 * no serial either: the Windows EDID enumeration the plug-in builds against
 * does not fill bytes 12-15 (and the DS1 reports 0), so two panels of the same
 * model and desktop size are one panel to this check. If either matters, key
 * the kept geometry by the panel SR verified on, plus the serial once the
 * enumeration provides it (0 = unknown: match on the rest).
 */
struct leia_panel_identity
{
	bool valid;
	uint16_t manufacturer_id;
	uint16_t product_id;
	//! Where it is on the desktop now. The size is part of the identity (as
	//! for P-c), the position is not: a moved panel is the same panel.
	struct leia_desktop_rect rect;
};

static inline bool
leia_panel_identity_same(const struct leia_panel_identity *a, const struct leia_panel_identity *b)
{
	return a->valid && b->valid && a->manufacturer_id == b->manufacturer_id && a->product_id == b->product_id &&
	       a->rect.width == b->rect.width && a->rect.height == b->rect.height;
}

enum leiasr_pending_geometry_source
{
	LEIASR_PENDING_GEOMETRY_NONE = 0,     //!< report nothing (as before #469)
	LEIASR_PENDING_GEOMETRY_VERIFIED = 1, //!< SR verified it for the attached panel
	LEIASR_PENDING_GEOMETRY_KEPT = 2,     //!< verified earlier on the same panel
};

/*!
 * Which geometry a DP whose weaver is not ready reports (see the file
 * comment).
 *
 * @param verified_valid The process geometry is verified (P-c drops it on an
 *                       unplug or a panel change, so it belongs to the panel
 *                       matched now).
 * @param kept_valid     A geometry was verified earlier in this process.
 * @param kept_on        The panel it was verified on.
 * @param attached       The panel the EDID table matches now (invalid: none).
 * @param bound          A per-screen DP's binding rect (NULL or invalid: not
 *                       per-screen).
 */
static inline enum leiasr_pending_geometry_source
leiasr_pending_geometry_pick(bool verified_valid,
                             bool kept_valid,
                             const struct leia_panel_identity *kept_on,
                             const struct leia_panel_identity *attached,
                             const struct leia_desktop_rect *bound)
{
	if (attached == NULL || !attached->valid) {
		return LEIASR_PENDING_GEOMETRY_NONE;
	}
	if (bound != NULL && bound->valid &&
	    (bound->left != attached->rect.left || bound->top != attached->rect.top ||
	     bound->width != attached->rect.width || bound->height != attached->rect.height)) {
		return LEIASR_PENDING_GEOMETRY_NONE;
	}
	if (verified_valid) {
		return LEIASR_PENDING_GEOMETRY_VERIFIED;
	}
	if (kept_valid && kept_on != NULL && leia_panel_identity_same(kept_on, attached)) {
		return LEIASR_PENDING_GEOMETRY_KEPT;
	}
	return LEIASR_PENDING_GEOMETRY_NONE;
}

static inline const char *
leiasr_pending_geometry_source_name(enum leiasr_pending_geometry_source s)
{
	switch (s) {
	case LEIASR_PENDING_GEOMETRY_VERIFIED: return "verified";
	case LEIASR_PENDING_GEOMETRY_KEPT: return "kept";
	default: return "none";
	}
}

/*
 * The attached-panel snapshot a pending DP reads. The DP getters run on the
 * render thread every frame while the weaver is pending, so they never
 * refresh the platform state themselves (registry, SR Service status, the
 * EDID enumeration on a monitor-set change, all under the platform state's
 * exclusive lock). A refresher thread does it every
 * LEIASR_PENDING_REFRESH_MS while a pending DP or a create retry asked for it
 * within LEIASR_PENDING_IDLE_STOP_MS, and keeps the attached panel; the
 * getters read that copy. A snapshot older than
 * LEIASR_PENDING_SNAPSHOT_MAX_AGE_MS (or none yet) reads as "no panel
 * attached": nothing is reported rather than a panel that may have gone.
 */
#define LEIASR_PENDING_REFRESH_MS 100
#define LEIASR_PENDING_SNAPSHOT_MAX_AGE_MS 500
#define LEIASR_PENDING_IDLE_STOP_MS 2000

//! May a pending DP trust the attached-panel snapshot taken at @p snapshot_ms (0 = none yet)?
static inline bool
leiasr_pending_snapshot_fresh(uint64_t now_ms, uint64_t snapshot_ms)
{
	if (snapshot_ms == 0) {
		return false;
	}
	if (snapshot_ms >= now_ms) {
		return true; // taken just after the reader read its clock
	}
	return now_ms - snapshot_ms <= LEIASR_PENDING_SNAPSHOT_MAX_AGE_MS;
}

//! Keep the refresher running? Only while someone asked at @p demand_ms (0 = never) recently.
static inline bool
leiasr_pending_refresher_wanted(uint64_t now_ms, uint64_t demand_ms)
{
	if (demand_ms == 0) {
		return false;
	}
	if (demand_ms >= now_ms) {
		return true; // asked just after the refresher read its clock
	}
	return now_ms - demand_ms < LEIASR_PENDING_IDLE_STOP_MS;
}

#ifdef __cplusplus
}
#endif
