// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  A stand-in conversion module for the provider's gate tests: a DLL
 *         named like the real one, with a VERSIONINFO resource
 *         (fake_module.rc.in) and NO conversion entry point. Built twice: one
 *         older than the provider's floor (must be refused without being
 *         loaded) and one new enough (loads, then fails for want of the entry
 *         point). Never shipped, never installed.
 */

#ifdef _WIN32
__declspec(dllexport)
#endif
int
fake_conversion_module_marker(void)
{
	return 42;
}
