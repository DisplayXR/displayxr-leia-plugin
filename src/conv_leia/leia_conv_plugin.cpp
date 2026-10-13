// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The Leia conversion provider (ADR-053): xrtConversionPluginNegotiate
 *         + the xrt_conversion_plugin_iface v1 over the NeurD module.
 *
 * A separate DLL from the display processor (DisplayXR-LeiaSR.dll): it never
 * loads the SR SDK, never creates a weaver or an SR session, and never touches
 * the display DLL. It compiles the same NeurD module source the display DLL's
 * lift slots use (../drv_leia/leia_lift_neurd.cpp — same search order, same
 * version floor, same convert / depth / off-axis paths), hosted the provider
 * way:
 *
 *  - no activation thread: warm() loads and initialises NeurD on the runtime's
 *    thread (leia_lift_neurd_set_manual_activation);
 *  - configuration only through host->get_setting (the module's knob names,
 *    plus Enable and NeurDPath);
 *  - images are runtime-allocated and imported (NT handle on the provider's
 *    OWN D3D11 device, created on the adapter the host names, or CPU memory);
 *  - every boundary struct is read and written only within its struct_size.
 *
 * Capabilities (cap_id, stable): 1 DEPTH_MONO, 2 DEPTH_VIDEO (TEMPORAL),
 * 3 VIEWS_MONO (2 views; + the depth of the same inference iff the module has
 * it). The version gate reports FAILED(VERSION), an absent module ABSENT(MODEL)
 * — see leia_conv_policy.h. Docs: docs/conversion-provider.md.
 *
 * Threading: the runtime serialises every [S] slot (one call in flight); the
 * [C] slots (get_provider_status, describe_cap, get_generation, get_state,
 * stream_get_output_desc) answer from a snapshot under a short mutex that is
 * never held across a module or D3D11 call.
 *
 * @ingroup conv_leia
 */

#include "xrt/xrt_conversion_plugin.h"

#include "leia_conv_d3d11.h"
#include "leia_conv_policy.h"
#include "leia_lift_neurd.h"

#include "util/u_logging.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#ifndef DXR_PLUGIN_GIT_DESC
#define DXR_PLUGIN_GIT_DESC "dev"
#endif

#define LEIA_CONV_ID "leia-conversion"

// The pure policy layer mirrors the ABI values; pin every pair.
static_assert(LEIA_CONV_STATE_READY == XRT_CONV_STATE_READY && LEIA_CONV_STATE_WARMING == XRT_CONV_STATE_WARMING &&
                  LEIA_CONV_STATE_ABSENT == XRT_CONV_STATE_ABSENT &&
                  LEIA_CONV_STATE_INCOMPATIBLE == XRT_CONV_STATE_INCOMPATIBLE &&
                  LEIA_CONV_STATE_UNLICENSED == XRT_CONV_STATE_UNLICENSED &&
                  LEIA_CONV_STATE_FAILED == XRT_CONV_STATE_FAILED &&
                  LEIA_CONV_STATE_DISABLED == XRT_CONV_STATE_DISABLED &&
                  LEIA_CONV_STATE_INSTALLED == XRT_CONV_STATE_INSTALLED,
              "LEIA_CONV_STATE_* must equal XRT_CONV_STATE_*");
static_assert(LEIA_CONV_FAIL_LICENCE == XRT_CONV_FAIL_LICENCE && LEIA_CONV_FAIL_LOAD == XRT_CONV_FAIL_LOAD &&
                  LEIA_CONV_FAIL_NETWORK == XRT_CONV_FAIL_NETWORK && LEIA_CONV_FAIL_VERSION == XRT_CONV_FAIL_VERSION &&
                  LEIA_CONV_FAIL_MODEL == XRT_CONV_FAIL_MODEL &&
                  LEIA_CONV_FAIL_DEVICE_LOST == XRT_CONV_FAIL_DEVICE_LOST,
              "LEIA_CONV_FAIL_* must equal XRT_CONV_FAIL_*");
static_assert(LEIA_CONV_LICENCE_UNKNOWN == XRT_CONV_LICENCE_UNKNOWN &&
                  LEIA_CONV_LICENCE_COMMERCIAL == XRT_CONV_LICENCE_COMMERCIAL &&
                  LEIA_CONV_LICENCE_RESEARCH_ONLY == XRT_CONV_LICENCE_RESEARCH_ONLY,
              "LEIA_CONV_LICENCE_* must equal XRT_CONV_LICENCE_*");
static_assert(LEIA_CONV_DEPTH_METRIC == XRT_CONV_DEPTH_METRIC &&
                  LEIA_CONV_DEPTH_METRIC_INVERSE == XRT_CONV_DEPTH_METRIC_INVERSE &&
                  LEIA_CONV_DEPTH_AFFINE_INVERSE == XRT_CONV_DEPTH_AFFINE_INVERSE &&
                  LEIA_CONV_DEPTH_AFFINE == XRT_CONV_DEPTH_AFFINE &&
                  LEIA_CONV_CONVERGENCE_AUTO == XRT_CONV_CONVERGENCE_AUTO &&
                  LEIA_CONV_CONVERGENCE_DECODED == XRT_CONV_CONVERGENCE_DECODED,
              "LEIA_CONV_DEPTH_* / CONVERGENCE_* must equal the ABI values");
static_assert(LEIA_CONV_MODULE_READY == LEIA_LIFT_MODULE_READY &&
                  LEIA_CONV_MODULE_ACTIVATING == LEIA_LIFT_MODULE_ACTIVATING &&
                  LEIA_CONV_MODULE_ABSENT == LEIA_LIFT_MODULE_ABSENT &&
                  LEIA_CONV_MODULE_TOO_OLD == LEIA_LIFT_MODULE_TOO_OLD &&
                  LEIA_CONV_MODULE_UNLICENSED == LEIA_LIFT_MODULE_UNLICENSED &&
                  LEIA_CONV_MODULE_LOAD_FAILED == LEIA_LIFT_MODULE_LOAD_FAILED &&
                  LEIA_CONV_MODULE_DISABLED == LEIA_LIFT_MODULE_DISABLED,
              "LEIA_CONV_MODULE_* must equal the module's LEIA_LIFT_MODULE_*");
static_assert(XRT_CONV_CONTENT_VIDEO == 0u && XRT_CONV_CONTENT_PHOTO == 1u,
              "the module's content hint is 0 video / 1 photo");

namespace {

/*
 *
 * Constants.
 *
 */

//! Concurrent streams per capability. NeurD's own limit (32) is process-wide
//! and shared by the three capabilities.
constexpr uint32_t kMaxStreamsPerCap = 8;
//! Long edge of a depth target (the module infers at ~720p; never upscaled).
constexpr uint32_t kDepthMaxEdge = 1280;
//! Long edge of the views' aux depth (the model's own depth is ~640 wide).
constexpr uint32_t kAuxDepthMaxEdge = 1024;
//! D3D11's texture limit (views side by side must fit it).
constexpr uint32_t kMaxTexDim = 16384;
//! Priors until a convert has been measured (DirectML, 720p).
constexpr uint32_t kPriorLatencyMs = 22;
//! Load + licence + model load, measured 8-10 s on a cold box.
constexpr uint32_t kEstWarmupMs = 10000;
//! The model's native long edge at the default 720p inference bucket.
constexpr uint32_t kPreferredInputEdge = 1280;

const char *const kSpdx = "LicenseRef-LeiaInc";

/*
 *
 * Host + instance.
 *
 */

const struct xrt_conversion_plugin_host_iface *g_host = nullptr;

//! The module's setting reader: host->get_setting, nothing else (never this
//! process's environment — in a satellite that is the host's, spec §2.4).
bool
host_setting(const char *key, char *buf, uint32_t buf_size)
{
	if (g_host == nullptr || !XRT_CONV_COVERS(g_host, struct xrt_conversion_plugin_host_iface, get_setting) ||
	    g_host->get_setting == nullptr) {
		return false;
	}
	return g_host->get_setting(key, buf, buf_size);
}

void
host_notify(uint32_t cap_id)
{
	if (g_host != nullptr && XRT_CONV_COVERS(g_host, struct xrt_conversion_plugin_host_iface, notify_changed) &&
	    g_host->notify_changed != nullptr) {
		g_host->notify_changed(cap_id);
	}
}

bool
host_adapter_hint(uint64_t *preferred, uint64_t *avoid)
{
	*preferred = *avoid = 0;
	if (g_host == nullptr || !XRT_CONV_COVERS(g_host, struct xrt_conversion_plugin_host_iface, get_adapter_hint) ||
	    g_host->get_adapter_hint == nullptr) {
		return false;
	}
	struct xrt_conv_adapter_hint h = {};
	h.struct_size = (uint32_t)sizeof(h);
	if (!g_host->get_adapter_hint(&h)) {
		return false;
	}
	*preferred = XRT_CONV_COVERS(&h, struct xrt_conv_adapter_hint, preferred_luid) ? h.preferred_luid : 0;
	*avoid = XRT_CONV_COVERS(&h, struct xrt_conv_adapter_hint, avoid_luid) ? h.avoid_luid : 0;
	return true;
}

struct conv_stream
{
	uint64_t handle;
	uint32_t cap_id;
	uint64_t lift_id;      //!< the module's stream
	uint32_t layout;       //!< XRT_CONV_VIEWS_* (views)
	bool want_depth;       //!< VIEWS: aux depth requested and available
	uint32_t depth_kind;   //!< XRT_CONV_DEPTH_* of the last depth returned (0 = none yet)
	uint64_t frames;
};

//! Everything [C] slots answer from (guarded by leia_conv::mtx).
struct snapshot
{
	struct leia_lift_neurd_presence presence;
	struct leia_lift_neurd_status status;
	bool activated;   //!< status is authoritative (a warm ran)
	bool warming;
	bool device_lost;
	uint64_t device_luid;
	uint32_t licence_class;
	bool metric_model; //!< VideoModel=metric (setting), for the provisional describe
};

struct leia_conv
{
	std::mutex mtx; //!< snapshot + streams; never held across a module / D3D11 call
	struct snapshot snap = {};
	std::vector<conv_stream> streams;
	uint64_t next_handle = 0;
	std::atomic<uint64_t> generation{1};
	uint64_t describe_key = 0;
	uint32_t last_state = 0xffffffffu;

	struct leia_lift_neurd *lift = nullptr; //!< [S] only
	struct leia_conv_d3d11 *dx = nullptr;   //!< [S] only
};

leia_conv *g_inst = nullptr;

leia_conv *
I(struct xrt_conversion_plugin_instance *inst)
{
	return reinterpret_cast<leia_conv *>(inst);
}

void
copy_str(char *dst, size_t cap, const char *src)
{
	if (cap == 0) {
		return;
	}
	(void)snprintf(dst, cap, "%s", src != nullptr ? src : "");
}

//! Copy @p full into the runtime's @p out, only within out->struct_size, never rewriting struct_size itself.
template <typename T>
void
write_within(T *out, const T &full)
{
	const size_t n = out->struct_size < sizeof(T) ? out->struct_size : sizeof(T);
	if (n > sizeof(uint32_t)) {
		memcpy(reinterpret_cast<uint8_t *>(out) + sizeof(uint32_t),
		       reinterpret_cast<const uint8_t *>(&full) + sizeof(uint32_t), n - sizeof(uint32_t));
	}
}

bool
setting_is_metric_model()
{
	char v[32] = {};
	return host_setting("VideoModel", v, sizeof(v)) && (_stricmp(v, "metric") == 0);
}

struct leia_conv_module_view
view_of(const struct snapshot &s)
{
	struct leia_conv_module_view v = {};
	v.enabled = s.presence.enabled;
	v.found = s.presence.found;
	v.too_old = s.presence.too_old;
	v.module_status = s.activated ? s.status.module_status : LEIA_CONV_MODULE_UNKNOWN;
	v.network_retry = s.activated && s.status.network_retry;
	v.recheckable = s.activated && s.status.recheckable;
	v.warming = s.warming;
	v.device_lost = s.device_lost;
	return v;
}

bool
is_ready(const struct snapshot &s)
{
	struct leia_conv_module_view v = view_of(s);
	struct leia_conv_readiness r;
	leia_conv_readiness_of(&v, &r);
	return r.state == XRT_CONV_STATE_READY;
}

//! VIEWS_MONO can return the depth of the same inference: once READY, iff the module has it;
//! before, provisionally (declared outputs may be provisional, spec §3.4 rule 6).
bool
views_have_depth(const struct snapshot &s)
{
#ifdef DXR_LEIA_NEURD_HAS_CONVERT_EX
	if (s.activated && s.status.module_status == LEIA_LIFT_MODULE_READY) {
		return s.status.float_depth;
	}
	return true;
#else
	(void)s;
	return false;
#endif
}

bool
views_offaxis(const struct snapshot &s)
{
#ifdef DXR_LEIA_NEURD_HAS_CONVERT_EX
	if (s.activated && s.status.module_status == LEIA_LIFT_MODULE_READY) {
		return s.status.offaxis;
	}
	return true;
#else
	(void)s;
	return false;
#endif
}

bool
metric_depth(const struct snapshot &s)
{
	if (s.activated && s.status.module_status == LEIA_LIFT_MODULE_READY) {
		return s.status.metric_depth;
	}
	return s.metric_model;
}

void
module_version_str(const struct snapshot &s, char *buf, size_t cap)
{
	if (s.activated && s.status.version[0] != '\0') {
		copy_str(buf, cap, s.status.version);
	} else if (s.presence.found && s.presence.have_version) {
		(void)snprintf(buf, cap, "%u.%u.%u", s.presence.ver_major, s.presence.ver_minor, s.presence.ver_patch);
	} else {
		buf[0] = '\0';
	}
}

void
fill_cap(const struct snapshot &s, uint32_t index, struct xrt_conv_capability *c)
{
	static const uint32_t k_tasks[LEIA_CONV_CAP_COUNT] = {XRT_CONV_TASK_DEPTH_MONO, XRT_CONV_TASK_DEPTH_VIDEO,
	                                                      XRT_CONV_TASK_VIEWS_MONO};
	static const char *const k_names[LEIA_CONV_CAP_COUNT] = {"Leia NeurD depth (image)", "Leia NeurD depth (video)",
	                                                         "Leia NeurD stereo views"};
	c->cap_id = index + 1u;
	c->task = k_tasks[index];
	c->lanes = XRT_CONV_LANE_STREAM;
	c->input_kinds = XRT_CONV_INPUT_MONO;
	c->import_memory_kinds = XRT_CONV_MEM_CPU | XRT_CONV_MEM_WIN32_NT_HANDLE;
	c->import_sync_kinds =
	    (1u << XRT_CONV_SYNC_NONE) | (1u << XRT_CONV_SYNC_KEYED_MUTEX) | (1u << XRT_CONV_SYNC_FENCE_NT);
	c->input_formats = (1u << XRT_CONV_FMT_RGBA8_UNORM) | (1u << XRT_CONV_FMT_RGBA8_SRGB) |
	                   (1u << XRT_CONV_FMT_BGRA8_UNORM) | (1u << XRT_CONV_FMT_BGRA8_SRGB);
	c->depth_output_format = XRT_CONV_FMT_R32_FLOAT;
	c->max_streams = kMaxStreamsPerCap;
	c->max_concurrent = 1;
	c->max_input_edge = 0; // the module picks its own inference bucket
	c->preferred_input_edge = kPreferredInputEdge;
	c->depth_measure = XRT_CONV_MEASURE_PLANAR;
	c->flags = XRT_CONV_CAP_NEEDS_NETWORK_TO_WARM; // the module's first licence activation

	if (c->task == XRT_CONV_TASK_VIEWS_MONO) {
		// NeurD's public output types fix the tile grid at two views (docs/lift-neurd.md
		// "N-view"); more views come from the runtime's composed pipeline.
		c->min_views = 2;
		c->max_views = 2;
		c->flags |= XRT_CONV_CAP_EXPLICIT_VIEWPOINTS;
		if (views_offaxis(s)) {
			c->flags |= XRT_CONV_CAP_OFFAXIS_CAMERA;
		}
		c->outputs = XRT_CONV_OUT_VIEWS;
		if (views_have_depth(s)) {
			c->outputs |= XRT_CONV_OUT_DEPTH;
		}
		c->depth_kind = metric_depth(s) ? XRT_CONV_DEPTH_METRIC : XRT_CONV_DEPTH_AFFINE_INVERSE;
		c->intrinsics_source = XRT_CONV_INTRINSICS_ASSUMED;
	} else {
		c->outputs = XRT_CONV_OUT_DEPTH;
		if (c->task == XRT_CONV_TASK_DEPTH_VIDEO) {
			c->flags |= XRT_CONV_CAP_TEMPORAL;
		}
		// Per-frame min-max normalised disparity: inverse depth up to scale + shift.
		c->depth_kind = XRT_CONV_DEPTH_AFFINE_INVERSE;
		c->intrinsics_source = XRT_CONV_INTRINSICS_NONE;
	}

	c->licence_class = s.licence_class;
	c->commercial_ok = s.licence_class == XRT_CONV_LICENCE_COMMERCIAL ? 1u : 0u;
	copy_str(c->licence_spdx, sizeof(c->licence_spdx), kSpdx);
	copy_str(c->model_id, sizeof(c->model_id), metric_depth(s) ? "neurd/video-metric" : "neurd/video-relative");
	module_version_str(s, c->model_version, sizeof(c->model_version));
	if (s.activated && s.status.backend[0] != '\0') {
		copy_str(c->backend, sizeof(c->backend), s.status.backend);
	} else {
		copy_str(c->backend, sizeof(c->backend), "neurd");
	}
	copy_str(c->display_name, sizeof(c->display_name), k_names[index]);
	c->device_luid = s.device_luid;
	c->est_warmup_ms = kEstWarmupMs;
	c->est_latency_ms = s.status.latency_ns != 0 ? (uint32_t)(s.status.latency_ns / 1000000ull) : kPriorLatencyMs;
}

//! Hash of everything describe_cap answers from (the generation moves with it).
uint64_t
describe_key_of(const struct snapshot &s)
{
	uint64_t k = 1469598103934665603ull;
	auto mix = [&k](uint64_t v) { k = (k ^ v) * 1099511628211ull; };
	mix(views_have_depth(s) ? 1u : 0u);
	mix(views_offaxis(s) ? 1u : 0u);
	mix(metric_depth(s) ? 1u : 0u);
	mix(s.licence_class);
	mix(s.device_luid);
	char buf[48];
	module_version_str(s, buf, sizeof(buf));
	for (const char *p = buf; *p != '\0'; p++) {
		mix((uint8_t)*p);
	}
	for (const char *p = s.activated ? s.status.backend : ""; *p != '\0'; p++) {
		mix((uint8_t)*p);
	}
	return k;
}

/*!
 * After any [S] step: recompute the licence class, bump the generation when
 * the describe answer moved, and push notify_changed for a describe or
 * readiness change. Call WITHOUT d->mtx held.
 */
void
publish(leia_conv *d)
{
	bool describe_moved = false;
	bool state_moved = false;
	{
		std::lock_guard<std::mutex> g(d->mtx);
		struct snapshot &s = d->snap;
		s.licence_class = leia_conv_licence_class(s.activated ? s.status.licence : "", s.presence.product_name);
		const uint64_t key = describe_key_of(s);
		if (key != d->describe_key) {
			if (d->describe_key != 0) {
				d->generation.fetch_add(1);
				describe_moved = true;
			}
			d->describe_key = key;
		}
		struct leia_conv_module_view v = view_of(s);
		struct leia_conv_readiness r;
		leia_conv_readiness_of(&v, &r);
		const uint32_t st = (r.state << 8) | r.failure_reason;
		if (st != d->last_state) {
			if (d->last_state != 0xffffffffu) {
				state_moved = true;
			}
			d->last_state = st;
			U_LOG_W("Leia conversion: state %u (reason %u%s)", r.state, r.failure_reason,
			        r.retryable != 0 ? ", retryable" : "");
		}
	}
	if (describe_moved) {
		host_notify(0);
	} else if (state_moved) {
		for (uint32_t c = 1; c <= LEIA_CONV_CAP_COUNT; c++) {
			host_notify(c);
		}
	}
}

void
refresh_presence(leia_conv *d)
{
	struct leia_lift_neurd_presence p;
	leia_lift_neurd_probe_presence(d->lift, &p);
	const bool metric = setting_is_metric_model();
	std::lock_guard<std::mutex> g(d->mtx);
	d->snap.presence = p;
	d->snap.metric_model = metric;
}

void
refresh_status(leia_conv *d)
{
	struct leia_lift_neurd_status st;
	leia_lift_neurd_get_status(d->lift, &st);
	std::lock_guard<std::mutex> g(d->mtx);
	d->snap.status = st;
	d->snap.activated = true;
}

bool
find_stream(leia_conv *d, uint64_t handle, conv_stream *out)
{
	std::lock_guard<std::mutex> g(d->mtx);
	for (const auto &s : d->streams) {
		if (s.handle == handle) {
			*out = s;
			return true;
		}
	}
	return false;
}

/*
 *
 * Provider iface.
 *
 */

bool
lc_get_provider_status(struct xrt_conv_provider_status *out)
{
	if (out == nullptr) {
		return false;
	}
	struct xrt_conv_provider_status full = {};
	full.struct_size = out->struct_size;
	struct leia_lift_neurd_presence p = {};
	if (g_inst != nullptr) {
		std::lock_guard<std::mutex> g(g_inst->mtx);
		p = g_inst->snap.presence;
	} else {
		// Before probe: a throwaway handle (reads the settings, loads nothing).
		struct leia_lift_neurd *l = leia_lift_neurd_create();
		leia_lift_neurd_probe_presence(l, &p);
		leia_lift_neurd_destroy(&l);
	}
	// xrt_plugin_platform_state values: 1 READY, 2 PLATFORM_ABSENT, 5 INCOMPATIBLE.
	if (!p.enabled) {
		full.state = 1u;
		copy_str(full.hint, sizeof(full.hint), "2D->3D conversion turned off by the provider's Enable setting");
	} else if (!p.found) {
		full.state = 2u;
		copy_str(full.hint, sizeof(full.hint), "the NeurD conversion module is not installed");
	} else if (p.too_old) {
		full.state = 5u;
		(void)snprintf(full.hint, sizeof(full.hint), "NeurD %u.%u.%u is older than the minimum %u.%u.%u",
		               p.ver_major, p.ver_minor, p.ver_patch, p.min_major, p.min_minor, p.min_patch);
	} else {
		full.state = 1u;
		copy_str(full.hint, sizeof(full.hint), "the NeurD conversion module is installed");
	}
	write_within(out, full);
	return true;
}

xrt_result_t
lc_probe(struct xrt_conversion_plugin_instance **out_inst)
{
	if (out_inst == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (g_inst == nullptr) {
		auto *d = new (std::nothrow) leia_conv();
		if (d == nullptr) {
			return XRT_ERROR_ALLOCATION;
		}
		d->lift = leia_lift_neurd_create(); // reads the settings; loads nothing
		if (d->lift == nullptr) {
			delete d;
			return XRT_ERROR_ALLOCATION;
		}
		g_inst = d;
		refresh_presence(d); // SearchPath + one registry read + the version resource: no load
		publish(d);
		const struct leia_lift_neurd_presence &p = d->snap.presence;
		U_LOG_W("Leia conversion: probe — module %s%s%s%s", !p.enabled ? "DISABLED (Enable=0)" : p.found ? "found (" : "not found",
		        p.found ? p.where : "", p.found ? ")" : "", p.too_old ? ", OLDER than the floor" : "");
	}
	*out_inst = reinterpret_cast<struct xrt_conversion_plugin_instance *>(g_inst);
	return XRT_SUCCESS;
}

xrt_result_t
lc_describe_cap(struct xrt_conversion_plugin_instance *inst,
                uint32_t index,
                struct xrt_conv_capability *out,
                uint32_t *out_count)
{
	if (out_count != nullptr) {
		*out_count = LEIA_CONV_CAP_COUNT;
	}
	if (out == nullptr) {
		return XRT_SUCCESS;
	}
	if (index >= LEIA_CONV_CAP_COUNT || inst == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_conv_capability full = {};
	full.struct_size = out->struct_size;
	{
		std::lock_guard<std::mutex> g(I(inst)->mtx);
		fill_cap(I(inst)->snap, index, &full);
	}
	write_within(out, full);
	return XRT_SUCCESS;
}

uint64_t
lc_get_generation(struct xrt_conversion_plugin_instance *inst)
{
	return inst != nullptr ? I(inst)->generation.load() : 0;
}

xrt_result_t
lc_get_state(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, struct xrt_conv_state *out)
{
	if (inst == nullptr || out == nullptr || cap_id == 0 || cap_id > LEIA_CONV_CAP_COUNT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_conv_state full = {};
	full.struct_size = out->struct_size;
	full.progress = -1.0f;
	{
		std::lock_guard<std::mutex> g(I(inst)->mtx);
		const struct snapshot &s = I(inst)->snap;
		struct leia_conv_module_view v = view_of(s);
		struct leia_conv_readiness r;
		leia_conv_readiness_of(&v, &r);
		full.state = r.state;
		full.failure_reason = r.failure_reason;
		full.retryable = r.retryable;
		module_version_str(s, full.version, sizeof(full.version));
		const struct leia_lift_neurd_presence &p = s.presence;
		if (s.device_lost) {
			copy_str(full.detail, sizeof(full.detail), "the provider's D3D11 device was lost; re-warming");
		} else if (s.warming) {
			copy_str(full.detail, sizeof(full.detail), "loading NeurD (licence check, model load)");
		} else if (s.activated) {
			copy_str(full.detail, sizeof(full.detail), s.status.detail);
		} else if (!p.enabled) {
			copy_str(full.detail, sizeof(full.detail), "turned off (Enable=0)");
		} else if (!p.found) {
			copy_str(full.detail, sizeof(full.detail), "NeurD is not installed");
		} else if (p.too_old) {
			(void)snprintf(full.detail, sizeof(full.detail), "NeurD %u.%u.%u is older than the minimum %u.%u.%u",
			               p.ver_major, p.ver_minor, p.ver_patch, p.min_major, p.min_minor, p.min_patch);
		} else {
			copy_str(full.detail, sizeof(full.detail), "NeurD installed, not loaded yet");
		}
	}
	write_within(out, full);
	return XRT_SUCCESS;
}

bool
never_cancelled(void *ctx)
{
	(void)ctx;
	return false;
}

xrt_result_t
lc_warm(struct xrt_conversion_plugin_instance *inst,
        uint32_t cap_id,
        bool (*is_cancelled)(void *ctx),
        void *cancel_ctx)
{
	leia_conv *d = I(inst);
	if (d == nullptr || cap_id == 0 || cap_id > LEIA_CONV_CAP_COUNT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (is_cancelled == nullptr) {
		is_cancelled = never_cancelled;
	}
	{
		std::lock_guard<std::mutex> g(d->mtx);
		if (is_ready(d->snap) && d->dx != nullptr) {
			return XRT_SUCCESS;
		}
	}
	if (is_cancelled(cancel_ctx)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	// A lost device is re-created below (DEVICE_LOST -> re-warm, spec §2.4).
	if (d->dx != nullptr && leia_conv_d3d11_lost(d->dx)) {
		leia_conv_d3d11_destroy(&d->dx);
	}
	{
		std::lock_guard<std::mutex> g(d->mtx);
		d->snap.warming = true;
		d->snap.device_lost = false;
	}
	publish(d);

	// Discovery + load + init on THIS (runtime-owned) thread. NeurD's init is not
	// interruptible; is_cancelled is honoured before and after it.
	refresh_presence(d);
	uint32_t ms = LEIA_LIFT_MODULE_DISABLED;
	{
		struct leia_lift_neurd_presence p;
		{
			std::lock_guard<std::mutex> g(d->mtx);
			p = d->snap.presence;
		}
		if (p.enabled) {
			ms = leia_lift_neurd_activate(d->lift);
		}
	}
	refresh_status(d);

	bool device_ok = true;
	if (ms == LEIA_LIFT_MODULE_READY && d->dx == nullptr && !is_cancelled(cancel_ctx)) {
		uint64_t preferred = 0, avoid = 0;
		(void)host_adapter_hint(&preferred, &avoid);
		if (preferred == 0) {
			struct leia_lift_neurd_status st;
			leia_lift_neurd_get_status(d->lift, &st);
			preferred = st.nd_luid; // no hint: next to NeurD, so its bridge stays same-adapter
		}
		d->dx = leia_conv_d3d11_create(preferred, avoid);
		device_ok = d->dx != nullptr;
	}
	{
		std::lock_guard<std::mutex> g(d->mtx);
		d->snap.warming = false;
		d->snap.device_lost = ms == LEIA_LIFT_MODULE_READY && !device_ok;
		d->snap.device_luid = d->dx != nullptr ? leia_conv_d3d11_luid(d->dx) : 0;
	}
	publish(d);

	std::lock_guard<std::mutex> g(d->mtx);
	return is_ready(d->snap) && d->dx != nullptr ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

xrt_result_t
lc_evict(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id)
{
	(void)inst;
	(void)cap_id;
	// NeurD is never de-initialised (a reload re-runs licensing and the model
	// load); the capability stays READY. Idempotent.
	return XRT_SUCCESS;
}

xrt_result_t
lc_stream_create(struct xrt_conversion_plugin_instance *inst,
                 uint32_t cap_id,
                 const struct xrt_conv_stream_info *info,
                 uint64_t *out_stream)
{
	leia_conv *d = I(inst);
	if (d == nullptr || info == nullptr || out_stream == nullptr || cap_id == 0 || cap_id > LEIA_CONV_CAP_COUNT ||
	    !XRT_CONV_COVERS(info, struct xrt_conv_stream_info, outputs)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_conv_stream_info si = {};
	memcpy(&si, info, info->struct_size < sizeof(si) ? info->struct_size : sizeof(si));

	bool have_depth = false;
	uint32_t per_cap = 0;
	{
		std::lock_guard<std::mutex> g(d->mtx);
		if (!is_ready(d->snap) || d->dx == nullptr) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED; // [S] contract: cap READY
		}
		have_depth = views_have_depth(d->snap);
		for (const auto &s : d->streams) {
			per_cap += s.cap_id == cap_id ? 1u : 0u;
		}
	}
	if (per_cap >= kMaxStreamsPerCap) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (si.input_kind != 0 && si.input_kind != XRT_CONV_INPUT_MONO) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	conv_stream ns = {};
	ns.cap_id = cap_id;
	struct leia_lift_neurd_stream_desc desc = {};
	desc.input_scale = 1.0f; // the runtime already applied any input scale
	if (cap_id == LEIA_CONV_CAP_VIEWS_MONO) {
		if (si.view_count != 0 && si.view_count != 2) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED; // max_views = 2
		}
		if (si.views_layout != XRT_CONV_VIEWS_ROW && si.views_layout != XRT_CONV_VIEWS_ARRAY) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
		if ((si.outputs & XRT_CONV_OUT_DEPTH) != 0 && !have_depth) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED; // READY and lacks the output
		}
		ns.layout = si.views_layout;
		ns.want_depth = (si.outputs & XRT_CONV_OUT_DEPTH) != 0;
		desc.mode = LEIA_LIFT_MODE_SBS;
		desc.content_hint = si.content_hint == XRT_CONV_CONTENT_PHOTO ? 1u : 0u;
		desc.aux_outputs = ns.want_depth ? LEIA_LIFT_AUX_DEPTH : 0u;
	} else {
		desc.mode = LEIA_LIFT_MODE_DEPTH;
		desc.content_hint = cap_id == LEIA_CONV_CAP_DEPTH_MONO ? 1u : 0u;
	}
	if (!leia_lift_neurd_stream_create(d->lift, &desc, &ns.lift_id)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	std::lock_guard<std::mutex> g(d->mtx);
	ns.handle = ++d->next_handle;
	d->streams.push_back(ns);
	*out_stream = ns.handle;
	return XRT_SUCCESS;
}

xrt_result_t
lc_stream_get_output_desc(struct xrt_conversion_plugin_instance *inst,
                          uint64_t stream,
                          uint32_t input_width,
                          uint32_t input_height,
                          struct xrt_conv_output_desc *out)
{
	leia_conv *d = I(inst);
	conv_stream s;
	if (d == nullptr || out == nullptr || !find_stream(d, stream, &s)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_conv_output_desc full = {};
	full.struct_size = out->struct_size;
	if (s.cap_id == LEIA_CONV_CAP_VIEWS_MONO) {
		leia_conv_views_extent(input_width, input_height, 2u, s.layout == XRT_CONV_VIEWS_ARRAY, kMaxTexDim,
		                       &full.views_width, &full.views_height);
		full.views_format = XRT_CONV_FMT_RGBA8_UNORM;
		if (s.want_depth) {
			leia_conv_depth_extent(input_width, input_height, kAuxDepthMaxEdge, &full.depth_width,
			                       &full.depth_height);
			full.depth_format = XRT_CONV_FMT_R32_FLOAT;
		}
	} else {
		leia_conv_depth_extent(input_width, input_height, kDepthMaxEdge, &full.depth_width, &full.depth_height);
		full.depth_format = XRT_CONV_FMT_R32_FLOAT;
	}
	write_within(out, full);
	return XRT_SUCCESS;
}

xrt_result_t
lc_stream_convert(struct xrt_conversion_plugin_instance *inst,
                  uint64_t stream,
                  const struct xrt_conv_frame_in *in,
                  const struct xrt_conv_params *params,
                  struct xrt_conv_frame_out *out)
{
	leia_conv *d = I(inst);
	conv_stream s;
	if (d == nullptr || in == nullptr || params == nullptr || out == nullptr || d->dx == nullptr ||
	    !find_stream(d, stream, &s) || !XRT_CONV_COVERS(in, struct xrt_conv_frame_in, images) ||
	    !XRT_CONV_COVERS(out, struct xrt_conv_frame_out, depth_info)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (in->image_count < 1 || in->image_role[0] != XRT_CONV_IMG_COLOR) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_conv_params pa = {};
	memcpy(&pa, params, params->struct_size < sizeof(pa) ? params->struct_size : sizeof(pa));
	const bool views = s.cap_id == LEIA_CONV_CAP_VIEWS_MONO;

	void *tex = nullptr;
	uint32_t w = 0, h = 0;
	if (!leia_conv_d3d11_import_input(d->dx, &in->images[0], &tex, &w, &h)) {
		goto fail;
	}
	if ((pa.frame_flags & XRT_CONV_FRAME_RESET) != 0) {
		leia_lift_neurd_stream_reset(d->lift, s.lift_id); // cut / seek: fresh temporal state
	}

	{
		// ---- Parameters: the runtime decided everything; translate units only.
		struct leia_lift_neurd_params lp = {};
		leia_conv_convergence(pa.convergence_mode, pa.convergence, s.depth_kind, &lp.convergence,
		                      &lp.convergence_metric_m);
		lp.strength = (pa.strength > 0.0f && std::isfinite(pa.strength)) ? pa.strength : 1.0f;
		lp.inpaint = (pa.frame_flags & XRT_CONV_FRAME_INPAINT) != 0 ? 1u : 0u;
		lp.view_count = 2;
		lp.has_policy = true;
		lp.rect_width_m = pa.rect_width_m;
		lp.rect_height_m = pa.rect_height_m;
		lp.baseline_m = pa.baseline_m;
		lp.axis_mode = pa.axis_mode != 0 ? pa.axis_mode : XRT_CONV_AXIS_X;
		lp.max_offset_m = pa.max_offset_m;
		lp.viewpoint_frame = pa.viewpoint_frame;
		lp.ref_z_m = pa.nominal_z_m > 0.0f ? pa.nominal_z_m : 0.0f;
		lp.has_app_rig = true;
		lp.viewpoint_source = pa.viewpoint_source;
		lp.nominal_z_m = pa.nominal_z_m;

		float vps[6] = {};
		uint32_t vfloats = 0;
		if (views && pa.viewpoint_count >= 2) {
			memcpy(vps, pa.viewpoints_xyz, sizeof(vps));
			vfloats = 6;
		}

		void *res = nullptr;
		uint32_t ow = 0, oh = 0, ofmt = 0;
		if (!leia_lift_neurd_convert(d->lift, s.lift_id, leia_conv_d3d11_context(d->dx), tex, w, h, &lp,
		                             vfloats > 0 ? vps : nullptr, vfloats, nullptr, nullptr, false, &res, &ow, &oh,
		                             &ofmt) ||
		    res == nullptr) {
			goto fail;
		}

		uint32_t written = 0;
		if (views) {
			if (out->views.memory_kind != 0) {
				bool ok = true;
				if (s.layout == XRT_CONV_VIEWS_ARRAY) {
					for (uint32_t v = 0; v < 2 && ok; v++) {
						ok = leia_conv_d3d11_write(d->dx, &out->views, out->views.array_layer + v, res,
						                           0.5f * (float)v, 0.5f * (float)(v + 1), LEIA_CONV_BLIT_COLOR);
					}
				} else {
					ok = leia_conv_d3d11_write(d->dx, &out->views, out->views.array_layer, res, 0.0f, 1.0f,
					                           LEIA_CONV_BLIT_COLOR);
				}
				if (!ok) {
					goto fail;
				}
				written |= XRT_CONV_OUT_VIEWS;
				struct xrt_conv_views_info *vi = &out->views_info;
				memset(vi, 0, sizeof(*vi));
				vi->view_count = 2;
				vi->layout = s.layout;
				const uint32_t m = pa.viewpoint_count > 2 ? 2 : pa.viewpoint_count;
				memcpy(vi->eye_xyz, pa.viewpoints_xyz, sizeof(float) * 3 * m);
				float b = pa.baseline_m;
				if (!(b > 0.0f) && m == 2) {
					const float dx = vps[3] - vps[0], dy = vps[4] - vps[1], dz = vps[5] - vps[2];
					b = std::sqrt(dx * dx + dy * dy + dz * dz);
				}
				vi->baseline_m = b;
			}
			struct leia_lift_neurd_depth dd = {};
			if (s.want_depth && out->depth.memory_kind != 0 && leia_lift_neurd_get_depth(d->lift, s.lift_id, &dd) &&
			    dd.resource != nullptr) {
				if (!leia_conv_d3d11_write(d->dx, &out->depth, out->depth.array_layer, dd.resource, 0.0f, 1.0f,
				                           LEIA_CONV_BLIT_DEPTH)) {
					goto fail;
				}
				written |= XRT_CONV_OUT_DEPTH;
				// Same mapping as the runtime's dp-lift adapter (parity with the
				// display plug-in's lift slots), intrinsics rescaled to the target.
				struct xrt_conv_depth_info *di = &out->depth_info;
				memset(di, 0, sizeof(*di));
				di->kind = leia_conv_depth_kind(dd.units, dd.encoding);
				di->measure = XRT_CONV_MEASURE_PLANAR;
				di->value_scale = dd.value_scale != 0.0f && std::isfinite(dd.value_scale) ? dd.value_scale : 1.0f;
				di->value_shift = std::isfinite(dd.value_offset) ? dd.value_offset : 0.0f;
				di->near_value = dd.near_depth > 0.0f ? dd.near_depth : 0.0f;
				di->far_value = dd.far_depth > 0.0f ? dd.far_depth : 0.0f;
				di->invalid_sample = NAN;
				if (dd.focal_x_px > 0.0f && dd.focal_y_px > 0.0f && dd.width > 0 && dd.height > 0) {
					const float sx = (float)out->depth.width / (float)dd.width;
					const float sy = (float)out->depth.height / (float)dd.height;
					di->fx = dd.focal_x_px * sx;
					di->fy = dd.focal_y_px * sy;
					di->cx = dd.principal_x_px * sx;
					di->cy = dd.principal_y_px * sy;
					di->intrinsics_source = XRT_CONV_INTRINSICS_ASSUMED;
				}
				di->source_width = dd.source_width != 0 ? dd.source_width : w;
				di->source_height = dd.source_height != 0 ? dd.source_height : h;
				if (dd.convergence_depth > 0.0f) {
					di->convergence_value = dd.convergence_depth;
					di->suggest_flags |= 4u; // convergence_value valid
				}
				if (dd.relief_valid != 0 && std::isfinite(dd.relief_scale) && std::isfinite(dd.relief_offset) &&
				    dd.relief_scale != 0.0f) {
					di->relief_valid = 1;
					di->relief_scale = dd.relief_scale;
					di->relief_offset = dd.relief_offset;
				}
				di->same_inference = dd.same_inference != 0 ? 1u : 0u;
				di->source_time_ns = pa.source_time_ns;
				di->provider_frame_id = dd.vendor_frame_id;
				s.depth_kind = di->kind;
			}
		} else if (out->depth.memory_kind != 0) {
			// The module's R8 relative depth (larger = farther) -> R32F inverse
			// depth up to scale + shift (larger = nearer): 1 - r.
			if (!leia_conv_d3d11_write(d->dx, &out->depth, out->depth.array_layer, res, 0.0f, 1.0f,
			                           ofmt == 61u /* DXGI_FORMAT_R8_UNORM */ ? LEIA_CONV_BLIT_DEPTH_FLIP
			                                                                 : LEIA_CONV_BLIT_DEPTH)) {
				goto fail;
			}
			written |= XRT_CONV_OUT_DEPTH;
			struct xrt_conv_depth_info *di = &out->depth_info;
			memset(di, 0, sizeof(*di));
			di->kind = XRT_CONV_DEPTH_AFFINE_INVERSE;
			di->measure = XRT_CONV_MEASURE_PLANAR;
			di->intrinsics_source = XRT_CONV_INTRINSICS_NONE;
			di->value_scale = 1.0f;
			di->invalid_sample = NAN;
			di->source_width = w;
			di->source_height = h;
			di->temporal = s.cap_id == LEIA_CONV_CAP_DEPTH_VIDEO ? 1u : 0u;
			di->same_inference = 1;
			di->source_time_ns = pa.source_time_ns;
			di->provider_frame_id = s.frames + 1;
			s.depth_kind = di->kind;
		}
		if (!leia_conv_d3d11_finish(d->dx)) {
			goto fail;
		}
		out->written = written;
		if (written == 0) {
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // nothing requested we could write: keep the previous result
		}
	}

	{
		std::lock_guard<std::mutex> g(d->mtx);
		for (auto &it : d->streams) {
			if (it.handle == stream) {
				it.frames++;
				it.depth_kind = s.depth_kind;
			}
		}
	}
	return XRT_SUCCESS;

fail:
	// Release whatever was acquired / written so far (keyed mutex, fence) so
	// nothing waits on a target this convert gave up on.
	(void)leia_conv_d3d11_finish(d->dx);
	out->written = 0;
	if (leia_conv_d3d11_lost(d->dx)) {
		{
			std::lock_guard<std::mutex> g(d->mtx);
			d->snap.device_lost = true;
		}
		publish(d);
	}
	return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // the runtime keeps the previous result
}

void
lc_stream_destroy(struct xrt_conversion_plugin_instance *inst, uint64_t stream)
{
	leia_conv *d = I(inst);
	if (d == nullptr) {
		return;
	}
	uint64_t lift_id = 0;
	bool found = false;
	{
		std::lock_guard<std::mutex> g(d->mtx);
		for (auto it = d->streams.begin(); it != d->streams.end(); ++it) {
			if (it->handle == stream) {
				lift_id = it->lift_id;
				d->streams.erase(it);
				found = true;
				break;
			}
		}
	}
	if (found) {
		leia_lift_neurd_stream_destroy(d->lift, lift_id);
		leia_conv_d3d11_clear_cache(d->dx); // import caches are dropped with the stream (§2.4)
	}
}

xrt_result_t
lc_job_execute(struct xrt_conversion_plugin_instance *inst,
               uint32_t cap_id,
               const struct xrt_conv_job_in *in,
               const struct xrt_conv_params *params,
               const struct xrt_conv_job_control *ctl,
               struct xrt_conv_job_out *out)
{
	(void)inst;
	(void)cap_id;
	(void)in;
	(void)params;
	(void)ctl;
	(void)out;
	return XRT_ERROR_FEATURE_NOT_SUPPORTED; // no capability declares the job lane
}

void
lc_destroy(struct xrt_conversion_plugin_instance *inst)
{
	leia_conv *d = I(inst);
	if (d == nullptr) {
		return;
	}
	std::vector<conv_stream> streams;
	{
		std::lock_guard<std::mutex> g(d->mtx);
		streams.swap(d->streams);
	}
	for (const auto &s : streams) {
		leia_lift_neurd_stream_destroy(d->lift, s.lift_id);
	}
	leia_conv_d3d11_destroy(&d->dx);
	leia_lift_neurd_destroy(&d->lift);
	if (g_inst == d) {
		g_inst = nullptr;
	}
	delete d;
}

struct xrt_conversion_plugin_iface g_iface = {
    (uint32_t)sizeof(struct xrt_conversion_plugin_iface),
    0,
    LEIA_CONV_ID,
    "Leia conversion (NeurD)",
    "Leia Inc.",
    DXR_PLUGIN_GIT_DESC,
    lc_get_provider_status,
    lc_probe,
    lc_describe_cap,
    lc_get_generation,
    lc_get_state,
    lc_warm,
    lc_evict,
    lc_stream_create,
    lc_stream_get_output_desc,
    lc_stream_convert,
    lc_stream_destroy,
    lc_job_execute,
    lc_destroy,
};

} // namespace

/*!
 * The DLL's only export. Stores @p host (valid until process end), hosts the
 * NeurD module the provider way (settings through the host, no activation
 * thread) and hands out the v1 iface. A runtime of another major is answered
 * with this provider's version and no iface (the loader rejects it).
 */
extern "C" XRT_CONVERSION_PLUGIN_EXPORT xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version)
{
	if (out_iface == nullptr || out_plugin_api_version == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	*out_plugin_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	if (runtime_api_version != XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT) {
		*out_iface = nullptr;
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	g_host = host;
	leia_lift_neurd_set_setting_reader(host_setting);
	leia_lift_neurd_set_manual_activation(true);
	*out_iface = &g_iface;
	return XRT_SUCCESS;
}
