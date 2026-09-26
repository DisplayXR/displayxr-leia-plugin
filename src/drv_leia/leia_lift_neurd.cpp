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
 *    NeurD is never de-initialised once up: DP instances come and go on focus
 *    changes, and re-init re-runs licensing + model load. The ref-count only
 *    decides when to shrink NeurD's memory pool.
 *
 *  - Device bridge (the LeiaMeet DxStereoConverter pattern). NeurD runs on ITS
 *    OWN D3D11 device (NeurD_get_dx_device), possibly on a different adapter
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
//! NeurD's MAX_STREAMS.
constexpr uint32_t kMaxStreams = 32;
//! N views go side by side in ONE row (runtime contract), so the output is
//! N x inference-width wide; 8 x 2560 (1440p) still fits D3D11's 16384 limit.
constexpr uint32_t kMaxViews = 8;
constexpr uint32_t kMaxTexDim = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
//! Retry period for a licence activation that failed on the network.
constexpr uint64_t kActivationRetryMs = 15000;
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
 * Env knobs (read once per DP handle, at leia_lift_neurd_create).
 *
 */

enum backend_choice
{
	BACKEND_AUTO = -1,
	BACKEND_CUDA = NEURD_BACKEND_CUDA,
	BACKEND_DIRECTML = NEURD_BACKEND_DIRECTML,
	BACKEND_OPENVINO = NEURD_BACKEND_OPENVINO,
};

struct knobs
{
	bool enabled;         //!< DXR_LEIA_LIFT (default on)
	int backend;          //!< DXR_LEIA_LIFT_BACKEND (default directml)
	int32_t autoscaling;  //!< DXR_LEIA_LIFT_SCALE (default 720p)
	bool scale_forced;    //!< DXR_LEIA_LIFT_SCALE was set: overrides the stream's input_scale
	float view_gain;      //!< DXR_LEIA_LIFT_VIEW_GAIN (default 1.0)
	float conv_gain;      //!< DXR_LEIA_LIFT_CONV_GAIN (default 0.4): relative convergence -> NeurD units
};

bool
env_ieq(const char *a, const char *b)
{
	return _stricmp(a, b) == 0;
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

	const char *e = std::getenv("DXR_LEIA_LIFT");
	if (e != nullptr && (e[0] == '0' || env_ieq(e, "off") || env_ieq(e, "false"))) {
		k.enabled = false;
	}

	e = std::getenv("DXR_LEIA_LIFT_BACKEND");
	if (e != nullptr && e[0] != '\0') {
		if (env_ieq(e, "auto")) {
			k.backend = BACKEND_AUTO;
		} else if (env_ieq(e, "cuda")) {
			k.backend = BACKEND_CUDA;
		} else if (env_ieq(e, "directml") || env_ieq(e, "dml")) {
			k.backend = BACKEND_DIRECTML;
		} else if (env_ieq(e, "openvino")) {
			k.backend = BACKEND_OPENVINO;
		} else {
			U_LOG_W("Leia lift: DXR_LEIA_LIFT_BACKEND='%s' not recognised (auto|directml|cuda|openvino) — "
			        "using directml",
			        e);
		}
	}

	e = std::getenv("DXR_LEIA_LIFT_SCALE");
	if (e != nullptr && e[0] != '\0') {
		k.scale_forced = true;
		if (env_ieq(e, "none") || env_ieq(e, "native") || env_ieq(e, "0")) {
			k.autoscaling = NEURD_INPUT_AUTOSCALING_NONE;
		} else if (env_ieq(e, "720") || env_ieq(e, "720p")) {
			k.autoscaling = NEURD_INPUT_AUTOSCALING_720P;
		} else if (env_ieq(e, "1080") || env_ieq(e, "1080p")) {
			k.autoscaling = NEURD_INPUT_AUTOSCALING_1080P;
		} else if (env_ieq(e, "1440") || env_ieq(e, "1440p")) {
			k.autoscaling = NEURD_INPUT_AUTOSCALING_1440P;
		} else {
			U_LOG_W("Leia lift: DXR_LEIA_LIFT_SCALE='%s' not recognised (720|1080|1440|none) — using 720", e);
		}
	}

	e = std::getenv("DXR_LEIA_LIFT_VIEW_GAIN");
	if (e != nullptr && e[0] != '\0') {
		float g = (float)std::atof(e);
		if (std::isfinite(g) && g >= 0.0f && g <= 10.0f) {
			k.view_gain = g;
		} else {
			U_LOG_W("Leia lift: DXR_LEIA_LIFT_VIEW_GAIN='%s' out of range [0,10] — using 1.0", e);
		}
	}
	e = std::getenv("DXR_LEIA_LIFT_CONV_GAIN");
	if (e != nullptr && e[0] != '\0') {
		float cg = (float)std::atof(e);
		if (std::isfinite(cg) && cg >= -2.0f && cg <= 2.0f) {
			k.conv_gain = cg;
		} else {
			U_LOG_W("Leia lift: DXR_LEIA_LIFT_CONV_GAIN='%s' out of range [-2,2] — using 0.4", e);
		}
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
const char *kUnpackR8Cs = R"(
cbuffer P : register(b0) { uint W; uint H; uint StrideBytes; uint Pad; };
RWByteAddressBuffer Src : register(u0);
RWTexture2D<unorm float> Dst : register(u1);
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= W || id.y >= H) return;
	Dst[id.xy] = (float)(Src.Load(id.y * StrideBytes + id.x * 4) & 0xFF) / 255.0;
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
	G_ABSENT,       //!< No NeurD.dll found — permanent for the process.
	G_LOADING,      //!< Background worker is loading / initialising.
	G_RETRY_WAIT,   //!< Licence activation hit the network; retry after next_retry_ms.
	G_READY,        //!< Up; converts allowed.
	G_FAILED,       //!< Permanent failure (bad licence, no DX device, ABI...).
};

struct global
{
	std::mutex mtx; //!< Serialises every NeurD call + NeurD-context use + the fields below.

	std::atomic<uint32_t> state{G_UNPROBED};
	std::atomic<bool> worker_running{false};
	std::atomic<uint64_t> next_retry_ms{0};
	std::atomic<uint64_t> latency_ns{0}; //!< EMA of full convert wall time; 0 = none yet.
	std::atomic<uint32_t> streams_live{0};

	int requested_backend = BACKEND_DIRECTML; //!< First acquirer's choice (NeurD's forced backend is sticky).
	int32_t default_autoscaling = NEURD_INPUT_AUTOSCALING_720P;

	HMODULE lib = nullptr;
	struct NeurD const *nd = nullptr;
	enum NeurD_backend backend = NEURD_BACKEND_DIRECTML;
	char backend_name[32] = {};

	ID3D11Device *nd_dev = nullptr; //!< NeurD-owned: never Released.
	ID3D11DeviceContext *nd_ctx = nullptr;
	ID3D11Query *nd_done = nullptr;
	ID3D11ComputeShader *cs_pack = nullptr;
	ID3D11ComputeShader *cs_unpack = nullptr;
	ID3D11ComputeShader *cs_unpack_r8 = nullptr;
	ID3D11Buffer *cb = nullptr;
	LUID nd_luid = {};

	int refcount = 0;
	uint64_t next_stream_id = 1;
	bool interactive_unavailable = false;

	//! Last-applied global NeurD properties (avoid a worker round-trip per prop per frame).
	bool props_valid = false;
	int32_t p_out_type = -1, p_tiles_w = -1, p_tiles_h = -1, p_inpaint = -1, p_autoscale = -1, p_autoconv = -1;
	float p_conv = -1.0f, p_gain = -1.0f;
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

//! Build the NeurD-device side of the bridge. Called with g.mtx held.
bool
setup_nd_device_locked()
{
	g.nd_dev = static_cast<ID3D11Device *>(NeurD_get_dx_device(g.nd));
	if (g.nd_dev == nullptr) {
		U_LOG_W("Leia lift: NeurD backend '%s' exposes no D3D11 device — the D3D11 lift path needs the "
		        "DirectML backend (DXR_LEIA_LIFT_BACKEND=directml). Lift unavailable.",
		        g.backend_name);
		return false;
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
		g.nd_dev = nullptr;
		return false;
	}
	if (device_luid(g.nd_dev, &g.nd_luid)) {
		U_LOG_W("Leia lift: NeurD D3D11 device on adapter LUID %08lx:%08lx (FL 0x%x)",
		        (unsigned long)g.nd_luid.HighPart, (unsigned long)g.nd_luid.LowPart,
		        (unsigned)g.nd_dev->GetFeatureLevel());
	}
	return true;
}

/*!
 * Background worker: load (once) + init (retryable). Runs detached; publishes
 * the outcome through g.state.
 */
void
activation_worker()
{
	uint32_t next = G_FAILED;
	{
		// Load happens once per process. NOT under g.mtx for the slow part —
		// but nothing else touches g.lib / g.nd until state says READY.
		if (g.nd == nullptr) {
			const char *where = "?";
			HMODULE lib = neurd_load_library(&where);
			if (lib == nullptr) {
				U_LOG_W("Leia lift: NeurD.dll present but LoadLibrary failed (err %lu) — lift unavailable",
				        (unsigned long)GetLastError());
				g.state.store(G_FAILED);
				g.worker_running.store(false);
				return;
			}
			auto load = reinterpret_cast<PFN_NeurD_load>(reinterpret_cast<void *>(GetProcAddress(lib, "NeurD_load")));
			struct NeurD_load_request req = {NEURD_VERSION};
			struct NeurD const *nd = load != nullptr ? load(&req) : nullptr;
			if (nd == nullptr) {
				U_LOG_W("Leia lift: NeurD.dll (%s) has no usable NeurD_load — lift unavailable", where);
				// Deliberately no FreeLibrary: NeurD may have spun threads.
				g.state.store(G_FAILED);
				g.worker_running.store(false);
				return;
			}
			U_LOG_W("Leia lift: loaded NeurD %u.%u.%u from %s (plug-in built against %u.%u.%u)",
			        NEURD_GET_VERSION_MAJOR(nd->version), NEURD_GET_VERSION_MINOR(nd->version),
			        NEURD_GET_VERSION_PATCH(nd->version), where,
			        NEURD_GET_VERSION_MAJOR(NEURD_VERSION),
			        NEURD_GET_VERSION_MINOR(NEURD_VERSION),
			        NEURD_GET_VERSION_PATCH(NEURD_VERSION));
			if (NEURD_GET_VERSION_MAJOR(nd->version) != 0 ||
			    !LEIA_NEURD_HAS(nd, convert_stream_dx) || !LEIA_NEURD_HAS(nd, get_dx_device) ||
			    !LEIA_NEURD_HAS(nd, create_stream) || !LEIA_NEURD_HAS(nd, set_prop_1i) ||
			    !LEIA_NEURD_HAS(nd, set_prop_1f)) {
				U_LOG_W("Leia lift: NeurD runtime too old/new for the D3D11 stream path (need 0.4.3+, "
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
		// backend across deinit, so this is effectively set once per process.
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
			opts.video_model_id = NEURD_MODEL_VIDEO_RELATIVE_FAST;
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
		g.interactive_unavailable = !LEIA_NEURD_HAS(nd, convert_stream_dx_interactive);
		g.props_valid = false;
		next = setup_nd_device_locked() ? (uint32_t)G_READY : (uint32_t)G_FAILED;
		if (next == G_READY) {
			U_LOG_W("Leia lift: NeurD READY — backend %s, interactive viewpoints %s", g.backend_name,
			        g.interactive_unavailable ? "UNAVAILABLE (NeurD < 0.4.5)" : "available");
		}
	}
	g.state.store(next);
	g.worker_running.store(false);
}

/*!
 * Advance the process state machine without blocking: probe presence on first
 * touch, and (re)start the activation worker when due.
 */
void
kick(int requested_backend, int32_t default_autoscaling)
{
	uint32_t s = g.state.load();
	if (s == G_UNPROBED) {
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
		return; // ABSENT / LOADING / READY / FAILED: nothing to do.
	}

	bool expect_idle = false;
	if (!g.worker_running.compare_exchange_strong(expect_idle, true)) {
		return;
	}
	if (s == G_UNPROBED) {
		g.requested_backend = requested_backend;
		g.default_autoscaling = default_autoscaling;
	}
	g.state.store(G_LOADING);
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
	case G_READY: return LEIA_LIFT_STATE_READY;
	case G_UNPROBED:
	case G_LOADING:
	case G_RETRY_WAIT: return LEIA_LIFT_STATE_ACTIVATING;
	default: return LEIA_LIFT_STATE_UNAVAILABLE;
	}
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

extern "C" void
leia_lift_neurd_map_viewpoint(const float in_m[3], float gain, float out_n[3])
{
	out_n[0] = clampf(gain * in_m[0] / kIpdRefM, -kViewpointClamp, kViewpointClamp);
	out_n[1] = clampf(gain * in_m[1] / kIpdRefM, -kViewpointClamp, kViewpointClamp);
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
	{
		std::lock_guard<std::mutex> lock(g.mtx);
		for (lift_stream *s : l->streams) {
			release_stream_locked(s);
			delete s;
			g.streams_live.fetch_sub(1);
		}
		l->streams.clear();
		if (l->acquired) {
			l->acquired = false;
			if (--g.refcount == 0 && g.state.load() == G_READY && LEIA_NEURD_HAS(g.nd, shrink_memory_pool)) {
				// Last user gone: give NeurD's pools back but stay initialised
				// (re-init = licence + model load; DPs are recreated on focus change).
				NeurD_shrink_memory_pool(g.nd);
				g.props_valid = false;
			}
		}
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
	kick(l->k.backend, l->k.autoscaling);

	uint32_t s = g.state.load();
	out->state = public_state(s);
	if (out->state == LEIA_LIFT_STATE_UNAVAILABLE) {
		return true;
	}
	out->modes = LEIA_LIFT_MODE_DEPTH | LEIA_LIFT_MODE_SBS | LEIA_LIFT_MODE_NVIEW;
	out->max_streams = kMaxStreams;
	out->max_views = kMaxViews;
	out->depth_semantics = 0; // relative: per-frame min-max normalised disparity
	uint64_t lat = g.latency_ns.load();
	if (s == G_READY) {
		// No lock: backend/backend_name are written once, before the seq_cst
		// store that publishes G_READY, and never again — caps must not wait
		// behind a convert holding g.mtx.
		snprintf(out->backend, sizeof(out->backend), "%s", g.backend_name);
		if (lat == 0) {
			lat = (g.backend == NEURD_BACKEND_DIRECTML) ? kPriorLatencyDirectMlNs
			      : (g.backend == NEURD_BACKEND_CUDA)   ? kPriorLatencyCudaNs
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
	if (desc->mode != LEIA_LIFT_MODE_DEPTH && desc->mode != LEIA_LIFT_MODE_SBS && desc->mode != LEIA_LIFT_MODE_NVIEW) {
		LIFT_WARN_ONCE("Leia lift: stream_create with unsupported mode %u", desc->mode);
		return false;
	}
	kick(l->k.backend, l->k.autoscaling);
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
	std::lock_guard<std::mutex> lock(g.mtx);
	for (auto it = l->streams.begin(); it != l->streams.end(); ++it) {
		if ((*it)->id == id) {
			release_stream_locked(*it);
			delete *it;
			l->streams.erase(it);
			g.streams_live.fetch_sub(1);
			return;
		}
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

	kick(l->k.backend, l->k.autoscaling);
	if (g.state.load() != G_READY) {
		return false; // ACTIVATING or UNAVAILABLE — caps says which.
	}

	LARGE_INTEGER qf, q0, q1;
	QueryPerformanceFrequency(&qf);
	QueryPerformanceCounter(&q0);

	try {
		std::lock_guard<std::mutex> lock(g.mtx);
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
		const float gain = (dp.strength >= 0.0f) ? clampf(dp.strength, 0.0f, 10.0f) : 1.0f;
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
		                prop_f(NEURD_PROP_GAIN_MULTIPLIER, gain, g.p_gain);
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
			st = NeurD_convert_stream_dx_interactive(nd, s->ns, &nin, vp.data(), (int)vp.size(),
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
