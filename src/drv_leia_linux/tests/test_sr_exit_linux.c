// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the exit-time SR teardown rules (leia_sr_exit_linux.h).
 *
 * Part 1 checks the pure rules. Part 2 checks the ordering they rely on, with
 * real exit handlers in a forked child: a "static" registered with atexit()
 * before the hook must still be alive when the hook runs, and the ELF
 * destructor that runs from _dl_fini afterwards must do nothing — the DS1
 * crash was exactly that destructor tearing down after the static was gone.
 */

#include "leia_sr_exit_linux.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

static void
test_arming(void)
{
	struct leia_sr_exit s = {0};
	// Generation 0 is "no context yet": nothing to arm for.
	CHECK(!leia_sr_exit_should_arm(&s, LEIA_SR_EXIT_SITE_CONTEXT, 0));
	// Every new context arms once.
	CHECK(leia_sr_exit_should_arm(&s, LEIA_SR_EXIT_SITE_CONTEXT, 1));
	CHECK(!leia_sr_exit_should_arm(&s, LEIA_SR_EXIT_SITE_CONTEXT, 1));
	CHECK(leia_sr_exit_should_arm(&s, LEIA_SR_EXIT_SITE_CONTEXT, 2)); // SRService restart: new context
	// The other sites arm once per process, whatever the generation.
	CHECK(leia_sr_exit_should_arm(&s, LEIA_SR_EXIT_SITE_WEAVER, 2));
	CHECK(!leia_sr_exit_should_arm(&s, LEIA_SR_EXIT_SITE_WEAVER, 2));
	CHECK(!leia_sr_exit_should_arm(&s, LEIA_SR_EXIT_SITE_WEAVER, 3));
	CHECK(leia_sr_exit_should_arm(&s, LEIA_SR_EXIT_SITE_DISPLAY_ENUM, 2));
	CHECK(!leia_sr_exit_should_arm(&s, LEIA_SR_EXIT_SITE_DISPLAY_ENUM, 2));
	CHECK(s.arm_count == 4);
	// Nothing arms once the hook has run (the process is exiting).
	(void)leia_sr_exit_on_hook(&s, false, 0, false);
	CHECK(!leia_sr_exit_should_arm(&s, LEIA_SR_EXIT_SITE_CONTEXT, 7));
	CHECK(s.arm_count == 4);
}

static void
test_hook_decisions(void)
{
	// A READY context, nothing alive on it: the hook tears it down, once.
	struct leia_sr_exit s = {0};
	CHECK(leia_sr_exit_on_hook(&s, true, 0, false) == LEIA_SR_EXIT_TEARDOWN);
	CHECK(leia_sr_exit_on_hook(&s, true, 0, false) == LEIA_SR_EXIT_NOTHING); // armed twice: second run idle
	// ...and the ELF destructor at the tail of exit does nothing.
	CHECK(leia_sr_exit_on_unload(&s, true) == LEIA_SR_EXIT_NOTHING);
	CHECK(leia_sr_exit_on_unload(&s, false) == LEIA_SR_EXIT_NOTHING);
	CHECK(!leia_sr_exit_allows_create(&s));

	// A weaver still alive (a session that never ended): no SR call at exit.
	struct leia_sr_exit w = {0};
	CHECK(leia_sr_exit_on_hook(&w, true, 1, false) == LEIA_SR_EXIT_LEAVE_WEAVER_ALIVE);
	CHECK(leia_sr_exit_on_unload(&w, true) == LEIA_SR_EXIT_NOTHING);

	// SRService gone: srDestroyInstance could block — leave it.
	struct leia_sr_exit d = {0};
	CHECK(leia_sr_exit_on_hook(&d, true, 0, true) == LEIA_SR_EXIT_LEAVE_CONTEXT_INVALID);
	// A live weaver wins over the invalid flag (either way: no SR call).
	struct leia_sr_exit wd = {0};
	CHECK(leia_sr_exit_on_hook(&wd, true, 2, true) == LEIA_SR_EXIT_LEAVE_WEAVER_ALIVE);

	// No context: nothing to do, but the process is still marked exiting.
	struct leia_sr_exit n = {0};
	CHECK(leia_sr_exit_on_hook(&n, false, 0, false) == LEIA_SR_EXIT_NOTHING);
	CHECK(!leia_sr_exit_allows_create(&n));

	// A real dlclose (no exit hook yet): the destructor tears down as before.
	struct leia_sr_exit u = {0};
	CHECK(leia_sr_exit_allows_create(&u));
	CHECK(leia_sr_exit_on_unload(&u, true) == LEIA_SR_EXIT_TEARDOWN);
	CHECK(leia_sr_exit_on_unload(&u, false) == LEIA_SR_EXIT_NOTHING);

	// NULL-safe.
	CHECK(!leia_sr_exit_should_arm(NULL, LEIA_SR_EXIT_SITE_CONTEXT, 1));
	CHECK(leia_sr_exit_on_hook(NULL, true, 0, false) == LEIA_SR_EXIT_NOTHING);
	CHECK(leia_sr_exit_allows_create(NULL));
}

/*
 * Part 2: the ordering, with real exit handlers (in a child process).
 *
 *   sr_static_ctor()  registers sr_static_dtor  (SR's startedContexts(), first used at context create)
 *   ctx_create()      arms the hook AFTER it    (sr_ctx_ensure -> LEIA_SR_EXIT_SITE_CONTEXT)
 *   exit(0)           -> hook (tears down: the static must still be alive)
 *                     -> sr_static_dtor
 *                     -> elf_destructor (_dl_fini: must do nothing)
 */
static struct leia_sr_exit g_child_exit;
static int g_static_alive;     //!< SR's static
static int g_ctx_ready;        //!< the plug-in's context
static int g_teardowns;        //!< teardowns that ran
static int g_teardown_on_dead; //!< teardowns that ran after the static was destroyed (the bug)

static void
child_teardown(void)
{
	g_teardowns++;
	if (!g_static_alive) {
		g_teardown_on_dead++;
	}
	g_ctx_ready = 0;
}

static void
child_hook(void)
{
	if (leia_sr_exit_on_hook(&g_child_exit, g_ctx_ready != 0, 0, false) == LEIA_SR_EXIT_TEARDOWN) {
		child_teardown();
	}
}

static void
sr_static_dtor(void)
{
	g_static_alive = 0;
}

static void
child_elf_destructor_and_report(void)
{
	// Stands in for the ELF destructor: registered FIRST, so it runs LAST,
	// exactly where _dl_fini runs relative to the handlers registered later.
	if (leia_sr_exit_on_unload(&g_child_exit, g_ctx_ready != 0) == LEIA_SR_EXIT_TEARDOWN) {
		child_teardown();
	}
	_exit((g_teardowns == 1 && g_teardown_on_dead == 0) ? 0 : 3);
}

/*!
 * One child run. @p arm = the fix (hook armed after the static); without it
 * the ELF destructor is the only teardown — the pre-fix plug-in — and must be
 * caught tearing down on a dead static (exit code 3), or this test proves
 * nothing.
 */
static void
test_exit_ordering(bool arm, int expected_code)
{
	const pid_t pid = fork();
	if (pid < 0) {
		CHECK(!"fork failed");
		return;
	}
	if (pid == 0) {
		atexit(child_elf_destructor_and_report); // ~ _dl_fini (registered at process start)
		g_static_alive = 1;
		atexit(sr_static_dtor); // SR's static, first used while the context is created
		g_ctx_ready = 1;
		if (arm && leia_sr_exit_should_arm(&g_child_exit, LEIA_SR_EXIT_SITE_CONTEXT, 1)) {
			atexit(child_hook);
		}
		// A later site re-arms; the second run must be idle.
		if (arm && leia_sr_exit_should_arm(&g_child_exit, LEIA_SR_EXIT_SITE_WEAVER, 1)) {
			atexit(child_hook);
		}
		exit(0);
	}
	int status = 0;
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == expected_code);
}

int
main(void)
{
	test_arming();
	test_hook_decisions();
	test_exit_ordering(true, 0);  // the fix: one teardown, static alive
	test_exit_ordering(false, 3); // the pre-fix order: teardown after the static died
	if (g_failures != 0) {
		fprintf(stderr, "test_sr_exit_linux: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_sr_exit_linux: all checks passed\n");
	return 0;
}
