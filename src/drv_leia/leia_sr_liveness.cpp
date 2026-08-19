// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR platform incarnation probe — implementation (leia-plugin#158).
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_sr_liveness.h"

#ifdef _WIN32

#include <windows.h>
#include <winsvc.h>
#include <winver.h>

#include <mutex>
#include <string>
#include <vector>

namespace {

//! The SCM key of the SR platform's core service ("Simulated Reality Service").
constexpr wchar_t kSrServiceName[] = L"SR Service";

//! Recompute the generation token at most this often — it is polled per-frame.
constexpr uint64_t kGenerationCacheMs = 1000;

std::mutex g_lock;
uint64_t g_gen_value = 0;      //!< Last computed token (0 = platform down).
uint64_t g_gen_stamp_ms = 0;   //!< GetTickCount64() when it was computed.
bool g_gen_valid = false;      //!< g_gen_value/g_gen_stamp_ms are meaningful.
uint64_t g_match_for_gen = 0;  //!< Generation g_match_value was computed for.
bool g_match_value = true;     //!< Cached client/platform version verdict.
bool g_match_valid = false;    //!< g_match_* are meaningful.

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

} // namespace

extern "C" uint64_t
leia_sr_liveness_platform_generation(void)
{
	const uint64_t now_ms = GetTickCount64();

	std::lock_guard<std::mutex> guard(g_lock);
	if (g_gen_valid && (now_ms - g_gen_stamp_ms) < kGenerationCacheMs) {
		return g_gen_value;
	}

	g_gen_value = compute_generation();
	g_gen_stamp_ms = now_ms;
	g_gen_valid = true;
	return g_gen_value;
}

extern "C" bool
leia_sr_liveness_client_matches_platform(void)
{
	const uint64_t gen = leia_sr_liveness_platform_generation();

	std::lock_guard<std::mutex> guard(g_lock);
	if (g_match_valid && g_match_for_gen == gen) {
		return g_match_value;
	}

	g_match_value = compute_client_matches();
	g_match_for_gen = gen;
	g_match_valid = true;
	return g_match_value;
}

#else // !_WIN32

/*
 * Non-Windows arms have no SCM and no SR client DLLs to compare. Report
 * "unknown" in both directions: token 0 (callers treat that as "cannot tell
 * generations apart", never as a restart) and "matches" for the tripwire.
 */

extern "C" uint64_t
leia_sr_liveness_platform_generation(void)
{
	return 0;
}

extern "C" bool
leia_sr_liveness_client_matches_platform(void)
{
	return true;
}

#endif // _WIN32
