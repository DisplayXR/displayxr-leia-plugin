// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Delay-load resolution of the SR platform client DLLs.
 *
 * The plug-in links the SR client DLLs (SimulatedRealityCore / Displays /
 * DirectX / OpenGL) with /DELAYLOAD, so the plug-in DLL itself loads on a box
 * WITHOUT the SR platform (install-order epic, displayxr-runtime#1803, P-a).
 * Two consequences, both handled here:
 *
 *  1. WHERE the DLLs come from. They used to be found only through the
 *     machine PATH (LeiaSR\Platform\bin), which a long-lived service
 *     snapshots at its own start — so a service that outlived an SR install
 *     never found them. The notification hook below resolves each SR DLL by
 *     FULL PATH under the SR install dir read from
 *     `HKLM\SOFTWARE\Dimenco\Simulated Reality` (default value; the client
 *     DLLs live in `<dir>\bin`), loading with LOAD_WITH_ALTERED_SEARCH_PATH so
 *     their own dependencies resolve from the same directory. Anything else —
 *     or a miss — returns NULL and the normal search (PATH) still applies.
 *
 *  2. WHEN it is safe to call SR. A missing delay-loaded DLL (or export)
 *     raises an SEH exception at the first call site. So nothing may call SR
 *     before leia_sr_client_bind() has returned LEIA_SR_BIND_OK: it binds
 *     EVERY SR import eagerly, inside __try/__except, so after one successful
 *     bind no later SR call can fault on resolution, and a failed bind is a
 *     clean status (MISSING / INCOMPATIBLE) instead of a crash.
 *
 * `DXR_LEIA_SR_DIR` (testing aid) overrides the DLL directory: it names the
 * directory that holds the client DLLs directly (the `bin` dir), and the
 * registry value is then not consulted for resolution.
 *
 * @ingroup drv_leia
 */

#include "leia_platform_state.h"

#include "util/u_logging.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <delayimp.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/*
 * The SR client DLLs this build delay-loads. Must match the /DELAYLOAD list in
 * src/drv_leia/CMakeLists.txt (same compile gates). Core and Displays are
 * always imported; DirectX / OpenGL only when their weavers are compiled in.
 */
static const char *const k_sr_dlls[] = {
    "SimulatedRealityCore.dll",
    "SimulatedRealityDisplays.dll",
#ifdef XRT_HAVE_LEIA_SR_D3D11
    "SimulatedRealityDirectX.dll",
#endif
#ifdef XRT_HAVE_LEIA_SR_GL
    "SimulatedRealityOpenGL.dll",
#endif
};
#define K_SR_DLL_COUNT (sizeof(k_sr_dlls) / sizeof(k_sr_dlls[0]))

static SRWLOCK g_dir_lock = SRWLOCK_INIT;
//! Cached SR client DLL directory (with trailing backslash). A POSITIVE read
//! is cached for the life of the process; a miss is re-read on the next call
//! (cheap), so an SR install that lands after this process started is found
//! without a restart.
static wchar_t g_dir[MAX_PATH];
static bool g_dir_valid = false;
static bool g_dir_logged = false;

static SRWLOCK g_bind_lock = SRWLOCK_INIT;
static volatile LONG g_bound = 0; //!< 1 once every SR import is bound.
static bool g_bind_logged = false;
static enum leia_sr_bind_result g_bind_logged_result = LEIA_SR_BIND_OK;

static bool
is_sr_dll(const char *name)
{
	for (size_t i = 0; i < K_SR_DLL_COUNT; i++) {
		if (_stricmp(name, k_sr_dlls[i]) == 0) {
			return true;
		}
	}
	return false;
}

//! Read the dir. Caller holds g_dir_lock exclusively.
static bool
read_dir_locked(void)
{
	if (g_dir_valid) {
		return true;
	}

	wchar_t buf[MAX_PATH];
	const char *env = getenv("DXR_LEIA_SR_DIR");
	if (env != NULL && env[0] != '\0') {
		if (MultiByteToWideChar(CP_UTF8, 0, env, -1, buf, MAX_PATH) == 0) {
			return false;
		}
	} else {
		DWORD size = sizeof(buf);
		LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Dimenco\\Simulated Reality", NULL,
		                          RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, buf, &size);
		if (st != ERROR_SUCCESS || buf[0] == L'\0') {
			return false;
		}
		// Install dir -> client DLL dir.
		size_t n = wcslen(buf);
		if (n > 0 && buf[n - 1] != L'\\') {
			if (wcscat_s(buf, MAX_PATH, L"\\") != 0) {
				return false;
			}
		}
		if (wcscat_s(buf, MAX_PATH, L"bin") != 0) {
			return false;
		}
	}

	size_t n = wcslen(buf);
	if (n > 0 && buf[n - 1] != L'\\' && buf[n - 1] != L'/') {
		if (wcscat_s(buf, MAX_PATH, L"\\") != 0) {
			return false;
		}
	}
	wcscpy_s(g_dir, MAX_PATH, buf);
	g_dir_valid = true;
	if (!g_dir_logged) {
		g_dir_logged = true;
		U_LOG_W("Leia SR: client DLL directory resolved to '%ls' (%s)", g_dir,
		        (env != NULL && env[0] != '\0') ? "DXR_LEIA_SR_DIR"
		                                        : "HKLM\\SOFTWARE\\Dimenco\\Simulated Reality");
	}
	return true;
}

//! Full-path load of one SR DLL from the resolved directory, or NULL.
static HMODULE
load_from_sr_dir(const char *name)
{
	wchar_t path[MAX_PATH];
	AcquireSRWLockExclusive(&g_dir_lock);
	bool ok = read_dir_locked();
	if (ok) {
		wcscpy_s(path, MAX_PATH, g_dir);
	}
	ReleaseSRWLockExclusive(&g_dir_lock);
	if (!ok) {
		return NULL;
	}

	wchar_t wname[64];
	if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64) == 0) {
		return NULL;
	}
	if (wcscat_s(path, MAX_PATH, wname) != 0) {
		return NULL;
	}
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
		return NULL; // let the normal search try
	}
	// Backslashes only — LoadLibraryExW's altered search path requires them.
	return LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

static FARPROC WINAPI
leia_sr_dli_hook(unsigned dliNotify, PDelayLoadInfo pdli)
{
	if (dliNotify == dliNotePreLoadLibrary && pdli != NULL && pdli->szDll != NULL && is_sr_dll(pdli->szDll)) {
		// NULL => the delay helper falls back to LoadLibraryExA(name) = the
		// normal search, so a PATH-only install keeps working.
		return (FARPROC)load_from_sr_dir(pdli->szDll);
	}
	return NULL;
}

// The delay-load helper reads this at every notification (delayimp.h).
const PfnDliHook __pfnDliNotifyHook2 = leia_sr_dli_hook;

//! SEH filter: delay-load failures are VcppException(ERROR_SEVERITY_ERROR, err).
static int
bind_filter(DWORD code, DWORD *out_code)
{
	*out_code = code;
	if (code == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
	    code == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND)) {
		return EXCEPTION_EXECUTE_HANDLER;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

//! Bind every import of one DLL; SEH-guarded.
static enum leia_sr_bind_result
bind_one(const char *name, DWORD *out_code)
{
	HRESULT hr = S_OK;
	*out_code = 0;
	__try {
		hr = __HrLoadAllImportsForDll(name);
	} __except (bind_filter(GetExceptionCode(), out_code)) {
		return *out_code == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND)
		           ? LEIA_SR_BIND_INCOMPATIBLE
		           : LEIA_SR_BIND_MISSING;
	}
	if (FAILED(hr)) {
		*out_code = (DWORD)hr;
		return LEIA_SR_BIND_MISSING;
	}
	return LEIA_SR_BIND_OK;
}

enum leia_sr_bind_result
leia_sr_client_bind(void)
{
	if (InterlockedCompareExchange(&g_bound, 0, 0) != 0) {
		return LEIA_SR_BIND_OK;
	}

	AcquireSRWLockExclusive(&g_bind_lock);
	enum leia_sr_bind_result res = LEIA_SR_BIND_OK;
	const char *failed = NULL;
	DWORD code = 0;
	if (InterlockedCompareExchange(&g_bound, 0, 0) == 0) {
		for (size_t i = 0; i < K_SR_DLL_COUNT; i++) {
			res = bind_one(k_sr_dlls[i], &code);
			if (res != LEIA_SR_BIND_OK) {
				failed = k_sr_dlls[i];
				break;
			}
		}
		if (res == LEIA_SR_BIND_OK) {
			InterlockedExchange(&g_bound, 1);
		}

		// One line per outcome change, not per attempt: callers retry on
		// every probe while the platform is missing.
		if (!g_bind_logged || g_bind_logged_result != res) {
			g_bind_logged = true;
			g_bind_logged_result = res;
			if (res == LEIA_SR_BIND_OK) {
				U_LOG_W("Leia SR: client DLLs bound (%u DLLs, delay-loaded)", (unsigned)K_SR_DLL_COUNT);
			} else {
				U_LOG_W(
				    "Leia SR: client DLLs NOT usable — %s %s (code 0x%08lx); the plug-in stays loaded "
				    "and declines until the SR platform is installed/updated",
				    failed != NULL ? failed : "?",
				    res == LEIA_SR_BIND_INCOMPATIBLE ? "is missing an export (SR platform too old/new)"
				                                     : "could not be loaded",
				    (unsigned long)code);
			}
		}
	}
	ReleaseSRWLockExclusive(&g_bind_lock);
	return InterlockedCompareExchange(&g_bound, 0, 0) != 0 ? LEIA_SR_BIND_OK : res;
}

bool
leia_sr_client_bound(void)
{
	return InterlockedCompareExchange(&g_bound, 0, 0) != 0;
}

#else // !_WIN32

enum leia_sr_bind_result
leia_sr_client_bind(void)
{
	return LEIA_SR_BIND_MISSING;
}

bool
leia_sr_client_bound(void)
{
	return false;
}

#endif // _WIN32
