// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Per-display user presence monitor (SR system event 20). See
 *         leia_sr_presence_win.h for the lifecycle and leia_sr_presence.h for
 *         the rules.
 * @ingroup drv_leia
 */

#ifdef DXR_LEIA_HAS_SR_V2

#include "leia_sr_presence_win.h"

#include "leia_display_claims_win.h"
#include "leia_sr_v2_common.h"

#include "util/u_logging.h"

#include <sr/sr_result.h>
#include <sr/sr_system.h>
#include <sr/sr_types.h>

#include <sr/sr_weaver.h>

#include <cstdlib>
#include <mutex>
#include <new>
#include <string.h>

struct leia_sr_presence_monitor
{
	SrSystemMonitor monitor = nullptr;
	//! Guards @ref state: the add-callback replay (calling thread) can overlap
	//! a live event (SR's delivery thread).
	std::mutex mtx;
	struct leia_sr_presence_state state = {};
	std::atomic<int32_t> *out = nullptr;
	const char *arm = "?";
	//! One-shot WARN: the first event that named this DP's display.
	bool logged_first = false;
};

namespace {

#ifdef DXR_LEIA_HAS_SR_USER_PRESENCE
constexpr int kUserPresenceEvent = (int)SR_EVENT_TYPE_DISPLAY_USER_PRESENCE;
#else
// SDK predates event 20: the capability query answers "unsupported", so no
// monitor is ever attached; -1 matches no SR event if one somehow were.
constexpr int kUserPresenceEvent = -1;
#endif

void SR_CALL
on_system_event(const SrSystemEvent *event, void *user_data)
{
	leia_sr_presence_monitor *m = static_cast<leia_sr_presence_monitor *>(user_data);
	if (m == nullptr || event == nullptr) {
		return;
	}
	if ((int)event->eventType != kUserPresenceEvent) {
		return;
	}
	struct leia_sr_presence_msg msg = {};
	if (!leia_sr_presence_parse(event->message, event->messageLength, &msg)) {
		return;
	}
	std::lock_guard<std::mutex> lock(m->mtx);
	if (!leia_sr_presence_matches(&m->state, &msg)) {
		return; // another display's camera
	}
	const enum leia_sr_presence before = m->state.state;
	if (!leia_sr_presence_feed(&m->state, event->timeUs, &msg)) {
		return; // unchanged, or older than what already applied
	}
	const enum leia_sr_presence after = m->state.state;
	m->out->store((int32_t)after, std::memory_order_relaxed);
	// Edges only, never per frame; but SR does not debounce a marginal face,
	// so after the first line the transitions go to INFO.
	const char *name = after == LEIA_SR_PRESENCE_PRESENT  ? "PRESENT"
	                   : after == LEIA_SR_PRESENCE_ABSENT ? "ABSENT"
	                                                      : "UNKNOWN (tracker not seen present yet)";
	if (!m->logged_first) {
		m->logged_first = true;
		U_LOG_W("SR %s presence: display 0x%016llx ('%s') first per-display change present=%d -> %s", m->arm,
		        (unsigned long long)m->state.display_id, m->state.serial, (int)msg.present, name);
	} else {
		U_LOG_I("SR %s presence: display 0x%016llx %d -> %s", m->arm, (unsigned long long)m->state.display_id,
		        (int)before, name);
	}
}

/*!
 * How many SR displays the plug-in's enumeration sees (-1 = it cannot
 * enumerate), and @p display_id's FPC serial for an event whose id is 0
 * (empty when SR does not know it).
 */
int32_t
enumerate(uint64_t display_id, char *out, size_t cap)
{
	out[0] = '\0';
	struct leia_win_sr_display d[LEIA_WIN_SR_MAX_DISPLAYS];
	const int32_t n = leia_win_sr_enumerate_displays(d, LEIA_WIN_SR_MAX_DISPLAYS);
	for (int32_t i = 0; display_id != 0 && i < n && i < LEIA_WIN_SR_MAX_DISPLAYS; i++) {
		if (d[i].display_id == display_id) {
			size_t k = 0;
			while (k + 1 < cap && k < sizeof(d[i].serial) && d[i].serial[k] != '\0') {
				out[k] = d[i].serial[k];
				k++;
			}
			out[k] = '\0';
			break;
		}
	}
	return n;
}

} // namespace

struct leia_sr_presence_monitor *
leia_sr_presence_monitor_create(
    SrInstance instance, bool user_presence_cap, uint64_t display_id, const char *arm, std::atomic<int32_t> *out_state)
{
	if (out_state == nullptr) {
		return nullptr;
	}
	out_state->store((int32_t)LEIA_SR_PRESENCE_UNKNOWN, std::memory_order_relaxed);
	if (instance == nullptr || !user_presence_cap) {
		// The common case on every SR runtime before the q4 line: today's
		// eye-pair heuristic, unchanged. The caps line already says so.
		return nullptr;
	}

	/*
	 * Multi-screen only. With one SR display the per-display event says
	 * nothing the eye pair does not, and the pair's loss edge is aligned with
	 * the weaver's own 2D fallback (MANAGED grace), which the raw, undebounced
	 * event is not. So a single-screen box keeps today's signal exactly; the
	 * event only takes over where screens can disagree. Counted at create: a
	 * display hot-plugged later engages with the next weaver this DP creates.
	 */
	char serial[LEIA_SR_PRESENCE_SERIAL_MAX];
	const int32_t sr_displays = enumerate(display_id, serial, sizeof(serial));
	if (sr_displays < 2) {
		U_LOG_W("SR %s presence: the runtime offers per-display user presence but SR enumerates %d display(s) - "
		        "display 0x%016llx keeps the eye-pair heuristic (single screen)",
		        arm != nullptr ? arm : "?", (int)sr_displays, (unsigned long long)display_id);
		return nullptr;
	}

	leia_sr_presence_monitor *m = new (std::nothrow) leia_sr_presence_monitor();
	if (m == nullptr) {
		return nullptr;
	}
	m->out = out_state;
	m->arm = arm != nullptr ? arm : "?";
	leia_sr_presence_init(&m->state, display_id, serial);

	SrSystemMonitorCreateInfo ci{};
	ci.sType = SR_TYPE_SYSTEM_MONITOR_CREATE_INFO;
	ci.pNext = nullptr;
	const SrResult cr = srCreateSystemMonitor(instance, &ci, &m->monitor);
	if (!SR_SUCCEEDED(cr) || m->monitor == nullptr) {
		U_LOG_W("SR %s presence: srCreateSystemMonitor failed (%s) - display 0x%016llx keeps the eye-pair "
		        "heuristic",
		        m->arm, leia_sr_v2_result_str(cr), (unsigned long long)display_id);
		delete m;
		return nullptr;
	}
	// May replay every display's last state on THIS thread before returning
	// (SR documents it); the callback only takes m->mtx, which we do not hold.
	const SrResult ar = srSystemMonitorAddCallback(m->monitor, on_system_event, m);
	if (!SR_SUCCEEDED(ar)) {
		U_LOG_W("SR %s presence: srSystemMonitorAddCallback failed (%s) - display 0x%016llx keeps the eye-pair "
		        "heuristic",
		        m->arm, leia_sr_v2_result_str(ar), (unsigned long long)display_id);
		srDestroySystemMonitor(m->monitor);
		delete m;
		out_state->store((int32_t)LEIA_SR_PRESENCE_UNKNOWN, std::memory_order_relaxed);
		return nullptr;
	}
	U_LOG_W("SR %s presence: per-display user presence ENGAGED for display 0x%016llx (serial '%s') - this DP's "
	        "is_tracking follows its own camera once it has reported a face",
	        m->arm, (unsigned long long)display_id, m->state.serial);
	return m;
}

void
leia_sr_presence_monitor_destroy(struct leia_sr_presence_monitor **monitor_ptr)
{
	if (monitor_ptr == nullptr || *monitor_ptr == nullptr) {
		return;
	}
	leia_sr_presence_monitor *m = *monitor_ptr;
	*monitor_ptr = nullptr;
	if (m->monitor != nullptr) {
		// Callback off first: after this returns no delivery can reach m.
		srSystemMonitorRemoveCallback(m->monitor, on_system_event, m);
		srDestroySystemMonitor(m->monitor);
		m->monitor = nullptr;
	}
	if (m->out != nullptr) {
		m->out->store((int32_t)LEIA_SR_PRESENCE_UNKNOWN, std::memory_order_relaxed);
	}
	delete m;
}

/*
 * Weaver tracking state + policy (LeiaSR tracker-rest gate, PR 2).
 */

#ifdef SR_WEAVER_TRACKING_POLICY_AVAILABLE
namespace {

//! Set once the SR runtime answered FUNCTION_UNSUPPORTED: an older runtime
//! will not grow the slot mid-process, so stop asking.
std::atomic<bool> g_tracking_query_unsupported{false};
std::atomic<bool> g_tracking_policy_unsupported{false};

//! -1 = unset/auto (no call), else an SrTrackingPolicy value.
int
tracking_policy_from_env()
{
	const char *e = std::getenv("DXR_LEIA_TRACKING_POLICY");
	if (e == nullptr || e[0] == '\0' || _stricmp(e, "auto") == 0) {
		return -1;
	}
	if (_stricmp(e, "always_on") == 0 || _stricmp(e, "on") == 0) {
		return (int)SR_TRACKING_POLICY_ALWAYS_ON;
	}
	if (_stricmp(e, "off") == 0) {
		return (int)SR_TRACKING_POLICY_OFF;
	}
	static std::atomic<bool> warned{false};
	if (!warned.exchange(true)) {
		U_LOG_W("DXR_LEIA_TRACKING_POLICY='%s' not recognised (auto | always_on | off); using auto", e);
	}
	return -1;
}

} // namespace
#endif

enum leia_sr_weaver_tracking
leia_sr_v2_weaver_tracking(SrWeaver weaver)
{
#ifdef SR_WEAVER_TRACKING_POLICY_AVAILABLE
	if (weaver == nullptr || g_tracking_query_unsupported.load(std::memory_order_relaxed)) {
		return LEIA_SR_WEAVER_TRACKING_UNKNOWN;
	}
	SrTrackingState st = SR_TRACKING_STATE_TRACKING;
	SrResult r = SR_ERROR_HANDLE_INVALID;
	try {
		r = srWeaverGetTrackingState(weaver, &st);
	} catch (...) {
		return LEIA_SR_WEAVER_TRACKING_UNKNOWN;
	}
	if (r == SR_ERROR_FUNCTION_UNSUPPORTED) {
		if (!g_tracking_query_unsupported.exchange(true)) {
			U_LOG_W("SR weaver tracking state: unsupported by this SR runtime; presence alone decides is_tracking");
		}
		return LEIA_SR_WEAVER_TRACKING_UNKNOWN;
	}
	if (!SR_SUCCEEDED(r)) {
		return LEIA_SR_WEAVER_TRACKING_UNKNOWN;
	}
	switch (st) {
	case SR_TRACKING_STATE_TRACKING: return LEIA_SR_WEAVER_TRACKING_TRACKING;
	case SR_TRACKING_STATE_LOST: return LEIA_SR_WEAVER_TRACKING_LOST;
	case SR_TRACKING_STATE_RESTED: return LEIA_SR_WEAVER_TRACKING_RESTED;
	default: return LEIA_SR_WEAVER_TRACKING_UNKNOWN;
	}
#else
	(void)weaver;
	return LEIA_SR_WEAVER_TRACKING_UNKNOWN;
#endif
}

void
leia_sr_v2_weaver_apply_tracking_policy(SrWeaver weaver, const char *arm)
{
#ifdef SR_WEAVER_TRACKING_POLICY_AVAILABLE
	const int policy = tracking_policy_from_env();
	if (weaver == nullptr || policy < 0 || g_tracking_policy_unsupported.load(std::memory_order_relaxed)) {
		return;
	}
	SrResult r = SR_ERROR_HANDLE_INVALID;
	try {
		r = srWeaverSetTrackingPolicy(weaver, (SrTrackingPolicy)policy);
	} catch (...) {
		return;
	}
	const char *name = policy == (int)SR_TRACKING_POLICY_OFF ? "off" : "always_on";
	if (r == SR_ERROR_FUNCTION_UNSUPPORTED) {
		if (!g_tracking_policy_unsupported.exchange(true)) {
			U_LOG_W("SR %s weaver: DXR_LEIA_TRACKING_POLICY=%s ignored, this SR runtime has no tracking policy",
			        arm, name);
		}
		return;
	}
	if (SR_SUCCEEDED(r)) {
		U_LOG_W("SR %s weaver: tracking policy %s (DXR_LEIA_TRACKING_POLICY)", arm, name);
	} else {
		U_LOG_W("SR %s weaver: tracking policy %s refused: %s (%d)", arm, name, leia_sr_v2_result_str(r), (int)r);
	}
#else
	(void)weaver;
	(void)arm;
#endif
}

#endif // DXR_LEIA_HAS_SR_V2
