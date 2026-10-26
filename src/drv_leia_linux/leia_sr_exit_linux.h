// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  When the process-wide SR context is torn down at process exit, and
 *         when the teardown must not touch SR at all.
 *
 * Pure decision logic, no SDK calls, so it is unit-testable without hardware
 * (tests/test_sr_exit_linux.c). leia_sr_linux_sdk.c is the only caller; every
 * function here runs under its g_ctx_lock.
 *
 * ## The bug this exists for (DisplayXR runtime #2311 DS1 runs, 2026-10-26)
 *
 * The SR context used to be torn down only by an ELF destructor
 * (`__attribute__((destructor)) sr_ctx_shutdown`). At process exit that is the
 * wrong moment: `exit()` runs every `atexit` / `__cxa_atexit` handler first and
 * `_dl_fini` (which runs ELF destructors) last, because ld.so registered
 * `_dl_fini` at process start, before anything else. SR's C++ statics are
 * registered with `__cxa_atexit` long after that (SR is dlopen'ed when the
 * plug-in first creates a context), so they are all destroyed BEFORE the
 * plug-in's destructor runs. `srDestroyInstance` -> `~SRContext` then erases
 * the context from `startedContexts()::contexts`, a function-local
 * `std::set<const SRContext*>` in libSimulatedRealityCore.so that is already
 * gone, and frees its node a second time. ASan reports this double free on
 * EVERY exit with a READY context, with or without a display hotplug. Under
 * glibc the outcome depends on whether the freed node was reused in between:
 * clean, "double free detected in tcache", "double free or corruption
 * (!prev)" or "corrupted size vs. prev_size".
 *
 * ## The rule
 *
 * Tear the context down from an `atexit` hook that is registered AFTER SR's
 * statics, so it runs BEFORE them (exit handlers run in reverse order of
 * registration). A static is registered when it is first used, so the hook is
 * (re-)armed right after each SR entry point that first pulls statics in:
 * every context creation (generation), the first weaver, and the first
 * display enumeration. A hook armed more than once runs more than once; only
 * the first run acts. The ELF destructor stays for a real `dlclose()` (no
 * exit: SR's statics are alive) and does nothing once the hook has run.
 *
 * At exit the hook still leaves SR alone in two cases, because the process is
 * going away anyway and SR would be the risk, not the leak:
 *  - a weaver is still alive on the context (a session that never ended: a
 *    render thread may be inside `srWeaverWeave` right now, which no plug-in
 *    lock covers);
 *  - the context was invalidated (SRService gone): `srDestroyInstance` joins
 *    SDK threads and can block forever on a dead connection (R-W10).
 *
 * Once the hook has run no new context may be created: a straggler thread
 * that asks for one would build it while SR's statics are being destroyed.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The SR entry points that first pull SR statics in (see "The rule").
enum leia_sr_exit_site
{
	LEIA_SR_EXIT_SITE_CONTEXT = 0, //!< a context became READY (re-armed per context generation)
	LEIA_SR_EXIT_SITE_WEAVER = 1,  //!< the first weaver of the process was created
	LEIA_SR_EXIT_SITE_DISPLAY_ENUM = 2, //!< the first srEnumerateDisplays of the process answered
};

//! What a teardown entry point should do with the context.
enum leia_sr_exit_action
{
	LEIA_SR_EXIT_NOTHING = 0,          //!< no READY context, or the hook already acted
	LEIA_SR_EXIT_TEARDOWN,             //!< destroy every SR handle now
	LEIA_SR_EXIT_LEAVE_WEAVER_ALIVE,   //!< a weaver lives on it: no SR call, the process is exiting
	LEIA_SR_EXIT_LEAVE_CONTEXT_INVALID //!< SRService is gone: srDestroyInstance could block, no SR call
};

struct leia_sr_exit
{
	//! The atexit hook has run: the process is exiting (or the .so is being finalised).
	bool hook_ran;
	//! Context generation SITE_CONTEXT last armed for (0 = none).
	uint32_t armed_generation;
	//! Bit per once-per-process site already armed.
	uint32_t armed_sites;
	//! Registrations requested so far (diagnostics, tests).
	uint32_t arm_count;
};

/*!
 * Should the exit hook be registered (again) now, after @p site? True at most
 * once per context generation for SITE_CONTEXT and once per process for the
 * other sites; never after the hook has run. Marks the site armed.
 */
static inline bool
leia_sr_exit_should_arm(struct leia_sr_exit *s, enum leia_sr_exit_site site, uint32_t generation)
{
	if (s == NULL || s->hook_ran) {
		return false;
	}
	if (site == LEIA_SR_EXIT_SITE_CONTEXT) {
		if (generation == 0 || generation == s->armed_generation) {
			return false;
		}
		s->armed_generation = generation;
	} else {
		const uint32_t bit = 1u << (uint32_t)site;
		if ((s->armed_sites & bit) != 0) {
			return false;
		}
		s->armed_sites |= bit;
	}
	s->arm_count++;
	return true;
}

/*!
 * The atexit hook ran. Only its first run acts (it may be registered several
 * times); every run marks the process as exiting.
 */
static inline enum leia_sr_exit_action
leia_sr_exit_on_hook(struct leia_sr_exit *s, bool ctx_ready, uint32_t live_weavers, bool ctx_invalid)
{
	if (s == NULL) {
		return LEIA_SR_EXIT_NOTHING;
	}
	const bool first = !s->hook_ran;
	s->hook_ran = true;
	if (!first || !ctx_ready) {
		return LEIA_SR_EXIT_NOTHING;
	}
	if (live_weavers > 0) {
		return LEIA_SR_EXIT_LEAVE_WEAVER_ALIVE;
	}
	if (ctx_invalid) {
		return LEIA_SR_EXIT_LEAVE_CONTEXT_INVALID;
	}
	return LEIA_SR_EXIT_TEARDOWN;
}

/*!
 * The ELF destructor ran: a real dlclose() (tear down as before), or the tail
 * of process exit after the hook (nothing: SR's statics may be gone).
 */
static inline enum leia_sr_exit_action
leia_sr_exit_on_unload(const struct leia_sr_exit *s, bool ctx_ready)
{
	if (s != NULL && s->hook_ran) {
		return LEIA_SR_EXIT_NOTHING;
	}
	return ctx_ready ? LEIA_SR_EXIT_TEARDOWN : LEIA_SR_EXIT_NOTHING;
}

//! May a context be created now? Not once the exit hook has run.
static inline bool
leia_sr_exit_allows_create(const struct leia_sr_exit *s)
{
	return s == NULL || !s->hook_ran;
}

#ifdef __cplusplus
}
#endif
