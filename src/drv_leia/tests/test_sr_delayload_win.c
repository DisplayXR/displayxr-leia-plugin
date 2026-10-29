// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the SR client DLL bind (leia_sr_delayload_win.c):
 *         the required/per-backend split and the failure hook that names the
 *         missing DLL or export.
 *
 * Links leia_sr_delayload_win.c with four stub DLLs carrying the SR client
 * DLL names (tests/sr_stub_dll.c), delay-loaded exactly as in the plug-in.
 * It links against the FULL stubs' import libraries; each scenario copies a
 * mix of full and short (one export fewer) stubs into a work directory, points
 * DXR_LEIA_SR_DIR at it and checks the bind results and the WARN text. One
 * scenario per process, because the binds latch.
 *
 * Usage: test_sr_delayload_win <scenario> <full stub dir> <short stub dir> <work dir>
 */

#include "leia_platform_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The stubs' exports. Referencing them puts each DLL in the delay-import table.
__declspec(dllimport) int core_present(void);
__declspec(dllimport) int core_added(void);
__declspec(dllimport) int displays_present(void);
__declspec(dllimport) int displays_added(void);
__declspec(dllimport) int dx_present(void);
__declspec(dllimport) int dx_added(void);
__declspec(dllimport) int gl_present(void);
__declspec(dllimport) int gl_added(void);

static int (*volatile const g_refs[])(void) = {
    core_present, core_added, displays_present, displays_added, dx_present, dx_added, gl_present, gl_added,
};

static char g_log[8192];
static int g_failures;

void
test_log_w(const char *fmt, ...)
{
	size_t n = strlen(g_log);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_log + n, sizeof(g_log) - n, fmt, ap);
	va_end(ap);
	n = strlen(g_log);
	if (n + 1 < sizeof(g_log)) {
		g_log[n] = '\n';
		g_log[n + 1] = '\0';
	}
}

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

#define CHECK_LOG(text) CHECK(strstr(g_log, (text)) != NULL)

static bool
stage(const char *from_dir, const char *work_dir, const char *dll)
{
	char src[MAX_PATH];
	char dst[MAX_PATH];
	snprintf(src, sizeof(src), "%s\\%s", from_dir, dll);
	snprintf(dst, sizeof(dst), "%s\\%s", work_dir, dll);
	if (!CopyFileA(src, dst, FALSE)) {
		fprintf(stderr, "copy %s -> %s failed (%lu)\n", src, dst, GetLastError());
		return false;
	}
	return true;
}

int
main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s <scenario> <full dir> <short dir> <work dir>\n", argv[0]);
		return 2;
	}
	const char *scenario = argv[1];
	const char *full = argv[2];
	const char *shrt = argv[3];
	const char *work = argv[4];

	// Fresh work dir: only the DLLs this scenario stages.
	CreateDirectoryA(work, NULL);
	const char *all[] = {"SimulatedRealityCore.dll", "SimulatedRealityDisplays.dll", "SimulatedRealityDirectX.dll",
	                     "SimulatedRealityOpenGL.dll"};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		char p[MAX_PATH];
		snprintf(p, sizeof(p), "%s\\%s", work, all[i]);
		DeleteFileA(p);
	}

	// A DLL missing from the SR dir falls back to the normal search; keep a
	// real SR install on PATH out of it.
	char sys[MAX_PATH];
	GetSystemDirectoryA(sys, MAX_PATH);
	SetEnvironmentVariableA("PATH", sys);
	_putenv_s("DXR_LEIA_SR_DIR", work);

	bool ok = true;
	if (strcmp(scenario, "backend_export") == 0) {
		// SR runtime whose OpenGL DLL lacks an export: only GL declines.
		ok = stage(full, work, all[0]) && stage(full, work, all[1]) && stage(full, work, all[2]) &&
		     stage(shrt, work, all[3]);
		if (ok) {
			CHECK(leia_sr_client_bind() == LEIA_SR_BIND_OK);
			CHECK(leia_sr_dx_bind());
			CHECK(!leia_sr_gl_bind());
			CHECK(!leia_sr_gl_bind()); // retried, but logged once
			CHECK(core_present() == 1 && displays_added() == 2 && dx_added() == 2);
			CHECK_LOG("client DLLs bound (2 required DLLs");
			CHECK_LOG("SimulatedRealityOpenGL.dll is missing export gl_added (SR platform too old/new) "
			          "(code 0xc06d007f)");
			CHECK_LOG("the OpenGL display processors decline");
			const char *first = strstr(g_log, "SimulatedRealityOpenGL.dll is missing");
			CHECK(first != NULL && strstr(first + 1, "SimulatedRealityOpenGL.dll is missing") == NULL);
			CHECK(strstr(g_log, "DirectX") == NULL);
		}
	} else if (strcmp(scenario, "required_export") == 0) {
		// Displays lacks an export: the plug-in-wide INCOMPATIBLE, named.
		ok = stage(full, work, all[0]) && stage(shrt, work, all[1]) && stage(full, work, all[2]) &&
		     stage(full, work, all[3]);
		if (ok) {
			CHECK(leia_sr_client_bind() == LEIA_SR_BIND_INCOMPATIBLE);
			CHECK(!leia_sr_client_bound());
			CHECK(!leia_sr_dx_bind());
			CHECK(!leia_sr_gl_bind());
			CHECK_LOG("client DLLs NOT usable");
			CHECK_LOG("SimulatedRealityDisplays.dll is missing export displays_added (SR platform too old/new) "
			          "(code 0xc06d007f)");
		}
	} else if (strcmp(scenario, "backend_absent") == 0) {
		// No DirectX DLL at all: D3D declines, GL still binds.
		ok = stage(full, work, all[0]) && stage(full, work, all[1]) && stage(full, work, all[3]);
		if (ok) {
			CHECK(leia_sr_client_bind() == LEIA_SR_BIND_OK);
			CHECK(!leia_sr_dx_bind());
			CHECK(leia_sr_gl_bind());
			CHECK(gl_added() == 2);
			CHECK_LOG("SimulatedRealityDirectX.dll could not be loaded (error 126) (code 0xc06d007e)");
			CHECK_LOG("the D3D11/D3D12 display processors decline");
		}
	} else {
		fprintf(stderr, "unknown scenario '%s'\n", scenario);
		return 2;
	}
	(void)g_refs;

	fprintf(stderr, "--- captured log (%s) ---\n%s", scenario, g_log);
	if (!ok) {
		return 2;
	}
	if (g_failures != 0) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("test_sr_delayload_win %s: OK\n", scenario);
	return 0;
}
