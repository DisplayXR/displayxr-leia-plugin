// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  dxr-rm-close: a tiny Windows Restart Manager driver for the NSIS
 *         installer (NSIS has no Restart Manager plug-in).
 *
 * Generic Windows only — it knows nothing about DisplayXR, the service or the
 * plug-in. The installer hands it the files it is about to replace or delete;
 * Restart Manager finds whoever has them mapped, closes those processes and,
 * after the installer is done, restarts the ones that registered for it
 * (RegisterApplicationRestart). See docs/installer.md.
 *
 * Restart Manager only lets the PRIMARY installer — the process that called
 * RmStartSession — call RmShutdown/RmRestart, so the two phases cannot be two
 * separate runs of this tool. Instead one `session` process spans both phases
 * and talks to the installer through files in a state directory:
 *
 *   dxr-rm-close.exe session <state-dir> <file> [<file> ...]
 *       1. RmStartSession, RmRegisterResources(<files that exist>), RmGetList,
 *          RmShutdown(RmForceShutdown).
 *       2. Writes <state-dir>\closed.txt  (first line = result code, below).
 *       3. Waits for <state-dir>\go.txt: "restart" (RmRestart) or "end" (no
 *          restart). No go.txt within 15 min => restart (never leave the box
 *          with its processes closed because an installer died).
 *       4. RmEndSession, writes <state-dir>\done.txt (result code).
 *     A human-readable trace goes to <state-dir>\log.txt (the installer
 *     DetailPrints it).
 *
 *   dxr-rm-close.exe list <file> [<file> ...]
 *       RmGetList only — prints the holders, closes nothing. Diagnostics.
 *
 *   dxr-rm-close.exe is-running <image.exe> [<wait-ms>]
 *       Exit 0 if a process with that image name runs (waiting up to wait-ms
 *       for one to appear), 1 if not.
 *
 * Result codes (closed.txt / done.txt / exit code):
 *   0   success: holders were closed (closed.txt) / restarted (done.txt)
 *   10  nothing held the files: nothing was closed, nothing will be restarted
 *   1   Restart Manager failed (the installer falls back to its own handling)
 *   2   usage error
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <restartmanager.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr int kRcOk = 0;
constexpr int kRcFail = 1;
constexpr int kRcUsage = 2;
constexpr int kRcNothingHeld = 10;

constexpr DWORD kGoTimeoutMs = 15 * 60 * 1000;
constexpr DWORD kPollMs = 100;

std::wstring g_log_path;

void
logf(const wchar_t *fmt, ...)
{
	wchar_t buf[2048];
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);

	fwprintf(stdout, L"%ls\n", buf);
	fflush(stdout);
	if (g_log_path.empty()) {
		return;
	}
	FILE *f = nullptr;
	// UTF-8 bytes; NSIS reads the file line by line for DetailPrint.
	if (_wfopen_s(&f, g_log_path.c_str(), L"ab") == 0 && f != nullptr) {
		char u8[4096];
		int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8), nullptr, nullptr);
		if (n > 1) {
			fwrite(u8, 1, (size_t)(n - 1), f);
			fwrite("\r\n", 1, 2, f);
		}
		fclose(f);
	}
}

//! Write a small marker file atomically (write .tmp, then rename over).
bool
write_marker(const std::wstring &path, int code, const wchar_t *detail)
{
	std::wstring tmp = path + L".tmp";
	FILE *f = nullptr;
	if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || f == nullptr) {
		return false;
	}
	char u8[512] = {};
	WideCharToMultiByte(CP_UTF8, 0, detail != nullptr ? detail : L"", -1, u8, sizeof(u8), nullptr, nullptr);
	fprintf(f, "%d\r\n%s\r\n", code, u8);
	fclose(f);
	return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool
file_exists(const std::wstring &p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

const wchar_t *
app_type_str(RM_APP_TYPE t)
{
	switch (t) {
	case RmMainWindow: return L"main-window";
	case RmOtherWindow: return L"other-window";
	case RmService: return L"service";
	case RmExplorer: return L"explorer";
	case RmConsole: return L"console";
	case RmCritical: return L"CRITICAL";
	default: return L"unknown";
	}
}

/*!
 * RmGetList with the usual grow-and-retry. Returns false on an RM error.
 */
bool
get_list(DWORD session, std::vector<RM_PROCESS_INFO> &out, DWORD *reasons)
{
	out.clear();
	for (int attempt = 0; attempt < 5; attempt++) {
		UINT needed = 0;
		UINT have = (UINT)out.size();
		DWORD r = RmGetList(session, &needed, &have, out.empty() ? nullptr : out.data(), reasons);
		if (r == ERROR_SUCCESS) {
			out.resize(have);
			return true;
		}
		if (r != ERROR_MORE_DATA) {
			logf(L"RmGetList failed: error %lu", (unsigned long)r);
			return false;
		}
		out.resize(needed + 4); // the list can grow between calls
	}
	logf(L"RmGetList kept growing: giving up");
	return false;
}

void
log_list(const std::vector<RM_PROCESS_INFO> &apps, DWORD reasons)
{
	logf(L"%u process(es) hold the files (reboot reasons 0x%lx):", (unsigned)apps.size(), (unsigned long)reasons);
	for (const RM_PROCESS_INFO &a : apps) {
		logf(L"  pid %lu  %ls  [%ls]  type=%ls restartable=%d status=0x%lx", (unsigned long)a.Process.dwProcessId,
		     a.strAppName, a.strServiceShortName[0] != L'\0' ? a.strServiceShortName : L"-",
		     app_type_str(a.ApplicationType), a.bRestartable ? 1 : 0, (unsigned long)a.AppStatus);
	}
}

/*!
 * Start a session and register the files that exist. Returns false on an RM
 * error; *out_registered = number of files registered (0 = none exist).
 */
bool
open_session(const std::vector<std::wstring> &files, DWORD *out_session, size_t *out_registered)
{
	WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
	DWORD session = 0;
	DWORD r = RmStartSession(&session, 0, key);
	if (r != ERROR_SUCCESS) {
		logf(L"RmStartSession failed: error %lu", (unsigned long)r);
		return false;
	}
	std::vector<LPCWSTR> existing;
	for (const std::wstring &f : files) {
		if (file_exists(f)) {
			existing.push_back(f.c_str());
			logf(L"register: %ls", f.c_str());
		} else {
			logf(L"skip (not present): %ls", f.c_str());
		}
	}
	if (!existing.empty()) {
		r = RmRegisterResources(session, (UINT)existing.size(), existing.data(), 0, nullptr, 0, nullptr);
		if (r != ERROR_SUCCESS) {
			logf(L"RmRegisterResources failed: error %lu", (unsigned long)r);
			RmEndSession(session);
			return false;
		}
	}
	*out_session = session;
	*out_registered = existing.size();
	return true;
}

int
cmd_list(const std::vector<std::wstring> &files)
{
	DWORD session = 0;
	size_t registered = 0;
	if (!open_session(files, &session, &registered)) {
		return kRcFail;
	}
	std::vector<RM_PROCESS_INFO> apps;
	DWORD reasons = 0;
	bool ok = registered == 0 || get_list(session, apps, &reasons);
	if (ok) {
		log_list(apps, reasons);
	}
	RmEndSession(session);
	return !ok ? kRcFail : (apps.empty() ? kRcNothingHeld : kRcOk);
}

//! Read the first token of go.txt. Empty string when absent.
std::wstring
read_go(const std::wstring &path)
{
	FILE *f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) {
		return L"";
	}
	char buf[64] = {};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	std::wstring w;
	for (size_t i = 0; i < n; i++) {
		char c = buf[i];
		if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
			if (!w.empty()) {
				break;
			}
			continue;
		}
		w.push_back((wchar_t)c);
	}
	return w;
}

int
cmd_session(const std::wstring &dir, const std::vector<std::wstring> &files)
{
	CreateDirectoryW(dir.c_str(), nullptr);
	g_log_path = dir + L"\\log.txt";
	const std::wstring closed = dir + L"\\closed.txt";
	const std::wstring go = dir + L"\\go.txt";
	const std::wstring done = dir + L"\\done.txt";
	DeleteFileW(closed.c_str());
	DeleteFileW(done.c_str());

	DWORD session = 0;
	size_t registered = 0;
	if (!open_session(files, &session, &registered)) {
		write_marker(closed, kRcFail, L"RmStartSession/RmRegisterResources failed");
		write_marker(done, kRcFail, L"no session");
		return kRcFail;
	}

	std::vector<RM_PROCESS_INFO> apps;
	DWORD reasons = 0;
	if (registered == 0 || (get_list(session, apps, &reasons) && apps.empty())) {
		logf(L"nothing holds the files: nothing to close");
		RmEndSession(session);
		write_marker(closed, kRcNothingHeld, L"nothing held");
		write_marker(done, kRcNothingHeld, L"nothing to restart");
		return kRcNothingHeld;
	}
	bool listed = !apps.empty();
	if (listed) {
		log_list(apps, reasons);
	}

	// RmForceShutdown: a holder with a non-pumping top-level window (seen with
	// 3rd-party libraries) otherwise makes RM give up at once with
	// ERROR_FAIL_SHUTDOWN and never restart it. Well-behaved holders still exit
	// on the close query long before RM's force timeout.
	DWORD t0 = GetTickCount();
	DWORD r = RmShutdown(session, RmForceShutdown, nullptr);
	int close_rc = kRcOk;
	if (r == ERROR_SUCCESS) {
		logf(L"RmShutdown(RmForceShutdown): success in %lu ms", (unsigned long)(GetTickCount() - t0));
	} else {
		logf(L"RmShutdown(RmForceShutdown): error %lu after %lu ms", (unsigned long)r,
		     (unsigned long)(GetTickCount() - t0));
		close_rc = kRcFail;
	}
	if (get_list(session, apps, &reasons)) {
		log_list(apps, reasons); // AppStatus now says what happened to each
	}
	write_marker(closed, close_rc, close_rc == kRcOk ? L"closed" : L"RmShutdown failed");

	// Phase 2: wait for the installer to say what to do.
	std::wstring what;
	DWORD waited = 0;
	while (waited < kGoTimeoutMs) {
		what = read_go(go);
		if (!what.empty()) {
			break;
		}
		Sleep(kPollMs);
		waited += kPollMs;
	}
	if (what.empty()) {
		logf(L"no go.txt after %lu s: restarting anyway", (unsigned long)(kGoTimeoutMs / 1000));
		what = L"restart";
	}

	int done_rc = kRcOk;
	if (what == L"restart") {
		// Microsoft: restart even when RmShutdown reported an error — RM
		// restarts exactly what it closed and what registered for restart.
		r = RmRestart(session, 0, nullptr);
		if (r == ERROR_SUCCESS) {
			logf(L"RmRestart: success");
		} else {
			logf(L"RmRestart: error %lu", (unsigned long)r);
			done_rc = kRcFail;
		}
		if (get_list(session, apps, &reasons)) {
			log_list(apps, reasons);
		}
	} else {
		logf(L"go = '%ls': ending the session without a restart", what.c_str());
	}
	RmEndSession(session);
	write_marker(done, done_rc, what.c_str());
	return done_rc;
}

bool
image_running(const wchar_t *image)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return false;
	}
	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	bool found = false;
	for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
		found = _wcsicmp(pe.szExeFile, image) == 0;
	}
	CloseHandle(snap);
	return found;
}

int
cmd_is_running(const wchar_t *image, DWORD wait_ms)
{
	DWORD waited = 0;
	for (;;) {
		if (image_running(image)) {
			logf(L"%ls is running", image);
			return kRcOk;
		}
		if (waited >= wait_ms) {
			logf(L"%ls is not running", image);
			return kRcFail;
		}
		Sleep(kPollMs);
		waited += kPollMs;
	}
}

int
usage()
{
	fwprintf(stderr,
	         L"usage:\n"
	         L"  dxr-rm-close session <state-dir> <file> [<file> ...]\n"
	         L"  dxr-rm-close list <file> [<file> ...]\n"
	         L"  dxr-rm-close is-running <image.exe> [<wait-ms>]\n");
	return kRcUsage;
}

} // namespace

int
wmain(int argc, wchar_t **argv)
{
	if (argc < 2) {
		return usage();
	}
	const std::wstring cmd = argv[1];
	if (cmd == L"session" && argc >= 4) {
		std::vector<std::wstring> files(argv + 3, argv + argc);
		return cmd_session(argv[2], files);
	}
	if (cmd == L"list" && argc >= 3) {
		std::vector<std::wstring> files(argv + 2, argv + argc);
		return cmd_list(files);
	}
	if (cmd == L"is-running" && (argc == 3 || argc == 4)) {
		DWORD wait_ms = argc == 4 ? (DWORD)wcstoul(argv[3], nullptr, 10) : 0;
		return cmd_is_running(argv[2], wait_ms);
	}
	return usage();
}
