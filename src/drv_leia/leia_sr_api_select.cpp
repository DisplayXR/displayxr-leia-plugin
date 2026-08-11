// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Implementation of the SR v1/v2 API selector. See the header for why
 *         this is a single decision point rather than a per-call-site check.
 * @ingroup drv_leia
 */

#include "leia_sr_api_select.h"

#include "util/u_logging.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef DXR_LEIA_HAS_SR_V2
#include <sr/sr_instance.h>
#include <sr/sr_result.h>
#include <sr/sr_version.h>
#endif

namespace {

enum leia_sr_api g_api = LEIA_SR_API_V1;
char g_reason[192] = "unresolved";
std::once_flag g_once;

void
set_reason(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_reason, sizeof(g_reason), fmt, ap);
	va_end(ap);
}

#ifdef DXR_LEIA_HAS_SR_V2
/*!
 * Is the v2 runtime actually usable on this machine?
 *
 * The loader is a static lib that `LoadLibrary`s `LeiaSR_runtime.dll` on first
 * use, so "can I create an instance?" is the honest probe — a machine with an
 * older SR Platform has no such DLL and fails here rather than at some later,
 * less obvious call. The instance is destroyed immediately; this only answers
 * the availability question. The real instance is created by whoever needs it.
 *
 * Deliberately NOT `srInitialize`d: initialisation starts trackers and can
 * touch the lens, and a capability probe must not have side effects a user
 * could see. Creation alone proves the runtime loaded and speaks our API
 * version, which is the whole question.
 */
bool
v2_runtime_usable(char *version_out, size_t version_cap)
{
	SrInstanceCreateInfo ci{};
	ci.sType = SR_TYPE_INSTANCE_CREATE_INFO;
	ci.pNext = nullptr;
	ci.apiVersion = SR_CURRENT_API_VERSION;
	ci.networkMode = SR_NETWORK_MODE_STANDALONE;

	SrInstance inst = nullptr;
	const SrResult r = srCreateInstance(&ci, &inst);
	if (!SR_SUCCEEDED(r) || inst == nullptr) {
		snprintf(version_out, version_cap, "srCreateInstance failed (%d)", static_cast<int>(r));
		return false;
	}

	// Best-effort: the version string makes the log line diagnostic rather than
	// merely affirmative. A failure here does not disqualify the runtime.
	char ver[96] = {0};
	if (!SR_SUCCEEDED(srGetRuntimeVersion(inst, ver, static_cast<uint32_t>(sizeof(ver)))) || ver[0] == '\0') {
		snprintf(ver, sizeof(ver), "version unknown");
	}
	snprintf(version_out, version_cap, "%s", ver);

	srDestroyInstance(inst);
	return true;
}
#endif // DXR_LEIA_HAS_SR_V2

void
resolve_once(void)
{
	// (0) Explicit override. This exists so BOTH paths are testable on one
	//     binary — every arm migration is verified with =v1 and =v2 and the
	//     results compared. Without it, exercising v1 would mean uninstalling
	//     a Platform, which is exactly the friction that lets a dual-path
	//     divergence go unnoticed.
	const char *env = std::getenv("DXR_LEIA_SR_API");
	if (env != nullptr && env[0] != '\0' && _stricmp(env, "auto") != 0) {
		if (_stricmp(env, "v1") == 0) {
			g_api = LEIA_SR_API_V1;
			set_reason("DXR_LEIA_SR_API=v1 (forced)");
			return;
		}
		if (_stricmp(env, "v2") == 0) {
#ifdef DXR_LEIA_HAS_SR_V2
			char ver[128] = {0};
			if (v2_runtime_usable(ver, sizeof(ver))) {
				g_api = LEIA_SR_API_V2;
				set_reason("DXR_LEIA_SR_API=v2 (forced), runtime %s", ver);
			} else {
				// Forced and unavailable is a bad state to paper over: the
				// operator asked for v2 and is about to silently measure v1.
				g_api = LEIA_SR_API_V1;
				set_reason("DXR_LEIA_SR_API=v2 FORCED BUT UNAVAILABLE (%s) - using v1", ver);
				U_LOG_E("SR API: v2 was forced but is unusable (%s). Falling back to v1 - "
				        "any A/B result from this run is NOT a v2 result.",
				        ver);
			}
#else
			g_api = LEIA_SR_API_V1;
			set_reason("DXR_LEIA_SR_API=v2 FORCED but plug-in built without v2 SDK - using v1");
			U_LOG_E("SR API: v2 was forced but this plug-in was built without the v2 SDK. "
			        "Falling back to v1 - any A/B result from this run is NOT a v2 result.");
#endif
			return;
		}
		U_LOG_W("SR API: DXR_LEIA_SR_API='%s' unrecognised (want v1|v2|auto) - resolving automatically", env);
	}

	// (1) Automatic. Prefer v2 when the runtime is actually there.
#ifdef DXR_LEIA_HAS_SR_V2
	char ver[128] = {0};
	if (v2_runtime_usable(ver, sizeof(ver))) {
		g_api = LEIA_SR_API_V2;
		set_reason("v2 runtime available (%s)", ver);
		return;
	}
	g_api = LEIA_SR_API_V1;
	set_reason("v2 runtime unavailable (%s) - SR Platform predates the C99 API", ver);
#else
	g_api = LEIA_SR_API_V1;
	set_reason("built without the v2 SDK (DXR_LEIA_HAS_SR_V2 undefined)");
#endif
}

} // namespace

extern "C" enum leia_sr_api
leia_sr_api_selected(void)
{
	std::call_once(g_once, [] {
		resolve_once();
		// Logged exactly once, at WARN so it survives the default filter. The
		// whole point of this layer is that "which path is live?" must never be
		// a question you answer by guessing.
		U_LOG_W("SR API: using %s - %s", g_api == LEIA_SR_API_V2 ? "v2 (C99)" : "v1 (legacy C++)", g_reason);
	});
	return g_api;
}

extern "C" const char *
leia_sr_api_reason(void)
{
	(void)leia_sr_api_selected(); // ensure resolved
	return g_reason;
}
