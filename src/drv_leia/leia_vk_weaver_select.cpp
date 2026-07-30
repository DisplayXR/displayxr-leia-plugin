// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Picks the correct SR Vulkan weaver DLL + ABI for the installed LeiaSR.
 * @ingroup drv_leia
 *
 * ## The three-way policy
 *
 * A Dec-2025 `DimencoWeaving` change made a **pixelLayout subpixel stamp**
 * replace the weaver's internal colour order. The consumer of that stamp landed
 * only in 1.37.1's `srVulkan` (ST-5318). So from SR 1.36 onward the core emits a
 * stamp that any pre-ST-5318 Vulkan weaver silently drops — the shader then
 * weaves on the legacy fixed colour-order assumption and the display shows a
 * *coloured replica* ghost of the scene. Hardware-validated on a Leia panel:
 *
 * | weaver     | SR core   | result |
 * |------------|-----------|--------|
 * | pre-stamp  | 1.34.12   | good   |
 * | pre-stamp  | 1.36.2    | BROKEN |
 * | pre-stamp  | 1.37.0    | BROKEN |
 * | stamp-aware| 1.36.2    | good   |
 * | stamp-aware| 1.37.0    | good   |
 *
 * D3D and GL never broke: their weavers are machine-installed and built with the
 * core. Only Vulkan was bundled by us, so only Vulkan went stale.
 *
 * Selection is **capability-keyed, not version-parsed**, wherever possible —
 * we prefer "is there a machine weaver?" over "what version string is this?",
 * because the machine weaver is by construction matched to its own core.
 */

#include "leia_vk_weaver.h"

#include "util/u_logging.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

constexpr const wchar_t *kMachineWeaverDll = L"SimulatedRealityVulkan.dll";
constexpr const wchar_t *kBundledStampDll = L"SimulatedRealityVulkan.dll";
constexpr const wchar_t *kBundledLegacyDll = L"SimulatedRealityVulkanBeta.dll";

//! Mangled `SR::CreateVulkanWeaver`. Verified byte-identical across the 1.35,
//! 1.36.4 and 1.37.1 weavers, so it is safe to resolve by name — but for the
//! same reason it is **useless as an ABI probe**.
constexpr const char *kCreateWeaverSymbol =
    "?CreateVulkanWeaver@SR@@YA?AW4WeaverErrorCode@@AEAVSRContext@1@PEAUVkDevice_T@@PEAUVkPhysicalDevice_T@@"
    "PEAUVkQueue_T@@PEAUVkCommandPool_T@@PEAUHWND__@@PEAPEAVIVulkanWeaver1@1@@Z";

leia_vk_weaver_backend g_backend{};
bool g_resolved = false;
bool g_ok = false;
std::once_flag g_once;

/*!
 * Absolute path of a file sitting next to this plug-in DLL.
 *
 * The plug-in directory is on none of the default DLL search paths, so every
 * bundled dependency must be loaded by full path.
 */
bool
path_beside_self(const wchar_t *leaf, wchar_t *out, size_t out_cap)
{
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCWSTR>(&path_beside_self), &self)) {
		return false;
	}

	wchar_t path[MAX_PATH];
	DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return false;
	}

	wchar_t *slash = wcsrchr(path, L'\\');
	if (slash == nullptr) {
		return false;
	}
	slash[1] = L'\0';

	if (wcslen(path) + wcslen(leaf) >= out_cap) {
		return false;
	}
	wcscpy_s(out, out_cap, path);
	wcscat_s(out, out_cap, leaf);
	return true;
}

//! Locate a DLL on the process search path (PATH) without loading it.
//! The SR Platform installer puts `Platform\bin` on the machine PATH, which is
//! how the D3D weaver has always been found.
bool
find_on_search_path(const wchar_t *leaf, wchar_t *out, size_t out_cap)
{
	DWORD n = SearchPathW(nullptr, leaf, nullptr, static_cast<DWORD>(out_cap), out, nullptr);
	return n > 0 && n < out_cap;
}

/*!
 * Is the installed core new enough to emit the subpixel stamp (SR >= 1.36)?
 *
 * Read from `DimencoWeaving.dll`'s file version rather than any SR API: this
 * runs before an `SRContext` exists, and the weaving DLL is the component that
 * actually changed. Returns false when the version cannot be read, which biases
 * to the legacy weaver — the safer default, since that is what shipped and what
 * every pre-1.36 machine needs.
 */
bool
core_emits_pixel_stamp(void)
{
	wchar_t dw[MAX_PATH];
	if (!find_on_search_path(L"DimencoWeaving.dll", dw, MAX_PATH)) {
		U_LOG_W("SR VK weaver: DimencoWeaving.dll not on PATH - assuming pre-stamp core");
		return false;
	}

	DWORD ignored = 0;
	DWORD size = GetFileVersionInfoSizeW(dw, &ignored);
	if (size == 0) {
		return false;
	}

	void *buf = malloc(size);
	if (buf == nullptr) {
		return false;
	}

	bool stamp = false;
	if (GetFileVersionInfoW(dw, 0, size, buf)) {
		VS_FIXEDFILEINFO *ffi = nullptr;
		UINT ffi_len = 0;
		if (VerQueryValueW(buf, L"\\", reinterpret_cast<LPVOID *>(&ffi), &ffi_len) && ffi != nullptr &&
		    ffi_len >= sizeof(VS_FIXEDFILEINFO)) {
			const WORD major = HIWORD(ffi->dwFileVersionMS);
			const WORD minor = LOWORD(ffi->dwFileVersionMS);
			stamp = (major > 1) || (major == 1 && minor >= 36);
			U_LOG_W("SR VK weaver: DimencoWeaving %u.%u -> core %s the pixel stamp", (unsigned)major,
			        (unsigned)minor, stamp ? "EMITS" : "predates");
		}
	}
	free(buf);
	return stamp;
}

//! Load by absolute path; ALTERED_SEARCH_PATH lets the weaver resolve its own
//! co-located dependencies from the directory it was loaded from.
HMODULE
load_absolute(const wchar_t *path)
{
	return LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

bool
try_backend(const wchar_t *path, enum leia_vk_abi abi, const char *why)
{
	if (path == nullptr || path[0] == L'\0') {
		return false;
	}
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
		return false;
	}

	HMODULE mod = load_absolute(path);
	if (mod == nullptr) {
		U_LOG_W("SR VK weaver: LoadLibraryExW('%ls') failed (err=%lu)", path, GetLastError());
		return false;
	}

	void *fn = reinterpret_cast<void *>(GetProcAddress(mod, kCreateWeaverSymbol));
	if (fn == nullptr) {
		U_LOG_W("SR VK weaver: '%ls' has no CreateVulkanWeaver export - skipping", path);
		return false;
	}

	g_backend.abi = abi;
	g_backend.module = mod;
	g_backend.create_fn = fn;
	g_backend.ops = (abi == LEIA_VK_ABI_STAMP) ? leia_vk_weaver_ops_stamp() : leia_vk_weaver_ops_legacy();
	wcscpy_s(g_backend.dll_path, MAX_PATH, path);

	U_LOG_W("SR VK weaver: using %s ABI from '%ls' (%s)", abi == LEIA_VK_ABI_STAMP ? "STAMP" : "LEGACY", path, why);
	return true;
}

void
resolve_once(void)
{
	wchar_t path[MAX_PATH];

	// (0) Dev/support override. LEIA_VK_WEAVER_DLL pins an absolute DLL path;
	//     LEIA_VK_WEAVER_ABI selects "stamp" or "legacy" for it. Intended for
	//     A/B-ing a candidate weaver on a real panel without reinstalling SR.
	if (const char *env = std::getenv("LEIA_VK_WEAVER_DLL"); env != nullptr && env[0] != '\0') {
		if (MultiByteToWideChar(CP_UTF8, 0, env, -1, path, MAX_PATH) > 0) {
			const char *abi_env = std::getenv("LEIA_VK_WEAVER_ABI");
			const bool legacy = abi_env != nullptr && _stricmp(abi_env, "legacy") == 0;
			if (try_backend(path, legacy ? LEIA_VK_ABI_LEGACY : LEIA_VK_ABI_STAMP, "LEIA_VK_WEAVER_DLL override")) {
				g_ok = true;
				return;
			}
			U_LOG_W("SR VK weaver: LEIA_VK_WEAVER_DLL='%s' unusable - falling through", env);
		}
	}

	// (1) SR >= 1.37 installs its own Vulkan weaver next to the D3D/GL ones.
	//     Prefer it unconditionally: it is matched to its core by construction,
	//     it is supported by Leia, and it means we ship no weaver at all on
	//     current machines. This is exactly how D3D has always worked, and why
	//     D3D never suffered the stamp break.
	if (find_on_search_path(kMachineWeaverDll, path, MAX_PATH) &&
	    try_backend(path, LEIA_VK_ABI_STAMP, "machine-installed weaver, SR >= 1.37")) {
		g_ok = true;
		return;
	}

	// (2) SR 1.36.x ships no Vulkan weaver but its core already emits the stamp,
	//     so the bundled stamp-aware build is required. Without this branch a
	//     1.36 machine renders the coloured replica.
	if (core_emits_pixel_stamp() && path_beside_self(kBundledStampDll, path, MAX_PATH) &&
	    try_backend(path, LEIA_VK_ABI_STAMP, "bundled stamp-aware weaver, SR 1.36.x")) {
		g_ok = true;
		return;
	}

	// (3) Pre-stamp core (SR <= 1.34.x): the long-shipped weaver is correct, and
	//     a stamp-aware one would be wrong here.
	if (path_beside_self(kBundledLegacyDll, path, MAX_PATH) &&
	    try_backend(path, LEIA_VK_ABI_LEGACY, "bundled legacy weaver, pre-stamp core")) {
		g_ok = true;
		return;
	}

	U_LOG_E("SR VK weaver: no usable weaver found - Vulkan weaving unavailable");
}

} // namespace

const struct leia_vk_weaver_backend *
leia_vk_weaver_select_backend(void)
{
	std::call_once(g_once, [] {
		resolve_once();
		g_resolved = true;
	});
	(void)g_resolved;
	return g_ok ? &g_backend : nullptr;
}
