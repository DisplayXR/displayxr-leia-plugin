// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Stand-in for one SR client DLL, for test_sr_delayload_win.
 *
 * Built once per SR client DLL name. SR_STUB_NAME prefixes its exports, and
 * SR_STUB_FULL adds `<name>_added`, the export an older SR runtime lacks: the
 * test links against the FULL import libraries and runs against a mix of full
 * and short DLLs, which is exactly an SR runtime version mismatch.
 */

#define SR_STUB_CAT2(a, b) a##b
#define SR_STUB_CAT(a, b) SR_STUB_CAT2(a, b)

__declspec(dllexport) int
SR_STUB_CAT(SR_STUB_NAME, _present)(void)
{
	return 1;
}

#ifdef SR_STUB_FULL
__declspec(dllexport) int
SR_STUB_CAT(SR_STUB_NAME, _added)(void)
{
	return 2;
}
#endif
