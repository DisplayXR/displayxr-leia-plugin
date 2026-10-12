// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Per-display user presence (SR system event 20): message parsing and
 *         the per-DP present / absent / unknown state. Plain C, no SDK types,
 *         so the rules are host-testable (tests/test_sr_presence.c); the
 *         SDK-typed monitor that feeds them is leia_sr_presence_win.cpp.
 *
 * ## Why
 *
 * On a multi-screen rig (laptop panel + DS1) each screen's DP must say "a
 * viewer is present at MY screen" from its OWN camera. Before this, the only
 * per-DP signal was the eye-pair heuristic (a collapsed pair = lost), which
 * follows whatever presence the SR weaver itself acts on, and the only
 * presence events were SR's machine-wide USER_FOUND / USER_LOST (an OR across
 * trackers on a new service, the active display alone on an old one). So
 * covering the laptop camera could read as "viewer lost" on the other panel.
 *
 * LeiaSR (q4 line, 1.39.0+2344 on) raises SR_EVENT_TYPE_DISPLAY_USER_PRESENCE
 * per display on every change, with no debounce (SR owns any smoothing; the
 * plug-in deliberately adds none), and replays every display's last state when
 * a callback is added. It is capability-gated:
 * SrDisplayUserPresenceCapabilities::displayUserPresenceEvent.
 *
 * ## The three states, and why "unknown" exists
 *
 * A display's tracker runs only while some client holds an eye tracker bound
 * to it; otherwise it is paused ("Camera resting") and reports present=0. That
 * 0 means "nobody is looking", not "the viewer left". So:
 *
 * - **UNKNOWN** until this DP has seen its display report present=1 at least
 *   once since the monitor attached. The add-callback replay arrives before
 *   the DP's own senses start (srInitialize runs after the senses exist), i.e.
 *   while the tracker may still be resting: its 0 is exactly the paused 0.
 *   Also UNKNOWN when the capability is absent, the event never names this
 *   display, or the DP has no tracker of its own (a pinned-viewer segment
 *   never initialises its senses, so it attaches no monitor at all).
 * - **PRESENT** on present=1.
 * - **ABSENT** on present=0 after the first 1.
 *
 * UNKNOWN falls back to the eye-pair heuristic, i.e. exactly today's
 * behaviour, which is also what the runtime would do with "no answer", so
 * "unknown" never has to cross the plug-in ABI (see @ref leia_sr_presence_apply).
 *
 * ## Matching an event to this DP
 *
 * The message is `display=<id> serial=<serial> present=<0|1>`, id in decimal
 * as srEnumerateDisplays reports it to the RECEIVING instance. The monitor is
 * created on the DP's own SR instance, the same one that resolved the DP's
 * display (leia_sr_v2_display_info::display_id), so a non-zero id is matched
 * by id. id 0 ("the runtime cannot name the display") falls back to the FPC
 * serial when this DP knows its display's serial. Per display only the event
 * with the latest timeUs counts: the replay (calling thread) can overlap a
 * live event (delivery thread).
 *
 * @ingroup drv_leia
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SR enum / struct-tag values this needs, as plain integers: the pinned SDK
 * (sr-sdk-v2-1.38.0.x) predates them. Values per LeiaSR
 * release-candidate/david-q4-2026. The SDK-typed side static_asserts them
 * against the real header when the SDK has it
 * (DXR_LEIA_SDK_HAS_SR_USER_PRESENCE). TODO: drop these once the SR v2 pin
 * moves to sr-sdk-v2-1.39.0.x and use the SDK names directly.
 */
#define LEIA_SR_EVENT_DISPLAY_USER_PRESENCE 20
#define LEIA_SR_TYPE_DISPLAY_USER_PRESENCE_CAPABILITIES 32

//! The serial field's capacity (SR: printable ASCII, no spaces).
#define LEIA_SR_PRESENCE_SERIAL_MAX 64

enum leia_sr_presence
{
	LEIA_SR_PRESENCE_UNKNOWN = 0, //!< No usable answer: fall back to the eye pair.
	LEIA_SR_PRESENCE_ABSENT = 1,  //!< This display's own tracker sees no face.
	LEIA_SR_PRESENCE_PRESENT = 2, //!< This display's own tracker sees a face.
};

//! One parsed event-20 message.
struct leia_sr_presence_msg
{
	uint64_t display_id; //!< 0 = the runtime could not name the display.
	char serial[LEIA_SR_PRESENCE_SERIAL_MAX];
	bool present;
};

//! Match literal @p lit at @p *pos (advancing past it).
static inline bool
leia_sr_presence_eat(const char *msg, uint32_t len, uint32_t *pos, const char *lit)
{
	const size_t n = strlen(lit);
	if (*pos + n > len || memcmp(msg + *pos, lit, n) != 0) {
		return false;
	}
	*pos += (uint32_t)n;
	return true;
}

/*!
 * Parse `display=<id> serial=<serial> present=<0|1>` (id decimal). @p len is
 * the SDK's messageLength (the message need not be terminated within it).
 * Rejects anything else, notably an older runtime that passes the service's
 * message through without the `display=` field (SR documents that case).
 * Hand-rolled rather than sscanf (MSVC C4996, and no locale surprises).
 */
static inline bool
leia_sr_presence_parse(const char *msg, uint32_t len, struct leia_sr_presence_msg *out)
{
	if (msg == NULL || out == NULL) {
		return false;
	}
	uint32_t pos = 0;
	if (!leia_sr_presence_eat(msg, len, &pos, "display=")) {
		return false;
	}
	uint64_t id = 0;
	uint32_t digits = 0;
	while (pos < len && msg[pos] >= '0' && msg[pos] <= '9') {
		const uint64_t d = (uint64_t)(msg[pos] - '0');
		if (id > (UINT64_MAX - d) / 10) {
			return false; // overflow
		}
		id = id * 10 + d;
		pos++;
		digits++;
	}
	if (digits == 0 || !leia_sr_presence_eat(msg, len, &pos, " serial=")) {
		return false;
	}
	char serial[LEIA_SR_PRESENCE_SERIAL_MAX] = {0};
	uint32_t n = 0;
	while (pos < len && msg[pos] != ' ' && msg[pos] != '\0') {
		if (n + 1 >= sizeof(serial)) {
			return false;
		}
		serial[n++] = msg[pos++];
	}
	if (!leia_sr_presence_eat(msg, len, &pos, " present=") || pos >= len) {
		return false;
	}
	const char p = msg[pos++];
	if (p != '0' && p != '1') {
		return false;
	}
	// Nothing but a terminator / whitespace may follow.
	while (pos < len && msg[pos] != '\0') {
		if (msg[pos] != ' ' && msg[pos] != '\r' && msg[pos] != '\n') {
			return false;
		}
		pos++;
	}
	out->display_id = id;
	memcpy(out->serial, serial, sizeof(out->serial));
	out->present = p == '1';
	return true;
}

/*!
 * One DP's presence state. Initialise with @ref leia_sr_presence_init.
 */
struct leia_sr_presence_state
{
	uint64_t display_id; //!< The DP's display as its SR instance names it (0 = unknown).
	char serial[LEIA_SR_PRESENCE_SERIAL_MAX]; //!< Its FPC serial, "" = unknown.
	uint64_t last_time;  //!< timeUs of the newest event applied.
	bool have_time;      //!< @ref last_time is meaningful.
	bool seen_present;   //!< present=1 has arrived since attach (see file comment).
	enum leia_sr_presence state;
};

static inline void
leia_sr_presence_init(struct leia_sr_presence_state *s, uint64_t display_id, const char *serial)
{
	memset(s, 0, sizeof(*s));
	s->display_id = display_id;
	if (serial != NULL) {
		size_t n = strlen(serial);
		if (n >= sizeof(s->serial)) {
			n = sizeof(s->serial) - 1;
		}
		memcpy(s->serial, serial, n);
	}
	s->state = LEIA_SR_PRESENCE_UNKNOWN;
}

//! Does @p m name this DP's display?
static inline bool
leia_sr_presence_matches(const struct leia_sr_presence_state *s, const struct leia_sr_presence_msg *m)
{
	if (m->display_id != 0) {
		return s->display_id != 0 && m->display_id == s->display_id;
	}
	// The runtime could not name it: the serial is the only key left.
	return s->serial[0] != '\0' && m->serial[0] != '\0' && strcmp(s->serial, m->serial) == 0;
}

/*!
 * Feed one event. @p time is the event's timeUs (a wall-clock tick; only ever
 * compared with other events' timeUs, never with any other clock).
 *
 * @return true when this DP's state changed.
 */
static inline bool
leia_sr_presence_feed(struct leia_sr_presence_state *s, uint64_t time, const struct leia_sr_presence_msg *m)
{
	if (!leia_sr_presence_matches(s, m)) {
		return false;
	}
	if (s->have_time && time < s->last_time) {
		// An older state (the replay racing a live event): the newer one
		// already applied wins.
		return false;
	}
	s->have_time = true;
	s->last_time = time;

	enum leia_sr_presence next;
	if (m->present) {
		s->seen_present = true;
		next = LEIA_SR_PRESENCE_PRESENT;
	} else {
		// 0 before the first 1 is a resting / starting tracker, not a viewer
		// who left.
		next = s->seen_present ? LEIA_SR_PRESENCE_ABSENT : LEIA_SR_PRESENCE_UNKNOWN;
	}
	const bool changed = next != s->state;
	s->state = next;
	return changed;
}

/*!
 * The DP's is_tracking: its own display's presence when known, else the
 * eye-pair heuristic (today's behaviour, byte for byte).
 *
 * @param p                  This DP's presence.
 * @param eye_pair_tracking  The collapsed-pair heuristic on the raw pair.
 */
static inline bool
leia_sr_presence_apply(enum leia_sr_presence p, bool eye_pair_tracking)
{
	switch (p) {
	case LEIA_SR_PRESENCE_PRESENT: return true;
	case LEIA_SR_PRESENCE_ABSENT: return false;
	default: return eye_pair_tracking;
	}
}

#ifdef __cplusplus
}
#endif
