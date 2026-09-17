// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Implementation of the opt-in per-weave eye-prediction recorder.
 * @ingroup drv_leia
 *
 * See leia_sr_predict_trace.h for what this is and the four rules it obeys.
 * This file is the machinery: three single-producer/single-consumer rings, a
 * low-priority writer thread, and a CSV.
 */

#include "leia_sr_predict_trace.h"

#ifdef DXR_LEIA_HAS_SR_V2

#include "leia_interface.h"
#include "leia_sr_v2_common.h"

#include "util/u_logging.h"

#include <sr/sr_eye_tracker.h>
#include <sr/sr_result.h>
#include <sr/sr_system.h>
#include <sr/sr_weaver.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

#include <windows.h>

namespace {

/* ------------------------------------------------------------------ *
 * Ring
 *
 * Single producer (one SDK thread, or the weave thread), single consumer
 * (the writer thread). Power-of-two capacity so the index wrap is a mask.
 *
 * OVERWRITE-OLDEST IS DELIBERATELY NOT AN OPTION. A ring that overwrites
 * turns "the writer fell behind" into a file whose rows are individually
 * plausible and collectively wrong -- the exact shape of diagnostic this
 * project has been burned by. Dropping and counting makes the loss a number
 * in the trailer that the scorer can refuse to work around.
 * ------------------------------------------------------------------ */

template <typename T, uint32_t CAP> struct ring
{
	static_assert((CAP & (CAP - 1)) == 0, "ring capacity must be a power of two");

	T slots[CAP];
	std::atomic<uint32_t> head{0}; //!< producer only
	std::atomic<uint32_t> tail{0}; //!< consumer only
	std::atomic<uint64_t> dropped{0};
	uint64_t written = 0; //!< consumer only

	//! Producer. True when the record was taken.
	bool
	push(const T &v)
	{
		const uint32_t h = head.load(std::memory_order_relaxed);
		const uint32_t t = tail.load(std::memory_order_acquire);
		if ((uint32_t)(h - t) >= CAP) {
			dropped.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		slots[h & (CAP - 1)] = v;
		head.store(h + 1, std::memory_order_release);
		return true;
	}

	//! Consumer. True when @p out was filled.
	bool
	pop(T *out)
	{
		const uint32_t t = tail.load(std::memory_order_relaxed);
		const uint32_t h = head.load(std::memory_order_acquire);
		if (t == h) {
			return false;
		}
		*out = slots[t & (CAP - 1)];
		tail.store(t + 1, std::memory_order_release);
		return true;
	}
};

//! Tracker sample, exactly as delivered. No transforms -- the scorer wants raw.
struct t_row
{
	uint64_t frame_id;
	uint64_t time_us;
	double l[3];
	double r[3];
};

//! USER_FOUND / USER_LOST only. Everything else is noise for this question.
struct s_row
{
	uint64_t time_us;
	int32_t event_type;
};

/*
 * Capacities. The tracker runs at tens of Hz and the weave at up to a few
 * hundred; the writer drains every ~10 ms. 2048 slots is two full seconds of
 * headroom on either stream, which is far more than a scheduling hiccup and
 * still under a megabyte for the pair.
 */
constexpr uint32_t RING_CAP_T = 2048;
constexpr uint32_t RING_CAP_S = 256;
constexpr uint32_t RING_CAP_W = 2048;

/* ------------------------------------------------------------------ *
 * Env
 * ------------------------------------------------------------------ */

/*!
 * Trim leading/trailing whitespace out of an env value.
 *
 * `set DXR_LEIA_SR_PREDICT_TRACE_DIR=C:\x && app.exe` in cmd.exe puts the
 * space BEFORE the `&&`, so the value carries a trailing blank and a path
 * built from it names a directory that does not exist. The selector
 * (leia_sr_api_select.cpp:93-139) was bitten by exactly this and the fix is
 * copied from there rather than reinvented.
 */
void
env_copy_trimmed(const char *raw, char *out, size_t out_cap)
{
	out[0] = '\0';
	if (raw == nullptr) {
		return;
	}
	while (*raw == ' ' || *raw == '\t') {
		raw++;
	}
	snprintf(out, out_cap, "%s", raw);
	size_t n = strlen(out);
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\r' || out[n - 1] == '\n')) {
		out[--n] = '\0';
	}
}

} // namespace

bool
leia_sr_predict_trace_enabled(void)
{
	// Read once, cached, announced once -- the same shape as
	// leia_sr_target_time_opt_in() (leia_sr_v2_common.cpp:50-68). The
	// recorder is a property of the RUN, so re-reading it per weaver could
	// only ever produce a file that disagrees with its sibling.
	static int cached = -1;
	if (cached < 0) {
		char v[32] = {0};
		env_copy_trimmed(getenv("DXR_LEIA_SR_PREDICT_TRACE"), v, sizeof(v));
		const bool on = v[0] != '\0' && (strcmp(v, "1") == 0 || _stricmp(v, "true") == 0 ||
		                                 _stricmp(v, "on") == 0);
		cached = on ? 1 : 0;
		if (on) {
			U_LOG_W("Leia SR: DXR_LEIA_SR_PREDICT_TRACE set - per-weave eye-prediction "
			        "recorder requested. OBSERVATIONAL ONLY: the weave path is unchanged, "
			        "but each weave gains one extra predictor read, so this run is not "
			        "comparable to an uninstrumented baseline");
		}
	}
	return cached == 1;
}

namespace {

/*!
 * `<dir>\predict_trace_<arm>_<pid>_<yyyymmdd-hhmmss>.csv`.
 *
 * Directory: `DXR_LEIA_SR_PREDICT_TRACE_DIR` if set, else
 * `%LOCALAPPDATA%\DisplayXR` with `%TEMP%` as the fallback -- the same two
 * roots the runtime's own dump sink uses
 * (displayxr-runtime `src/xrt/compositor/util/comp_rear_budget.c:70-93`), so a
 * bug report has one place to look rather than two.
 */
bool
build_trace_path(const char *arm, char *out, size_t out_cap)
{
	out[0] = '\0';

	char dir[MAX_PATH] = {0};
	env_copy_trimmed(getenv("DXR_LEIA_SR_PREDICT_TRACE_DIR"), dir, sizeof(dir));

	if (dir[0] == '\0') {
		const char *base = getenv("LOCALAPPDATA");
		if (base != nullptr && base[0] != '\0') {
			snprintf(dir, sizeof(dir), "%s\\DisplayXR", base);
		} else {
			base = getenv("TEMP");
			if (base == nullptr || base[0] == '\0') {
				return false;
			}
			snprintf(dir, sizeof(dir), "%s", base);
		}
	}

	// Best effort: the DisplayXR directory normally exists already (the
	// runtime's logs live there), and an explicit DIR is the caller's
	// problem. ERROR_ALREADY_EXISTS is the common answer and is not a
	// failure -- the fopen below is the real test either way.
	CreateDirectoryA(dir, nullptr);

	SYSTEMTIME st{};
	GetLocalTime(&st);
	snprintf(out, out_cap, "%s\\predict_trace_%s_%lu_%04u%02u%02u-%02u%02u%02u.csv", dir,
	         arm != nullptr ? arm : "unknown", (unsigned long)GetCurrentProcessId(), st.wYear, st.wMonth,
	         st.wDay, st.wHour, st.wMinute, st.wSecond);
	return true;
}

} // namespace

/* ------------------------------------------------------------------ *
 * The recorder
 * ------------------------------------------------------------------ */

struct leia_sr_predict_trace
{
	// SDK objects this recorder owns. Created before srInitialize, torn
	// down before srDestroyInstance.
	SrInstance instance = nullptr;
	SrEyeTracker tracker = nullptr;
	SrSystemMonitor monitor = nullptr;

	ring<t_row, RING_CAP_T> ring_t;
	ring<s_row, RING_CAP_S> ring_s;
	ring<struct leia_sr_predict_trace_weave, RING_CAP_W> ring_w;

	std::atomic<uint64_t> skipped_no_clock{0};
	std::atomic<uint64_t> skipped_no_eyes{0};
	std::atomic<uint64_t> ref_failed{0}; //!< W rows written with ref_ok = 0

	std::thread writer;
	std::atomic<bool> quit{false};

	FILE *fp = nullptr;
	char path[MAX_PATH] = {0};
	char buf[64 * 1024]; //!< stdio buffer, so a flush is one write syscall

	// Header material, copied at create so the writer thread owns no
	// pointer into the caller's frame.
	char arm[16] = {0};
	char weaver_arm[16] = {0};
	char plugin_version[96] = {0};
	char sdk_pin[64] = {0};
	char sr_runtime_version[96] = {0};
	char api_reason[192] = {0};
	char panel_id[96] = {0};
	char monitor_device[64] = {0};
	int32_t monitor_orientation_deg = -1;
	uint64_t initial_weaver_latency_us = 0;
	uint32_t display_pixel_width = 0;
	uint32_t display_pixel_height = 0;
	int32_t display_screen_left = 0;
	int32_t display_screen_top = 0;
	float display_width_m = 0.0f;
	float display_height_m = 0.0f;

	bool header_written = false;
	//! The clock-gate delta is not known when the header goes out: the first
	//! T row (~120 Hz, right after srInitialize) forces the header well before
	//! the first weave runs the gate. So the header omits the line and the
	//! writer appends `# clock_gate_delta_us=` the first time the gate has run;
	//! destroy writes `n/a` if it never did. The scorer reads keys by name and
	//! keeps the FIRST value it sees, which is why the header must not emit a
	//! placeholder.
	bool gate_line_pending = false;
	uint64_t start_ms = 0;
	//! One simultaneous reading of the SDK clock and the system clock, taken
	//! at create. The S rows' timestamps turned out (2026-09-16 smoke) NOT to
	//! be in the srGetTimeUs domain, so the scorer needs a pair to map them;
	//! recording both costs nothing and lets the mapping be done offline
	//! without guessing which epoch the SDK meant.
	uint64_t clock_pair_sr_us = 0;
	uint64_t clock_pair_filetime = 0; //!< 100 ns ticks since 1601-01-01 (FILETIME)
};

namespace {

/* ------------------------------------------------------------------ *
 * SDK callbacks
 *
 * Both fire on the SDK's own worker threads (sr_eye_tracker.h:148-151,
 * sr_system.h:176-178). The ONLY thing either body may do is a lock-free ring
 * push: no mutex that the weave thread also takes, no allocation, no I/O, no
 * logging. That is not a style preference -- a blocking callback stalls the
 * tracking pipeline the recorder exists to measure, which would make the
 * measurement a function of the measuring.
 *
 * Because they take no lock, srEyeTrackerRemoveCallback /
 * srSystemMonitorRemoveCallback (which block until an in-flight invocation
 * finishes) can never deadlock against them.
 * ------------------------------------------------------------------ */

void SR_CALL
on_eye_pair(const SrEyePair *pair, void *user_data)
{
	leia_sr_predict_trace *rec = static_cast<leia_sr_predict_trace *>(user_data);
	if (rec == nullptr || pair == nullptr) {
		return;
	}
	t_row row{};
	row.frame_id = pair->frameId;
	// The CAPTURE instant, in the srGetTimeUs/QPC domain -- not the instant
	// anything was predicted for (sr_eye_tracker.h:57-70). That is precisely
	// why it can serve as ground truth for a prediction made elsewhere.
	row.time_us = pair->timeUs;
	row.l[0] = pair->leftEye.x;
	row.l[1] = pair->leftEye.y;
	row.l[2] = pair->leftEye.z;
	row.r[0] = pair->rightEye.x;
	row.r[1] = pair->rightEye.y;
	row.r[2] = pair->rightEye.z;
	(void)rec->ring_t.push(row);
}

void SR_CALL
on_system_event(const SrSystemEvent *event, void *user_data)
{
	leia_sr_predict_trace *rec = static_cast<leia_sr_predict_trace *>(user_data);
	if (rec == nullptr || event == nullptr) {
		return;
	}
	// Presence only. A weave whose evaluation instant falls inside a
	// USER_LOST..USER_FOUND span has no eyes to be right or wrong about --
	// the SDK animates the pair toward the display default there
	// (sr_weaver.h:293-298), and scoring that as prediction error would
	// measure the fallback animation instead.
	if (event->eventType != SR_EVENT_TYPE_USER_FOUND && event->eventType != SR_EVENT_TYPE_USER_LOST) {
		return;
	}
	s_row row{};
	row.time_us = event->timeUs;
	row.event_type = (int32_t)event->eventType;
	(void)rec->ring_s.push(row);
}

/* ------------------------------------------------------------------ *
 * Writer thread
 * ------------------------------------------------------------------ */

void
write_header(leia_sr_predict_trace *rec)
{
	// The clock-gate delta is the one header value that does not exist yet
	// when the recorder is created: on the target arm the gate runs inside
	// the first weave's w_target_time_available(), and on the legacy arm it
	// never runs at all. So the header is deferred to the first drain that
	// has a weave in it (or to a one-second timeout, so a run that never
	// weaves still produces a readable file). Nothing else in the header
	// moves, and the scorer reads `#key=value` by name, not by line number.
	int64_t gate_delta_us = 0;
	const bool gate_known = leia_sr_v2_clock_gate_last_delta(&gate_delta_us);

	fprintf(rec->fp, "# format=dxr-leia-predict-trace-2\n");
	fprintf(rec->fp, "# arm=%s\n", rec->arm);
	fprintf(rec->fp, "# weaver_arm=%s\n", rec->weaver_arm);
	fprintf(rec->fp, "# plugin_version=%s\n", rec->plugin_version);
	fprintf(rec->fp, "# sr_sdk_pin=%s\n", rec->sdk_pin);
	fprintf(rec->fp, "# sr_runtime_version=%s\n", rec->sr_runtime_version);
	fprintf(rec->fp, "# note_runtime_version=srGetRuntimeVersion is a hardcoded stub in SDK 1584 (always "
	                 "1.0.0); sr_sdk_pin is the build we compiled against\n");
	fprintf(rec->fp, "# sr_api_reason=%s\n", rec->api_reason);
	fprintf(rec->fp, "# pid=%lu\n", (unsigned long)GetCurrentProcessId());
	if (gate_known) {
		fprintf(rec->fp, "# clock_gate_delta_us=%lld\n", (long long)gate_delta_us);
	} else {
		// Not known yet (target arm: the gate runs inside the first weave,
		// after the first T row forced this header out) or never (legacy
		// arm). Emit nothing here; the writer appends the line when the gate
		// has run and destroy writes n/a if it never did.
		rec->gate_line_pending = true;
	}
	fprintf(rec->fp, "# panel_id=%s\n", rec->panel_id);
	fprintf(rec->fp, "# monitor_device=%s\n", rec->monitor_device);
	fprintf(rec->fp, "# monitor_orientation_deg=%d\n", rec->monitor_orientation_deg);
	fprintf(rec->fp, "# display_px=%ux%u\n", rec->display_pixel_width, rec->display_pixel_height);
	fprintf(rec->fp, "# display_origin=%d,%d\n", rec->display_screen_left, rec->display_screen_top);
	fprintf(rec->fp, "# display_size_m=%.4f,%.4f\n", (double)rec->display_width_m,
	        (double)rec->display_height_m);
	fprintf(rec->fp, "# initial_weaver_latency_us=%llu\n",
	        (unsigned long long)rec->initial_weaver_latency_us);
	// No SDK call reports the tracker's nominal rate (searched the whole
	// 1584 header tree). 0 means "unknown"; the scorer estimates the period
	// from the T stream's own spacing instead.
	// Clock pair (see the struct): lets the scorer map any timestamp that is
	// NOT in the srGetTimeUs domain onto it. The S rows are the known case.
	fprintf(rec->fp, "# clock_pair_sr_us=%llu\n", (unsigned long long)rec->clock_pair_sr_us);
	fprintf(rec->fp, "# clock_pair_filetime_100ns_since_1601=%llu\n",
	        (unsigned long long)rec->clock_pair_filetime);
	fprintf(rec->fp, "# note_S_timeUs=observed NOT in the srGetTimeUs domain (2026-09-16 smoke: looked "
	                 "like 100 ns ticks since the Unix epoch); map through clock_pair; advisory until "
	                 "pinned by the SDK\n");
	fprintf(rec->fp, "# tracker_nominal_hz=0\n");
	// Documentation constants, for the scorer's prose only. NOTHING at
	// runtime reads or applies these -- they are here so a file can be
	// interpreted years later without the SDK sources to hand.
	fprintf(rec->fp, "# max_prediction_scene_s=-0.00436\n");
	fprintf(rec->fp, "# max_prediction_distance_cm=15\n");
	fprintf(rec->fp, "# units=positions_mm times_us_srGetTimeUs_domain\n");
	fprintf(rec->fp, "# legend_T=T,frameId,timeUs,lx,ly,lz,rx,ry,rz\n");
	fprintf(rec->fp, "# legend_S=S,eventType,timeUs  (16=USER_FOUND 17=USER_LOST)\n");
	fprintf(rec->fp,
	        "# legend_W=W,seq,arm,now_us,push_us,scanout_us,pushed,last_set_latency_us,"
	        "target_us,horizon_us,resolved_us,read_now_us,plx,ply,plz,prx,pry,prz,swap_flag,"
	        "ref_now_us,rlx,rly,rlz,rrx,rry,rrz,ref_ok\n");
	fprintf(rec->fp,
	        "# reference=srEyeTrackerPredict(tracker,0) on the recorder's own tracker handle, per weave; "
	        "ref_instant_us = ref_now_us + min(8333, max_prediction_scene_s*1e6); the filter's smoothed "
	        "low-lag estimate, NOT a raw measurement (none exists in the C99 surface; enablePrediction is "
	        "ignored by SDK 1584)\n");
	fprintf(rec->fp,
	        "# note_T=the tracker callback stream is an ECHO of predict() calls (updated only at the end "
	        "of predict/predictAt), so T rows are DIAGNOSTIC ONLY and must not be used as ground truth; "
	        "timeUs==0 means never-measured, and it stays STALE after a face is lost (use S)\n");
	fprintf(rec->fp,
	        "# note_observer=each weave gains three extra predictor calls (getter, clock, reference), one "
	        "of which emits a T row; not comparable to an uninstrumented baseline\n");
	rec->header_written = true;
}

//! Drain all three rings once. Consumer side; the only writer of rec->fp.
void
drain(leia_sr_predict_trace *rec)
{
	t_row t{};
	while (rec->ring_t.pop(&t)) {
		if (!rec->header_written) {
			write_header(rec);
		}
		fprintf(rec->fp, "T,%llu,%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", (unsigned long long)t.frame_id,
		        (unsigned long long)t.time_us, t.l[0], t.l[1], t.l[2], t.r[0], t.r[1], t.r[2]);
		rec->ring_t.written++;
	}

	s_row s{};
	while (rec->ring_s.pop(&s)) {
		if (!rec->header_written) {
			write_header(rec);
		}
		fprintf(rec->fp, "S,%d,%llu\n", (int)s.event_type, (unsigned long long)s.time_us);
		rec->ring_s.written++;
	}

	struct leia_sr_predict_trace_weave w{};
	while (rec->ring_w.pop(&w)) {
		if (!rec->header_written) {
			write_header(rec);
		}
		fprintf(rec->fp,
		        "W,%llu,%s,%llu,%llu,%llu,%u,%llu,%llu,%llu,%llu,%llu,"
		        "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,"
		        "%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u\n",
		        (unsigned long long)w.seq, rec->arm, (unsigned long long)w.now_us,
		        (unsigned long long)w.push_us, (unsigned long long)w.scanout_us, (unsigned)w.pushed,
		        (unsigned long long)w.last_set_latency_us, (unsigned long long)w.target_us,
		        (unsigned long long)w.horizon_us, (unsigned long long)w.resolved_us,
		        (unsigned long long)w.read_now_us, w.pl[0], w.pl[1], w.pl[2], w.pr[0], w.pr[1], w.pr[2],
		        (unsigned)w.swap_flag, (unsigned long long)w.ref_now_us, w.rl[0], w.rl[1], w.rl[2],
		        w.rr[0], w.rr[1], w.rr[2], (unsigned)w.ref_ok);
		rec->ring_w.written++;
	}
}

void
writer_body(leia_sr_predict_trace *rec)
{
	// Below normal: this thread must never win against the weave thread or
	// the SDK's tracker thread. It is doing text formatting for a file
	// nobody reads until the run is over.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

	// An exception escaping a std::thread body is std::terminate.
	try {
		for (;;) {
			const bool quitting = rec->quit.load(std::memory_order_acquire);
			drain(rec);
			if (rec->header_written && rec->gate_line_pending) {
				int64_t d = 0;
				if (leia_sr_v2_clock_gate_last_delta(&d)) {
					fprintf(rec->fp, "# clock_gate_delta_us=%lld\n", (long long)d);
					rec->gate_line_pending = false;
				}
			}
			// Flush every drain: a hard-killed player (TerminateProcess) never
			// reaches destroy, and without this the last CRT buffer -- up to
			// 64 KiB of rows -- dies with it and the file ends mid-row. One
			// flush per 10 ms on a below-normal thread is nothing.
			if (rec->fp != nullptr) {
				fflush(rec->fp);
			}
			if (!rec->header_written && (GetTickCount64() - rec->start_ms) >= 1000) {
				// A run that never wove still deserves a readable
				// file saying which arm it was.
				write_header(rec);
			}
			if (quitting) {
				// One more pass AFTER observing quit, so anything the
				// producer pushed between the last drain and the flag
				// is on disk. (quit is read before the drain above, so
				// that drain is already the post-flag one; this break
				// is the loop exit, not a missing pass.)
				break;
			}
			Sleep(10);
		}
	} catch (...) {
		U_LOG_E("Leia predict trace: writer thread aborted on an exception - the trace file is "
		        "truncated and must not be scored");
	}
}

/*!
 * Which monitor the weaver's window is on, and how it is rotated.
 *
 * MonitorFromWindow rather than MonitorFromPoint: a HWND needs no coordinate
 * conversion, so this cannot be wrong about the HOST application's DPI space
 * the way a point-based lookup from inside a DLL can be. Diagnostics only --
 * nothing here feeds geometry back into the weave.
 */
void
query_monitor_identity(void *hwnd, char *device_out, size_t device_cap, int32_t *orientation_out)
{
	device_out[0] = '\0';
	*orientation_out = -1;

	HMONITOR mon = nullptr;
	if (hwnd != nullptr) {
		mon = MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTONULL);
	}
	if (mon == nullptr) {
		return;
	}

	MONITORINFOEXA mi{};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoA(mon, (MONITORINFO *)&mi)) {
		return;
	}
	snprintf(device_out, device_cap, "%s", mi.szDevice);

	DEVMODEA dm{};
	dm.dmSize = sizeof(dm);
	if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
	    (dm.dmFields & DM_DISPLAYORIENTATION) != 0) {
		switch (dm.dmDisplayOrientation) {
		case DMDO_DEFAULT: *orientation_out = 0; break;
		case DMDO_90: *orientation_out = 90; break;
		case DMDO_180: *orientation_out = 180; break;
		case DMDO_270: *orientation_out = 270; break;
		default: *orientation_out = -1; break;
		}
	}
}

//! Panel identity, from the EDID probe the plug-in already ran at discovery.
void
query_panel_id(char *out, size_t cap)
{
	// The SR v2 API exposes no product/profile identifier (searched the 1584
	// header tree: srDisplay* reports geometry only). The plug-in's OWN
	// identity for a panel is the EDID manufacturer+product pair it matched
	// in leia_edid_probe.c, so that is what goes in the file.
	struct leia_display_probe_result r = {};
	if (leia_edid_get_cached_result(&r) && (r.manufacturer_id != 0 || r.product_id != 0)) {
		snprintf(out, cap, "edid:%04X:%04X", (unsigned)r.manufacturer_id, (unsigned)r.product_id);
		return;
	}
	snprintf(out, cap, "unknown");
}

} // namespace

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */

struct leia_sr_predict_trace *
leia_sr_predict_trace_create(SrInstance instance, const struct leia_sr_predict_trace_open_info *info)
{
	if (!leia_sr_predict_trace_enabled() || instance == nullptr || info == nullptr) {
		return nullptr;
	}

	// Probe the clock FIRST, on the real instance. srGetTimeUs is an
	// appended dispatch slot; on an older runtime the trampoline answers
	// SR_ERROR_FUNCTION_UNSUPPORTED rather than dispatching into a NULL. A
	// recorder with no clock can produce no W row at all, so there is no
	// point creating senses for it.
	uint64_t probe_now_us = 0;
	const SrResult tr = srGetTimeUs(instance, &probe_now_us);
	if (!SR_SUCCEEDED(tr)) {
		U_LOG_W("Leia predict trace: srGetTimeUs unavailable (%s) - recorder disabled for this "
		        "weaver",
		        leia_sr_v2_result_str(tr));
		return nullptr;
	}

	leia_sr_predict_trace *rec = new (std::nothrow) leia_sr_predict_trace;
	if (rec == nullptr) {
		U_LOG_W("Leia predict trace: allocation failed - recorder disabled for this weaver");
		return nullptr;
	}
	rec->instance = instance;
	rec->start_ms = GetTickCount64();
	{
		// Clock pair for the header: system time and SDK time read back to
		// back, SDK second so the pair is as tight as two calls allow.
		FILETIME ft;
		GetSystemTimePreciseAsFileTime(&ft);
		uint64_t pair_sr_us = probe_now_us;
		(void)srGetTimeUs(instance, &pair_sr_us);
		rec->clock_pair_filetime = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
		rec->clock_pair_sr_us = pair_sr_us;
	}

	snprintf(rec->arm, sizeof(rec->arm), "%s", info->arm != nullptr ? info->arm : "unknown");
	snprintf(rec->weaver_arm, sizeof(rec->weaver_arm), "%s",
	         info->weaver_arm != nullptr ? info->weaver_arm : "unknown");
	snprintf(rec->plugin_version, sizeof(rec->plugin_version), "%s",
	         info->plugin_version != nullptr ? info->plugin_version : "unknown");
	snprintf(rec->sdk_pin, sizeof(rec->sdk_pin), "%s", info->sdk_pin != nullptr ? info->sdk_pin : "unknown");
	snprintf(rec->api_reason, sizeof(rec->api_reason), "%s",
	         info->api_reason != nullptr ? info->api_reason : "unknown");
	rec->display_pixel_width = info->display_pixel_width;
	rec->display_pixel_height = info->display_pixel_height;
	rec->display_screen_left = info->display_screen_left;
	rec->display_screen_top = info->display_screen_top;
	rec->display_width_m = info->display_width_m;
	rec->display_height_m = info->display_height_m;

	// Live runtime version alongside the compile-time pin: the pin says what
	// we built against, this says what answered.
	if (!SR_SUCCEEDED(srGetRuntimeVersion(instance, rec->sr_runtime_version,
	                                      (uint32_t)sizeof(rec->sr_runtime_version))) ||
	    rec->sr_runtime_version[0] == '\0') {
		snprintf(rec->sr_runtime_version, sizeof(rec->sr_runtime_version), "unknown");
	}

	if (info->weaver != nullptr) {
		uint64_t lat_us = 0;
		if (SR_SUCCEEDED(srWeaverGetLatency(info->weaver, &lat_us))) {
			rec->initial_weaver_latency_us = lat_us;
		}
	}

	query_panel_id(rec->panel_id, sizeof(rec->panel_id));
	query_monitor_identity(info->hwnd, rec->monitor_device, sizeof(rec->monitor_device),
	                       &rec->monitor_orientation_deg);

	if (!build_trace_path(rec->arm, rec->path, sizeof(rec->path))) {
		U_LOG_W("Leia predict trace: no writable directory (set DXR_LEIA_SR_PREDICT_TRACE_DIR) - "
		        "recorder disabled for this weaver");
		delete rec;
		return nullptr;
	}
	rec->fp = fopen(rec->path, "wb");
	if (rec->fp == nullptr) {
		U_LOG_W("Leia predict trace: could not open '%s' - recorder disabled for this weaver",
		        rec->path);
		delete rec;
		return nullptr;
	}
	setvbuf(rec->fp, rec->buf, _IOFBF, sizeof(rec->buf));

	/*
	 * SENSES BEFORE srInitialize. The SDK states the rule twice --
	 * sr_eye_tracker.h:179 ("must be created ... before srInitialize") and
	 * :258 ("Callbacks must be registered before srInitialize") -- and the
	 * Linux arm already obeys it (drv_leia_linux/leia_sr_linux_sdk.c:256-276).
	 * The caller guarantees we are on that side of the call.
	 *
	 * enablePrediction is SR_TRUE to match the Linux arm. It costs nothing
	 * here: this tracker's PREDICTION is never read. Only its raw-sample
	 * callback is, and that delivers the measurement regardless.
	 */
	SrEyeTrackerCreateInfo tci{};
	tci.sType = SR_TYPE_EYE_TRACKER_CREATE_INFO;
	tci.pNext = nullptr;
	tci.enablePrediction = SR_TRUE;
	const SrResult er = srCreateEyeTracker(instance, &tci, &rec->tracker);
	if (!SR_SUCCEEDED(er) || rec->tracker == nullptr) {
		// No tracker means no ground truth, which means the file cannot be
		// scored. Fail the whole recorder rather than ship a W-only file
		// that looks scoreable and is not.
		U_LOG_W("Leia predict trace: srCreateEyeTracker failed (%s) - no ground truth, recorder "
		        "disabled for this weaver",
		        leia_sr_v2_result_str(er));
		rec->tracker = nullptr;
		fclose(rec->fp);
		rec->fp = nullptr;
		remove(rec->path);
		delete rec;
		return nullptr;
	}
	const SrResult ar = srEyeTrackerAddCallback(rec->tracker, on_eye_pair, rec);
	if (!SR_SUCCEEDED(ar)) {
		U_LOG_W("Leia predict trace: srEyeTrackerAddCallback failed (%s) - no ground truth, "
		        "recorder disabled for this weaver",
		        leia_sr_v2_result_str(ar));
		srDestroyEyeTracker(rec->tracker);
		rec->tracker = nullptr;
		fclose(rec->fp);
		rec->fp = nullptr;
		remove(rec->path);
		delete rec;
		return nullptr;
	}

	// The monitor is a NICE-TO-HAVE, unlike the tracker: without it the
	// scorer simply cannot exclude untracked spans, which it says so about.
	SrSystemMonitorCreateInfo mci{};
	mci.sType = SR_TYPE_SYSTEM_MONITOR_CREATE_INFO;
	mci.pNext = nullptr;
	const SrResult mr = srCreateSystemMonitor(instance, &mci, &rec->monitor);
	if (SR_SUCCEEDED(mr) && rec->monitor != nullptr) {
		if (!SR_SUCCEEDED(srSystemMonitorAddCallback(rec->monitor, on_system_event, rec))) {
			U_LOG_W("Leia predict trace: srSystemMonitorAddCallback failed - no USER_FOUND/"
			        "USER_LOST rows; untracked spans cannot be excluded from the score");
			srDestroySystemMonitor(rec->monitor);
			rec->monitor = nullptr;
		}
	} else {
		U_LOG_W("Leia predict trace: srCreateSystemMonitor failed (%s) - no USER_FOUND/USER_LOST "
		        "rows; untracked spans cannot be excluded from the score",
		        leia_sr_v2_result_str(mr));
		rec->monitor = nullptr;
	}

	try {
		rec->writer = std::thread([rec]() { writer_body(rec); });
	} catch (...) {
		U_LOG_W("Leia predict trace: writer thread could not be created - recorder disabled for "
		        "this weaver");
		srEyeTrackerRemoveCallback(rec->tracker, on_eye_pair, rec);
		srDestroyEyeTracker(rec->tracker);
		rec->tracker = nullptr;
		if (rec->monitor != nullptr) {
			srSystemMonitorRemoveCallback(rec->monitor, on_system_event, rec);
			srDestroySystemMonitor(rec->monitor);
			rec->monitor = nullptr;
		}
		fclose(rec->fp);
		rec->fp = nullptr;
		remove(rec->path);
		delete rec;
		return nullptr;
	}

	U_LOG_W("Leia predict trace: recording %s arm (%s weaver) to %s", rec->arm, rec->weaver_arm, rec->path);
	return rec;
}

void
leia_sr_predict_trace_destroy(struct leia_sr_predict_trace **rec_ptr)
{
	if (rec_ptr == nullptr || *rec_ptr == nullptr) {
		return;
	}
	leia_sr_predict_trace *rec = *rec_ptr;
	// NULL the caller's pointer first: every producer reaches this recorder
	// through that field and checks it, so a racing weave sees "no recorder"
	// rather than one being dismantled. Same discipline the arm already uses
	// for its other SDK objects.
	*rec_ptr = nullptr;

	/*
	 * Order matters, and every step of it is load-bearing:
	 *
	 *  1. remove callbacks  -- documented to BLOCK until an in-flight
	 *     invocation finishes (sr_eye_tracker.h:274-280, sr_system.h:302-307),
	 *     and guaranteed not to be invoked again after it returns. Our
	 *     callbacks take no lock, so this can never deadlock against them.
	 *  2. destroy the senses -- before the instance, never after.
	 *  3. stop the writer and flush -- only once no producer can push again.
	 */
	if (rec->tracker != nullptr) {
		srEyeTrackerRemoveCallback(rec->tracker, on_eye_pair, rec);
	}
	if (rec->monitor != nullptr) {
		srSystemMonitorRemoveCallback(rec->monitor, on_system_event, rec);
	}
	if (rec->tracker != nullptr) {
		srDestroyEyeTracker(rec->tracker);
		rec->tracker = nullptr;
	}
	if (rec->monitor != nullptr) {
		srDestroySystemMonitor(rec->monitor);
		rec->monitor = nullptr;
	}

	rec->quit.store(true, std::memory_order_release);
	if (rec->writer.joinable()) {
		rec->writer.join();
	}

	if (rec->fp != nullptr) {
		// Final drain on THIS thread: the writer has exited, so it is the
		// only consumer now.
		drain(rec);
		if (!rec->header_written) {
			write_header(rec);
		}
		if (rec->gate_line_pending) {
			int64_t d = 0;
			if (leia_sr_v2_clock_gate_last_delta(&d)) {
				fprintf(rec->fp, "# clock_gate_delta_us=%lld\n", (long long)d);
			} else {
				// Honest value for a gate that never ran (the legacy arm).
				fprintf(rec->fp, "# clock_gate_delta_us=n/a\n");
			}
			rec->gate_line_pending = false;
		}
		fprintf(rec->fp,
		        "# trailer rows_T=%llu rows_S=%llu rows_W=%llu dropped_T=%llu dropped_S=%llu "
		        "dropped_W=%llu skipped_W_no_clock=%llu skipped_W_no_eyes=%llu ref_failed=%llu\n",
		        (unsigned long long)rec->ring_t.written, (unsigned long long)rec->ring_s.written,
		        (unsigned long long)rec->ring_w.written,
		        (unsigned long long)rec->ring_t.dropped.load(std::memory_order_relaxed),
		        (unsigned long long)rec->ring_s.dropped.load(std::memory_order_relaxed),
		        (unsigned long long)rec->ring_w.dropped.load(std::memory_order_relaxed),
		        (unsigned long long)rec->skipped_no_clock.load(std::memory_order_relaxed),
		        (unsigned long long)rec->skipped_no_eyes.load(std::memory_order_relaxed),
		        (unsigned long long)rec->ref_failed.load(std::memory_order_relaxed));
		fflush(rec->fp);
		fclose(rec->fp);
		rec->fp = nullptr;
		U_LOG_W("Leia predict trace: closed %s (%llu weave rows, %llu tracker rows)", rec->path,
		        (unsigned long long)rec->ring_w.written, (unsigned long long)rec->ring_t.written);
	}

	delete rec;
}

void
leia_sr_predict_trace_on_weave(struct leia_sr_predict_trace *rec, const struct leia_sr_predict_trace_weave *w)
{
	if (rec == nullptr || w == nullptr) {
		return;
	}
	struct leia_sr_predict_trace_weave row = *w;
	row.ref_ok = 0;
	row.ref_now_us = 0;
	// The reference (see the struct doc): the tracker handle's own zero-
	// horizon predict, clock-stamped immediately before. Same call, same
	// handle, same expression on both arms. It is the third extra predictor
	// call per weave and it emits a T row of its own -- both stated in the
	// header. A failure leaves ref_ok = 0 and the row is still recorded,
	// because the prediction half of it is still evidence.
	if (rec->tracker != nullptr && rec->instance != nullptr) {
		uint64_t ref_now_us = 0;
		if (SR_SUCCEEDED(srGetTimeUs(rec->instance, &ref_now_us))) {
			SrEyePair pair{};
			if (SR_SUCCEEDED(srEyeTrackerPredict(rec->tracker, 0, &pair))) {
				row.ref_now_us = ref_now_us;
				row.rl[0] = pair.leftEye.x;
				row.rl[1] = pair.leftEye.y;
				row.rl[2] = pair.leftEye.z;
				row.rr[0] = pair.rightEye.x;
				row.rr[1] = pair.rightEye.y;
				row.rr[2] = pair.rightEye.z;
				row.ref_ok = 1;
			} else {
				rec->ref_failed.fetch_add(1, std::memory_order_relaxed);
			}
		} else {
			rec->ref_failed.fetch_add(1, std::memory_order_relaxed);
		}
	}
	(void)rec->ring_w.push(row);
}

void
leia_sr_predict_trace_skip_weave(struct leia_sr_predict_trace *rec, enum leia_sr_predict_trace_skip why)
{
	if (rec == nullptr) {
		return;
	}
	if (why == LEIA_SR_PREDICT_TRACE_SKIP_NO_CLOCK) {
		rec->skipped_no_clock.fetch_add(1, std::memory_order_relaxed);
	} else {
		rec->skipped_no_eyes.fetch_add(1, std::memory_order_relaxed);
	}
}

#endif // DXR_LEIA_HAS_SR_V2
