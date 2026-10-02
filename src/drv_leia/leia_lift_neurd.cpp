// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  NeurD-backed 2D->3D conversion ("lift") for the Leia D3D11 DP.
 *
 * Design notes (full write-up: docs/lift-neurd.md):
 *
 *  - Dynamic load only. NeurD.dll is located the way NeurD's own loader does it
 *    (PATH -> NEURD_PATH -> HKLM\SOFTWARE\LeiaInc\NeurD default value) but it
 *    is not a link dependency: a machine without NeurD loads this plug-in
 *    exactly as before and lift reports unavailable.
 *
 *  - One NeurD instance per process (NeurD's rule). Loaded + initialised on a
 *    detached background thread, never on a caller's thread: NeurD_init runs a
 *    licence activation that needs the network on first use. A network failure
 *    is reported as ACTIVATING and retried on later calls, rate-limited.
 *    DP instances come and go on focus changes, and re-init re-runs licensing +
 *    model load, so the DP ref-count only decides when to shrink NeurD's memory
 *    pool. What unloads NeurD is IDLE TIME (IdleUnloadSec, default 300 s): once
 *    a READY NeurD has had no lift stream for that long, a watcher thread
 *    deinits it and FreeLibrary's it, so NeurD's installer can replace the
 *    files the long-lived service would otherwise lock forever. The module then
 *    sits in G_IDLE — caps still say READY (the runtime froze them at READY
 *    anyway), stream_create still succeeds and kicks a full re-activation, and
 *    converts fail fast until NeurD is READY again (~5 s). NeurD's OpenSSL
 *    pins itself and can never unload, so a private copy of it is loaded
 *    first (ShadowDeps) and that copy is what stays pinned.
 *
 *  - Device bridge (the LeiaMeet DxStereoConverter pattern). NeurD runs on ITS
 *    OWN D3D11 device (NeurD_get_dx_device, 0.4.3+), possibly on a different adapter
 *    than the runtime's service device on hybrid boxes. NeurD's DX input must
 *    be a raw RGBA8 ID3D11Buffer with D3D11_RESOURCE_MISC_SHARED, and NeurD
 *    hands its output back on the input buffer's device. Buffers do not share
 *    reliably across devices, textures do — so all buffers live on NeurD's
 *    device, and the crossing is done with legacy-shared TEXTURES created on
 *    NeurD's device and opened on ours:
 *
 *        ours:  input tex --CopySubresourceRegion--> in_bridge (opened)
 *        CPU:   event-query drain on our context
 *        NeurD: in_bridge SRV --pack CS--> raw input buffer ; drain
 *        NeurD: convert_stream_dx[_interactive] (blocking)
 *        NeurD: output buffer --unpack CS--> out_bridge UAV ; drain
 *        ours:  out_bridge (opened) is returned to the runtime
 *
 *    Every hop is CPU-drained with an event query: NeurD's DirectML work runs
 *    on its own queue, unordered against any D3D11 context.
 *
 *    NeurD 0.3.11-0.4.2 has the stream API but no get_dx_device. There the
 *    plug-in creates the "NeurD device" itself (default adapter, like the SDK's
 *    own example) and owns it; NeurD 0.3.x's CUDA DX path runs its D3D11-CUDA
 *    interop on whatever device the input buffer lives on and hands back an
 *    RGBA8 texture on that same device, so the bridge is unchanged.
 *
 *  - NeurD properties are process-global, so the (set props -> convert) pair
 *    must be atomic across streams: one process-wide mutex serialises every
 *    NeurD call and every use of NeurD's immediate context.
 *
 *  - Never throws, never blocks the caller on activation; errors return false
 *    with a WARN once per site.
 */

#include "leia_lift_neurd.h"
// The REAL NeurD header, fetched at build time from the private LeiaInc/media_sdk
// repo at the pinned NEURD_SDK_REF (never committed here; see docs/lift-neurd.md).
// Only its types, PFN typedefs and header-inline table wrappers are used — the
// wrappers dispatch through the table NeurD_load returns and version-check each
// entry — so there is no import lib and no link dependency on NeurD, and any
// NeurD signature drift is a compile error in this file.
#include <NeurD.h>

#include "util/u_logging.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h> // ID3D11Multithread
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

//! Table entry @p name is present in the LOADED runtime (version test first, so
//! an older, physically shorter table is never read past its end). The
//! NEURD_<name>_INTRODUCED_IN constants come from the real header.
#define LEIA_NEURD_HAS(nd, name) ((nd) != nullptr && (nd)->version >= NEURD_##name##_INTRODUCED_IN && (nd)->name != nullptr)

namespace {

/*
 *
 * Constants + small helpers.
 *
 */

//! Reference inter-pupillary distance: NeurD's default stereo pattern places
//! the two views at x = -0.5 / +0.5, i.e. ONE unit of x == one "nominal eye
//! baseline". Mapping metres through this IPD makes a centred, tracked viewer
//! reproduce the default pattern exactly.
constexpr float kIpdRefM = 0.063f;
//! Clamp for mapped viewpoints — well past any comfortable look-around; stops a
//! tracker glitch from asking NeurD for a wildly extrapolated view.
constexpr float kViewpointClamp = 3.0f;
//! NEURD_PROP_GAIN_MULTIPLIER at strength 1 (DepthGain knob). Panel-calibrated 2026-09-26.
constexpr float kDefaultDepthGain = 2.0f;
//! NEURD_PROP_DILATE_RADIO (Dilate knob). NeurD's own default is 3; the foreground grew visibly
//! past its silhouette on the panel (David, 2026-09-26: "the disparity map dilation is a bit too
//! much, reduce a bit").
constexpr int32_t kDefaultDilate = 2;
//! Oldest NeurD lift accepts by default (MinVersion knob). A product decision, deliberately NOT
//! NEURD_VERSION: only 0.4.6 is hardware-tested, and older ones fail converts with the default
//! backend instead of reporting unavailable, so callers would not fall back to their own path.
constexpr uint64_t kDefaultMinNeurDVersion = NEURD_MAKE_VERSION(0, 4, 6);
//! NeurD's MAX_STREAMS.
constexpr uint32_t kMaxStreams = 32;
//! N views go side by side in ONE row (runtime contract), so the output is
//! N x inference-width wide; 8 x 2560 (1440p) still fits D3D11's 16384 limit.
constexpr uint32_t kMaxViews = 8;
constexpr uint32_t kMaxTexDim = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
//! Retry period for a licence activation that failed on the network.
constexpr uint64_t kActivationRetryMs = 15000;
//! IdleUnloadSec: default, and the clamp for a non-zero value (0 = never unload).
constexpr uint32_t kDefaultIdleUnloadSec = 300;
constexpr uint32_t kIdleUnloadMinSec = 5;
constexpr uint32_t kIdleUnloadMaxSec = 86400;
//! The idle watcher re-reads the idle clock at least this often, so it notices a
//! new stream (and exits) promptly instead of sleeping out a stale deadline.
constexpr uint64_t kIdleWatchMaxSleepMs = 1000;
//! Pause between NeurD_deinit and FreeLibrary. Insurance, not a guarantee: NeurD
//! documents no join of its worker thread (init(multithreaded) runs one), and a
//! thread still unwinding inside NeurD.dll when it is unmapped would crash.
constexpr DWORD kUnloadSettleMs = 250;
//! A PHOTO stream's convert waits this long (polling, no lock held) for a reload
//! in progress instead of failing: a photo may submit exactly one frame, and the
//! runtime — whose caps froze at READY — consumes a failed frame for good. Same
//! order as convert's existing worst case (three kDrainTimeoutMs drains).
constexpr uint64_t kPhotoReloadWaitMs = 8000;
constexpr DWORD kReloadPollMs = 20;
//! Upper bound for any CPU drain of a GPU queue (device-lost guard).
constexpr uint64_t kDrainTimeoutMs = 2000;
//! Latency priors (reported until a measurement exists).
constexpr uint64_t kPriorLatencyDirectMlNs = 22000000ull;
constexpr uint64_t kPriorLatencyCudaNs = 14000000ull;
constexpr uint64_t kPriorLatencyOtherNs = 40000000ull;

#define LIFT_WARN_ONCE(...)                                                                                    \
	do {                                                                                                   \
		static std::atomic<bool> warned_{false};                                                       \
		if (!warned_.exchange(true)) {                                                                 \
			U_LOG_W(__VA_ARGS__);                                                                  \
		}                                                                                              \
	} while (0)

template <typename T>
void
safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

inline float
clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

const char *
status_str(enum NeurD_status s)
{
	switch (s) {
	case NEURD_SUCCESS: return "SUCCESS";
	case NEURD_IN_PROGRESS: return "IN_PROGRESS";
	case NEURD_GENERIC_ERROR: return "GENERIC_ERROR";
	case NEURD_INVALID_ARG: return "INVALID_ARG";
	case NEURD_INVALID_LICENSE: return "INVALID_LICENSE";
	case NEURD_UNAVAILABLE_OUTDATED_RUNTIME: return "UNAVAILABLE_OUTDATED_RUNTIME";
	case NEURD_NOT_INITIALIZED: return "NOT_INITIALIZED";
	case NEURD_LICENSE_NETWORK_ERROR: return "LICENSE_NETWORK_ERROR";
	case NEURD_INVALID_MODEL: return "INVALID_MODEL";
	case NEURD_FILESYSTEM_ERROR: return "FILESYSTEM_ERROR";
	case NEURD_NETWORK_ERROR: return "NETWORK_ERROR";
	case NEURD_ABORTED: return "ABORTED";
	default: return "UNKNOWN";
	}
}

const char *
backend_str(enum NeurD_backend b)
{
	switch (b) {
	case NEURD_BACKEND_CUDA: return "cuda";
	case NEURD_BACKEND_DIRECTML: return "directml";
	case NEURD_BACKEND_OPENVINO: return "openvino";
	default: return "unknown";
	}
}

/*
 *
 * Knobs (read once per DP handle, at leia_lift_neurd_create). Each comes from
 * the environment, else HKLM\SOFTWARE\DisplayXR\Leia\Lift (REG_SZ, same
 * grammar), else its default. The registry exists because a respawned service
 * (tray relaunch, HKLM Run at logon, crash restart) starts with the logon env
 * and would otherwise lose env-only knobs silently.
 *
 */

enum backend_choice
{
	BACKEND_AUTO = -1,
	BACKEND_CUDA = NEURD_BACKEND_CUDA,
	BACKEND_DIRECTML = NEURD_BACKEND_DIRECTML,
	BACKEND_OPENVINO = NEURD_BACKEND_OPENVINO,
};

enum knob_src
{
	KSRC_DEFAULT = 0,
	KSRC_ENV,
	KSRC_REG,
};

const char *
knob_src_str(knob_src s)
{
	switch (s) {
	case KSRC_ENV: return "env";
	case KSRC_REG: return "reg";
	default: return "default";
	}
}

struct knobs
{
	bool enabled;             //!< DXR_LEIA_LIFT (default on; env only)
	int backend;              //!< DXR_LEIA_LIFT_BACKEND / Backend (default directml)
	int32_t autoscaling;      //!< DXR_LEIA_LIFT_SCALE / Scale (default 720p)
	bool scale_forced;        //!< Scale was set: overrides the stream's input_scale
	float view_gain;          //!< DXR_LEIA_LIFT_VIEW_GAIN / ViewGain (default 1.0)
	float conv_gain;          //!< DXR_LEIA_LIFT_CONV_GAIN / ConvGain (default 0.4): relative convergence -> NeurD units
	uint64_t interactive_min; //!< DXR_LEIA_LIFT_INTERACTIVE_MIN / InteractiveMin; 0 = unset (header's INTRODUCED_IN)
	uint64_t min_version;     //!< DXR_LEIA_LIFT_MIN_VERSION / MinVersion (default 0.4.6): older NeurD is refused
	int32_t video_model;      //!< DXR_LEIA_LIFT_VIDEO_MODEL / VideoModel: fast (default) | metric (NeurD >= 0.4.6)
	float depth_gain;         //!< DXR_LEIA_LIFT_DEPTH_GAIN / DepthGain (default 2.0): NeurD gain at strength 1
	int32_t dilate;           //!< DXR_LEIA_LIFT_DILATE / Dilate (default 2): NEURD_PROP_DILATE_RADIO, px
	uint32_t idle_unload_sec; //!< DXR_LEIA_LIFT_IDLE_UNLOAD_SEC / IdleUnloadSec (default 300; 0 = never unload)
	bool shadow_deps;         //!< DXR_LEIA_LIFT_SHADOW_DEPS / ShadowDeps (default 1): private copy of NeurD's OpenSSL
	knob_src src_backend, src_scale, src_view_gain, src_conv_gain, src_interactive_min, src_min_version,
	    src_video_model, src_depth_gain, src_dilate, src_idle_unload, src_shadow_deps;
};

bool
env_ieq(const char *a, const char *b)
{
	return _stricmp(a, b) == 0;
}

const wchar_t *const kLiftRegKey = L"SOFTWARE\\DisplayXR\\Leia\\Lift";

/*!
 * One knob's raw string: env @p env_name if set and non-empty, else REG_SZ
 * @p reg_name under @p reg (may be NULL), else absent. @p out is UTF-8.
 */
knob_src
knob_lookup(HKEY reg, const char *env_name, const wchar_t *reg_name, char *out, size_t cap)
{
	out[0] = '\0';
	const char *e = std::getenv(env_name);
	if (e != nullptr && e[0] != '\0') {
		snprintf(out, cap, "%s", e);
		return KSRC_ENV;
	}
	if (reg == nullptr) {
		return KSRC_DEFAULT;
	}
	wchar_t w[128] = {};
	DWORD cb = sizeof(w); // bytes, incl. the terminator RegGetValueW guarantees
	LSTATUS r = RegGetValueW(reg, nullptr, reg_name, RRF_RT_REG_SZ, nullptr, w, &cb);
	if (r == ERROR_FILE_NOT_FOUND) {
		return KSRC_DEFAULT;
	}
	if (r != ERROR_SUCCESS) {
		U_LOG_W("Leia lift: HKLM\\SOFTWARE\\DisplayXR\\Leia\\Lift\\%ls unreadable (error %ld, REG_SZ <= 127 chars "
		        "expected) — ignored",
		        reg_name, (long)r);
		return KSRC_DEFAULT;
	}
	if (w[0] == L'\0') {
		return KSRC_DEFAULT;
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, nullptr, nullptr);
	if (n <= 0) {
		out[0] = '\0';
		return KSRC_DEFAULT;
	}
	return KSRC_REG;
}

// ---- One grammar per knob, shared by both sources. Each returns false on a
// value it does not recognise (caller WARNs and keeps the default).

bool
parse_backend(const char *v, int *out)
{
	if (env_ieq(v, "auto")) {
		*out = BACKEND_AUTO;
	} else if (env_ieq(v, "cuda")) {
		*out = BACKEND_CUDA;
	} else if (env_ieq(v, "directml") || env_ieq(v, "dml")) {
		*out = BACKEND_DIRECTML;
	} else if (env_ieq(v, "openvino")) {
		*out = BACKEND_OPENVINO;
	} else {
		return false;
	}
	return true;
}

bool
parse_scale(const char *v, int32_t *out)
{
	if (env_ieq(v, "none") || env_ieq(v, "native") || env_ieq(v, "0")) {
		*out = NEURD_INPUT_AUTOSCALING_NONE;
	} else if (env_ieq(v, "720") || env_ieq(v, "720p")) {
		*out = NEURD_INPUT_AUTOSCALING_720P;
	} else if (env_ieq(v, "1080") || env_ieq(v, "1080p")) {
		*out = NEURD_INPUT_AUTOSCALING_1080P;
	} else if (env_ieq(v, "1440") || env_ieq(v, "1440p")) {
		*out = NEURD_INPUT_AUTOSCALING_1440P;
	} else {
		return false;
	}
	return true;
}

bool
parse_float_in(const char *v, float lo, float hi, float *out)
{
	char *end = nullptr;
	float f = std::strtof(v, &end);
	if (end == v || *end != '\0' || !std::isfinite(f) || f < lo || f > hi) {
		return false;
	}
	*out = f;
	return true;
}

//! Strict "MAJ.MIN.PAT" (no sscanf: MSVC C4996). Parts wider than
//! NEURD_MAKE_VERSION's fields are rejected rather than silently masked.
bool
parse_version(const char *v, uint64_t *out)
{
	unsigned long part[3] = {0, 0, 0};
	const char *c = v;
	bool ok = true;
	for (int i = 0; i < 3 && ok; i++) {
		char *end = nullptr;
		part[i] = std::strtoul(c, &end, 10);
		ok = end != c && *end == (i < 2 ? '.' : '\0');
		c = end + (i < 2 ? 1 : 0);
	}
	if (!ok || part[0] > 0xffffUL || part[1] > 0xffffUL) {
		return false;
	}
	*out = NEURD_MAKE_VERSION(part[0], part[1], part[2]);
	return true;
}

//! parse_version, clamped to >= 0.4.4.
bool
parse_interactive_min(const char *v, uint64_t *out)
{
	uint64_t ver = 0;
	if (!parse_version(v, &ver)) {
		return false;
	}
	const uint64_t floor_v = NEURD_MAKE_VERSION(0, 4, 4);
	*out = (ver < floor_v) ? floor_v : ver;
	return true;
}

//! IdleUnloadSec: whole seconds. 0 = never unload; anything else is clamped to
//! [kIdleUnloadMinSec, kIdleUnloadMaxSec] (*clamped says so). False on garbage.
bool
parse_idle_unload(const char *v, uint32_t *out, bool *clamped)
{
	if (v[0] < '0' || v[0] > '9') { // no sign, no leading blank (strtoull would wrap "-5")
		return false;
	}
	char *end = nullptr;
	unsigned long long n = std::strtoull(v, &end, 10); // saturates on overflow -> clamped below
	if (*end != '\0') {
		return false;
	}
	*clamped = false;
	if (n != 0 && n < kIdleUnloadMinSec) {
		n = kIdleUnloadMinSec;
		*clamped = true;
	} else if (n > kIdleUnloadMaxSec) {
		n = kIdleUnloadMaxSec;
		*clamped = true;
	}
	*out = (uint32_t)n;
	return true;
}

const char *
scale_str(int32_t a)
{
	switch (a) {
	case NEURD_INPUT_AUTOSCALING_NONE: return "none";
	case NEURD_INPUT_AUTOSCALING_720P: return "720";
	case NEURD_INPUT_AUTOSCALING_1080P: return "1080";
	case NEURD_INPUT_AUTOSCALING_1440P: return "1440";
	default: return "?";
	}
}

const char *
backend_choice_str(int b)
{
	return b == BACKEND_AUTO ? "auto" : backend_str((enum NeurD_backend)b);
}

struct knobs
read_knobs()
{
	struct knobs k = {};
	k.enabled = true;
	k.backend = BACKEND_DIRECTML;
	k.autoscaling = NEURD_INPUT_AUTOSCALING_720P;
	k.view_gain = 1.0f;
	k.conv_gain = 0.4f;
	k.interactive_min = 0;
	k.min_version = kDefaultMinNeurDVersion;
	k.video_model = NEURD_MODEL_VIDEO_RELATIVE_FAST;
	k.depth_gain = kDefaultDepthGain;
	k.dilate = kDefaultDilate;
	k.idle_unload_sec = kDefaultIdleUnloadSec;
	k.shadow_deps = true;

	const char *e = std::getenv("DXR_LEIA_LIFT");
	if (e != nullptr && (e[0] == '0' || env_ieq(e, "off") || env_ieq(e, "false"))) {
		k.enabled = false;
	}

	// Explicit 64-bit view: the service is 64-bit, but a WOW64 host must not
	// be redirected to Wow6432Node.
	HKEY reg = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kLiftRegKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &reg) != ERROR_SUCCESS) {
		reg = nullptr;
	}

	char v[256];
	knob_src src;

	src = knob_lookup(reg, "DXR_LEIA_LIFT_BACKEND", L"Backend", v, sizeof(v));
	if (src != KSRC_DEFAULT) {
		if (parse_backend(v, &k.backend)) {
			k.src_backend = src;
		} else {
			U_LOG_W("Leia lift: backend '%s' (%s) not recognised (auto|directml|cuda|openvino) — using directml", v,
			        knob_src_str(src));
		}
	}

	src = knob_lookup(reg, "DXR_LEIA_LIFT_SCALE", L"Scale", v, sizeof(v));
	if (src != KSRC_DEFAULT) {
		k.scale_forced = true; // as before: a set-but-unrecognised value still forces 720
		if (parse_scale(v, &k.autoscaling)) {
			k.src_scale = src;
		} else {
			U_LOG_W("Leia lift: scale '%s' (%s) not recognised (720|1080|1440|none) — using 720", v,
			        knob_src_str(src));
		}
	}

	src = knob_lookup(reg, "DXR_LEIA_LIFT_VIEW_GAIN", L"ViewGain", v, sizeof(v));
	if (src != KSRC_DEFAULT) {
		if (parse_float_in(v, 0.0f, 10.0f, &k.view_gain)) {
			k.src_view_gain = src;
		} else {
			U_LOG_W("Leia lift: view gain '%s' (%s) not a number in [0,10] — using 1.0", v, knob_src_str(src));
		}
	}

	src = knob_lookup(reg, "DXR_LEIA_LIFT_CONV_GAIN", L"ConvGain", v, sizeof(v));
	if (src != KSRC_DEFAULT) {
		if (parse_float_in(v, -2.0f, 2.0f, &k.conv_gain)) {
			k.src_conv_gain = src;
		} else {
			U_LOG_W("Leia lift: conv gain '%s' (%s) not a number in [-2,2] — using 0.4", v, knob_src_str(src));
		}
	}

	src = knob_lookup(reg, "DXR_LEIA_LIFT_INTERACTIVE_MIN", L"InteractiveMin", v, sizeof(v));
	if (src != KSRC_DEFAULT) {
		if (parse_interactive_min(v, &k.interactive_min)) {
			k.src_interactive_min = src;
		} else {
			U_LOG_W("Leia lift: interactive min '%s' (%s) not a version (e.g. 0.4.4) — ignored", v,
			        knob_src_str(src));
		}
	}

	// No clamp: a floor below 0.3.11 just defers to the stream-API check at activation.
	src = knob_lookup(reg, "DXR_LEIA_LIFT_MIN_VERSION", L"MinVersion", v, sizeof(v));
	if (src != KSRC_DEFAULT) {
		if (parse_version(v, &k.min_version)) {
			k.src_min_version = src;
		} else {
			U_LOG_W("Leia lift: min version '%s' (%s) not a version (e.g. 0.4.6) — using %u.%u.%u", v,
			        knob_src_str(src), (unsigned)NEURD_GET_VERSION_MAJOR(kDefaultMinNeurDVersion),
			        (unsigned)NEURD_GET_VERSION_MINOR(kDefaultMinNeurDVersion),
			        (unsigned)NEURD_GET_VERSION_PATCH(kDefaultMinNeurDVersion));
		}
	}

	// The video streams' depth model (NeurD_init_with_options, 0.4.6+): 'fast' = the relative
	// real-time model, 'metric' = NEURD_MODEL_VIDEO_METRIC_QUALITY (metric depth, closer to what the
	// web path's MoGe gives). Photo streams always use the relative photo model (the only one the
	// photo slot accepts).
	src = knob_lookup(reg, "DXR_LEIA_LIFT_DEPTH_GAIN", L"DepthGain", v, sizeof(v));
	if (src != KSRC_DEFAULT) {
		if (parse_float_in(v, 0.0f, 10.0f, &k.depth_gain)) {
			k.src_depth_gain = src;
		} else {
			U_LOG_W("Leia lift: depth gain '%s' (%s) not a number in [0,10] — using %.1f", v, knob_src_str(src),
			        (double)kDefaultDepthGain);
		}
	}

	src = knob_lookup(reg, "DXR_LEIA_LIFT_DILATE", L"Dilate", v, sizeof(v));
	if (src != KSRC_DEFAULT) {
		float d = 0.0f;
		if (parse_float_in(v, 0.0f, 16.0f, &d)) {
			k.dilate = (int32_t)(d + 0.5f);
			k.src_dilate = src;
		} else {
			U_LOG_W("Leia lift: dilate '%s' (%s) not a number in [0,16] — using %d", v, knob_src_str(src),
			        (int)kDefaultDilate);
		}
	}

	src = knob_lookup(reg, "DXR_LEIA_LIFT_VIDEO_MODEL", L"VideoModel", v, sizeof(v));
	if (src != KSRC_DEFAULT) {
		if (env_ieq(v, "metric")) {
			k.video_model = NEURD_MODEL_VIDEO_METRIC_QUALITY;
			k.src_video_model = src;
		} else if (env_ieq(v, "fast")) {
			k.video_model = NEURD_MODEL_VIDEO_RELATIVE_FAST;
			k.src_video_model = src;
		} else {
			U_LOG_W("Leia lift: video model '%s' (%s) not recognised (fast|metric) — using fast", v,
			        knob_src_str(src));
		}
	}

	// Seconds without any lift stream after which a READY NeurD is deinit'd and
	// FreeLibrary'd (its installer can then replace the files); 0 = never.
	src = knob_lookup(reg, "DXR_LEIA_LIFT_IDLE_UNLOAD_SEC", L"IdleUnloadSec", v, sizeof(v));
	if (src != KSRC_DEFAULT) {
		bool clamped = false;
		if (parse_idle_unload(v, &k.idle_unload_sec, &clamped)) {
			k.src_idle_unload = src;
			if (clamped) {
				U_LOG_W("Leia lift: idle unload '%s' (%s) out of range — clamped to %u s (0 = never, else %u..%u)",
				        v, knob_src_str(src), k.idle_unload_sec, kIdleUnloadMinSec, kIdleUnloadMaxSec);
			}
		} else {
			U_LOG_W("Leia lift: idle unload '%s' (%s) not a whole number of seconds (0 = never) — using %u", v,
			        knob_src_str(src), kDefaultIdleUnloadSec);
		}
	}

	// Load a private copy of NeurD's self-pinning OpenSSL before NeurD (see
	// shadow_self_pinning_deps); 0 = never, NeurD's own file then gets pinned.
	src = knob_lookup(reg, "DXR_LEIA_LIFT_SHADOW_DEPS", L"ShadowDeps", v, sizeof(v));
	if (src != KSRC_DEFAULT) {
		if (env_ieq(v, "1") || env_ieq(v, "on") || env_ieq(v, "true")) {
			k.shadow_deps = true;
			k.src_shadow_deps = src;
		} else if (env_ieq(v, "0") || env_ieq(v, "off") || env_ieq(v, "false")) {
			k.shadow_deps = false;
			k.src_shadow_deps = src;
		} else {
			U_LOG_W("Leia lift: shadow deps '%s' (%s) not recognised (1|0) — using 1", v, knob_src_str(src));
		}
	}

	if (reg != nullptr) {
		RegCloseKey(reg);
	}
	return k;
}

/*
 *
 * Compute shaders for the bridge (compiled at runtime on NeurD's device).
 *
 */

// Bridge texture (typed RGBA8/BGRA8 UNORM view — the view's format swizzles, so
// .rgba is always R,G,B,A) -> tightly packed RGBA8 raw buffer.
const char *kPackCs = R"(
cbuffer P : register(b0) { uint W; uint H; uint StrideBytes; uint Pad; };
Texture2D<float4> Src : register(t0);
RWByteAddressBuffer Dst : register(u0);
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= W || id.y >= H) return;
	uint4 u = (uint4)round(saturate(Src.Load(int3(id.xy, 0))) * 255.0);
	Dst.Store(id.y * StrideBytes + id.x * 4, u.r | (u.g << 8) | (u.b << 16) | (u.a << 24));
}
)";

// NeurD's RGBA8 raw output buffer -> bridge output texture.
const char *kUnpackCs = R"(
cbuffer P : register(b0) { uint W; uint H; uint StrideBytes; uint Pad; };
RWByteAddressBuffer Src : register(u0);
RWTexture2D<unorm float4> Dst : register(u1);
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= W || id.y >= H) return;
	uint v = Src.Load(id.y * StrideBytes + id.x * 4);
	Dst[id.xy] = float4(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF) / 255.0;
}
)";

// NeurD's RGBA8 depth buffer -> single-channel R8 bridge (DEPTH contract: one
// channel). NeurD writes the normalised disparity replicated into RGB; R is kept.
// Polarity: NeurD is disparity-like (near = HIGH); the lift spec's RELATIVE is
// larger = FARTHER, so the value is flipped here. Dst is an R8_UNORM UAV, so
// 1 - v stays in [0,1].
const char *kUnpackR8Cs = R"(
cbuffer P : register(b0) { uint W; uint H; uint StrideBytes; uint Pad; };
RWByteAddressBuffer Src : register(u0);
RWTexture2D<unorm float> Dst : register(u1);
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= W || id.y >= H) return;
	Dst[id.xy] = 1.0 - (float)(Src.Load(id.y * StrideBytes + id.x * 4) & 0xFF) / 255.0;
}
)";

/*
 *
 * Process-wide NeurD instance.
 *
 */

enum gstate : uint32_t
{
	G_UNPROBED = 0, //!< Nothing looked at yet.
	G_ABSENT,       //!< No NeurD.dll found (first probe, or a reload) — permanent for the process.
	G_LOADING,      //!< Background worker is loading / initialising (first activation or a reload).
	G_RETRY_WAIT,   //!< Licence activation hit the network; retry after next_retry_ms.
	G_READY,        //!< Up; converts allowed.
	G_FAILED,       //!< Permanent failure (bad licence, no DX device, ABI...).
	G_UNLOADING,    //!< Was READY; the idle watcher is tearing NeurD down (busy, transient).
	G_IDLE,         //!< Was READY; unloaded while idle. Caps still READY; a stream reloads it.
};

struct global
{
	std::mutex mtx; //!< Serialises every NeurD call + NeurD-context use + the fields below.

	std::atomic<uint32_t> state{G_UNPROBED};
	std::atomic<bool> worker_running{false};
	std::atomic<uint64_t> next_retry_ms{0};
	std::atomic<uint64_t> latency_ns{0}; //!< EMA of full convert wall time; 0 = none yet.
	std::atomic<uint32_t> streams_live{0};

	// Idle unload. streams_live and idle_since_ms change together under mtx:
	// idle_since_ms is 0 while a stream exists, else when the last one went (or
	// when READY was reached with none).
	std::atomic<uint64_t> idle_unload_ms{0}; //!< First acquirer's IdleUnloadSec; 0 = never unload.
	std::atomic<uint64_t> idle_since_ms{0};
	std::atomic<bool> watcher_running{false}; //!< At most one idle watcher thread.
	std::atomic<bool> reloading{false};       //!< The current activation follows an idle unload.

	//! What caps reports while READY / UNLOADING / IDLE: a copy taken when READY is
	//! published, so caps never reads fields a reload is rewriting (and never
	//! waits behind mtx). Leaf lock: nothing is called while it is held.
	std::mutex caps_mtx;
	char caps_backend_name[32] = {};
	enum NeurD_backend caps_backend = NEURD_BACKEND_DIRECTML;

	int requested_backend = BACKEND_DIRECTML; //!< First acquirer's choice (NeurD's forced backend is sticky).
	int32_t default_autoscaling = NEURD_INPUT_AUTOSCALING_720P;
	struct knobs k0 = {}; //!< First acquirer's knobs (logged + interactive_min at activation; reused by reloads).

	//! Both forgotten by the idle unload: after FreeLibrary everything that pointed
	//! into NeurD (this table, its device, its context) is dangling.
	HMODULE lib = nullptr;
	struct NeurD const *nd = nullptr;
	enum NeurD_backend backend = NEURD_BACKEND_DIRECTML;
	char backend_name[32] = {};

	ID3D11Device *nd_dev = nullptr; //!< NeurD-owned (never Released) unless nd_dev_owned.
	bool nd_dev_owned = false;      //!< nd_dev is the plug-in's own device (NeurD < 0.4.3): Release it.
	ID3D11DeviceContext *nd_ctx = nullptr;
	ID3D11Query *nd_done = nullptr;
	ID3D11ComputeShader *cs_pack = nullptr;
	ID3D11ComputeShader *cs_unpack = nullptr;
	ID3D11ComputeShader *cs_unpack_r8 = nullptr;
	ID3D11Buffer *cb = nullptr;
	LUID nd_luid = {};

	int refcount = 0;
	uint64_t next_stream_id = 1;
	//! Written under g.mtx; atomic because caps reads it lock-free.
	std::atomic<bool> interactive_unavailable{false};
	//! DXR_LEIA_LIFT_INTERACTIVE_MIN admitted a NeurD older than the header's
	//! INTRODUCED_IN: call the table slot directly (the inline wrapper would
	//! re-check 0.4.5 and return OUTDATED_RUNTIME). Set at activation.
	bool interactive_direct_slot = false;

	//! Private copies loaded by the shadow pre-load (ShadowDeps). Never freed: OpenSSL
	//! pins itself anyway. Touched only by the activation worker (one at a time).
	HMODULE shadow_mods[4] = {};
	uint32_t shadow_count = 0;

	//! Last-applied global NeurD properties (avoid a worker round-trip per prop per frame).
	bool props_valid = false;
	int32_t p_out_type = -1, p_tiles_w = -1, p_tiles_h = -1, p_inpaint = -1, p_autoscale = -1, p_autoconv = -1;
	float p_conv = -1.0f, p_gain = -1.0f;
	int32_t p_dilate = -1;
};

global g;

uint64_t
now_ms()
{
	return GetTickCount64();
}

/*
 * DLL location — mirrors NeurD's own loader order. Presence probe is separate
 * from the load so caps can say "activating" without pulling NeurD (and its
 * CUDA/DirectML/OpenVINO dependency tree) into the process on the caller's
 * thread.
 */
bool
registry_neurd_path(char *out, size_t cap)
{
	HKEY key = nullptr;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\LeiaInc\\NeurD", 0, KEY_READ, &key) != ERROR_SUCCESS) {
		return false;
	}
	char dir[MAX_PATH] = {};
	DWORD sz = sizeof(dir);
	LSTATUS r = RegGetValueA(key, nullptr, nullptr, RRF_RT_REG_SZ, nullptr, dir, &sz);
	RegCloseKey(key);
	if (r != ERROR_SUCCESS || dir[0] == '\0') {
		return false;
	}
	size_t n = strlen(dir);
	const char *sep = (dir[n - 1] == '\\' || dir[n - 1] == '/') ? "" : "\\";
	int w = snprintf(out, cap, "%s%sNeurD.dll", dir, sep);
	return w > 0 && (size_t)w < cap;
}

bool
file_exists(const char *path)
{
	DWORD a = GetFileAttributesA(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool
neurd_present()
{
	char buf[MAX_PATH];
	if (SearchPathA(nullptr, "NeurD.dll", nullptr, MAX_PATH, buf, nullptr) != 0) {
		return true;
	}
	const char *np = std::getenv("NEURD_PATH");
	if (np != nullptr && np[0] != '\0' && file_exists(np)) {
		return true;
	}
	return registry_neurd_path(buf, sizeof(buf)) && file_exists(buf);
}

HMODULE
neurd_load_library(const char **out_where)
{
	HMODULE h = LoadLibraryA("NeurD.dll");
	if (h != nullptr) {
		*out_where = "PATH";
		return h;
	}
	const char *np = std::getenv("NEURD_PATH");
	if (np != nullptr && np[0] != '\0') {
		h = LoadLibraryExA(np, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (h != nullptr) {
			*out_where = "NEURD_PATH";
			return h;
		}
	}
	char path[MAX_PATH];
	if (registry_neurd_path(path, sizeof(path))) {
		h = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (h != nullptr) {
			*out_where = "HKLM\\SOFTWARE\\LeiaInc\\NeurD";
			return h;
		}
	}
	return nullptr;
}

/*
 *
 * Shadow pre-load of NeurD's self-pinning dependencies (ShadowDeps).
 *
 * OpenSSL 3 pins libcrypto in the process at init (GET_MODULE_HANDLE_EX_FLAG_PIN),
 * so the idle unload frees every NeurD module but that one, and its file in
 * NeurD's directory stays locked. The loader satisfies a static import by BASE
 * NAME with an already-loaded module of that name, wherever it came from — so
 * before NeurD.dll is (re)loaded, a byte-identical private copy is loaded from a
 * DisplayXR-owned per-user directory, and that copy is what ends up pinned.
 * Never in an elevated process: the shadow directory is user-writable, and
 * loading code from it there would be a DLL-planting path.
 *
 */

const wchar_t *const kShadowPatterns[] = {L"libcrypto-*.dll", L"libssl-*.dll"}; // libcrypto first: libssl imports it
constexpr uint32_t kShadowMaxPerPattern = 4;
constexpr uint32_t kShadowMaxStaleDirs = 32;
constexpr uint32_t kShadowMaxFilesPerDir = 16;
constexpr DWORD kShadowCompareChunk = 64 * 1024;

bool
path_is_under(const wchar_t *path, const wchar_t *dir)
{
	const size_t n = wcslen(dir);
	return n != 0 && _wcsnicmp(path, dir, n) == 0;
}

//! Directory of @p file_path, absolute, long-name, with a trailing backslash.
bool
dir_of(const wchar_t *file_path, wchar_t *dir, DWORD cap)
{
	wchar_t *file_part = nullptr;
	const DWORD n = GetFullPathNameW(file_path, cap, dir, &file_part);
	if (n == 0 || n >= cap || file_part == nullptr) {
		return false;
	}
	*file_part = L'\0';
	const DWORD l = GetLongPathNameW(dir, dir, cap); // in place; a short PATH entry must still compare equal
	return l != 0 && l < cap;
}

/*!
 * Directory of the NeurD.dll neurd_load_library() is about to load, resolved in
 * the same order: the DLL search path, NEURD_PATH, HKLM\SOFTWARE\LeiaInc\NeurD.
 * (The worker checks the loaded module's directory against it afterwards.)
 */
bool
neurd_dll_dir(wchar_t *dir, DWORD cap)
{
	wchar_t path[MAX_PATH];
	DWORD n = SearchPathW(nullptr, L"NeurD.dll", nullptr, MAX_PATH, path, nullptr);
	if (n > 0 && n < MAX_PATH) {
		return dir_of(path, dir, cap);
	}
	n = GetEnvironmentVariableW(L"NEURD_PATH", path, MAX_PATH);
	if (n > 0 && n < MAX_PATH) {
		const DWORD a = GetFileAttributesW(path);
		if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			return dir_of(path, dir, cap);
		}
	}
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LeiaInc\\NeurD", 0, KEY_READ, &key) != ERROR_SUCCESS) {
		return false;
	}
	wchar_t inst[MAX_PATH] = {};
	DWORD cb = sizeof(inst);
	const LSTATUS r = RegGetValueW(key, nullptr, nullptr, RRF_RT_REG_SZ, nullptr, inst, &cb);
	RegCloseKey(key);
	if (r != ERROR_SUCCESS || inst[0] == L'\0') {
		return false;
	}
	const size_t len = wcslen(inst);
	const wchar_t *sep = (inst[len - 1] == L'\\' || inst[len - 1] == L'/') ? L"" : L"\\";
	const int w = std::swprintf(path, MAX_PATH, L"%ls%lsNeurD.dll", inst, sep);
	return w > 0 && w < MAX_PATH && dir_of(path, dir, cap);
}

//! "%LOCALAPPDATA%\DisplayXR\NeurDShadow\" (not created here).
bool
shadow_root(wchar_t *out, DWORD cap)
{
	wchar_t lad[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return false;
	}
	while (n > 0 && (lad[n - 1] == L'\\' || lad[n - 1] == L'/')) {
		lad[--n] = L'\0';
	}
	const int w = std::swprintf(out, cap, L"%ls\\DisplayXR\\NeurDShadow\\", lad);
	return n > 0 && w > 0 && (DWORD)w < cap;
}

//! Create @p dir if missing; true only if it is then a real directory (not a
//! junction / symlink — housekeeping deletes inside it).
bool
ensure_plain_dir(const wchar_t *dir)
{
	if (!CreateDirectoryW(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
		return false;
	}
	const DWORD a = GetFileAttributesW(dir);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
	       (a & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

/*!
 * True if this process runs elevated or above medium integrity (SYSTEM, a
 * service account) — or if that cannot be determined: the shadow is skipped then.
 */
bool
process_is_elevated()
{
	HANDLE tok = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
		return true;
	}
	bool elevated = true;
	TOKEN_ELEVATION e = {};
	DWORD cb = 0;
	if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &cb) && e.TokenIsElevated == 0) {
		alignas(8) unsigned char buf[128];
		if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &cb)) {
			PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buf)->Label.Sid;
			const UCHAR subs = *GetSidSubAuthorityCount(sid);
			elevated = subs == 0 || *GetSidSubAuthority(sid, subs - 1u) >= SECURITY_MANDATORY_HIGH_RID;
		}
	}
	CloseHandle(tok);
	return elevated;
}

HANDLE
open_for_read(const wchar_t *path, DWORD share)
{
	return CreateFileW(path, GENERIC_READ, share, nullptr, OPEN_EXISTING,
	                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

//! Full byte compare of two open files (from offset 0). False on any read error.
bool
files_identical(HANDLE a, HANDLE b)
{
	LARGE_INTEGER sa = {}, sb = {}, zero = {};
	if (!GetFileSizeEx(a, &sa) || !GetFileSizeEx(b, &sb) || sa.QuadPart != sb.QuadPart ||
	    !SetFilePointerEx(a, zero, nullptr, FILE_BEGIN) || !SetFilePointerEx(b, zero, nullptr, FILE_BEGIN)) {
		return false;
	}
	unsigned char *buf = new (std::nothrow) unsigned char[2 * (size_t)kShadowCompareChunk];
	if (buf == nullptr) {
		return false;
	}
	bool same = true;
	for (LONGLONG left = sa.QuadPart; same && left > 0;) {
		const DWORD want = (DWORD)std::min<LONGLONG>(left, kShadowCompareChunk);
		DWORD ga = 0, gb = 0;
		same = ReadFile(a, buf, want, &ga, nullptr) && ReadFile(b, buf + kShadowCompareChunk, want, &gb, nullptr) &&
		       ga == want && gb == want && memcmp(buf, buf + kShadowCompareChunk, want) == 0;
		left -= want;
	}
	delete[] buf;
	return same;
}

//! Original vs the file at @p other: same bytes? (Both opened read-only, briefly.)
bool
paths_identical(const wchar_t *orig, const wchar_t *other)
{
	HANDLE a = open_for_read(orig, FILE_SHARE_READ | FILE_SHARE_DELETE);
	HANDLE b = open_for_read(other, FILE_SHARE_READ | FILE_SHARE_DELETE);
	const bool same = a != INVALID_HANDLE_VALUE && b != INVALID_HANDLE_VALUE && files_identical(a, b);
	if (a != INVALID_HANDLE_VALUE) {
		CloseHandle(a);
	}
	if (b != INVALID_HANDLE_VALUE) {
		CloseHandle(b);
	}
	return same;
}

//! One dependency @p base found in @p nd_dir: load its private copy (or note why not).
void
shadow_one(const wchar_t *nd_dir, const wchar_t *root, const WIN32_FIND_DATAW &fd)
{
	const wchar_t *base = fd.cFileName;
	wchar_t orig[MAX_PATH];
	int w = std::swprintf(orig, MAX_PATH, L"%ls%ls", nd_dir, base);
	if (w <= 0 || w >= MAX_PATH) {
		return;
	}

	// Already loaded under that base name: it satisfies NeurD's import whatever we do.
	HMODULE have = GetModuleHandleW(base);
	if (have != nullptr) {
		wchar_t lp[MAX_PATH];
		const DWORD n = GetModuleFileNameW(have, lp, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) {
			return;
		}
		if (path_is_under(lp, root)) {
			// Our copy from an earlier cycle. After a vendor upgrade NeurD's file may
			// differ from it; the reloaded NeurD then runs against the copy loaded
			// first (same ABI-major name) — fine until restart, but say so.
			if (!paths_identical(orig, lp)) {
				LIFT_WARN_ONCE("Leia lift: NeurD's %ls changed on disk since this process loaded its private copy "
				               "(%ls). The reloaded NeurD keeps running against the copy loaded first — it works "
				               "until the service restarts; restart the service to pick up the new library",
				               base, lp);
			}
		} else if (path_is_under(lp, nd_dir)) {
			LIFT_WARN_ONCE("Leia lift: %ls is already loaded from NeurD's directory (%ls) — that file stays locked "
			               "until the service restarts",
			               base, lp);
		}
		return;
	}
	if (g.shadow_count >= sizeof(g.shadow_mods) / sizeof(g.shadow_mods[0])) {
		LIFT_WARN_ONCE("Leia lift: too many OpenSSL modules to shadow — %ls not shadowed", base);
		return;
	}

	// <root>\<size>-<mtime>\<base>: an upgraded OpenSSL gets its own directory, a
	// matching copy is reused across service runs. The name is only a cache key;
	// what is loaded is decided by the byte compare below.
	wchar_t dir[MAX_PATH], shadow[MAX_PATH];
	w = std::swprintf(dir, MAX_PATH, L"%ls%08lx%08lx-%08lx%08lx\\", root, (unsigned long)fd.nFileSizeHigh,
	                  (unsigned long)fd.nFileSizeLow, (unsigned long)fd.ftLastWriteTime.dwHighDateTime,
	                  (unsigned long)fd.ftLastWriteTime.dwLowDateTime);
	if (w <= 0 || w >= MAX_PATH || !ensure_plain_dir(dir)) {
		U_LOG_W("Leia lift: cannot create the shadow directory for %ls under %ls — not shadowed", base, root);
		return;
	}
	w = std::swprintf(shadow, MAX_PATH, L"%ls%ls", dir, base);
	if (w <= 0 || w >= MAX_PATH) {
		U_LOG_W("Leia lift: shadow path for %ls too long — not shadowed", base);
		return;
	}

	HANDLE src = open_for_read(orig, FILE_SHARE_READ | FILE_SHARE_DELETE);
	if (src == INVALID_HANDLE_VALUE) {
		const unsigned long err = (unsigned long)GetLastError();
		U_LOG_W("Leia lift: cannot read %ls (err %lu) — not shadowed", orig, err);
		return;
	}
	// The copy is opened WITHOUT write/delete sharing and stays open across the
	// LoadLibrary: the bytes compared are the bytes mapped.
	HANDLE dst = open_for_read(shadow, FILE_SHARE_READ);
	if (dst == INVALID_HANDLE_VALUE || !files_identical(src, dst)) {
		if (dst != INVALID_HANDLE_VALUE) {
			CloseHandle(dst);
		}
		if (!CopyFileW(orig, shadow, FALSE)) {
			const unsigned long err = (unsigned long)GetLastError();
			U_LOG_W("Leia lift: copying %ls to %ls failed (err %lu) — not shadowed", orig, shadow, err);
			CloseHandle(src);
			return;
		}
		dst = open_for_read(shadow, FILE_SHARE_READ);
		if (dst == INVALID_HANDLE_VALUE || !files_identical(src, dst)) {
			U_LOG_W("Leia lift: the shadow copy %ls does not match %ls — not loaded", shadow, orig);
			if (dst != INVALID_HANDLE_VALUE) {
				CloseHandle(dst);
			}
			CloseHandle(src);
			return;
		}
	}
	CloseHandle(src);
	HMODULE h = LoadLibraryExW(shadow, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	const DWORD err = GetLastError();
	CloseHandle(dst);
	if (h == nullptr) {
		U_LOG_W("Leia lift: loading the shadow copy %ls failed (err %lu) — not shadowed; NeurD's own %ls will be "
		        "pinned",
		        shadow, (unsigned long)err, base);
		return;
	}
	g.shadow_mods[g.shadow_count++] = h; // never freed
	U_LOG_W("Leia lift: loaded a private copy of NeurD's %ls from %ls — it is the one the process pins; NeurD's own "
	        "file stays replaceable",
	        base, dir);
}

/*!
 * Best-effort removal of stale <root>\<key>\ directories (not the ones this
 * process holds). Bounded; never recurses below <root>\<key>\, skips junctions /
 * symlinks, ignores every failure (a copy pinned by a running process cannot be
 * deleted, and that is fine).
 */
void
shadow_housekeeping(const wchar_t *root)
{
	wchar_t keep[4][MAX_PATH] = {};
	uint32_t n_keep = 0;
	for (uint32_t i = 0; i < g.shadow_count && n_keep < 4; i++) {
		wchar_t p[MAX_PATH];
		const DWORD n = GetModuleFileNameW(g.shadow_mods[i], p, MAX_PATH);
		if (n > 0 && n < MAX_PATH && dir_of(p, keep[n_keep], MAX_PATH)) {
			n_keep++;
		}
	}
	wchar_t spec[MAX_PATH];
	int w = std::swprintf(spec, MAX_PATH, L"%ls*", root);
	if (w <= 0 || w >= MAX_PATH) {
		return;
	}
	WIN32_FIND_DATAW fd;
	HANDLE f = FindFirstFileW(spec, &fd);
	if (f == INVALID_HANDLE_VALUE) {
		return;
	}
	uint32_t dirs = 0;
	do {
		const DWORD a = fd.dwFileAttributes;
		if ((a & FILE_ATTRIBUTE_DIRECTORY) == 0 || (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
		    wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
			continue;
		}
		if (++dirs > kShadowMaxStaleDirs) {
			break;
		}
		wchar_t sub[MAX_PATH];
		w = std::swprintf(sub, MAX_PATH, L"%ls%ls\\", root, fd.cFileName);
		if (w <= 0 || w >= MAX_PATH) {
			continue;
		}
		bool in_use = false;
		for (uint32_t i = 0; i < n_keep; i++) {
			in_use = in_use || _wcsicmp(sub, keep[i]) == 0;
		}
		if (in_use) {
			continue;
		}
		wchar_t sub_spec[MAX_PATH];
		w = std::swprintf(sub_spec, MAX_PATH, L"%ls*", sub);
		WIN32_FIND_DATAW ff;
		HANDLE inner = (w > 0 && w < MAX_PATH) ? FindFirstFileW(sub_spec, &ff) : INVALID_HANDLE_VALUE;
		if (inner != INVALID_HANDLE_VALUE) {
			uint32_t files = 0;
			do {
				if ((ff.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
					continue; // no recursion; a link is never followed
				}
				if (++files > kShadowMaxFilesPerDir) {
					break;
				}
				wchar_t victim[MAX_PATH];
				w = std::swprintf(victim, MAX_PATH, L"%ls%ls", sub, ff.cFileName);
				if (w > 0 && w < MAX_PATH) {
					DeleteFileW(victim);
				}
			} while (FindNextFileW(inner, &ff));
			FindClose(inner);
		}
		RemoveDirectoryW(sub); // fails while anything is left in it: ignored
	} while (FindNextFileW(f, &fd));
	FindClose(f);
}

/*!
 * Before NeurD.dll is (re)loaded from @p nd_dir: load a private copy of each
 * self-pinning dependency found there (kShadowPatterns) that is not loaded yet,
 * then tidy stale copies. Every failure is a WARN and the load goes on without
 * the shadow — this never stands in lift's way. Activation worker only.
 */
void
shadow_self_pinning_deps(const wchar_t *nd_dir)
{
	if (process_is_elevated()) {
		LIFT_WARN_ONCE("Leia lift: process is elevated — NOT loading NeurD's OpenSSL from the user-writable shadow "
		               "directory (ShadowDeps); NeurD's own copy will be pinned and stay locked until the process "
		               "exits");
		return;
	}
	wchar_t root[MAX_PATH], parent[MAX_PATH];
	if (!shadow_root(root, MAX_PATH)) {
		LIFT_WARN_ONCE("Leia lift: LOCALAPPDATA unset or too long — NeurD's OpenSSL not shadowed");
		return;
	}
	// <LOCALAPPDATA>\DisplayXR\ then <...>\NeurDShadow\ — both must be plain directories.
	const size_t root_len = wcslen(root);
	wcsncpy_s(parent, MAX_PATH, root, root_len - wcslen(L"NeurDShadow\\"));
	if (!ensure_plain_dir(parent) || !ensure_plain_dir(root)) {
		LIFT_WARN_ONCE("Leia lift: cannot create %ls (or it is a link) — NeurD's OpenSSL not shadowed", root);
		return;
	}
	for (const wchar_t *pattern : kShadowPatterns) {
		wchar_t spec[MAX_PATH];
		const int w = std::swprintf(spec, MAX_PATH, L"%ls%ls", nd_dir, pattern);
		if (w <= 0 || w >= MAX_PATH) {
			continue;
		}
		WIN32_FIND_DATAW fd;
		HANDLE f = FindFirstFileW(spec, &fd);
		if (f == INVALID_HANDLE_VALUE) {
			continue;
		}
		uint32_t found = 0;
		do {
			// FindFirstFile also matches 8.3 short names: insist on a real *.dll file.
			const size_t len = wcslen(fd.cFileName);
			if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 || len < 4 ||
			    _wcsicmp(fd.cFileName + len - 4, L".dll") != 0) {
				continue;
			}
			if (++found > kShadowMaxPerPattern) {
				break;
			}
			shadow_one(nd_dir, root, fd);
		} while (FindNextFileW(f, &fd));
		FindClose(f);
	}
	shadow_housekeeping(root);
}

void
neurd_log_cb(const char *msg, int size)
{
	if (msg == nullptr || size <= 0) {
		return;
	}
	// Not NUL-terminated (NeurD contract). Trim a trailing newline.
	while (size > 0 && (msg[size - 1] == '\n' || msg[size - 1] == '\r')) {
		size--;
	}
	// Until the module is READY these are one-off activation/init lines (licence, model paths,
	// adapter) and the only place NeurD says WHY an init failed — surface them at the default
	// log level. Once READY they can be per-stream chatter, so drop back to INFO.
	if (g.state.load() != G_READY) {
		U_LOG_W("NeurD: %.*s", size, msg);
	} else {
		U_LOG_I("NeurD: %.*s", size, msg);
	}
}

//! Installed instead of neurd_log_cb before the idle unload. A no-op rather than
//! NULL: NeurD does not document whether it NULL-checks its logger, and this
//! function lives in the plug-in, which outlives the NeurD mapping.
void
neurd_log_discard(const char *msg, int size)
{
	(void)msg;
	(void)size;
}

bool
enable_mt_protection(ID3D11DeviceContext *ctx, const char *who)
{
	ID3D11Multithread *mt = nullptr;
	if (FAILED(ctx->QueryInterface(__uuidof(ID3D11Multithread), (void **)&mt)) || mt == nullptr) {
		U_LOG_W("Leia lift: %s context has no ID3D11Multithread — concurrent use is unsafe", who);
		return false;
	}
	if (!mt->GetMultithreadProtected()) {
		mt->SetMultithreadProtected(TRUE);
		U_LOG_W("Leia lift: enabled ID3D11Multithread protection on the %s immediate context", who);
	}
	mt->Release();
	return true;
}

bool
device_luid(ID3D11Device *dev, LUID *out)
{
	IDXGIDevice *dxgi = nullptr;
	if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)) || dxgi == nullptr) {
		return false;
	}
	IDXGIAdapter *ad = nullptr;
	bool ok = false;
	if (SUCCEEDED(dxgi->GetAdapter(&ad)) && ad != nullptr) {
		DXGI_ADAPTER_DESC d = {};
		if (SUCCEEDED(ad->GetDesc(&d))) {
			*out = d.AdapterLuid;
			ok = true;
		}
		ad->Release();
	}
	dxgi->Release();
	return ok;
}

ID3D11ComputeShader *
compile_cs(ID3D11Device *dev, const char *src, const char *name)
{
	ID3DBlob *blob = nullptr;
	ID3DBlob *err = nullptr;
	HRESULT hr = D3DCompile(src, strlen(src), name, nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &err);
	if (FAILED(hr)) {
		U_LOG_E("Leia lift: %s compile failed: %s", name,
		        err != nullptr ? (const char *)err->GetBufferPointer() : "(no log)");
		safe_release(err);
		return nullptr;
	}
	safe_release(err);
	ID3D11ComputeShader *cs = nullptr;
	hr = dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Leia lift: CreateComputeShader(%s) failed: 0x%08x", name, (unsigned)hr);
		return nullptr;
	}
	return cs;
}

/*!
 * NeurD_convert_stream_dx_interactive, honouring DXR_LEIA_LIFT_INTERACTIVE_MIN:
 * the header's inline wrapper re-checks version >= INTRODUCED_IN (0.4.5), so
 * when the override admitted an older-reporting runtime the table slot is
 * called directly (same arguments minus @p nd). Called with g.mtx held.
 */
enum NeurD_status
nd_convert_stream_dx_interactive(struct NeurD const *nd, struct NeurD_stream *stream, struct NeurD_image const *input,
                                 const float *viewpoints_xyz, int size, enum NeurD_pixel_format output_pix_fmt,
                                 struct NeurD_image *output)
{
	if (g.interactive_direct_slot && nd->convert_stream_dx_interactive != nullptr) {
		return nd->convert_stream_dx_interactive(stream, input, viewpoints_xyz, size, output_pix_fmt, output);
	}
	return NeurD_convert_stream_dx_interactive(nd, stream, input, viewpoints_xyz, size, output_pix_fmt, output);
}

//! Drop g.nd_dev: Released only when the plug-in created it. Called with g.mtx held.
void
release_nd_dev_locked()
{
	if (g.nd_dev_owned) {
		safe_release(g.nd_dev);
	}
	g.nd_dev = nullptr;
	g.nd_dev_owned = false;
}

//! Build the NeurD-device side of the bridge. Called with g.mtx held.
bool
setup_nd_device_locked()
{
	if (LEIA_NEURD_HAS(g.nd, get_dx_device)) {
		g.nd_dev = static_cast<ID3D11Device *>(NeurD_get_dx_device(g.nd));
		g.nd_dev_owned = false;
		if (g.nd_dev == nullptr) {
			U_LOG_W("Leia lift: NeurD backend '%s' exposes no D3D11 device — the D3D11 lift path needs the "
			        "DirectML backend (DXR_LEIA_LIFT_BACKEND=directml). Lift unavailable.",
			        g.backend_name);
			return false;
		}
	} else {
		// NeurD < 0.4.3: no get_dx_device. NeurD takes the device from the
		// input buffer (GetDevice) and returns its output on that device, so
		// the plug-in supplies the device — default adapter, as NeurD's own
		// example does.
		ID3D11Device *dev = nullptr;
		ID3D11DeviceContext *ctx = nullptr;
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		                               nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
		if (FAILED(hr) || dev == nullptr || ctx == nullptr) {
			U_LOG_W("Leia lift: NeurD < 0.4.3 and D3D11CreateDevice for the plug-in's bridge device failed "
			        "(0x%08x) — lift unavailable",
			        (unsigned)hr);
			safe_release(ctx);
			safe_release(dev);
			return false;
		}
		// GetImmediateContext below takes its own reference.
		ctx->Release();
		g.nd_dev = dev;
		g.nd_dev_owned = true;
		LUID l = {};
		const bool have_luid = device_luid(dev, &l);
		U_LOG_W("Leia lift: NeurD < 0.4.3: no get_dx_device — using the plug-in's own D3D11 device on the "
		        "default adapter (LUID %08lx:%08lx)%s",
		        have_luid ? (unsigned long)l.HighPart : 0UL, have_luid ? (unsigned long)l.LowPart : 0UL,
		        g.backend == NEURD_BACKEND_CUDA ? ""
		                                        : " — NeurD 0.3.x's DX path needs the CUDA backend "
		                                          "(DXR_LEIA_LIFT_BACKEND=cuda); converts may fail");
	}
	g.nd_dev->GetImmediateContext(&g.nd_ctx);
	enable_mt_protection(g.nd_ctx, "NeurD");

	D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
	g.cs_pack = compile_cs(g.nd_dev, kPackCs, "leia_lift_pack");
	g.cs_unpack = compile_cs(g.nd_dev, kUnpackCs, "leia_lift_unpack");
	g.cs_unpack_r8 = compile_cs(g.nd_dev, kUnpackR8Cs, "leia_lift_unpack_r8");
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = 16;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (g.cs_pack == nullptr || g.cs_unpack == nullptr || g.cs_unpack_r8 == nullptr || FAILED(g.nd_dev->CreateQuery(&qd, &g.nd_done)) ||
	    FAILED(g.nd_dev->CreateBuffer(&bd, nullptr, &g.cb))) {
		U_LOG_W("Leia lift: bridge setup on NeurD's device failed — lift unavailable");
		safe_release(g.cs_pack);
		safe_release(g.cs_unpack);
		safe_release(g.cs_unpack_r8);
		safe_release(g.nd_done);
		safe_release(g.cb);
		safe_release(g.nd_ctx);
		release_nd_dev_locked();
		return false;
	}
	if (device_luid(g.nd_dev, &g.nd_luid)) {
		U_LOG_W("Leia lift: NeurD D3D11 device on adapter LUID %08lx:%08lx (FL 0x%x)",
		        (unsigned long)g.nd_luid.HighPart, (unsigned long)g.nd_luid.LowPart,
		        (unsigned)g.nd_dev->GetFeatureLevel());
	}
	return true;
}

void
arm_idle_watcher();

/*!
 * Background worker: load (once per activation cycle) + init (retryable). Runs
 * detached; publishes the outcome through g.state.
 */
void
activation_worker()
{
	uint32_t next = G_FAILED;
	const bool reload = g.reloading.load();
	{
		// Load happens once per activation cycle: on first use, and again after
		// an idle unload (which leaves g.nd / g.lib NULL) — the same path both
		// times, so an upgraded NeurD is picked up and an older one refused.
		// NOT under g.mtx for the slow part — but nothing else touches g.lib /
		// g.nd until state says READY (the idle unload starts only FROM READY,
		// and kick() starts this worker only from UNPROBED / RETRY_WAIT / IDLE).
		if (g.nd == nullptr) {
			// First activation AND every reload: put private copies of NeurD's
			// self-pinning OpenSSL in place before NeurD's own imports resolve.
			wchar_t nd_dir[MAX_PATH] = {};
			bool shadowed = false;
			if (g.k0.shadow_deps) {
				shadowed = neurd_dll_dir(nd_dir, MAX_PATH);
				if (shadowed) {
					shadow_self_pinning_deps(nd_dir);
				} else {
					LIFT_WARN_ONCE("Leia lift: cannot resolve NeurD.dll's directory — its OpenSSL is not shadowed");
				}
			}
			const char *where = "?";
			HMODULE lib = neurd_load_library(&where);
			if (lib == nullptr) {
				U_LOG_W("Leia lift: NeurD.dll present but LoadLibrary failed (err %lu) — lift unavailable",
				        (unsigned long)GetLastError());
				g.state.store(G_FAILED);
				g.worker_running.store(false);
				return;
			}
			if (shadowed) {
				// The loader's search may have picked another NeurD.dll than our
				// resolution did; then the shadowing looked in the wrong place.
				wchar_t got[MAX_PATH], got_dir[MAX_PATH];
				const DWORD n = GetModuleFileNameW(lib, got, MAX_PATH);
				if (n > 0 && n < MAX_PATH && dir_of(got, got_dir, MAX_PATH) && _wcsicmp(got_dir, nd_dir) != 0) {
					LIFT_WARN_ONCE("Leia lift: NeurD.dll loaded from %ls but its OpenSSL was looked up in %ls — "
					               "its own OpenSSL may end up pinned (file locked until restart)",
					               got_dir, nd_dir);
				}
			}
			auto load = reinterpret_cast<PFN_NeurD_load>(reinterpret_cast<void *>(GetProcAddress(lib, "NeurD_load")));
			struct NeurD_load_request req = {NEURD_VERSION};
			struct NeurD const *nd = load != nullptr ? load(&req) : nullptr;
			if (nd == nullptr) {
				U_LOG_W("Leia lift: NeurD.dll (%s) has no usable NeurD_load — lift unavailable%s", where,
				        reload ? " (reload after the idle unload: NeurD may refuse a second load in one "
				                 "process — set IdleUnloadSec=0 if this repeats)"
				               : "");
				// Deliberately no FreeLibrary: NeurD may have spun threads.
				g.state.store(G_FAILED);
				g.worker_running.store(false);
				return;
			}
			U_LOG_W("Leia lift: %s NeurD %u.%u.%u from %s (plug-in built against %u.%u.%u)",
			        reload ? "reloading (after the idle unload)" : "loaded",
			        NEURD_GET_VERSION_MAJOR(nd->version), NEURD_GET_VERSION_MINOR(nd->version),
			        NEURD_GET_VERSION_PATCH(nd->version), where,
			        NEURD_GET_VERSION_MAJOR(NEURD_VERSION),
			        NEURD_GET_VERSION_MINOR(NEURD_VERSION),
			        NEURD_GET_VERSION_PATCH(NEURD_VERSION));
			// Before backend / init / licensing: an unsupported NeurD never initialises.
			// Self-sufficient WARN — the knobs line below prints only after a good init.
			if (nd->version < g.k0.min_version) {
				U_LOG_W("Leia lift: NeurD %u.%u.%u is older than the minimum %u.%u.%u (%s) — lift unavailable; "
				        "callers fall back to their own conversion path. Install NeurD >= %u.%u.%u, or set "
				        "MinVersion (HKLM\\SOFTWARE\\DisplayXR\\Leia\\Lift) / DXR_LEIA_LIFT_MIN_VERSION to override",
				        NEURD_GET_VERSION_MAJOR(nd->version), NEURD_GET_VERSION_MINOR(nd->version),
				        NEURD_GET_VERSION_PATCH(nd->version), NEURD_GET_VERSION_MAJOR(g.k0.min_version),
				        NEURD_GET_VERSION_MINOR(g.k0.min_version), NEURD_GET_VERSION_PATCH(g.k0.min_version),
				        knob_src_str(g.k0.src_min_version), NEURD_GET_VERSION_MAJOR(g.k0.min_version),
				        NEURD_GET_VERSION_MINOR(g.k0.min_version), NEURD_GET_VERSION_PATCH(g.k0.min_version));
				// Deliberately no FreeLibrary: NeurD may have spun threads.
				g.state.store(G_FAILED);
				g.worker_running.store(false);
				return;
			}
			if (NEURD_GET_VERSION_MAJOR(nd->version) != 0 ||
			    !LEIA_NEURD_HAS(nd, convert_stream_dx) || !LEIA_NEURD_HAS(nd, create_stream) ||
			    !LEIA_NEURD_HAS(nd, set_prop_1i) || !LEIA_NEURD_HAS(nd, set_prop_1f)) {
				U_LOG_W("Leia lift: NeurD runtime too old/new for the D3D11 stream path (need 0.3.11+, "
				        "major 0) — lift unavailable");
				g.state.store(G_FAILED);
				g.worker_running.store(false);
				return;
			}
			g.lib = lib;
			g.nd = nd;
			if (LEIA_NEURD_HAS(nd, set_logger_callback)) {
				NeurD_set_logger_callback(nd, neurd_log_cb);
			}
		}

		struct NeurD const *nd = g.nd;

		// Properties that must precede init (NeurD reads some at init time):
		// the defaults this plug-in wants for every stream.
		NeurD_set_prop_1i(nd, NEURD_PROP_OUTPUT_TYPE, NEURD_OUTPUT_IMAGE_TYPE_SBS);
		NeurD_set_prop_1i(nd, NEURD_PROP_INPUT_AUTOSCALING, g.default_autoscaling);
		NeurD_set_prop_1i(nd, NEURD_PROP_INPAINT_TYPE, NEURD_INPAINT_TYPE_V1_STRETCH);

		// Backend must be forced immediately before init. NeurD keeps a forced
		// backend across deinit; after an idle unload the DLL is mapped afresh,
		// so it is forced again here — with the same first-activation choice,
		// which is also what a DLL that never really unmapped still holds.
		if (g.requested_backend != BACKEND_AUTO && LEIA_NEURD_HAS(nd, set_backend)) {
			enum NeurD_status bs = NeurD_set_backend(nd, (enum NeurD_backend)g.requested_backend);
			if (bs != NEURD_SUCCESS) {
				U_LOG_W("Leia lift: NeurD_set_backend(%s) -> %s", backend_str((enum NeurD_backend)g.requested_backend),
				        status_str(bs));
			}
		}

		enum NeurD_status st;
		if (LEIA_NEURD_HAS(nd, init_with_options)) {
			struct NeurD_init_options opts = {};
			opts.struct_size = sizeof(opts);
			opts.photo_model_id = NEURD_MODEL_PHOTO_RELATIVE_QUALITY;
			opts.video_model_id = g.k0.video_model;
			st = NeurD_init_with_options(nd, 1 /* multithreaded: we call from the runtime's lift thread */, &opts);
		} else if (LEIA_NEURD_HAS(nd, init)) {
			st = NeurD_init(nd, 1);
		} else {
			st = NEURD_UNAVAILABLE_OUTDATED_RUNTIME;
		}

		if (st == NEURD_LICENSE_NETWORK_ERROR || st == NEURD_NETWORK_ERROR) {
			g.next_retry_ms.store(now_ms() + kActivationRetryMs);
			U_LOG_W("Leia lift: NeurD licence activation needs the network (%s) — reporting ACTIVATING, "
			        "retrying in %llus",
			        status_str(st), (unsigned long long)(kActivationRetryMs / 1000));
			g.state.store(G_RETRY_WAIT);
			g.worker_running.store(false);
			return;
		}
		if (st != NEURD_SUCCESS) {
			U_LOG_W("Leia lift: NeurD init failed: %s — lift unavailable", status_str(st));
			g.state.store(G_FAILED);
			g.worker_running.store(false);
			return;
		}

		enum NeurD_backend be = (g.requested_backend == BACKEND_AUTO)
		                                 ? NEURD_BACKEND_DIRECTML
		                                 : (enum NeurD_backend)g.requested_backend;
		if (LEIA_NEURD_HAS(nd, get_backend)) {
			NeurD_get_backend(nd, &be);
		}

		std::lock_guard<std::mutex> lock(g.mtx);
		g.backend = be;
		snprintf(g.backend_name, sizeof(g.backend_name), "neurd-%s", backend_str(be));
		const struct knobs &k0 = g.k0;
		uint64_t interactive_min = NEURD_convert_stream_dx_interactive_INTRODUCED_IN;
		if (k0.interactive_min != 0) {
			interactive_min = k0.interactive_min;
		}
		char idle_str[24];
		if (k0.idle_unload_sec == 0) {
			snprintf(idle_str, sizeof(idle_str), "off");
		} else {
			snprintf(idle_str, sizeof(idle_str), "%us", k0.idle_unload_sec);
		}
		// Once per process: a reload runs with the same first-activation knobs.
		if (!reload) {
			U_LOG_W("Leia lift: knobs backend=%s(%s) interactive_min=%u.%u.%u(%s) min_version=%u.%u.%u(%s) "
			        "scale=%s(%s) view_gain=%.2f(%s) conv_gain=%.2f(%s) video_model=%s(%s) depth_gain=%.2f(%s) "
			        "dilate=%d(%s) idle_unload=%s(%s) shadow_deps=%s(%s) "
			        "[env > HKLM\\SOFTWARE\\DisplayXR\\Leia\\Lift > default]",
			        backend_choice_str(k0.backend), knob_src_str(k0.src_backend),
			        (unsigned)NEURD_GET_VERSION_MAJOR(interactive_min),
			        (unsigned)NEURD_GET_VERSION_MINOR(interactive_min),
			        (unsigned)NEURD_GET_VERSION_PATCH(interactive_min), knob_src_str(k0.src_interactive_min),
			        (unsigned)NEURD_GET_VERSION_MAJOR(k0.min_version), (unsigned)NEURD_GET_VERSION_MINOR(k0.min_version),
			        (unsigned)NEURD_GET_VERSION_PATCH(k0.min_version), knob_src_str(k0.src_min_version),
			        k0.scale_forced ? scale_str(k0.autoscaling) : "per-stream", knob_src_str(k0.src_scale),
			        (double)k0.view_gain, knob_src_str(k0.src_view_gain), (double)k0.conv_gain,
			        knob_src_str(k0.src_conv_gain),
			        k0.video_model == NEURD_MODEL_VIDEO_METRIC_QUALITY ? "metric" : "fast",
			        knob_src_str(k0.src_video_model), (double)k0.depth_gain, knob_src_str(k0.src_depth_gain),
			        (int)k0.dilate, knob_src_str(k0.src_dilate), idle_str, knob_src_str(k0.src_idle_unload),
			        k0.shadow_deps ? "on" : "off", knob_src_str(k0.src_shadow_deps));
		}
		if (k0.interactive_min != 0) {
			U_LOG_W("Leia lift: DXR_LEIA_LIFT_INTERACTIVE_MIN=%u.%u.%u (%s) — assuming interactive convert on NeurD "
			        "%u.%u.%u (only for the internal-interactive dev package; a stock 0.4.4 will crash here)",
			        (unsigned)NEURD_GET_VERSION_MAJOR(interactive_min),
			        (unsigned)NEURD_GET_VERSION_MINOR(interactive_min),
			        (unsigned)NEURD_GET_VERSION_PATCH(interactive_min), knob_src_str(k0.src_interactive_min),
			        (unsigned)NEURD_GET_VERSION_MAJOR(nd->version), (unsigned)NEURD_GET_VERSION_MINOR(nd->version),
			        (unsigned)NEURD_GET_VERSION_PATCH(nd->version));
		}
		// Recomputed on every activation, reloads included: the NeurD now loaded
		// may be a different (upgraded) version than the one before the unload.
		g.interactive_unavailable = !(nd->version >= interactive_min && nd->convert_stream_dx_interactive != nullptr);
		g.interactive_direct_slot = !g.interactive_unavailable.load() &&
		                            nd->version < NEURD_convert_stream_dx_interactive_INTRODUCED_IN;
		g.props_valid = false;
		next = setup_nd_device_locked() ? (uint32_t)G_READY : (uint32_t)G_FAILED;
		if (next == G_READY) {
			{
				std::lock_guard<std::mutex> cl(g.caps_mtx);
				snprintf(g.caps_backend_name, sizeof(g.caps_backend_name), "%s", g.backend_name);
				g.caps_backend = g.backend;
			}
			// Idle clock: READY with no stream counts as idle from now (a stream
			// destroyed while this reload ran is not credited — keeps it loaded a
			// little longer, never shorter).
			if (g.streams_live.load() == 0) {
				g.idle_since_ms.store(now_ms());
			}
			U_LOG_W("Leia lift: NeurD READY%s — backend %s, interactive viewpoints %s",
			        reload ? " again after the idle unload" : "", g.backend_name,
			        g.interactive_unavailable ? "UNAVAILABLE (NeurD < 0.4.5)" : "available");
		}
	}
	if (next == G_READY) {
		g.reloading.store(false); // before READY is visible: the next unload may start a new reload
	}
	g.state.store(next);
	g.worker_running.store(false);
	if (next == G_READY) {
		arm_idle_watcher(); // no-op unless idle unload is on and no stream exists
	}
}

/*!
 * Advance the process state machine without blocking: probe presence on first
 * touch, and (re)start the activation worker when due. @p reload_ok: the caller
 * is a stream (create / convert), so an idle-unloaded NeurD may be brought back;
 * a caps poll passes false and never re-locks the DLLs.
 */
void
kick(const struct knobs &k, bool reload_ok)
{
	uint32_t s = g.state.load();
	if (s == G_IDLE) {
		if (!reload_ok) {
			return;
		}
		// A full re-activation, exactly like the first: presence probe here,
		// then the worker's load + MinVersion + backend + init + licence. NeurD
		// may have been uninstalled since the unload — that is final, as at
		// first probe.
		if (!neurd_present()) {
			uint32_t expect = G_IDLE;
			if (g.state.compare_exchange_strong(expect, G_ABSENT)) {
				U_LOG_W("Leia lift: NeurD.dll no longer found when reloading after the idle unload — lift "
				        "unavailable until the service restarts (open streams fail their frames)");
			}
			return;
		}
	} else if (s == G_UNPROBED) {
		// Racing first callers are fine: presence is idempotent and only one
		// wins the worker_running exchange below.
		if (!neurd_present()) {
			uint32_t expect = G_UNPROBED;
			if (g.state.compare_exchange_strong(expect, G_ABSENT)) {
				U_LOG_W("Leia lift: NeurD.dll not found (PATH / NEURD_PATH / HKLM\\SOFTWARE\\LeiaInc\\NeurD) — "
				        "2D->3D lift unavailable; plug-in otherwise unaffected");
			}
			return;
		}
	} else if (s == G_RETRY_WAIT) {
		if (now_ms() < g.next_retry_ms.load()) {
			return;
		}
	} else {
		return; // ABSENT / LOADING / READY / FAILED / UNLOADING: nothing to do.
	}

	// Also refuses while a worker that just published READY has not yet cleared
	// the flag; the next kick (every create / convert) tries again.
	bool expect_idle = false;
	if (!g.worker_running.compare_exchange_strong(expect_idle, true)) {
		return;
	}
	if (s == G_UNPROBED) {
		g.requested_backend = k.backend;
		g.default_autoscaling = k.autoscaling;
		g.k0 = k;
		g.idle_unload_ms.store((uint64_t)k.idle_unload_sec * 1000ull);
	}
	// CAS, not store: a racing caller's presence probe may have moved the state
	// (e.g. IDLE -> ABSENT) since it was read, and that verdict must stand.
	uint32_t expect_s = s;
	if (!g.state.compare_exchange_strong(expect_s, G_LOADING)) {
		g.worker_running.store(false);
		return;
	}
	if (s == G_IDLE) {
		g.reloading.store(true); // a licence retry (RETRY_WAIT) of a reload keeps it set
	}
	try {
		std::thread(activation_worker).detach();
	} catch (...) {
		U_LOG_W("Leia lift: could not start the NeurD activation thread");
		g.state.store(s);
		g.worker_running.store(false);
	}
}

uint32_t
public_state(uint32_t s)
{
	switch (s) {
	case G_READY:
	case G_UNLOADING: // was READY; the runtime has frozen its caps at READY anyway
	case G_IDLE: return LEIA_LIFT_STATE_READY;
	case G_UNPROBED:
	case G_LOADING:
	case G_RETRY_WAIT: return LEIA_LIFT_STATE_ACTIVATING;
	default: return LEIA_LIFT_STATE_UNAVAILABLE;
	}
}

/*
 *
 * Idle unload (IdleUnloadSec). The runtime keeps its one lift DP for the life
 * of the service and stops polling caps once READY, so neither the DP
 * ref-count nor caps can drive this: the signal is "no lift stream for N
 * seconds". One watcher thread at a time sleeps (no lock held) until that is
 * true, then unloads; it exits as soon as a stream exists or NeurD is not
 * READY, and is re-armed when the last stream goes or READY is reached with
 * none.
 *
 */

//! Every condition for an idle unload except the elapsed time. Lock-free; the
//! unload re-checks all of it under g.mtx.
bool
idle_pending()
{
	return g.idle_unload_ms.load() != 0 && g.state.load() == G_READY && g.streams_live.load() == 0 &&
	       g.idle_since_ms.load() != 0;
}

/*!
 * Count the modules mapped from @p dir (NeurD's directory, with its trailing
 * backslash; subdirectories included) and list their relative names into
 * @p out, comma-separated, truncated to @p cap. Diagnostic for the idle unload:
 * whatever is still listed after FreeLibrary keeps its file locked. Modules
 * under the ShadowDeps directory are never counted, even should NeurD's
 * directory contain it: those are our copies, not NeurD's files.
 * K32EnumProcessModules is resolved at run time (kernel32; no psapi link).
 */
uint32_t
modules_mapped_from(const wchar_t *dir, char *out, size_t cap)
{
	out[0] = '\0';
	wchar_t shadow[MAX_PATH];
	if (!shadow_root(shadow, MAX_PATH)) {
		shadow[0] = L'\0';
	}
	const size_t dir_len = wcslen(dir);
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	using pfn_enum_modules = BOOL(WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD);
	auto enum_modules = k32 == nullptr ? nullptr
	                                   : reinterpret_cast<pfn_enum_modules>(reinterpret_cast<void *>(
	                                         GetProcAddress(k32, "K32EnumProcessModules")));
	HMODULE mods[1024];
	DWORD need = 0;
	if (dir_len == 0 || enum_modules == nullptr ||
	    !enum_modules(GetCurrentProcess(), mods, (DWORD)sizeof(mods), &need)) {
		return 0;
	}
	const DWORD n = std::min<DWORD>(need / (DWORD)sizeof(HMODULE), (DWORD)(sizeof(mods) / sizeof(mods[0])));
	uint32_t count = 0;
	size_t used = 0;
	for (DWORD i = 0; i < n; i++) {
		// A module unloaded since the enumeration just fails here and is skipped.
		wchar_t path[MAX_PATH];
		const DWORD len = GetModuleFileNameW(mods[i], path, MAX_PATH);
		if (len == 0 || len >= MAX_PATH || len <= dir_len || _wcsnicmp(path, dir, dir_len) != 0 ||
		    path_is_under(path, shadow)) {
			continue;
		}
		count++;
		char name[MAX_PATH * 3];
		if (WideCharToMultiByte(CP_UTF8, 0, path + dir_len, -1, name, (int)sizeof(name), nullptr, nullptr) <= 0) {
			continue;
		}
		const int w = snprintf(out + used, cap - used, "%s%s", used != 0 ? ", " : "", name);
		if (w > 0) {
			used = std::min(used + (size_t)w, cap - 1); // truncated: keep what fit
		}
	}
	return count;
}

//! Drop everything this module created on NeurD's device or holds of it (the
//! bridge shaders, constant buffer, event query, the context reference, and the
//! device itself when the plug-in owns it). Called with g.mtx held, BEFORE
//! NeurD_deinit tears the device down. Per-stream bridges need nothing here:
//! an idle unload happens only with no stream at all.
void
release_nd_bridge_locked()
{
	safe_release(g.cs_pack);
	safe_release(g.cs_unpack);
	safe_release(g.cs_unpack_r8);
	safe_release(g.cb);
	safe_release(g.nd_done);
	safe_release(g.nd_ctx);
	release_nd_dev_locked();
	g.nd_luid = {};
}

/*!
 * Unload an idle NeurD (watcher thread). The decision and the hand-off happen
 * under g.mtx, with the state moved READY -> UNLOADING first; NeurD_deinit and
 * FreeLibrary then run WITHOUT the lock, so a stream create / destroy or a DP
 * destroy never waits behind NeurD's teardown. UNLOADING alone keeps every
 * other NeurD caller out: convert re-checks READY under g.mtx; stream destroy
 * and DP destroy call NeurD only in READY (and no stream can own a NeurD stream
 * now — there were none, and a new one gets its NeurD side only after a
 * reload); caps never calls NeurD; and kick() starts the activation worker only
 * from IDLE, which is published after FreeLibrary has returned.
 */
void
idle_unload(uint64_t idle_ms)
{
	HMODULE lib = nullptr;
	struct NeurD const *nd = nullptr;
	uint64_t idle_for_ms = 0;
	{
		std::lock_guard<std::mutex> lock(g.mtx);
		const uint64_t since = g.idle_since_ms.load();
		const uint64_t now = now_ms();
		if (g.state.load() != G_READY || g.streams_live.load() != 0 || since == 0 || now < since + idle_ms) {
			return; // a stream came (or the clock restarted) meanwhile: the watcher re-evaluates
		}
		uint32_t expect = G_READY;
		if (!g.state.compare_exchange_strong(expect, G_UNLOADING)) {
			return;
		}
		idle_for_ms = now - since;
		release_nd_bridge_locked();
		lib = g.lib;
		nd = g.nd;
		g.lib = nullptr;
		g.nd = nullptr;
		g.props_valid = false; // the next NeurD instance starts from its own defaults
		g.interactive_direct_slot = false;
	}

	U_LOG_W("Leia lift: no lift stream for %llus (IdleUnloadSec=%llu) — unloading NeurD so its installer can "
	        "replace the files; the next stream reloads it",
	        (unsigned long long)(idle_for_ms / 1000), (unsigned long long)(idle_ms / 1000));

	// NeurD's directory, for the leftover-module diagnostic (before the unmap).
	wchar_t dir[MAX_PATH] = {};
	if (lib != nullptr) {
		const DWORD n = GetModuleFileNameW(lib, dir, MAX_PATH);
		wchar_t *slash = (n > 0 && n < MAX_PATH) ? wcsrchr(dir, L'\\') : nullptr;
		if (slash != nullptr) {
			slash[1] = L'\0';
		} else {
			dir[0] = L'\0';
		}
	}
	char names[512];
	const uint32_t mapped_before = modules_mapped_from(dir, names, sizeof(names));

	const uint64_t t0 = now_ms();
	if (nd != nullptr) {
		try {
			// Logger first (NeurD forgets it with the unmap anyway; this keeps
			// any teardown-time callback out of the plug-in), then deinit.
			if (LEIA_NEURD_HAS(nd, set_logger_callback)) {
				NeurD_set_logger_callback(nd, neurd_log_discard);
			}
			if (LEIA_NEURD_HAS(nd, deinit)) {
				NeurD_deinit(nd);
			}
		} catch (...) {
			U_LOG_W("Leia lift: exception from NeurD teardown — unloading anyway");
		}
	}
	Sleep(kUnloadSettleMs);
	if (lib != nullptr) {
		FreeLibrary(lib);
	}
	const uint32_t mapped_after = modules_mapped_from(dir, names, sizeof(names));
	U_LOG_W("Leia lift: NeurD unloaded (deinit + FreeLibrary, %llu ms); %u of %u module(s) from %ls still mapped%s%s",
	        (unsigned long long)(now_ms() - t0), mapped_after, mapped_before, dir[0] != L'\0' ? dir : L"?",
	        mapped_after != 0 ? " (their files stay locked): " : "", mapped_after != 0 ? names : "");
	g.state.store(G_IDLE);
}

DWORD WINAPI
idle_watcher_proc(LPVOID self_ref)
{
	try {
		for (;;) {
			if (!idle_pending()) {
				g.watcher_running.store(false);
				// The last stream may have gone between the test and the store
				// and found watcher_running still set (so it started no one):
				// re-test, and take the job back if so.
				bool expect = false;
				if (!idle_pending() || !g.watcher_running.compare_exchange_strong(expect, true)) {
					break;
				}
				continue;
			}
			const uint64_t idle_ms = g.idle_unload_ms.load();
			const uint64_t due = g.idle_since_ms.load() + idle_ms;
			const uint64_t now = now_ms();
			if (now < due) {
				Sleep((DWORD)std::min<uint64_t>(due - now, kIdleWatchMaxSleepMs));
				continue;
			}
			idle_unload(idle_ms); // re-checks under g.mtx; READY -> IDLE, or nothing
		}
	} catch (...) {
		// Only std::mutex::lock can throw here, before any state change.
		g.watcher_running.store(false);
	}
	if (self_ref != nullptr) {
		FreeLibraryAndExitThread(static_cast<HMODULE>(self_ref), 0);
	}
	return 0;
}

/*!
 * Start the idle watcher if an idle unload is pending and none runs (a running
 * one re-reads the idle clock). Lock-free; call it after the last stream goes
 * and after READY is published.
 */
void
arm_idle_watcher()
{
	if (!idle_pending()) {
		return;
	}
	bool expect = false;
	if (!g.watcher_running.compare_exchange_strong(expect, true)) {
		return;
	}
	// The watcher sleeps for minutes, so it pins this plug-in DLL (a reference
	// taken here, dropped by FreeLibraryAndExitThread): an unload of the plug-in
	// under it would otherwise crash. If the pin fails it still runs, unpinned —
	// the activation thread's model.
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
	                        reinterpret_cast<LPCWSTR>(reinterpret_cast<void *>(&idle_watcher_proc)), &self)) {
		self = nullptr;
	}
	HANDLE th = CreateThread(nullptr, 0, idle_watcher_proc, self, 0, nullptr);
	if (th == nullptr) {
		const unsigned long err = (unsigned long)GetLastError();
		LIFT_WARN_ONCE("Leia lift: could not start the idle-unload watcher (err %lu) — NeurD stays loaded", err);
		if (self != nullptr) {
			FreeLibrary(self);
		}
		g.watcher_running.store(false);
		return;
	}
	CloseHandle(th);
}

/*
 *
 * Per-stream bridge.
 *
 */

struct lift_stream
{
	uint64_t id;
	uint32_t mode;
	uint32_t content_hint;
	float input_scale;
	struct NeurD_stream *ns; //!< Created lazily on first convert (NeurD must be READY).

	ID3D11Device *dev; //!< Caller's device (AddRef'd) the bridges are opened on.
	ID3D11Query *our_done;

	// Input bridge.
	uint32_t in_w, in_h;
	DXGI_FORMAT in_family; //!< R8G8B8A8_TYPELESS or B8G8R8A8_TYPELESS.
	ID3D11Texture2D *in_nd;
	ID3D11Texture2D *in_ours;
	ID3D11ShaderResourceView *in_srv;
	ID3D11Buffer *in_buf;
	ID3D11UnorderedAccessView *in_uav;

	// Output bridge.
	uint32_t out_w, out_h;
	DXGI_FORMAT out_fmt; //!< R8G8B8A8_UNORM, or R8_UNORM for DEPTH
	ID3D11Texture2D *out_nd;
	ID3D11Texture2D *out_ours;
	ID3D11UnorderedAccessView *out_uav;
	ID3D11Resource *out_src_key; //!< NeurD output resource the raw UAV below was made for.
	ID3D11UnorderedAccessView *out_src_uav;
};

void
release_in_bridge(lift_stream *s)
{
	safe_release(s->in_uav);
	safe_release(s->in_buf);
	safe_release(s->in_srv);
	safe_release(s->in_ours);
	safe_release(s->in_nd);
	s->in_w = s->in_h = 0;
}

void
release_out_bridge(lift_stream *s)
{
	safe_release(s->out_src_uav);
	s->out_src_key = nullptr; // not owned
	safe_release(s->out_uav);
	safe_release(s->out_ours);
	safe_release(s->out_nd);
	s->out_w = s->out_h = 0;
	s->out_fmt = DXGI_FORMAT_UNKNOWN;
}

//! Called with g.mtx held.
void
release_stream_locked(lift_stream *s)
{
	release_in_bridge(s);
	release_out_bridge(s);
	safe_release(s->our_done);
	safe_release(s->dev);
	if (s->ns != nullptr && g.state.load() == G_READY && LEIA_NEURD_HAS(g.nd, destroy_stream)) {
		NeurD_destroy_stream(g.nd, s->ns);
	}
	s->ns = nullptr;
}

/*!
 * Shared texture on NeurD's device, opened on the caller's device. Legacy
 * (non-NT) handle — the one NeurD itself and LeiaMeet rely on; not owned, no
 * CloseHandle.
 */
bool
make_shared_tex(ID3D11Device *ours, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, ID3D11Texture2D **out_nd,
                ID3D11Texture2D **out_ours)
{
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = bind;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	if (FAILED(g.nd_dev->CreateTexture2D(&td, nullptr, out_nd))) {
		return false;
	}
	IDXGIResource *dr = nullptr;
	HANDLE hshared = nullptr;
	bool ok = SUCCEEDED((*out_nd)->QueryInterface(__uuidof(IDXGIResource), (void **)&dr)) && dr != nullptr &&
	          SUCCEEDED(dr->GetSharedHandle(&hshared)) && hshared != nullptr &&
	          SUCCEEDED(ours->OpenSharedResource(hshared, __uuidof(ID3D11Texture2D), (void **)out_ours));
	safe_release(dr);
	if (!ok) {
		safe_release(*out_nd);
		return false;
	}
	return true;
}

bool
ensure_in_bridge(lift_stream *s, uint32_t w, uint32_t h, DXGI_FORMAT family)
{
	if (s->in_nd != nullptr && s->in_w == w && s->in_h == h && s->in_family == family) {
		return true;
	}
	release_in_bridge(s);
	if (!make_shared_tex(s->dev, w, h, family, D3D11_BIND_SHADER_RESOURCE, &s->in_nd, &s->in_ours)) {
		LIFT_WARN_ONCE("Leia lift: input bridge texture %ux%u create/open failed", w, h);
		return false;
	}
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = (family == DXGI_FORMAT_B8G8R8A8_TYPELESS) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;

	// Raw input buffer NeurD accepts: flat RGBA8, stride w*4, SHARED.
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = w * h * 4;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_SHARED;
	D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
	ud.Format = DXGI_FORMAT_R32_TYPELESS;
	ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	ud.Buffer.NumElements = w * h;
	ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;

	if (FAILED(g.nd_dev->CreateShaderResourceView(s->in_nd, &sd, &s->in_srv)) ||
	    FAILED(g.nd_dev->CreateBuffer(&bd, nullptr, &s->in_buf)) ||
	    FAILED(g.nd_dev->CreateUnorderedAccessView(s->in_buf, &ud, &s->in_uav))) {
		LIFT_WARN_ONCE("Leia lift: input bridge views/buffer %ux%u failed", w, h);
		release_in_bridge(s);
		return false;
	}
	s->in_w = w;
	s->in_h = h;
	s->in_family = family;
	return true;
}

bool
ensure_out_bridge(lift_stream *s, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	if (s->out_nd != nullptr && s->out_w == w && s->out_h == h && s->out_fmt == fmt) {
		return true;
	}
	release_out_bridge(s);
	if (w > kMaxTexDim || h > kMaxTexDim) {
		LIFT_WARN_ONCE("Leia lift: output %ux%u exceeds the D3D11 texture limit (%u) — lower view_count or "
		               "DXR_LEIA_LIFT_SCALE",
		               w, h, kMaxTexDim);
		return false;
	}
	if (!make_shared_tex(s->dev, w, h, fmt,
	                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &s->out_nd, &s->out_ours) ||
	    FAILED(g.nd_dev->CreateUnorderedAccessView(s->out_nd, nullptr, &s->out_uav))) {
		LIFT_WARN_ONCE("Leia lift: output bridge texture %ux%u failed", w, h);
		release_out_bridge(s);
		return false;
	}
	s->out_w = w;
	s->out_h = h;
	s->out_fmt = fmt;
	return true;
}

//! Spin until @p q (an EVENT query already End()ed on @p ctx) signals.
bool
drain(ID3D11DeviceContext *ctx, ID3D11Query *q, const char *who)
{
	ctx->End(q);
	ctx->Flush();
	const uint64_t t0 = now_ms();
	HRESULT hr;
	while ((hr = ctx->GetData(q, nullptr, 0, 0)) == S_FALSE) {
		if (now_ms() - t0 > kDrainTimeoutMs) {
			U_LOG_W("Leia lift: %s queue drain timed out (%llu ms) — device hung/lost?", who,
			        (unsigned long long)kDrainTimeoutMs);
			return false;
		}
		std::this_thread::yield();
	}
	if (hr != S_OK) {
		U_LOG_W("Leia lift: %s queue drain failed 0x%08x — device lost?", who, (unsigned)hr);
		return false;
	}
	return true;
}

bool
set_cb(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(g.nd_ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		return false;
	}
	uint32_t *p = static_cast<uint32_t *>(m.pData);
	p[0] = a;
	p[1] = b;
	p[2] = c;
	p[3] = d;
	g.nd_ctx->Unmap(g.cb, 0);
	return true;
}

void
unbind_cs()
{
	ID3D11UnorderedAccessView *nu[2] = {nullptr, nullptr};
	ID3D11ShaderResourceView *ns = nullptr;
	g.nd_ctx->CSSetUnorderedAccessViews(0, 2, nu, nullptr);
	g.nd_ctx->CSSetShaderResources(0, 1, &ns);
	g.nd_ctx->CSSetShader(nullptr, nullptr, 0);
}

inline UINT
groups16(uint32_t n)
{
	return (n + 15) / 16;
}

//! Copy/unpack NeurD's output into the stream's output bridge (NeurD device).
bool
stage_output(lift_stream *s, const struct NeurD_image &out)
{
	auto *res = static_cast<ID3D11Resource *>(out.data);
	if (res == nullptr || out.width <= 0 || out.height <= 0) {
		return false;
	}
	const uint32_t w = (uint32_t)out.width;
	const uint32_t h = (uint32_t)out.height;

	ID3D11Texture2D *tex = nullptr;
	if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) && tex != nullptr) {
		// NOTE: DEPTH on this path is NOT polarity-flipped (still NeurD's
		// near = high) — only kUnpackR8Cs flips. Only NeurD 0.3.x's CUDA DX
		// path returns a texture.
		// A texture result is copied as-is (RGBA8 even for DEPTH; out_format
		// says so). NeurD's stream path returns buffers; this is defensive.
		if (!ensure_out_bridge(s, w, h, DXGI_FORMAT_R8G8B8A8_UNORM)) {
			tex->Release();
			return false;
		}
		// Region copy: NeurD's internal texture may exceed the reported size.
		D3D11_BOX box = {0, 0, 0, w, h, 1};
		g.nd_ctx->CopySubresourceRegion(s->out_nd, 0, 0, 0, 0, tex, 0, &box);
		tex->Release();
		return true;
	}

	ID3D11Buffer *buf = nullptr;
	if (FAILED(res->QueryInterface(__uuidof(ID3D11Buffer), (void **)&buf)) || buf == nullptr) {
		LIFT_WARN_ONCE("Leia lift: NeurD output is neither a texture nor a buffer");
		return false;
	}
	const bool depth = (s->mode == LEIA_LIFT_MODE_DEPTH);
	if (!ensure_out_bridge(s, w, h, depth ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM)) {
		buf->Release();
		return false;
	}
	D3D11_BUFFER_DESC bd = {};
	buf->GetDesc(&bd);
	uint32_t stride = (out.stride >= (int32_t)(w * 4)) ? (uint32_t)out.stride : w * 4;
	if ((uint64_t)stride * h > bd.ByteWidth) {
		buf->Release();
		LIFT_WARN_ONCE("Leia lift: NeurD output buffer too small (%u < %llu bytes)", bd.ByteWidth,
		               (unsigned long long)stride * h);
		return false;
	}
	if (s->out_src_key != res || s->out_src_uav == nullptr) {
		safe_release(s->out_src_uav);
		D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
		ud.Format = DXGI_FORMAT_R32_TYPELESS;
		ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		ud.Buffer.NumElements = bd.ByteWidth / 4;
		ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(g.nd_dev->CreateUnorderedAccessView(buf, &ud, &s->out_src_uav))) {
			buf->Release();
			LIFT_WARN_ONCE("Leia lift: raw UAV over NeurD's output buffer failed");
			return false;
		}
		s->out_src_key = res;
	}
	buf->Release();

	if (!set_cb(w, h, stride, 0)) {
		return false;
	}
	ID3D11UnorderedAccessView *uavs[2] = {s->out_src_uav, s->out_uav};
	g.nd_ctx->CSSetShader(depth ? g.cs_unpack_r8 : g.cs_unpack, nullptr, 0);
	g.nd_ctx->CSSetConstantBuffers(0, 1, &g.cb);
	g.nd_ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	g.nd_ctx->Dispatch(groups16(w), groups16(h), 1);
	unbind_cs();
	return true;
}

//! Apply one global NeurD property only if it changed. Called with g.mtx held.
bool
prop_i(enum NeurD_prop p, int32_t v, int32_t &cache)
{
	if (g.props_valid && cache == v) {
		return true;
	}
	enum NeurD_status st = NeurD_set_prop_1i(g.nd, p, v);
	if (st != NEURD_SUCCESS) {
		U_LOG_W("Leia lift: NeurD_set_prop_1i(%d, %d) -> %s", (int)p, (int)v, status_str(st));
		cache = -1;
		return false;
	}
	cache = v;
	return true;
}

bool
prop_f(enum NeurD_prop p, float v, float &cache)
{
	if (g.props_valid && cache == v) {
		return true;
	}
	enum NeurD_status st = NeurD_set_prop_1f(g.nd, p, v);
	if (st != NEURD_SUCCESS) {
		U_LOG_W("Leia lift: NeurD_set_prop_1f(%d, %f) -> %s", (int)p, (double)v, status_str(st));
		cache = -1.0f;
		return false;
	}
	cache = v;
	return true;
}


int32_t
autoscale_for(const lift_stream *s, const struct knobs &k, uint32_t in_h)
{
	// An explicit DXR_LEIA_LIFT_SCALE is a developer override and wins; else
	// the stream's input_scale (1 = native) picks the bucket; else the default.
	if (k.scale_forced || !(s->input_scale > 0.0f) || s->input_scale > 1.0f) {
		return k.autoscaling;
	}
	// Smallest NeurD bucket that still covers the requested inference height.
	float target = s->input_scale * (float)in_h;
	if (target <= 720.0f) {
		return NEURD_INPUT_AUTOSCALING_720P;
	}
	if (target <= 1080.0f) {
		return NEURD_INPUT_AUTOSCALING_1080P;
	}
	if (target <= 1440.0f) {
		return NEURD_INPUT_AUTOSCALING_1440P;
	}
	return NEURD_INPUT_AUTOSCALING_NONE;
}

} // namespace

/*
 *
 * Per-DP handle.
 *
 */

struct leia_lift_neurd
{
	struct knobs k;
	bool acquired; //!< Holds a ref on the process instance.
	std::vector<lift_stream *> streams;
};

namespace {

bool
reload_in_progress(uint32_t s)
{
	return s == G_UNLOADING || s == G_IDLE || s == G_LOADING;
}

/*!
 * convert() found NeurD not READY. For a PHOTO stream with a reload in progress
 * (after an idle unload), wait for it — at most kPhotoReloadWaitMs, polling,
 * holding no lock — and return true once READY. Video streams, and every other
 * state, return false at once. Runs on the runtime's lift thread, which can do
 * nothing useful meanwhile anyway: no stream converts until NeurD is back.
 */
bool
wait_reload_if_photo(struct leia_lift_neurd *l, uint64_t id)
{
	if (!reload_in_progress(g.state.load())) {
		return false;
	}
	bool photo = false;
	try {
		std::lock_guard<std::mutex> lock(g.mtx);
		for (const lift_stream *it : l->streams) {
			if (it->id == id) {
				photo = (it->content_hint == 1);
				break;
			}
		}
	} catch (...) {
		return false;
	}
	if (!photo) {
		return false;
	}
	const uint64_t deadline = now_ms() + kPhotoReloadWaitMs;
	for (;;) {
		const uint32_t s = g.state.load();
		if (s == G_READY) {
			return true;
		}
		if (!reload_in_progress(s)) {
			return false; // the reload failed: ABSENT / FAILED / RETRY_WAIT
		}
		if (now_ms() >= deadline) {
			LIFT_WARN_ONCE("Leia lift: NeurD reload took over %llu ms — photo frame not converted",
			               (unsigned long long)kPhotoReloadWaitMs);
			return false;
		}
		if (s == G_IDLE) {
			kick(l->k, true); // the unload finished after our first kick: start the reload now
		}
		Sleep(kReloadPollMs);
	}
}

} // namespace

extern "C" void
leia_lift_neurd_map_viewpoint(const float in_m[3], float gain, float out_n[3])
{
	out_n[0] = clampf(gain * in_m[0] / kIpdRefM, -kViewpointClamp, kViewpointClamp);
	// A lenticular panel has horizontal parallax only. Passing the tracked eye
	// HEIGHT made NeurD render the frame as seen from above/below the display
	// centre: the lifted image shifted vertically with head height (visible
	// "jumps" as the head moved) and a filled band appeared at the top edge
	// (David, panel, 2026-09-26). Vertical viewpoint offset is never wanted.
	(void)in_m[1];
	out_n[1] = 0.0f;
	// NeurD's z is a dimensionless depth offset whose relation to viewer
	// distance is not specified; head z is deliberately not mapped (see doc).
	out_n[2] = 0.0f;
}

extern "C" struct leia_lift_neurd *
leia_lift_neurd_create(void)
{
	auto *l = new (std::nothrow) leia_lift_neurd();
	if (l == nullptr) {
		return nullptr;
	}
	l->k = read_knobs();
	l->acquired = false;
	if (!l->k.enabled) {
		LIFT_WARN_ONCE("Leia lift: disabled by DXR_LEIA_LIFT=0");
	}
	return l;
}

extern "C" void
leia_lift_neurd_destroy(struct leia_lift_neurd **plift)
{
	if (plift == nullptr || *plift == nullptr) {
		return;
	}
	struct leia_lift_neurd *l = *plift;
	*plift = nullptr;
	bool went_idle = false;
	{
		std::lock_guard<std::mutex> lock(g.mtx);
		for (lift_stream *s : l->streams) {
			release_stream_locked(s);
			delete s;
			if (g.streams_live.fetch_sub(1) == 1) {
				g.idle_since_ms.store(now_ms());
				went_idle = true;
			}
		}
		l->streams.clear();
		if (l->acquired) {
			l->acquired = false;
			if (--g.refcount == 0 && g.state.load() == G_READY && LEIA_NEURD_HAS(g.nd, shrink_memory_pool)) {
				// Last user gone: give NeurD's pools back but stay initialised
				// (re-init = licence + model load; DPs are recreated on focus change).
				// Unloading is the idle watcher's call, on time, not on DP count.
				NeurD_shrink_memory_pool(g.nd);
				g.props_valid = false;
			}
		}
	}
	if (went_idle) {
		arm_idle_watcher();
	}
	delete l;
}

extern "C" bool
leia_lift_neurd_get_caps(struct leia_lift_neurd *l, struct leia_lift_neurd_caps *out)
{
	if (out == nullptr) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (l == nullptr || !l->k.enabled) {
		return true; // modes 0 / UNAVAILABLE — a valid answer, not a failure.
	}
	kick(l->k, false); // may start a first activation / licence retry; never reloads an idle-unloaded NeurD

	uint32_t s = g.state.load();
	out->state = public_state(s);
	if (out->state == LEIA_LIFT_STATE_UNAVAILABLE) {
		return true;
	}
	// Never NVIEW: NeurD's Config::adjust() re-derives the tile grid from the
	// output type on every convert (SBS 2x1, TB 1x2, DEPTH 1x1), and the public
	// OUTPUT_TYPE prop accepts only 0..2, so OUTPUT_TILES_W/H are overridden —
	// N > 2 tiles per convert is impossible through the public API (<= 0.4.6).
	out->modes = LEIA_LIFT_MODE_DEPTH | LEIA_LIFT_MODE_SBS;
	out->max_streams = kMaxStreams;
	out->max_views = kMaxViews;
	out->depth_semantics = 0; // relative: per-frame min-max normalised depth, larger = farther (NeurD's near=high disparity is flipped in kUnpackR8Cs)
	uint64_t lat = g.latency_ns.load();
	if (out->state == LEIA_LIFT_STATE_READY) {
		// READY, or UNLOADING / IDLE after an idle unload: report what READY
		// reported, from the copy taken when READY was published — not from
		// g.backend_name, which a reload rewrites — under the leaf caps_mtx, so
		// caps never waits behind a convert holding g.mtx.
		enum NeurD_backend be;
		{
			std::lock_guard<std::mutex> cl(g.caps_mtx);
			snprintf(out->backend, sizeof(out->backend), "%s", g.caps_backend_name);
			be = g.caps_backend;
		}
		if (lat == 0) {
			lat = (be == NEURD_BACKEND_DIRECTML) ? kPriorLatencyDirectMlNs
			      : (be == NEURD_BACKEND_CUDA)   ? kPriorLatencyCudaNs
			                                     : kPriorLatencyOtherNs;
		}
	} else {
		snprintf(out->backend, sizeof(out->backend), "neurd-%s",
		         l->k.backend == BACKEND_AUTO ? "auto" : backend_str((enum NeurD_backend)l->k.backend));
		if (lat == 0) {
			lat = kPriorLatencyDirectMlNs;
		}
	}
	out->typical_latency_ns = lat;
	return true;
}

extern "C" bool
leia_lift_neurd_stream_create(struct leia_lift_neurd *l, const struct leia_lift_neurd_stream_desc *desc, uint64_t *out_id)
{
	if (l == nullptr || desc == nullptr || out_id == nullptr || !l->k.enabled) {
		return false;
	}
	if (desc->mode == LEIA_LIFT_MODE_NVIEW) {
		LIFT_WARN_ONCE("Leia lift: N-view not supported — NeurD public API tiles are fixed by output type; "
		               "N-view needs a tiled output type (ask Leia)");
		return false;
	}
	if (desc->mode != LEIA_LIFT_MODE_DEPTH && desc->mode != LEIA_LIFT_MODE_SBS) {
		LIFT_WARN_ONCE("Leia lift: stream_create with unsupported mode %u", desc->mode);
		return false;
	}
	// Starts the reload of an idle-unloaded NeurD. Must still SUCCEED while it
	// reloads (IDLE / UNLOADING / LOADING): the runtime marks a stream whose
	// create failed as failed for good. Its NeurD side is created lazily by the
	// first convert after READY, from the parameters stored here.
	kick(l->k, true);
	if (public_state(g.state.load()) == LEIA_LIFT_STATE_UNAVAILABLE) {
		return false;
	}

	std::lock_guard<std::mutex> lock(g.mtx);
	if (g.streams_live.load() >= kMaxStreams) {
		LIFT_WARN_ONCE("Leia lift: stream limit (%u, NeurD's MAX_STREAMS) reached", kMaxStreams);
		return false;
	}
	auto *s = new (std::nothrow) lift_stream();
	if (s == nullptr) {
		return false;
	}
	s->id = g.next_stream_id++;
	s->mode = desc->mode;
	s->content_hint = desc->content_hint;
	s->input_scale = desc->input_scale;
	l->streams.push_back(s);
	g.streams_live.fetch_add(1);
	g.idle_since_ms.store(0); // not idle: a running watcher sees this and exits
	if (!l->acquired) {
		l->acquired = true;
		g.refcount++;
	}
	*out_id = s->id;
	U_LOG_W("Leia lift: stream %llu created (mode %u, %s)", (unsigned long long)s->id, s->mode,
	        s->content_hint == 1 ? "photo" : "video");
	return true;
}

extern "C" void
leia_lift_neurd_stream_destroy(struct leia_lift_neurd *l, uint64_t id)
{
	if (l == nullptr) {
		return;
	}
	bool went_idle = false;
	{
		std::lock_guard<std::mutex> lock(g.mtx);
		for (auto it = l->streams.begin(); it != l->streams.end(); ++it) {
			if ((*it)->id == id) {
				release_stream_locked(*it);
				delete *it;
				l->streams.erase(it);
				if (g.streams_live.fetch_sub(1) == 1) {
					g.idle_since_ms.store(now_ms()); // the idle clock starts now
					went_idle = true;
				}
				break;
			}
		}
	}
	if (went_idle) {
		arm_idle_watcher(); // no-op unless NeurD is READY and idle unload is on
	}
}

extern "C" bool
leia_lift_neurd_convert(struct leia_lift_neurd *l,
                        uint64_t id,
                        void *d3d11_context,
                        void *input,
                        uint32_t w,
                        uint32_t h,
                        const struct leia_lift_neurd_params *p,
                        const float *viewpoints_m,
                        uint32_t viewpoint_floats,
                        const float *eye_left,
                        const float *eye_right,
                        bool eyes_valid,
                        void **out_resource,
                        uint32_t *out_w,
                        uint32_t *out_h,
                        uint32_t *out_format)
{
	if (l == nullptr || !l->k.enabled || d3d11_context == nullptr || input == nullptr || w == 0 || h == 0 ||
	    out_resource == nullptr || out_w == nullptr || out_h == nullptr || out_format == nullptr) {
		return false;
	}
	*out_resource = nullptr;

	kick(l->k, true); // also (re)starts the reload of an idle-unloaded NeurD
	if (g.state.load() != G_READY && !wait_reload_if_photo(l, id)) {
		// ACTIVATING / UNAVAILABLE (caps says which), or reloading after an idle
		// unload: a video frame fails, fast and silently — the runtime counts it
		// and tries the next one, so a ~5 s reload costs ~300 frames, no WARNs.
		return false;
	}

	LARGE_INTEGER qf, q0, q1;
	QueryPerformanceFrequency(&qf);
	QueryPerformanceCounter(&q0);

	try {
		std::lock_guard<std::mutex> lock(g.mtx);
		// Re-check under the lock: the idle unload leaves READY only while
		// holding g.mtx, so from here to the unlock g.nd and NeurD's device stay
		// valid. (It cannot start while this stream exists anyway — belt and braces.)
		if (g.state.load() != G_READY || g.nd == nullptr) {
			return false;
		}
		struct NeurD const *nd = g.nd;

		lift_stream *s = nullptr;
		for (lift_stream *it : l->streams) {
			if (it->id == id) {
				s = it;
				break;
			}
		}
		if (s == nullptr) {
			LIFT_WARN_ONCE("Leia lift: convert on unknown stream id %llu", (unsigned long long)id);
			return false;
		}

		// ---- Validate the input.
		auto *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);
		auto *in_res = static_cast<ID3D11Resource *>(input);
		ID3D11Texture2D *in_tex = nullptr;
		if (FAILED(in_res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&in_tex)) || in_tex == nullptr) {
			LIFT_WARN_ONCE("Leia lift: input is not an ID3D11Texture2D");
			return false;
		}
		D3D11_TEXTURE2D_DESC idesc = {};
		in_tex->GetDesc(&idesc);
		DXGI_FORMAT family = DXGI_FORMAT_UNKNOWN;
		switch (idesc.Format) {
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UINT: family = DXGI_FORMAT_R8G8B8A8_TYPELESS; break;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: family = DXGI_FORMAT_B8G8R8A8_TYPELESS; break;
		default: break;
		}
		if (family == DXGI_FORMAT_UNKNOWN || idesc.SampleDesc.Count != 1 || w > idesc.Width || h > idesc.Height) {
			in_tex->Release();
			LIFT_WARN_ONCE("Leia lift: unsupported input (DXGI fmt %u, %ux%u, samples %u; asked %ux%u) — need "
			               "single-sampled RGBA8/BGRA8",
			               (unsigned)idesc.Format, idesc.Width, idesc.Height, idesc.SampleDesc.Count, w, h);
			return false;
		}

		// ---- Bind the stream to the caller's device (first use / device change).
		ID3D11Device *dev = nullptr;
		in_tex->GetDevice(&dev);
		if (s->dev != dev) {
			release_in_bridge(s);
			release_out_bridge(s);
			safe_release(s->our_done);
			safe_release(s->dev);
			s->dev = dev; // keeps GetDevice's reference
			D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
			if (FAILED(dev->CreateQuery(&qd, &s->our_done))) {
				in_tex->Release();
				LIFT_WARN_ONCE("Leia lift: CreateQuery on the caller's device failed");
				return false;
			}
			enable_mt_protection(ctx, "caller");
			LUID ours = {};
			if (device_luid(dev, &ours)) {
				bool same = ours.HighPart == g.nd_luid.HighPart && ours.LowPart == g.nd_luid.LowPart;
				U_LOG_W("Leia lift: stream %llu — caller adapter LUID %08lx:%08lx, NeurD %s (%s); bridging "
				        "through shared textures either way",
				        (unsigned long long)s->id, (unsigned long)ours.HighPart, (unsigned long)ours.LowPart,
				        same ? "on the SAME adapter" : "on a DIFFERENT adapter",
				        same ? "same-GPU copy" : "cross-adapter copy");
			}
		} else {
			dev->Release();
		}

		// ---- Lazily create the NeurD stream.
		if (s->ns == nullptr) {
			enum NeurD_status cs = NeurD_create_stream(nd, &s->ns);
			if (cs != NEURD_SUCCESS || s->ns == nullptr) {
				in_tex->Release();
				s->ns = nullptr;
				LIFT_WARN_ONCE("Leia lift: NeurD_create_stream -> %s", status_str(cs));
				return false;
			}
		}

		if (!ensure_in_bridge(s, w, h, family)) {
			in_tex->Release();
			return false;
		}

		// ---- 1. Caller's texture -> input bridge (our device), CPU-drained.
		D3D11_BOX box = {0, 0, 0, w, h, 1};
		ctx->CopySubresourceRegion(s->in_ours, 0, 0, 0, 0, in_tex, 0, &box);
		in_tex->Release();
		if (!drain(ctx, s->our_done, "caller")) {
			return false;
		}

		// ---- 2. Pack bridge texture -> raw RGBA8 buffer (NeurD device), drained.
		if (!set_cb(w, h, w * 4, 0)) {
			return false;
		}
		g.nd_ctx->CSSetShader(g.cs_pack, nullptr, 0);
		g.nd_ctx->CSSetConstantBuffers(0, 1, &g.cb);
		g.nd_ctx->CSSetShaderResources(0, 1, &s->in_srv);
		g.nd_ctx->CSSetUnorderedAccessViews(0, 1, &s->in_uav, nullptr);
		g.nd_ctx->Dispatch(groups16(w), groups16(h), 1);
		unbind_cs();
		if (!drain(g.nd_ctx, g.nd_done, "NeurD (pack)")) {
			return false;
		}

		// ---- 3. Global NeurD properties for this stream's mode.
		struct leia_lift_neurd_params dp = {-1.0f, 1.0f, 0, 2};
		if (p != nullptr) {
			dp = *p;
		}
		uint32_t views = 2;
		uint32_t cols = 2, rows = 1;
		int32_t out_type = NEURD_OUTPUT_IMAGE_TYPE_SBS;
		if (s->mode == LEIA_LIFT_MODE_DEPTH) {
			out_type = NEURD_OUTPUT_IMAGE_TYPE_DEPTH;
			views = 1;
			cols = rows = 1;
		} else if (s->mode == LEIA_LIFT_MODE_NVIEW) {
			// Runtime contract: N views side by side in ONE row, view 0 leftmost.
			views = std::min(std::max(dp.view_count, 2u), kMaxViews);
			cols = views;
			rows = 1;
		}
		const bool auto_conv = !(dp.convergence >= 0.0f);
		// strength: 1 = NeurD's calibrated budget, 0 = flat; negative/NaN = default.
		// The element's strength (1 = nominal) times the depth-gain calibration:
		// NeurD's own gain 1 read as too weak on the panel (David, 2026-09-26:
		// strength 2 "stronger, I like this as default").
		const float strength = (dp.strength >= 0.0f) ? dp.strength : 1.0f;
		const float gain = clampf(strength * l->k.depth_gain, 0.0f, 10.0f);
		// NeurD always fills disocclusions; non-zero picks the blur fill, 0 the
		// cheaper edge stretch.
		const int32_t inpaint = (dp.inpaint != 0) ? NEURD_INPAINT_TYPE_V1_BLUR : NEURD_INPAINT_TYPE_V1_STRETCH;

		bool props_ok = prop_i(NEURD_PROP_OUTPUT_TYPE, out_type, g.p_out_type) &&
		                prop_i(NEURD_PROP_OUTPUT_TILES_W, (int32_t)cols, g.p_tiles_w) &&
		                prop_i(NEURD_PROP_OUTPUT_TILES_H, (int32_t)rows, g.p_tiles_h) &&
		                prop_i(NEURD_PROP_INPAINT_TYPE, inpaint, g.p_inpaint) &&
		                prop_i(NEURD_PROP_INPUT_AUTOSCALING, autoscale_for(s, l->k, h),
		                       g.p_autoscale) &&
		                prop_i(NEURD_PROP_AUTO_CONVERGENCE, auto_conv ? 1 : 0, g.p_autoconv) &&
		                prop_f(NEURD_PROP_GAIN_MULTIPLIER, gain, g.p_gain) &&
		                prop_i(NEURD_PROP_DILATE_RADIO, l->k.dilate, g.p_dilate);
		if (props_ok && !auto_conv) {
			// Runtime: convergence = RELATIVE depth placed at the display plane,
			// [0,1] over the frame's depth range (0 = nearest on the glass, 1 =
			// farthest). NeurD: a disparity offset in [-0.2, 0.2]. Linear map
			// about the mid-range, K = DXR_LEIA_LIFT_CONV_GAIN (default 0.4 spans
			// NeurD's full range; negate K if the sign proves reversed on a
			// panel). UNCALIBRATED — see docs/lift-neurd.md.
			const float c = clampf(dp.convergence, 0.0f, 1.0f);
			const float nd_conv = clampf(l->k.conv_gain * (c - 0.5f), -0.2f, 0.2f);
			props_ok = prop_f(NEURD_PROP_CONVERGENCE, nd_conv, g.p_conv);
		}
		g.props_valid = props_ok;
		if (!props_ok) {
			return false;
		}

		// ---- 4. Viewpoints (SBS / NVIEW): explicit, else tracked eyes, else default.
		std::vector<float> vp;
		if (s->mode != LEIA_LIFT_MODE_DEPTH) {
			const uint32_t slots = cols * rows; // NeurD wants one triplet per grid tile
			std::vector<float> src_m; // metres, (x,y,z) per view
			if (viewpoints_m != nullptr && viewpoint_floats >= 3 && viewpoint_floats % 3 == 0) {
				const uint32_t k = viewpoint_floats / 3;
				if (k == views) {
					src_m.assign(viewpoints_m, viewpoints_m + viewpoint_floats);
				} else if (k == 2) {
					eye_left = viewpoints_m;
					eye_right = viewpoints_m + 3;
					eyes_valid = true;
				} else {
					LIFT_WARN_ONCE("Leia lift: %u explicit viewpoints for %u views — using tracked eyes", k,
					               views);
				}
			}
			if (src_m.empty() && eyes_valid && eye_left != nullptr && eye_right != nullptr) {
				// N views one eye-baseline apart, centred on the eye midpoint.
				float c[3], d[3];
				for (int a = 0; a < 3; a++) {
					c[a] = 0.5f * (eye_left[a] + eye_right[a]);
					d[a] = eye_right[a] - eye_left[a];
				}
				for (uint32_t i = 0; i < views; i++) {
					float t = (float)i - 0.5f * (float)(views - 1);
					for (int a = 0; a < 3; a++) {
						src_m.push_back(c[a] + t * d[a]);
					}
				}
			}
			if (!src_m.empty()) {
				vp.reserve(slots * 3);
				for (uint32_t i = 0; i < slots; i++) {
					uint32_t v = std::min(i, views - 1); // extra grid slots repeat the last view
					float n[3];
					leia_lift_neurd_map_viewpoint(&src_m[v * 3], l->k.view_gain, n);
					vp.insert(vp.end(), n, n + 3);
				}
			} else if (s->mode == LEIA_LIFT_MODE_NVIEW) {
				// Untracked N-view: evenly spaced, one unit apart (the ±0.5 stereo
				// spacing extended), centred on 0.
				for (uint32_t i = 0; i < slots; i++) {
					uint32_t v = std::min(i, views - 1);
					vp.push_back((float)v - 0.5f * (float)(views - 1));
					vp.push_back(0.0f);
					vp.push_back(0.0f);
				}
			}
			// SBS untracked: vp stays empty -> NeurD's own default ±0.5 pattern.

			// Which viewpoints this convert used, logged once per change: an A/B of
			// DXR_LEIA_LIFT_VIEW_GAIN with nobody in front of the tracker lands on the
			// default pattern and reads as "the gain does nothing".
			const int vp_src = !vp.empty() ? (eyes_valid ? 1 : 2) : 0;
			static std::atomic<int> s_last_vp_src{-1};
			if (s_last_vp_src.exchange(vp_src) != vp_src) {
				U_LOG_W("Leia lift: viewpoints = %s (mode %d, %u view(s)%s)",
				        vp_src == 1 ? "TRACKED eyes" : (vp_src == 2 ? "explicit/untracked N-view" : "DEFAULT pattern (no tracked eyes)"),
				        (int)s->mode, views, vp_src != 0 ? ", view_gain applied" : ", view_gain NOT applied");
			}
		}

		// ---- 5. Convert (blocking).
		struct NeurD_image nin = {};
		nin.data = s->in_buf;
		nin.width = (int32_t)w;
		nin.height = (int32_t)h;
		nin.stride = (int32_t)(w * 4);
		nin.pix_fmt = NEURD_PIXEL_FORMAT_RGBA8;
		struct NeurD_image nout = {};
		enum NeurD_status st;
		if (!vp.empty() && !g.interactive_unavailable) {
			st = nd_convert_stream_dx_interactive(nd, s->ns, &nin, vp.data(), (int)vp.size(),
			                                      NEURD_PIXEL_FORMAT_RGBA8, &nout);
			if (st == NEURD_UNAVAILABLE_OUTDATED_RUNTIME) {
				g.interactive_unavailable = true;
				U_LOG_W("Leia lift: NeurD interactive convert unavailable — tracked viewpoints ignored");
			}
			if (st != NEURD_SUCCESS && s->mode == LEIA_LIFT_MODE_SBS) {
				LIFT_WARN_ONCE("Leia lift: interactive convert -> %s; retrying with the default pattern",
				               status_str(st));
				st = NeurD_convert_stream_dx(nd, s->ns, &nin, NEURD_PIXEL_FORMAT_RGBA8, &nout);
			}
		} else if (s->mode == LEIA_LIFT_MODE_NVIEW && g.interactive_unavailable) {
			LIFT_WARN_ONCE("Leia lift: N-view needs NeurD >= 0.4.5 (interactive convert)");
			return false;
		} else {
			st = NeurD_convert_stream_dx(nd, s->ns, &nin, NEURD_PIXEL_FORMAT_RGBA8, &nout);
		}
		if (st != NEURD_SUCCESS || nout.data == nullptr) {
			LIFT_WARN_ONCE("Leia lift: NeurD convert failed: %s", status_str(st));
			return false;
		}

		// ---- 5b. Trust NeurD's actual layout, not the requested one: tiles in
		// the one-row output = output aspect / input aspect (robust to NeurD's
		// inference autoscaling). A mismatch would be mislabelled downstream.
		if (nout.width <= 0 || nout.height <= 0) {
			LIFT_WARN_ONCE("Leia lift: NeurD output has no size (%dx%d)", nout.width, nout.height);
			return false;
		}
		const double tiles_f =
		    ((double)nout.width * (double)h) / ((double)nout.height * (double)w);
		const uint32_t tiles = (uint32_t)(tiles_f + 0.5);
		if (tiles != cols * rows) {
			LIFT_WARN_ONCE("Leia lift: NeurD returned %dx%d (%u tile(s)) for a %ux%u input, expected %u — "
			               "frame dropped",
			               nout.width, nout.height, tiles, w, h, cols * rows);
			return false;
		}

		// ---- 6. Output -> bridge (NeurD device), drained; hand ours back.
		if (!stage_output(s, nout) || !drain(g.nd_ctx, g.nd_done, "NeurD (unpack)")) {
			return false;
		}

		*out_resource = static_cast<ID3D11Resource *>(s->out_ours);
		*out_w = s->out_w;
		*out_h = s->out_h;
		*out_format = (uint32_t)s->out_fmt;
	} catch (...) {
		// std::vector / std::mutex can throw; nothing may escape into the C runtime.
		LIFT_WARN_ONCE("Leia lift: exception in convert — frame dropped");
		return false;
	}

	QueryPerformanceCounter(&q1);
	const uint64_t ns = (uint64_t)((double)(q1.QuadPart - q0.QuadPart) * 1e9 / (double)qf.QuadPart);
	const uint64_t prev = g.latency_ns.load();
	g.latency_ns.store(prev == 0 ? ns : (prev * 7 + ns) / 8);
	return true;
}
