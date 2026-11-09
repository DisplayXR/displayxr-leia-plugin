// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  NeurD module of the conversion provider's POSIX arm (desktop Linux):
 *         discovery, load, licence / init, streams and the CPU convert over
 *         libNeurD.so. Implements the leia_lift_neurd.h lifecycle the provider
 *         uses (the Windows arm's ../drv_leia/leia_lift_neurd.cpp is D3D11
 *         throughout and is not compiled here) plus leia_conv_neurd_cpu.h.
 *
 *  - Dynamic load only. libNeurD.so is found without loading it (presence is a
 *    stat + VERSION.txt read), in this order:
 *      1. the provider's NeurDPath setting (the .so, or the directory holding it);
 *      2. each directory of LD_LIBRARY_PATH (where NeurD's own POSIX loader,
 *         dlopen("libNeurD.so"), would look first);
 *      3. NEURD_PATH (file or directory; the Windows loader's variable);
 *      4. the per-user install root ${XDG_DATA_HOME:-~/.local/share}/LeiaInc/NeurD-Runtime;
 *      5. the machine install root /opt/LeiaInc/NeurD-Runtime.
 *    The install's VERSION.txt (written by NeurD's own install rules next to
 *    the library) is the pre-load version, the VERSIONINFO twin of Windows.
 *
 *  - One NeurD per process (NeurD's rule), loaded and initialised on the
 *    runtime's thread by warm() (manual activation; the provider never runs
 *    an activation thread). Initialised multithreaded: NeurD then runs every
 *    call on its own worker, which owns its EGL / OpenGL context, so the
 *    runtime may call from any thread. Never de-initialised, never dlclose'd
 *    (NeurD spins threads).
 *
 *  - The CPU convert: packed BGR8 in, BGR8 side-by-side views out, owned by
 *    NeurD until the next convert. NeurD's Linux backends (OpenVINO; CUDA when
 *    built with TensorRT) run inference there; OpenVINO picks GPU / NPU / CPU
 *    itself (NEURD_OV_DEVICE in the hosting process's environment overrides).
 *    With NeurD_convert_stream_cpu_ex (NeurD >= 0.5.0) the views follow the
 *    viewer exactly as the Windows module's NeurD_convert_stream_dx_ex does:
 *    the same metric off-axis camera built by the same helper
 *    (leia_lift_oa_build) from the runtime's rect-relative viewpoints, the same
 *    dimensionless fallback (leia_lift_vp_map), and the float depth of the
 *    same inference (OpenVINO). Older NeurD: NeurD_convert_stream_cpu, its
 *    fixed pair scaled to the eyes' separation, no depth, no look-around.
 *
 *  - Never throws; errors return false with one WARN per site.
 *
 * @ingroup conv_leia
 */

#include "leia_conv_neurd_cpu.h"
#include "leia_lift_depth.h"
#include "leia_lift_viewpoint.h"

#include <NeurD.h>

#include "util/u_logging.h"

#include <dlfcn.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#define LEIA_NEURD_HAS(nd, name) ((nd) != nullptr && (nd)->version >= NEURD_##name##_INTRODUCED_IN && (nd)->name != nullptr)

namespace {

/*
 *
 * Constants (the Windows module's values where the knob exists there too).
 *
 */

constexpr float kDefaultDepthGain = 2.0f;
constexpr int32_t kDefaultDilate = 2;
constexpr float kDefaultConvGain = 0.4f;
constexpr float kDefaultViewGain = 1.0f;
constexpr float kDefaultYGain = 1.0f;
constexpr float kDefaultZGain = 0.5f;
constexpr float kDefaultReliefM = 0.08f;
constexpr float kDefaultMetricReliefMaxM = 0.3f;
constexpr uint64_t kDefaultMinNeurDVersion = NEURD_MAKE_VERSION(0, 4, 6);
//! First NeurD with per-stream properties (NeurD_set_stream_prop_*).
constexpr uint64_t kStreamPropsVersion = NEURD_MAKE_VERSION(0, 4, 9);
constexpr uint64_t kActivationRetryMs = 15000;
constexpr const char *kLibName = "libNeurD.so";
constexpr const char *kMachineRoot = "/opt/LeiaInc/NeurD-Runtime";
constexpr const char *kUserSubdir = "LeiaInc/NeurD-Runtime";

#define LIFT_WARN_ONCE(...)                                                                                    \
	do {                                                                                                   \
		static std::atomic<bool> warned_{false};                                                       \
		if (!warned_.exchange(true)) {                                                                 \
			U_LOG_W(__VA_ARGS__);                                                                  \
		}                                                                                              \
	} while (0)

uint64_t
now_ms()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

uint64_t
now_ns()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
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
 * Settings (the provider's host->get_setting only, spec §2.4).
 *
 */

leia_lift_neurd_setting_fn g_setting_reader = nullptr;

bool
setting_lookup(const char *key, char *out, size_t cap)
{
	if (cap == 0) {
		return false;
	}
	out[0] = '\0';
	if (g_setting_reader == nullptr || !g_setting_reader(key, out, (uint32_t)cap)) {
		out[0] = '\0';
		return false;
	}
	out[cap - 1] = '\0';
	return out[0] != '\0';
}

constexpr int kBackendAuto = -1;

struct knobs
{
	bool enabled = true;
	int backend = kBackendAuto;
	//! 1080p, not the Windows module's 720p: NeurD's CPU path renders the views at
	//! the autoscaled size, and a smaller frame than the input costs a CPU resample
	//! back up to the target (measured: +45 ms per 1080p frame); inference runs at
	//! the model's own size either way.
	int32_t autoscaling = NEURD_INPUT_AUTOSCALING_1080P;
	float depth_gain = kDefaultDepthGain;
	int32_t dilate = kDefaultDilate;
	float conv_gain = kDefaultConvGain;
	uint64_t min_version = kDefaultMinNeurDVersion;
	int32_t video_model = NEURD_MODEL_VIDEO_RELATIVE_FAST;
	// The Windows module's look-around knobs (same names, ranges and defaults).
	float view_gain = kDefaultViewGain;
	float y_gain = kDefaultYGain;
	float z_gain = kDefaultZGain;
	bool offaxis = true;
	float relief_m = kDefaultReliefM;
	float metric_relief_max_m = kDefaultMetricReliefMaxM;
};

bool
parse_float_in(const char *v, float lo, float hi, float *out)
{
	char *end = nullptr;
	const float f = std::strtof(v, &end);
	if (end == v || *end != '\0' || !std::isfinite(f) || f < lo || f > hi) {
		return false;
	}
	*out = f;
	return true;
}

bool
parse_version(const char *v, uint64_t *out)
{
	unsigned a = 0, b = 0, c = 0;
	if (std::sscanf(v, "%u.%u.%u", &a, &b, &c) != 3) {
		return false;
	}
	*out = NEURD_MAKE_VERSION(a, b, c);
	return true;
}

knobs
read_knobs()
{
	knobs k;
	char v[128];
	if (setting_lookup("Enable", v, sizeof(v))) {
		k.enabled = !(strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "off") == 0);
	}
	if (setting_lookup("Backend", v, sizeof(v))) {
		if (strcasecmp(v, "auto") == 0) {
			k.backend = kBackendAuto;
		} else if (strcasecmp(v, "openvino") == 0) {
			k.backend = NEURD_BACKEND_OPENVINO;
		} else if (strcasecmp(v, "cuda") == 0) {
			k.backend = NEURD_BACKEND_CUDA;
		} else {
			U_LOG_W("Leia conversion: Backend='%s' not recognised (auto | openvino | cuda) — auto", v);
		}
	}
	if (setting_lookup("Scale", v, sizeof(v))) {
		if (strcasecmp(v, "none") == 0 || strcasecmp(v, "native") == 0 || strcmp(v, "0") == 0) {
			k.autoscaling = NEURD_INPUT_AUTOSCALING_NONE;
		} else if (strcasecmp(v, "720") == 0 || strcasecmp(v, "720p") == 0) {
			k.autoscaling = NEURD_INPUT_AUTOSCALING_720P;
		} else if (strcasecmp(v, "1080") == 0 || strcasecmp(v, "1080p") == 0) {
			k.autoscaling = NEURD_INPUT_AUTOSCALING_1080P;
		} else if (strcasecmp(v, "1440") == 0 || strcasecmp(v, "1440p") == 0) {
			k.autoscaling = NEURD_INPUT_AUTOSCALING_1440P;
		} else {
			U_LOG_W("Leia conversion: Scale='%s' not recognised (none | 720p | 1080p | 1440p) — 1080p", v);
		}
	}
	if (setting_lookup("DepthGain", v, sizeof(v)) && !parse_float_in(v, 0.0f, 10.0f, &k.depth_gain)) {
		U_LOG_W("Leia conversion: DepthGain='%s' not in [0, 10] — %.1f", v, (double)kDefaultDepthGain);
		k.depth_gain = kDefaultDepthGain;
	}
	if (setting_lookup("Dilate", v, sizeof(v))) {
		char *end = nullptr;
		const long d = std::strtol(v, &end, 10);
		if (end != v && *end == '\0' && d >= 0 && d <= 16) {
			k.dilate = (int32_t)d;
		} else {
			U_LOG_W("Leia conversion: Dilate='%s' not in [0, 16] — %d", v, (int)kDefaultDilate);
		}
	}
	if (setting_lookup("ConvGain", v, sizeof(v)) && !parse_float_in(v, -2.0f, 2.0f, &k.conv_gain)) {
		U_LOG_W("Leia conversion: ConvGain='%s' not in [-2, 2] — %.1f", v, (double)kDefaultConvGain);
		k.conv_gain = kDefaultConvGain;
	}
	if (setting_lookup("MinVersion", v, sizeof(v)) && !parse_version(v, &k.min_version)) {
		U_LOG_W("Leia conversion: MinVersion='%s' is not x.y.z — 0.4.6", v);
		k.min_version = kDefaultMinNeurDVersion;
	}
	if (setting_lookup("VideoModel", v, sizeof(v))) {
		k.video_model = strcasecmp(v, "metric") == 0 ? NEURD_MODEL_VIDEO_METRIC_QUALITY : NEURD_MODEL_VIDEO_RELATIVE_FAST;
	}
	struct fknob
	{
		const char *key;
		float lo, hi, def;
		float *dst;
	};
	const fknob fk[] = {
	    {"ViewGain", 0.0f, 10.0f, kDefaultViewGain, &k.view_gain},
	    {"YGain", 0.0f, 10.0f, kDefaultYGain, &k.y_gain},
	    {"ZGain", -2.0f, 2.0f, kDefaultZGain, &k.z_gain},
	    {"ReliefDepthM", 0.001f, 5.0f, kDefaultReliefM, &k.relief_m},
	    {"MetricReliefMaxM", 0.001f, 50.0f, kDefaultMetricReliefMaxM, &k.metric_relief_max_m},
	};
	for (const fknob &f : fk) {
		if (setting_lookup(f.key, v, sizeof(v)) && !parse_float_in(v, f.lo, f.hi, f.dst)) {
			U_LOG_W("Leia conversion: %s='%s' not in [%g, %g] — %g", f.key, v, (double)f.lo, (double)f.hi,
			        (double)f.def);
			*f.dst = f.def;
		}
	}
	if (setting_lookup("OffAxis", v, sizeof(v))) {
		k.offaxis = !(strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "off") == 0);
	}
	return k;
}

/*
 *
 * Discovery (no load).
 *
 */

bool
is_file(const std::string &p)
{
	struct stat st;
	return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool
is_dir(const std::string &p)
{
	struct stat st;
	return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

//! @p p names the library itself or a directory holding it; the library's path or "".
std::string
lib_at(const std::string &p)
{
	if (is_file(p)) {
		return p;
	}
	if (is_dir(p)) {
		const std::string f = p + "/" + kLibName;
		return is_file(f) ? f : std::string();
	}
	return std::string();
}

std::string
neurd_resolve(const char **where)
{
	char v[PATH_MAX];
	if (setting_lookup("NeurDPath", v, sizeof(v))) {
		*where = "NeurDPath setting";
		return lib_at(v); // pinned: nothing else is looked at
	}
	if (const char *ld = std::getenv("LD_LIBRARY_PATH"); ld != nullptr && ld[0] != '\0') {
		std::string all(ld);
		size_t at = 0;
		while (at <= all.size()) {
			const size_t colon = all.find(':', at);
			const std::string dir = all.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
			const std::string f = dir.empty() ? std::string() : lib_at(dir);
			if (!f.empty()) {
				*where = "LD_LIBRARY_PATH";
				return f;
			}
			if (colon == std::string::npos) {
				break;
			}
			at = colon + 1;
		}
	}
	if (const char *np = std::getenv("NEURD_PATH"); np != nullptr && np[0] != '\0') {
		const std::string f = lib_at(np);
		if (!f.empty()) {
			*where = "NEURD_PATH";
			return f;
		}
	}
	{
		std::string root;
		const char *xdg = std::getenv("XDG_DATA_HOME");
		if (xdg != nullptr && xdg[0] == '/') {
			root = xdg;
		} else if (const char *home = std::getenv("HOME"); home != nullptr && home[0] == '/') {
			root = std::string(home) + "/.local/share";
		}
		if (!root.empty()) {
			const std::string f = lib_at(root + "/" + kUserSubdir);
			if (!f.empty()) {
				*where = "per-user install";
				return f;
			}
		}
	}
	{
		const std::string f = lib_at(kMachineRoot);
		if (!f.empty()) {
			*where = "machine install";
			return f;
		}
	}
	*where = "not found";
	return std::string();
}

//! VERSION.txt next to the library: "x.y.z[-pre][+build.hash]".
bool
read_install_version(const std::string &lib, uint32_t *maj, uint32_t *min, uint32_t *pat, char *full, size_t cap)
{
	const size_t slash = lib.rfind('/');
	const std::string path = (slash == std::string::npos ? std::string(".") : lib.substr(0, slash)) + "/VERSION.txt";
	FILE *f = std::fopen(path.c_str(), "r");
	if (f == nullptr) {
		return false;
	}
	char line[96] = {};
	const bool got = std::fgets(line, sizeof(line), f) != nullptr;
	std::fclose(f);
	unsigned a = 0, b = 0, c = 0;
	if (!got || std::sscanf(line, "%u.%u.%u", &a, &b, &c) != 3) {
		return false;
	}
	*maj = a;
	*min = b;
	*pat = c;
	line[strcspn(line, "\r\n")] = '\0';
	snprintf(full, cap, "%s", line);
	return true;
}

/*
 *
 * Process-wide NeurD state.
 *
 */

struct global
{
	std::mutex mtx; //!< everything below
	void *lib = nullptr;
	struct NeurD const *nd = nullptr;
	uint32_t module_status = LEIA_LIFT_MODULE_UNKNOWN;
	bool network_retry = false;
	bool recheckable = false;
	char version[48] = {};
	char detail[96] = {};
	char backend[32] = {};
	bool stream_props = false;
	//! NeurD_convert_stream_cpu_ex is present (NeurD >= 0.5.0 and a provider built
	//! against its header): look-around, the off-axis camera, float depth.
	bool cpu_ex = false;
	//! cpu_ex's float depth: the OpenVINO backend reads its raw output back.
	bool float_depth = false;
	//! The off-axis camera worked (or was not tried yet): false once NeurD said its
	//! process has no GL view renderer (then the dimensionless camera).
	bool offaxis_ok = true;
	uint64_t latency_ns = 0;
	uint64_t next_retry_ms = 0;
	knobs k;
	std::string loaded_path;
	// Last process-wide values written (so a convert re-sets only what moved).
	bool p_valid = false;
	int32_t p_autoscale = -1;
	int32_t p_inpaint = -1, p_autoconv = -1, p_dilate = -1;
	float p_gain = NAN, p_conv = NAN;
};

global g;

//! NeurD's licence line. Its own lock: NeurD logs from its worker thread while
//! activate() holds g.mtx and waits for NeurD_init on that same worker.
std::mutex g_licence_mtx;
char g_licence[64] = {};

void
neurd_log_cb(const char *msg, int size)
{
	if (msg == nullptr || size <= 0) {
		return;
	}
	while (size > 0 && (msg[size - 1] == '\n' || msg[size - 1] == '\r')) {
		size--;
	}
	const std::string line(msg, (size_t)size);
	const size_t at = line.find("License:");
	if (at != std::string::npos) {
		std::lock_guard<std::mutex> lock(g_licence_mtx);
		snprintf(g_licence, sizeof(g_licence), "%s", line.c_str() + at);
	}
	U_LOG_I("NeurD: %.*s", size, msg);
}

void
set_failed(uint32_t status, bool recheckable, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

void
set_failed(uint32_t status, bool recheckable, const char *fmt, ...)
{
	// Caller holds g.mtx.
	g.module_status = status;
	g.recheckable = recheckable;
	g.network_retry = false;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g.detail, sizeof(g.detail), fmt, ap);
	va_end(ap);
}

} // namespace

/*
 *
 * The leia_lift_neurd.h lifecycle (the subset the conversion provider uses).
 *
 */

struct lift_stream
{
	uint64_t id;
	uint32_t mode;
	struct NeurD_stream *ns;
	bool sp_off;      //!< per-stream props refused once: process-wide from then on
	bool sp_conv_set; //!< CONVERGENCE overridden once (NeurD keeps a per-stream auto result after that)
	int32_t sp_inpaint = -1, sp_autoconv = -1, sp_dilate = -1;
	float sp_gain = NAN, sp_conv = NAN;
	// The Windows module's per-stream camera state.
	bool aux_depth = false;     //!< the runtime asked for the depth of the same inference
	bool retain = false;        //!< convert with want_depth (aux depth, or the metric off-axis scale)
	float auto_zero_h = 0.0f;   //!< NeurD's own auto-convergence (last convert) as h0; 0 = unknown
	float metric_scale = 0.0f;  //!< metric model's inverse_depth_scale once seen; 0 = relative / unknown
	uint32_t cam_logged = ~0u;  //!< camera path last logged
	uint32_t vp_logged = ~0u;   //!< viewpoint contract last logged
	bool depth_valid = false;   //!< depth_out describes the last convert's depth
	struct leia_lift_neurd_depth depth_out = {};
};

struct leia_lift_neurd
{
	std::vector<lift_stream> streams;
	uint64_t next_id = 0;
};

extern "C" void
leia_lift_neurd_set_setting_reader(leia_lift_neurd_setting_fn fn)
{
	g_setting_reader = fn;
}

extern "C" void
leia_lift_neurd_set_manual_activation(bool manual)
{
	(void)manual; // the POSIX arm only ever activates manually (warm)
}

extern "C" struct leia_lift_neurd *
leia_lift_neurd_create(void)
{
	return new (std::nothrow) leia_lift_neurd();
}

extern "C" void
leia_lift_neurd_destroy(struct leia_lift_neurd **lift)
{
	if (lift == nullptr || *lift == nullptr) {
		return;
	}
	struct leia_lift_neurd *l = *lift;
	while (!l->streams.empty()) {
		leia_lift_neurd_stream_destroy(l, l->streams.back().id);
	}
	delete l;
	*lift = nullptr;
}

extern "C" bool
leia_lift_neurd_probe_presence(struct leia_lift_neurd *lift, struct leia_lift_neurd_presence *out)
{
	(void)lift;
	if (out == nullptr) {
		return false;
	}
	*out = {};
	const knobs k = read_knobs();
	out->enabled = k.enabled;
	out->min_major = (uint32_t)NEURD_GET_VERSION_MAJOR(k.min_version);
	out->min_minor = (uint32_t)NEURD_GET_VERSION_MINOR(k.min_version);
	out->min_patch = (uint32_t)NEURD_GET_VERSION_PATCH(k.min_version);
	if (!k.enabled) {
		return true;
	}
	const char *where = "?";
	const std::string path = neurd_resolve(&where);
	snprintf(out->where, sizeof(out->where), "%s", where);
	if (path.empty()) {
		return true;
	}
	out->found = true;
	snprintf(out->path, sizeof(out->path), "%s", path.c_str());
	char full[48] = {};
	if (read_install_version(path, &out->ver_major, &out->ver_minor, &out->ver_patch, full, sizeof(full))) {
		out->have_version = true;
		const uint64_t v = NEURD_MAKE_VERSION(out->ver_major, out->ver_minor, out->ver_patch);
		out->too_old = v < k.min_version;
		// NeurD owns AUTO on the cpu_ex path only (see leia_lift_neurd_get_status).
#ifdef DXR_LEIA_NEURD_HAS_CPU_EX
		out->own_autoconv = v >= NEURD_convert_stream_cpu_ex_INTRODUCED_IN;
#else
		out->own_autoconv = false;
#endif
	}
	return true;
}

extern "C" uint32_t
leia_lift_neurd_activate(struct leia_lift_neurd *lift)
{
	(void)lift;
	std::lock_guard<std::mutex> lock(g.mtx);
	if (g.module_status == LEIA_LIFT_MODULE_READY) {
		return g.module_status;
	}
	if (g.nd == nullptr && g.lib != nullptr) {
		return g.module_status; // a refused library stays mapped: needs a process restart
	}
	if (g.network_retry && now_ms() < g.next_retry_ms) {
		return g.module_status;
	}
	g.k = read_knobs();
	if (!g.k.enabled) {
		g.module_status = LEIA_LIFT_MODULE_DISABLED;
		snprintf(g.detail, sizeof(g.detail), "turned off (Enable=0)");
		return g.module_status;
	}

	if (g.nd == nullptr) {
		const char *where = "?";
		const std::string path = neurd_resolve(&where);
		if (path.empty()) {
			set_failed(LEIA_LIFT_MODULE_ABSENT, true, "NeurD is not installed (libNeurD.so not found)");
			U_LOG_W("Leia conversion: libNeurD.so not found (NeurDPath / LD_LIBRARY_PATH / NEURD_PATH / "
			        "~/.local/share/%s / %s)",
			        kUserSubdir, kMachineRoot);
			return g.module_status;
		}
		uint32_t fma = 0, fmi = 0, fpa = 0;
		char full[48] = {};
		if (read_install_version(path, &fma, &fmi, &fpa, full, sizeof(full)) &&
		    NEURD_MAKE_VERSION(fma, fmi, fpa) < g.k.min_version) {
			set_failed(LEIA_LIFT_MODULE_TOO_OLD, true, "NeurD %u.%u.%u is older than the minimum %u.%u.%u", fma,
			           fmi, fpa, (unsigned)NEURD_GET_VERSION_MAJOR(g.k.min_version),
			           (unsigned)NEURD_GET_VERSION_MINOR(g.k.min_version),
			           (unsigned)NEURD_GET_VERSION_PATCH(g.k.min_version));
			U_LOG_W("Leia conversion: %s", g.detail);
			return g.module_status;
		}
		void *lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (lib == nullptr) {
			const char *e = dlerror();
			set_failed(LEIA_LIFT_MODULE_LOAD_FAILED, true, "libNeurD.so found but failed to load");
			U_LOG_W("Leia conversion: libNeurD.so at %s (%s) failed to load: %s", path.c_str(), where,
			        e != nullptr ? e : "?");
			return g.module_status;
		}
		auto load = reinterpret_cast<PFN_NeurD_load>(dlsym(lib, "NeurD_load"));
		struct NeurD_load_request req = {NEURD_VERSION};
		struct NeurD const *nd = load != nullptr ? load(&req) : nullptr;
		g.lib = lib; // never dlclose'd from here on: NeurD may have spun threads
		if (nd == nullptr) {
			set_failed(LEIA_LIFT_MODULE_LOAD_FAILED, false, "libNeurD.so has no usable NeurD_load");
			U_LOG_W("Leia conversion: %s (%s)", g.detail, path.c_str());
			return g.module_status;
		}
		U_LOG_W("Leia conversion: loaded NeurD %u.%u.%u from %s (%s; provider built against %u.%u.%u)",
		        (unsigned)NEURD_GET_VERSION_MAJOR(nd->version), (unsigned)NEURD_GET_VERSION_MINOR(nd->version),
		        (unsigned)NEURD_GET_VERSION_PATCH(nd->version), path.c_str(), where,
		        (unsigned)NEURD_GET_VERSION_MAJOR(NEURD_VERSION), (unsigned)NEURD_GET_VERSION_MINOR(NEURD_VERSION),
		        (unsigned)NEURD_GET_VERSION_PATCH(NEURD_VERSION));
		snprintf(g.version, sizeof(g.version), "%u.%u.%u", (unsigned)NEURD_GET_VERSION_MAJOR(nd->version),
		         (unsigned)NEURD_GET_VERSION_MINOR(nd->version), (unsigned)NEURD_GET_VERSION_PATCH(nd->version));
		if (nd->version < g.k.min_version) {
			set_failed(LEIA_LIFT_MODULE_TOO_OLD, false, "NeurD %s is older than the minimum %u.%u.%u", g.version,
			           (unsigned)NEURD_GET_VERSION_MAJOR(g.k.min_version),
			           (unsigned)NEURD_GET_VERSION_MINOR(g.k.min_version),
			           (unsigned)NEURD_GET_VERSION_PATCH(g.k.min_version));
			U_LOG_W("Leia conversion: %s", g.detail);
			return g.module_status;
		}
		if (NEURD_GET_VERSION_MAJOR(nd->version) != 0 || !LEIA_NEURD_HAS(nd, create_stream) ||
		    !LEIA_NEURD_HAS(nd, convert_stream_cpu) || !LEIA_NEURD_HAS(nd, set_prop_1i) ||
		    !LEIA_NEURD_HAS(nd, set_prop_1f)) {
			set_failed(LEIA_LIFT_MODULE_TOO_OLD, false, "NeurD %s lacks the CPU stream API", g.version);
			U_LOG_W("Leia conversion: %s", g.detail);
			return g.module_status;
		}
		if (LEIA_NEURD_HAS(nd, set_logger_callback)) {
			NeurD_set_logger_callback(nd, neurd_log_cb);
		}
		g.nd = nd;
		g.loaded_path = path;
	}

	struct NeurD const *nd = g.nd;
	// Process-wide defaults that NeurD reads at init.
	NeurD_set_prop_1i(nd, NEURD_PROP_OUTPUT_TYPE, NEURD_OUTPUT_IMAGE_TYPE_SBS);
	NeurD_set_prop_1i(nd, NEURD_PROP_INPUT_AUTOSCALING, g.k.autoscaling);
	NeurD_set_prop_1i(nd, NEURD_PROP_INPAINT_TYPE, NEURD_INPAINT_TYPE_V1_STRETCH);
	if (g.k.backend != kBackendAuto && LEIA_NEURD_HAS(nd, set_backend)) {
		const enum NeurD_status bs = NeurD_set_backend(nd, (enum NeurD_backend)g.k.backend);
		if (bs != NEURD_SUCCESS) {
			U_LOG_W("Leia conversion: NeurD_set_backend(%s) -> %s", backend_str((enum NeurD_backend)g.k.backend),
			        status_str(bs));
		}
	}

	// The licence activation (licensed builds) can need the network: ACTIVATING
	// (network_retry) is reported until a later warm succeeds.
	g.module_status = LEIA_LIFT_MODULE_ACTIVATING;
	const uint64_t t0 = now_ms();
	enum NeurD_status st;
	if (LEIA_NEURD_HAS(nd, init_with_options)) {
		struct NeurD_init_options opts = {};
		opts.struct_size = (uint32_t)sizeof(opts);
		opts.photo_model_id = NEURD_MODEL_PHOTO_RELATIVE_QUALITY;
		opts.video_model_id = g.k.video_model;
		st = NeurD_init_with_options(nd, NEURD_TRUE, &opts);
	} else if (LEIA_NEURD_HAS(nd, init)) {
		st = NeurD_init(nd, NEURD_TRUE);
	} else {
		st = NEURD_UNAVAILABLE_OUTDATED_RUNTIME;
	}
	if (st == NEURD_LICENSE_NETWORK_ERROR || st == NEURD_NETWORK_ERROR) {
		g.network_retry = true;
		g.next_retry_ms = now_ms() + kActivationRetryMs;
		snprintf(g.detail, sizeof(g.detail), "NeurD licence activation needs the network");
		U_LOG_W("Leia conversion: NeurD licence activation needs the network (%s) — retry in %llus",
		        status_str(st), (unsigned long long)(kActivationRetryMs / 1000));
		return g.module_status;
	}
	if (st != NEURD_SUCCESS) {
		if (st == NEURD_INVALID_LICENSE) {
			set_failed(LEIA_LIFT_MODULE_UNLICENSED, false, "NeurD licence refused (INVALID_LICENSE)");
		} else {
			set_failed(LEIA_LIFT_MODULE_LOAD_FAILED, false, "NeurD init failed: %s", status_str(st));
		}
		U_LOG_W("Leia conversion: %s", g.detail);
		return g.module_status;
	}
	enum NeurD_backend be = g.k.backend == kBackendAuto ? NEURD_BACKEND_OPENVINO : (enum NeurD_backend)g.k.backend;
	if (LEIA_NEURD_HAS(nd, get_backend)) {
		NeurD_get_backend(nd, &be);
	}
	snprintf(g.backend, sizeof(g.backend), "neurd-%s", backend_str(be));
	g.stream_props = nd->version >= kStreamPropsVersion && LEIA_NEURD_HAS(nd, set_stream_prop_1f) &&
	                 LEIA_NEURD_HAS(nd, set_stream_prop_1i);
#ifdef DXR_LEIA_NEURD_HAS_CPU_EX
	g.cpu_ex = LEIA_NEURD_HAS(nd, convert_stream_cpu_ex);
#else
	g.cpu_ex = false;
#endif
	g.float_depth = g.cpu_ex && be == NEURD_BACKEND_OPENVINO;
	g.offaxis_ok = true;
	g.module_status = LEIA_LIFT_MODULE_READY;
	g.network_retry = false;
	g.recheckable = false;
	g.p_valid = false;
	snprintf(g.detail, sizeof(g.detail), "NeurD %s ready (%s, CPU transport)", g.version, g.backend);
	U_LOG_W("Leia conversion: NeurD READY in %llu ms — backend %s, per-stream props %s, depth_gain=%.2f "
	        "dilate=%d conv_gain=%.2f scale=%d view_gain=%.2f y_gain=%.2f offaxis=%d relief_m=%.3f [CPU transport: %s]",
	        (unsigned long long)(now_ms() - t0), g.backend, g.stream_props ? "yes" : "no (NeurD < 0.4.9)",
	        (double)g.k.depth_gain, (int)g.k.dilate, (double)g.k.conv_gain, (int)g.k.autoscaling,
	        (double)g.k.view_gain, (double)g.k.y_gain, g.k.offaxis ? 1 : 0, (double)g.k.relief_m,
	        g.cpu_ex ? (g.float_depth ? "convert_stream_cpu_ex, look-around + float depth"
	                                  : "convert_stream_cpu_ex, look-around, no float depth on this backend")
	                 : "views only, no look-around (NeurD < 0.5.0 or provider built without cpu_ex)");
	return g.module_status;
}

extern "C" void
leia_lift_neurd_get_status(struct leia_lift_neurd *lift, struct leia_lift_neurd_status *out)
{
	(void)lift;
	if (out == nullptr) {
		return;
	}
	*out = {};
	std::lock_guard<std::mutex> lock(g.mtx);
	out->module_status = g.module_status;
	out->network_retry = g.network_retry;
	out->recheckable = g.recheckable;
	snprintf(out->version, sizeof(out->version), "%s", g.version);
	snprintf(out->detail, sizeof(out->detail), "%s", g.detail);
	{
		std::lock_guard<std::mutex> ll(g_licence_mtx);
		snprintf(out->licence, sizeof(out->licence), "%s", g_licence);
	}
	if (g.module_status == LEIA_LIFT_MODULE_READY) {
		snprintf(out->backend, sizeof(out->backend), "%s", g.backend);
	}
	// NeurD_convert_stream_cpu_ex: the Windows module's contract (float depth
	// on OpenVINO, the off-axis camera, NeurD owns AUTO). The plain CPU convert
	// returns views only and the runtime keeps AUTO.
	const bool ready = g.module_status == LEIA_LIFT_MODULE_READY;
	out->float_depth = ready && g.float_depth;
	out->metric_depth = out->float_depth && g.k.video_model == NEURD_MODEL_VIDEO_METRIC_QUALITY;
	out->offaxis = ready && g.cpu_ex && g.k.offaxis && g.offaxis_ok;
	out->own_autoconv = ready && g.cpu_ex;
	out->latency_ns = g.latency_ns;
	out->nd_luid = 0;
}

extern "C" bool
leia_lift_neurd_stream_create(struct leia_lift_neurd *lift,
                              const struct leia_lift_neurd_stream_desc *desc,
                              uint64_t *out_id)
{
	if (lift == nullptr || desc == nullptr || out_id == nullptr || desc->mode != LEIA_LIFT_MODE_SBS) {
		return false; // the CPU path synthesises two views; no depth / N-view output
	}
	std::lock_guard<std::mutex> lock(g.mtx);
	if (g.module_status != LEIA_LIFT_MODULE_READY || g.nd == nullptr) {
		return false;
	}
	struct NeurD_stream *ns = nullptr;
	const enum NeurD_status st = NeurD_create_stream(g.nd, &ns);
	if (st != NEURD_SUCCESS || ns == nullptr) {
		LIFT_WARN_ONCE("Leia conversion: NeurD_create_stream -> %s", status_str(st));
		return false;
	}
	lift_stream s = {};
	s.id = ++lift->next_id;
	s.mode = desc->mode;
	s.ns = ns;
	s.sp_inpaint = s.sp_autoconv = s.sp_dilate = -1;
	s.sp_gain = s.sp_conv = NAN;
	s.aux_depth = (desc->aux_outputs & LEIA_LIFT_AUX_DEPTH) != 0;
	// As the Windows module: the float depth of each convert when the runtime asked
	// for it, or when the metric off-axis camera needs the model's scale.
	const bool metric_oa = g.k.video_model == NEURD_MODEL_VIDEO_METRIC_QUALITY && g.cpu_ex && g.k.offaxis;
	if (g.float_depth && (s.aux_depth || metric_oa)) {
		s.retain = true;
		U_LOG_W("Leia conversion: stream %llu float depth per convert (NeurD_convert_stream_cpu_ex, %s)",
		        (unsigned long long)s.id, s.aux_depth ? "aux depth requested" : "metric off-axis scale");
	} else if (s.aux_depth) {
		LIFT_WARN_ONCE("Leia conversion: aux depth requested but NeurD has no float depth here (needs >= 0.5.0 "
		               "convert_stream_cpu_ex on OpenVINO) — none will be returned");
	}
	lift->streams.push_back(s);
	*out_id = s.id;
	return true;
}

extern "C" void
leia_lift_neurd_stream_destroy(struct leia_lift_neurd *lift, uint64_t id)
{
	if (lift == nullptr) {
		return;
	}
	for (auto it = lift->streams.begin(); it != lift->streams.end(); ++it) {
		if (it->id == id) {
			std::lock_guard<std::mutex> lock(g.mtx);
			if (g.nd != nullptr && it->ns != nullptr) {
				NeurD_destroy_stream(g.nd, it->ns);
			}
			lift->streams.erase(it);
			return;
		}
	}
}

extern "C" void
leia_lift_neurd_stream_reset(struct leia_lift_neurd *lift, uint64_t id)
{
	// NeurD's CPU path keeps no temporal state per stream: nothing to reset.
	(void)lift;
	(void)id;
}

namespace {

bool
prop_i(enum NeurD_prop p, int32_t v, int32_t &last)
{
	if (g.p_valid && last == v) {
		return true;
	}
	const enum NeurD_status st = NeurD_set_prop_1i(g.nd, p, v);
	if (st != NEURD_SUCCESS) {
		LIFT_WARN_ONCE("Leia conversion: NeurD_set_prop_1i(%d, %d) -> %s", (int)p, (int)v, status_str(st));
		return false;
	}
	last = v;
	return true;
}

bool
prop_f(enum NeurD_prop p, float v, float &last)
{
	if (g.p_valid && last == v) {
		return true;
	}
	const enum NeurD_status st = NeurD_set_prop_1f(g.nd, p, v);
	if (st != NEURD_SUCCESS) {
		LIFT_WARN_ONCE("Leia conversion: NeurD_set_prop_1f(%d, %.3f) -> %s", (int)p, (double)v, status_str(st));
		return false;
	}
	last = v;
	return true;
}

bool
sprop_i(lift_stream &s, enum NeurD_prop p, int32_t v, int32_t &last)
{
	if (last == v) {
		return true;
	}
	const enum NeurD_status st = NeurD_set_stream_prop_1i(g.nd, s.ns, p, v);
	if (st != NEURD_SUCCESS) {
		LIFT_WARN_ONCE("Leia conversion: NeurD_set_stream_prop_1i(%d, %d) -> %s", (int)p, (int)v,
		               status_str(st));
		return false;
	}
	last = v;
	return true;
}

bool
sprop_f(lift_stream &s, enum NeurD_prop p, float v, float &last)
{
	if (last == v) {
		return true;
	}
	const enum NeurD_status st = NeurD_set_stream_prop_1f(g.nd, s.ns, p, v);
	if (st != NEURD_SUCCESS) {
		LIFT_WARN_ONCE("Leia conversion: NeurD_set_stream_prop_1f(%d, %.3f) -> %s", (int)p, (double)v,
		               status_str(st));
		return false;
	}
	last = v;
	return true;
}

} // namespace

namespace {

//! One WARN per camera-path change of a stream (lifecycle, never per frame), as
//! the Windows module's log_camera.
void
log_camera(lift_stream *s,
           const struct leia_lift_neurd_params &dp,
           bool use_oa,
           bool runtime_vp,
           bool dimless,
           const struct leia_lift_oa_screen &sc)
{
	const uint32_t src_bits = dp.has_app_rig ? (dp.viewpoint_source & 7u) : 0u;
	const uint32_t key = (use_oa ? 1u : 0u) | (dimless ? 2u : 0u) | (src_bits << 2) |
	                     ((s->metric_scale > 0.0f ? 1u : 0u) << 5) | (runtime_vp ? 1u << 6 : 0u);
	if (s->cam_logged == key) {
		return;
	}
	s->cam_logged = key;
	if (use_oa) {
		const bool rig = dp.has_app_rig && (dp.viewpoint_source == LEIA_LIFT_VIEWPOINTS_DISPLAY_RIG ||
		                                    dp.viewpoint_source == LEIA_LIFT_VIEWPOINTS_CAMERA_RIG);
		U_LOG_W(
		    "Leia conversion: stream %llu camera = OFF-AXIS (screen %.0fx%.0f mm, relief %.0f mm %s, screen %.0f mm "
		    "behind the nearest relief plane, nominal eye %.0f mm; viewpoints %s%s) [convert_stream_cpu_ex]",
		    (unsigned long long)s->id, (double)(sc.screen_width_m * 1000.0f),
		    (double)(sc.screen_height_m * 1000.0f), (double)(sc.depth_scale_m * 1000.0f),
		    s->metric_scale > 0.0f ? "metric (MetricReliefMaxM cap)" : "relative (ReliefDepthM)",
		    (double)(sc.convergence_depth_m * 1000.0f), (double)(sc.nominal_eye_z_m * 1000.0f),
		    rig ? "= the app's rig, as given" : "tracked/explicit x ViewGain/YGain",
		    runtime_vp ? "" : " — none passed: default pair at the nominal distance");
		return;
	}
	U_LOG_W("Leia conversion: stream %llu camera = %s (%s)", (unsigned long long)s->id,
	        dimless ? "LEGACY dimensionless [convert_stream_cpu_ex]" : "FIXED pair [convert_stream_cpu, no look-around]",
	        !g.cpu_ex        ? "NeurD has no convert_stream_cpu_ex (< 0.5.0)"
	        : !g.k.offaxis   ? "OffAxis=0"
	        : !g.offaxis_ok  ? "NeurD has no GL view renderer in this process"
	                         : "the runtime passed no rect-relative viewpoint policy / rect size");
}

} // namespace

extern "C" bool
leia_lift_neurd_convert_cpu(struct leia_lift_neurd *lift,
                            uint64_t id,
                            const struct leia_conv_bgr8_image *in,
                            const struct leia_lift_neurd_params *params,
                            const float *viewpoints_m,
                            uint32_t viewpoint_floats,
                            struct leia_conv_bgr8_image *out)
{
	if (lift == nullptr || in == nullptr || in->data == nullptr || params == nullptr || out == nullptr ||
	    in->width == 0 || in->height == 0 || in->stride != in->width * 3u) {
		return false; // NeurD's plain CPU convert reads a tight BGR8 array (it ignores the stride)
	}
	*out = {};
	lift_stream *s = nullptr;
	for (auto &it : lift->streams) {
		if (it.id == id) {
			s = &it;
		}
	}
	if (s == nullptr) {
		return false;
	}
	s->depth_valid = false;
	std::lock_guard<std::mutex> lock(g.mtx);
	if (g.module_status != LEIA_LIFT_MODULE_READY || g.nd == nullptr) {
		return false;
	}
	struct NeurD const *nd = g.nd;
	constexpr uint32_t views = 2;

	// ---- Parameters, exactly as the Windows module maps them.
	struct leia_lift_neurd_params dp = *params;
	if (dp.convergence_metric_m > 0.0f) {
		// The plane as a metric depth (the units of the metric aux depth this stream
		// returned): h0 = S / (255 d) is the 8-bit disparity NeurD puts at zero
		// parallax. No scale yet (first frames) = AUTO.
		if (s->metric_scale > 0.0f) {
			const float h0 = leia_lift_h_from_metric_depth(s->metric_scale, dp.convergence_metric_m);
			dp.convergence = h0 > 0.0f ? clampf(1.0f - h0, 0.0f, 1.0f) : -1.0f;
		} else {
			dp.convergence = -1.0f;
		}
	}
	const bool auto_conv = !(dp.convergence >= 0.0f);
	const float nd_conv = clampf(g.k.conv_gain * (clampf(dp.convergence, 0.0f, 1.0f) - 0.5f), -0.2f, 0.2f);
	float strength = dp.strength >= 0.0f && std::isfinite(dp.strength) ? dp.strength : 1.0f;
	const int32_t inpaint = dp.inpaint != 0 ? NEURD_INPAINT_TYPE_V1_BLUR : NEURD_INPAINT_TYPE_V1_STRETCH;

	// ---- Viewpoints (the Windows module's step 4): the runtime's, else none.
	const bool runtime_vp = viewpoints_m != nullptr && viewpoint_floats >= 3;
	std::vector<float> src_m; // metres, (x,y,z) per view
	if (runtime_vp && viewpoint_floats % 3 == 0 && viewpoint_floats / 3 == views) {
		src_m.assign(viewpoints_m, viewpoints_m + viewpoint_floats);
	}
	std::vector<float> vp; // dimensionless NeurD pattern, one triplet per view
	struct leia_lift_vp_policy pol = {};
	pol.active = dp.has_policy && runtime_vp;
	pol.baseline_m = dp.baseline_m;
	pol.axis_mode = dp.axis_mode;
	pol.max_offset_m = dp.max_offset_m;
	pol.ref_z_m = dp.ref_z_m;
	if (!src_m.empty()) {
		const struct leia_lift_vp_gains gains = {g.k.view_gain, g.k.y_gain, g.k.z_gain};
		for (uint32_t i = 0; i < views; i++) {
			float n[3];
			leia_lift_vp_map(&pol, &gains, &src_m[i * 3], n);
			vp.insert(vp.end(), n, n + 3);
		}
	}
	const uint32_t vp_key = pol.active ? (1u | (dp.axis_mode << 1)) : 0u;
	if (!vp.empty() && s->vp_logged != vp_key) {
		s->vp_logged = vp_key;
		U_LOG_W("Leia conversion: stream %llu viewpoints = %s (frame %s, axis %u, baseline %.1f mm, rect %.0fx%.0f "
		        "mm, ref z %.0f mm) view_gain=%.2f y_gain=%.2f",
		        (unsigned long long)s->id, pol.active ? "runtime policy" : "legacy mapping",
		        dp.viewpoint_frame == 1u ? "rect" : "panel", (unsigned)dp.axis_mode,
		        (double)(dp.baseline_m * 1000.0f), (double)(dp.rect_width_m * 1000.0f),
		        (double)(dp.rect_height_m * 1000.0f), (double)(dp.ref_z_m * 1000.0f), (double)g.k.view_gain,
		        (double)g.k.y_gain);
	}

	// ---- Metric off-axis camera (the Windows module's step 4b, same helper).
	bool use_oa = false;
	float oa_zero_h = -1.0f;
	struct leia_lift_oa_screen sc = {};
	std::vector<float> oa_eyes;
	if (g.cpu_ex && g.offaxis_ok && g.k.offaxis && dp.has_policy && dp.viewpoint_frame == 1u &&
	    dp.rect_width_m > 0.0f && dp.rect_height_m > 0.0f) {
		struct leia_lift_oa_in oi = {};
		oi.rect_width_m = dp.rect_width_m;
		oi.rect_height_m = dp.rect_height_m;
		oi.nominal_z_m = (dp.has_app_rig && dp.nominal_z_m > 0.0f) ? dp.nominal_z_m : dp.ref_z_m;
		oi.viewpoint_source = dp.has_app_rig ? dp.viewpoint_source : LEIA_LIFT_VIEWPOINTS_TRACKED;
		oi.view_gain = g.k.view_gain;
		oi.y_gain = g.k.y_gain;
		oi.baseline_m = dp.baseline_m;
		oi.strength = dp.strength;
		oi.convergence = dp.convergence; // < 0 = auto
		oi.auto_zero_h = s->auto_zero_h;
		oi.metric = s->metric_scale > 0.0f;
		oi.inverse_depth_scale = s->metric_scale;
		oi.relief_m = g.k.relief_m;
		oi.metric_relief_max_m = g.k.metric_relief_max_m;
		const uint32_t n_in = (runtime_vp && !src_m.empty()) ? (uint32_t)(src_m.size() / 3) : 0u;
		oa_eyes.resize((size_t)std::max(n_in, 2u) * 3);
		uint32_t n_out = 0;
		if (leia_lift_oa_build(&oi, n_in > 0 ? src_m.data() : nullptr, n_in, &sc, oa_eyes.data(), &n_out,
		                       &oa_zero_h)) {
			oa_eyes.resize((size_t)n_out * 3);
			use_oa = true;
		}
	}
	// cpu_ex runs whenever it has something the plain convert cannot do.
	const bool use_ex = g.cpu_ex && (use_oa || !vp.empty() || s->retain);

	if (!use_ex) {
		// Plain CPU convert: NeurD's fixed pair is x = ±0.5, one unit = the nominal
		// 63 mm baseline; its disparity scales with the gain, so the pair's
		// horizontal separation is honoured as a gain factor (no look-around).
		float sep = 0.0f;
		if (runtime_vp && viewpoint_floats >= 6) {
			sep = viewpoints_m[3] - viewpoints_m[0];
		} else if (dp.baseline_m > 0.0f) {
			sep = dp.baseline_m;
		}
		if (std::isfinite(sep) && sep > 0.0f) {
			strength *= std::min(sep / LEIA_LIFT_VP_REF_BASELINE_M, 10.0f);
		} else if (runtime_vp && viewpoint_floats >= 6 && std::isfinite(sep) && sep <= 0.0f) {
			strength = 0.0f; // coincident (or swapped) eyes: no parallax
		}
	}
	const float gain = clampf(strength * g.k.depth_gain, 0.0f, 10.0f);

	// OUTPUT_TYPE stays SBS (set before init; this arm never changes it).
	bool ok = prop_i(NEURD_PROP_INPUT_AUTOSCALING, g.k.autoscaling, g.p_autoscale);
	bool per_stream = false;
	if (ok && g.stream_props && !s->sp_off) {
		per_stream = sprop_i(*s, NEURD_PROP_INPAINT_TYPE, inpaint, s->sp_inpaint) &&
		             sprop_i(*s, NEURD_PROP_AUTO_CONVERGENCE, auto_conv ? 1 : 0, s->sp_autoconv) &&
		             sprop_f(*s, NEURD_PROP_GAIN_MULTIPLIER, gain, s->sp_gain) &&
		             sprop_i(*s, NEURD_PROP_DILATE_RADIO, g.k.dilate, s->sp_dilate);
		if (per_stream && (!auto_conv || !s->sp_conv_set)) {
			per_stream = sprop_f(*s, NEURD_PROP_CONVERGENCE, auto_conv ? 0.0f : nd_conv, s->sp_conv);
			s->sp_conv_set = per_stream;
		}
		if (!per_stream) {
			U_LOG_W("Leia conversion: stream %llu per-stream props refused — process-wide props from now on",
			        (unsigned long long)s->id);
			s->sp_off = true;
		}
	}
	if (ok && !per_stream) {
		ok = prop_i(NEURD_PROP_INPAINT_TYPE, inpaint, g.p_inpaint) &&
		     prop_i(NEURD_PROP_AUTO_CONVERGENCE, auto_conv ? 1 : 0, g.p_autoconv) &&
		     prop_f(NEURD_PROP_GAIN_MULTIPLIER, gain, g.p_gain) &&
		     prop_i(NEURD_PROP_DILATE_RADIO, g.k.dilate, g.p_dilate);
		if (ok && !auto_conv) {
			ok = prop_f(NEURD_PROP_CONVERGENCE, nd_conv, g.p_conv);
		}
	}
	g.p_valid = ok;
	if (!ok) {
		return false;
	}

	struct NeurD_image img = {};
	img.data = const_cast<uint8_t *>(in->data);
	img.width = (int32_t)in->width;
	img.height = (int32_t)in->height;
	img.stride = (int32_t)in->stride;
	img.pix_fmt = NEURD_PIXEL_FORMAT_BGR8;
	struct NeurD_image res = {};
	const uint64_t t0 = now_ns();
	enum NeurD_status st = NEURD_GENERIC_ERROR;
	bool converted = false;
#ifdef DXR_LEIA_NEURD_HAS_CPU_EX
	struct NeurD_depth_info ex_info = {};
	const float *ex_depth = nullptr;
	struct NeurD_screen_desc oa_screen = {};
	if (use_ex) {
		static const float kDefaultPair[6] = {-0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f};
		std::vector<struct NeurD_view_frustum> oa_views;
		struct NeurD_convert_params cp = {};
		cp.struct_size = (uint32_t)sizeof(cp);
		cp.output_pix_fmt = NEURD_PIXEL_FORMAT_BGR8;
		cp.want_depth = s->retain ? NEURD_TRUE : NEURD_FALSE;
		if (use_oa) {
			oa_screen.struct_size = (uint32_t)sizeof(oa_screen);
			oa_screen.screen_width_m = sc.screen_width_m;
			oa_screen.screen_height_m = sc.screen_height_m;
			oa_screen.depth_scale_m = sc.depth_scale_m;
			oa_screen.convergence_depth_m = sc.convergence_depth_m;
			oa_screen.nominal_eye_z_m = sc.nominal_eye_z_m;
			oa_views.resize(oa_eyes.size() / 3);
			for (size_t i = 0; i < oa_views.size(); i++) {
				oa_views[i].eye_x_m = oa_eyes[i * 3 + 0];
				oa_views[i].eye_y_m = oa_eyes[i * 3 + 1];
				oa_views[i].eye_z_m = oa_eyes[i * 3 + 2];
			}
			cp.camera = NEURD_CAMERA_MODEL_OFFAXIS;
			cp.screen = &oa_screen;
			cp.views = oa_views.data();
			cp.view_count = (int32_t)oa_views.size();
		} else {
			cp.camera = NEURD_CAMERA_MODEL_DIMENSIONLESS;
			cp.viewpoint_pattern_xyz = vp.empty() ? kDefaultPair : vp.data();
			cp.viewpoint_pattern_size = vp.empty() ? 6 : (int32_t)vp.size();
		}
		ex_info.struct_size = (uint32_t)sizeof(ex_info);
		struct NeurD_convert_outputs co = {};
		co.struct_size = (uint32_t)sizeof(co);
		co.depth_info = &ex_info;
		st = NeurD_convert_stream_cpu_ex(nd, s->ns, &img, &cp, &co);
		if (st == NEURD_UNAVAILABLE_OUTDATED_RUNTIME && co.views.data != nullptr) {
			LIFT_WARN_ONCE("Leia conversion: NeurD returned no float depth (backend without a host depth path)");
			st = NEURD_SUCCESS;
		}
		if (st == NEURD_SUCCESS && co.views.data != nullptr) {
			converted = true;
			res = co.views;
			ex_depth = static_cast<const float *>(co.depth.data);
		} else if (use_oa) {
			if (st == NEURD_UNAVAILABLE_OUTDATED_RUNTIME) {
				g.offaxis_ok = false; // no GL view renderer: never again in this process
			}
			LIFT_WARN_ONCE("Leia conversion: off-axis convert -> %s (screen %.3fx%.3f m, D %.3f, C %.3f, N %.3f) — "
			               "the fixed pair for that frame",
			               status_str(st), (double)sc.screen_width_m, (double)sc.screen_height_m,
			               (double)sc.depth_scale_m, (double)sc.convergence_depth_m, (double)sc.nominal_eye_z_m);
			use_oa = false;
		} else {
			LIFT_WARN_ONCE("Leia conversion: NeurD_convert_stream_cpu_ex -> %s — the fixed pair for that frame",
			               status_str(st));
		}
	}
#endif
	if (!converted) {
		st = NeurD_convert_stream_cpu(nd, s->ns, &img, NEURD_PIXEL_FORMAT_BGR8, &res);
	}
	const uint64_t dt = now_ns() - t0;
	if (st != NEURD_SUCCESS || res.data == nullptr || res.width <= 0 || res.height <= 0 || res.stride < res.width * 3) {
		LIFT_WARN_ONCE("Leia conversion: NeurD CPU convert (%ux%u) -> %s", in->width, in->height, status_str(st));
		return false;
	}
	log_camera(s, dp, use_oa, runtime_vp, converted && !use_oa, sc);

#ifdef DXR_LEIA_NEURD_HAS_CPU_EX
	// ---- Depth of the same inference (the Windows module's capture_depth_locked):
	// the metric scale, NeurD's own auto-convergence (the off-axis AUTO anchor),
	// and the aux depth when the runtime asked for it.
	if (converted && s->retain && ex_depth != nullptr) {
		s->metric_scale = (ex_info.units == NEURD_DEPTH_UNITS_METRIC_METERS) ? ex_info.inverse_depth_scale : 0.0f;
		s->auto_zero_h = auto_conv ? leia_lift_nd_legacy_zero_h(ex_info.convergence) : 0.0f;
		if (s->aux_depth && ex_info.width > 0 && ex_info.height > 0) {
			struct leia_lift_nd_depth_info di = {};
			di.units = (ex_info.units == NEURD_DEPTH_UNITS_METRIC_METERS) ? LEIA_LIFT_DEPTH_UNITS_METRIC
			                                                              : LEIA_LIFT_DEPTH_UNITS_RELATIVE;
			di.width = ex_info.width;
			di.height = ex_info.height;
			di.source_width = ex_info.source_width;
			di.source_height = ex_info.source_height;
			di.focal_px_x = ex_info.focal_px_x;
			di.focal_px_y = ex_info.focal_px_y;
			di.principal_px_x = ex_info.principal_px_x;
			di.principal_px_y = ex_info.principal_px_y;
			di.near_m = ex_info.near_m;
			di.far_m = ex_info.far_m;
			di.inverse_depth_scale = ex_info.inverse_depth_scale;
			const float zero_h = use_oa ? oa_zero_h : leia_lift_nd_legacy_zero_h(ex_info.convergence);
			struct leia_lift_depth_meta m = {};
			leia_lift_depth_meta_from_info(&di, zero_h, &m);
			struct leia_lift_neurd_depth &o = s->depth_out;
			o = {};
			o.resource = const_cast<float *>(ex_depth); // host float32, row stride width * 4
			o.format = 0;
			o.width = (uint32_t)ex_info.width;
			o.height = (uint32_t)ex_info.height;
			o.units = m.units;
			o.encoding = m.encoding;
			o.value_scale = m.value_scale;
			o.value_offset = m.value_offset;
			o.source_width = in->width;
			o.source_height = in->height;
			o.focal_x_px = m.focal_x_px;
			o.focal_y_px = m.focal_y_px;
			o.principal_x_px = m.principal_x_px;
			o.principal_y_px = m.principal_y_px;
			o.near_depth = m.near_depth;
			o.far_depth = m.far_depth;
			o.convergence_depth = m.convergence_depth;
			o.same_inference = 1;
			o.vendor_frame_id = ex_info.frame_id;
			if (use_oa && leia_lift_oa_relief(m.units, ex_info.inverse_depth_scale, sc.convergence_depth_m,
			                                  sc.depth_scale_m, &o.relief_scale, &o.relief_offset)) {
				o.relief_valid = 1;
			}
			s->depth_valid = true;
		}
	}
#endif

	g.latency_ns = g.latency_ns == 0 ? dt : (g.latency_ns * 7 + dt) / 8;
	out->data = static_cast<const uint8_t *>(res.data);
	out->width = (uint32_t)res.width;
	out->height = (uint32_t)res.height;
	out->stride = (uint32_t)res.stride;
	return true;
}

extern "C" bool
leia_lift_neurd_get_depth(struct leia_lift_neurd *lift, uint64_t id, struct leia_lift_neurd_depth *out)
{
	if (lift == nullptr || out == nullptr) {
		return false;
	}
	for (const auto &it : lift->streams) {
		if (it.id == id && it.depth_valid) {
			*out = it.depth_out;
			return true;
		}
	}
	return false;
}
