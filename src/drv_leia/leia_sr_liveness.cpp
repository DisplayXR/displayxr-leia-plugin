// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR platform incarnation probe — implementation (leia-plugin#158).
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_sr_liveness.h"

#include "util/u_logging.h"

#ifdef _WIN32

#include <windows.h>
#include <winsvc.h>
#include <winver.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

/*
 * ==========================================================================
 * DXR_LEIA_HAS_SR_CONNECTION_STATE — a gate DELIBERATELY NARROWER than
 * DXR_LEIA_HAS_SR_V2. Do NOT "simplify" the two into one.
 *
 * Everything behind it is the SDK's own identity (`srGetPlatformInstanceId`)
 * and liveness (`srInstanceGetConnectionState`) pair, ST-5673. Two independent
 * reasons it cannot ride on DXR_LEIA_HAS_SR_V2:
 *
 *  1. AGE. The SDK this repo pins (SR_V2_TAG) has the v2 API but predates
 *     these two functions, so an unconditional v2 gate would not compile the
 *     pinned/CI build at all.
 *
 *  2. DISPATCH-INDEX LINEAGE — the dangerous one. The SDK drop the functions
 *     first appeared in carries five appended dispatch slots from other,
 *     still-unmerged PRs positioned AHEAD of them, so the two sit at DIFFERENT
 *     loader dispatch-table indices there than on the release-candidate
 *     lineage. A binary linked against one lineage's loader running against
 *     the other lineage's runtime mis-dispatches BY SLOT INDEX: silent type
 *     confusion, not a clean SR_ERROR_FUNCTION_UNSUPPORTED. The gate therefore
 *     has to be an explicit per-SDK opt-in that a stock configure never turns
 *     on by accident.
 *
 * Gate OFF must be byte-for-byte today's behaviour: the SCM probe alone.
 * ==========================================================================
 */
#ifdef DXR_LEIA_HAS_SR_CONNECTION_STATE
#include <sr/sr_instance.h>
#include <sr/sr_result.h>
#endif

namespace {

//! The SCM key of the SR platform's core service ("Simulated Reality Service").
constexpr wchar_t kSrServiceName[] = L"SR Service";

//! Recompute the generation token at most this often — it is polled per-frame.
constexpr uint64_t kGenerationCacheMs = 1000;

/*!
 * #169: re-read the client/platform version verdict at most this often.
 *
 * A time bound ON TOP OF the generation key, not instead of it. The generation
 * is a constant 0 for the whole time the platform is down, which is exactly the
 * window an upgrade replaces the files in — keyed on generation alone, a
 * verdict taken at the start of that window would still be the answer at the
 * end of it.
 */
constexpr uint64_t kMatchCacheMs = 1000;

std::mutex g_lock;
uint64_t g_gen_value = 0;      //!< Last SCM token (0 = platform down).
uint64_t g_gen_stamp_ms = 0;   //!< GetTickCount64() when it was computed.
bool g_gen_valid = false;      //!< g_gen_value/g_gen_stamp_ms are meaningful.
// Second cache slot, for the SDK-reported platform id. Kept SEPARATE from the
// SCM slot above and keyed by the instance it was asked through, so a bare
// leia_sr_liveness_platform_generation() call (the version tripwire makes one
// every second) cannot evict an arm's id, nor an arm's id answer the bare call.
const void *g_gen_v2_key = nullptr;
uint64_t g_gen_v2_value = 0;
uint64_t g_gen_v2_stamp_ms = 0;
bool g_gen_v2_valid = false;
uint64_t g_match_for_gen = 0;   //!< Generation g_match_value was computed for.
uint64_t g_match_stamp_ms = 0;  //!< GetTickCount64() when it was computed.
bool g_match_value = true;      //!< Cached client/platform version verdict.
bool g_match_valid = false;     //!< g_match_* are meaningful.
//! #169: a positive mismatch is IRREVERSIBLE — Windows never swaps a mapped
//! image, so the client DLLs can only get staler. Latching it keeps a later
//! undeterminable read (a file locked mid-install reads as "cannot tell", which
//! by contract means "matches") from re-opening the gate.
bool g_match_latched_false = false;
//! #169: one process-wide WARN edge for leia_sr_liveness_weaver_create_allowed.
std::atomic<bool> g_warned_create_refused{false};

/*!
 * The SR service's process id, or 0 when it is not running.
 *
 * SERVICE_QUERY_STATUS is granted to Authenticated Users by the service's own
 * SDDL, so this works unelevated — which matters, because displayxr-service.exe
 * runs as the logged-in user.
 */
DWORD
query_sr_service_pid(void)
{
	SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (scm == nullptr) {
		return 0;
	}

	DWORD pid = 0;
	SC_HANDLE svc = OpenServiceW(scm, kSrServiceName, SERVICE_QUERY_STATUS);
	if (svc != nullptr) {
		SERVICE_STATUS_PROCESS ssp = {};
		DWORD needed = 0;
		if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed) &&
		    ssp.dwCurrentState == SERVICE_RUNNING) {
			pid = ssp.dwProcessId;
		}
		CloseServiceHandle(svc);
	}

	CloseServiceHandle(scm);
	return pid;
}

/*!
 * Compute a fresh generation token. See the header for the contract.
 *
 * pid alone would be a weak token (Windows recycles pids), so the process
 * CREATION TIME is mixed in — the pair is unique for as long as anything can
 * observe it. If the process cannot be opened at all (hardening, a service
 * running as another account), fall back to a pid-only token rather than
 * reporting "down": a weaker token still catches the restart case, whereas a
 * false 0 would claim the platform vanished.
 */
uint64_t
compute_generation(void)
{
	DWORD pid = query_sr_service_pid();
	if (pid == 0) {
		return 0;
	}

	uint64_t token = 0;
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (proc != nullptr) {
		FILETIME created = {}, exited = {}, kernel = {}, user = {};
		if (GetProcessTimes(proc, &created, &exited, &kernel, &user)) {
			ULARGE_INTEGER c;
			c.LowPart = created.dwLowDateTime;
			c.HighPart = created.dwHighDateTime;
			token = (uint64_t)c.QuadPart ^ ((uint64_t)pid << 1);
		}
		CloseHandle(proc);
	}

	if (token == 0) {
		// Either OpenProcess/GetProcessTimes failed, or the XOR landed on
		// zero. Both need a non-zero answer — the service IS running.
		token = ((uint64_t)pid << 32) | 1u;
	}
	return token;
}

//! Pack a VS_FIXEDFILEINFO file version into one comparable integer.
uint64_t
pack_version(const VS_FIXEDFILEINFO *ffi)
{
	return ((uint64_t)ffi->dwFileVersionMS << 32) | (uint64_t)ffi->dwFileVersionLS;
}

/*!
 * File version of a module AS MAPPED in this process, read out of the loaded
 * image's own resource section.
 *
 * Deliberately NOT GetFileVersionInfo on the module's path: after an upgrade
 * the on-disk file is the NEW one while this image stays the OLD one, and
 * reading the path would report the new version for the old code — which is
 * precisely the skew this exists to detect.
 *
 * @return 0 if unavailable.
 */
uint64_t
mapped_module_version(HMODULE mod)
{
	// RT_VERSION expands through MAKEINTRESOURCE, i.e. the ANSI flavour, so it
	// is an LPSTR even in a wide build. It is an ORDINAL, not a string, so the
	// cast to the W entry point's parameter type is exact, not a reinterpretation
	// of character data.
	HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(VS_VERSION_INFO), reinterpret_cast<LPCWSTR>(RT_VERSION));
	if (res == nullptr) {
		return 0;
	}
	DWORD size = SizeofResource(mod, res);
	HGLOBAL loaded = LoadResource(mod, res);
	if (size == 0 || loaded == nullptr) {
		return 0;
	}
	const void *data = LockResource(loaded);
	if (data == nullptr) {
		return 0;
	}

	// VerQueryValue wants a writable buffer.
	std::vector<uint8_t> copy((const uint8_t *)data, (const uint8_t *)data + size);
	VS_FIXEDFILEINFO *ffi = nullptr;
	UINT ffi_len = 0;
	if (!VerQueryValueW(copy.data(), L"\\", (LPVOID *)&ffi, &ffi_len) || ffi == nullptr ||
	    ffi_len < sizeof(VS_FIXEDFILEINFO)) {
		return 0;
	}
	return pack_version(ffi);
}

/*!
 * Directory the SR platform is INSTALLED in, derived from a module that is
 * always mapped straight out of it.
 *
 * `SimulatedRealityCore.dll` is the anchor on purpose. The obvious alternative
 * — ask each module for its own path — is WRONG here: the runtime pre-loads a
 * sanitized per-build COPY of `DimencoWeaving.dll` (displayxr-runtime#434) from
 * %LOCALAPPDATA%\DisplayXR\ThirdParty\..., so that module's path names the
 * cache, not the install. `SimulatedRealityCore.dll` is not sanitized and maps
 * from `...\LeiaSR\Platform\bin`.
 *
 * @return false when the anchor is not loaded or its path cannot be taken.
 *         Callers must answer "undeterminable" then, never "mismatch".
 */
bool
sr_platform_dir(std::wstring &out)
{
	HMODULE anchor = GetModuleHandleW(L"SimulatedRealityCore.dll");
	if (anchor == nullptr) {
		return false;
	}

	wchar_t path[MAX_PATH * 2] = {};
	DWORD n = GetModuleFileNameW(anchor, path, (DWORD)(sizeof(path) / sizeof(path[0])));
	if (n == 0 || n >= (sizeof(path) / sizeof(path[0]))) {
		return false;
	}

	wchar_t *slash = wcsrchr(path, L'\\');
	if (slash == nullptr) {
		return false;
	}
	*slash = L'\0';
	out.assign(path);
	return true;
}

/*!
 * File version of @p name ON DISK in the installed platform directory — i.e.
 * what is installed now.
 *
 * Takes the directory rather than the HMODULE deliberately. Deriving the path
 * from `GetModuleFileNameW(mod)` is a structural false negative for the one
 * module most likely to be stale: `DimencoWeaving.dll` is mapped from the
 * runtime's sanitized COPY (see sr_platform_dir), so that comparison pits the
 * cached copy against ITSELF and can only ever report "matches". Observed live:
 * mapped v1.37.0+1470 against installed v1.37.0+1473, verdict "matches", zero
 * warnings.
 *
 * @return 0 if unavailable.
 */
uint64_t
installed_module_version(const std::wstring &dir, const wchar_t *name)
{
	const std::wstring path = dir + L"\\" + name;

	DWORD handle = 0;
	DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
	if (size == 0) {
		return 0;
	}
	std::vector<uint8_t> buf(size);
	if (!GetFileVersionInfoW(path.c_str(), handle, size, buf.data())) {
		return 0;
	}
	VS_FIXEDFILEINFO *ffi = nullptr;
	UINT ffi_len = 0;
	if (!VerQueryValueW(buf.data(), L"\\", (LPVOID *)&ffi, &ffi_len) || ffi == nullptr ||
	    ffi_len < sizeof(VS_FIXEDFILEINFO)) {
		return 0;
	}
	return pack_version(ffi);
}

/*!
 * Does one SR client module, as mapped, match its installed file?
 *
 * "Not loaded" and "no version resource" both answer true — see the header:
 * undeterminable must never read as a mismatch.
 */
bool
module_matches_disk(const std::wstring &platform_dir, const wchar_t *name)
{
	HMODULE mod = GetModuleHandleW(name);
	if (mod == nullptr) {
		return true;
	}
	uint64_t mapped = mapped_module_version(mod);
	uint64_t disk = installed_module_version(platform_dir, name);
	if (mapped == 0 || disk == 0) {
		return true;
	}
	return mapped == disk;
}

bool
compute_client_matches(void)
{
	// The on-disk side must be read from the INSTALLED platform directory, not
	// from each module's own path — DimencoWeaving.dll is the sanitized copy the
	// runtime pre-loads, which is exactly why the directory is anchored on
	// SimulatedRealityCore.dll instead (see sr_platform_dir).
	std::wstring dir;
	if (!sr_platform_dir(dir)) {
		// Undeterminable — never a mismatch. See the header.
		return true;
	}

	// DimencoWeaving carries the weaver itself; SimulatedRealityCore the
	// client transport. Either one going stale is worth the warning.
	return module_matches_disk(dir, L"DimencoWeaving.dll") && module_matches_disk(dir, L"SimulatedRealityCore.dll");
}

#ifdef DXR_LEIA_HAS_SR_CONNECTION_STATE
/*!
 * Platform identity straight from the SDK, or 0 when it will not say.
 *
 * 0 is returned for every failure, not just the documented
 * SR_ERROR_RUNTIME_UNAVAILABLE: a pre-ST-5673 platform leaves the loader's
 * dispatch slot NULL and its trampoline answers SR_ERROR_FUNCTION_UNSUPPORTED
 * instead (verified by disassembling srSDK_loader.lib — the trampoline
 * null-checks the slot, so calling into an older platform is safe). Either way
 * the caller falls back to the SCM probe, which is what a mixed install needs.
 *
 * 0 doubles as the "down/unknown" sentinel everywhere in this file, and the SDK
 * documents the id as never 0 on success, so no folding is needed — a 0 simply
 * cannot be mistaken for an identity.
 */
uint64_t
sdk_platform_instance_id(void *inst)
{
	if (inst == nullptr) {
		return 0;
	}

	uint64_t id = 0;
	// The v2 API is C99 and cannot throw, but this file is reached from C and a
	// stray exception would cross the ABI — belt and braces, exactly as every
	// other SR call site in this plug-in.
	try {
		if (!SR_SUCCEEDED(srGetPlatformInstanceId(static_cast<SrInstance>(inst), &id))) {
			return 0;
		}
	} catch (...) {
		return 0;
	}
	return id;
}
#endif // DXR_LEIA_HAS_SR_CONNECTION_STATE

} // namespace

extern "C" uint64_t
leia_sr_liveness_platform_generation_ex(void *sr_instance_v2)
{
	const uint64_t now_ms = GetTickCount64();

	std::lock_guard<std::mutex> guard(g_lock);

	if (sr_instance_v2 != nullptr) {
		if (g_gen_v2_valid && g_gen_v2_key == sr_instance_v2 &&
		    (now_ms - g_gen_v2_stamp_ms) < kGenerationCacheMs) {
			return g_gen_v2_value;
		}
#ifdef DXR_LEIA_HAS_SR_CONNECTION_STATE
		const uint64_t id = sdk_platform_instance_id(sr_instance_v2);
		if (id != 0) {
			g_gen_v2_key = sr_instance_v2;
			g_gen_v2_value = id;
			g_gen_v2_stamp_ms = now_ms;
			g_gen_v2_valid = true;
			return id;
		}
		// The SDK would not answer — an older platform, or one that is gone.
		// Fall through to the SCM probe, and deliberately do NOT cache that in
		// the v2 slot: the identity API must be retried on the next poll rather
		// than written off for this instance's lifetime.
#endif
	}

	if (g_gen_valid && (now_ms - g_gen_stamp_ms) < kGenerationCacheMs) {
		return g_gen_value;
	}

	g_gen_value = compute_generation();
	g_gen_stamp_ms = now_ms;
	g_gen_valid = true;
	return g_gen_value;
}

extern "C" uint64_t
leia_sr_liveness_platform_generation(void)
{
	return leia_sr_liveness_platform_generation_ex(nullptr);
}

extern "C" bool
leia_sr_liveness_connection_is_dead(void *sr_instance_v2)
{
#ifdef DXR_LEIA_HAS_SR_CONNECTION_STATE
	if (sr_instance_v2 == nullptr) {
		return false;
	}

	SrConnectionState state = SR_CONNECTION_STATE_CONNECTED;
	try {
		if (!SR_SUCCEEDED(
		        srInstanceGetConnectionState(static_cast<SrInstance>(sr_instance_v2), &state))) {
			// Includes SR_ERROR_FUNCTION_UNSUPPORTED from a pre-ST-5673
			// platform. Undeterminable is never "dead".
			return false;
		}
	} catch (...) {
		return false;
	}

	// SR_CONNECTION_STATE_RECONNECTING is reserved and never returned by this
	// runtime; it is explicitly NOT treated as dead, so nothing here waits for a
	// state the SDK does not produce.
	return state == SR_CONNECTION_STATE_DISCONNECTED;
#else
	(void)sr_instance_v2;
	return false;
#endif // DXR_LEIA_HAS_SR_CONNECTION_STATE
}

extern "C" bool
leia_sr_liveness_client_matches_platform(void)
{
	{
		// Cheap out before touching the SCM: once mismatched, always
		// mismatched (see g_match_latched_false).
		std::lock_guard<std::mutex> guard(g_lock);
		if (g_match_latched_false) {
			return false;
		}
	}

	// Deliberately OUTSIDE the lock — it takes g_lock itself.
	const uint64_t gen = leia_sr_liveness_platform_generation();
	const uint64_t now_ms = GetTickCount64();

	std::lock_guard<std::mutex> guard(g_lock);
	if (g_match_latched_false) {
		return false; // another thread latched while we were unlocked
	}
	if (g_match_valid && g_match_for_gen == gen && (now_ms - g_match_stamp_ms) < kMatchCacheMs) {
		return g_match_value;
	}

	g_match_value = compute_client_matches();
	g_match_for_gen = gen;
	g_match_stamp_ms = now_ms;
	g_match_valid = true;
	if (!g_match_value) {
		g_match_latched_false = true;
	}
	return g_match_value;
}

extern "C" bool
leia_sr_liveness_weaver_create_allowed(void)
{
	if (leia_sr_liveness_client_matches_platform()) {
		return true;
	}

	if (!g_warned_create_refused.exchange(true, std::memory_order_relaxed)) {
		U_LOG_W("SR platform was UPGRADED while this process was running — the OLD SR client DLLs are "
		        "still mapped (Windows cannot swap a loaded image), so building a weaver from them "
		        "against the new platform faults inside the vendor runtime (leia-plugin#169). Refusing "
		        "every weaver create in this process; the panel falls back to a flat blit. RESTART the "
		        "DisplayXR service to map the new SR client libraries and recover 3D.");
	}
	return false;
}

#else // !_WIN32

/*
 * Non-Windows arms have no SCM and no SR client DLLs to compare. Report
 * "unknown" in both directions: token 0 (callers treat that as "cannot tell
 * generations apart", never as a restart) and "matches" for the tripwire.
 */

extern "C" uint64_t
leia_sr_liveness_platform_generation_ex(void *sr_instance_v2)
{
	(void)sr_instance_v2;
	return 0;
}

extern "C" uint64_t
leia_sr_liveness_platform_generation(void)
{
	return 0;
}

extern "C" bool
leia_sr_liveness_connection_is_dead(void *sr_instance_v2)
{
	(void)sr_instance_v2;
	return false;
}

extern "C" bool
leia_sr_liveness_client_matches_platform(void)
{
	return true;
}

extern "C" bool
leia_sr_liveness_weaver_create_allowed(void)
{
	return true;
}

#endif // _WIN32
