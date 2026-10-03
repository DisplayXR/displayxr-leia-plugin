// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Test-only process for dxr-rm-close (never shipped; EXCLUDE_FROM_ALL).
 *
 *   rm-test-holder <file> <marker> [--register]
 *
 * Holds <file> open with no sharing, appends one line "<pid> started<args>" to
 * <marker>, optionally registers for a Restart Manager restart (with the same
 * arguments), and exits cleanly on the Restart Manager close query
 * (WM_QUERYENDSESSION + ENDSESSION_CLOSEAPP), WM_ENDSESSION or WM_CLOSE —
 * the same contract the DisplayXR service implements.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>

static LRESULT CALLBACK
wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m) {
	case WM_QUERYENDSESSION:
		if ((l & ENDSESSION_CLOSEAPP) != 0) {
			PostQuitMessage(0);
		}
		return TRUE;
	case WM_ENDSESSION:
		if (w) {
			PostQuitMessage(0);
		}
		return 0;
	case WM_CLOSE: PostQuitMessage(0); return 0;
	default: return DefWindowProcW(h, m, w, l);
	}
}

int
wmain(int argc, wchar_t **argv)
{
	if (argc < 3) {
		return 2;
	}
	const bool reg = argc >= 4 && wcscmp(argv[3], L"--register") == 0;

	FILE *mf = nullptr;
	if (_wfopen_s(&mf, argv[2], L"a") == 0 && mf != nullptr) {
		fwprintf(mf, L"%lu started%ls\n", GetCurrentProcessId(), reg ? L" (registered)" : L"");
		fclose(mf);
	}

	HANDLE f = CreateFileW(argv[1], GENERIC_READ, 0 /* no sharing */, nullptr, OPEN_ALWAYS, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		return 3;
	}
	if (reg) {
		wchar_t args[1024];
		_snwprintf_s(args, _countof(args), _TRUNCATE, L"\"%ls\" \"%ls\" --register", argv[1], argv[2]);
		HRESULT hr = RegisterApplicationRestart(args, RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT);
		if (_wfopen_s(&mf, argv[2], L"a") == 0 && mf != nullptr) {
			fwprintf(mf, L"%lu RegisterApplicationRestart hr=0x%08lx\n", GetCurrentProcessId(), (unsigned long)hr);
			fclose(mf);
		}
	}

	WNDCLASSW wc = {};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"RmTestHolder";
	RegisterClassW(&wc);
	CreateWindowExW(0, wc.lpszClassName, L"rm-test-holder", WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr,
	                wc.hInstance, nullptr);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	CloseHandle(f);
	return 0;
}
