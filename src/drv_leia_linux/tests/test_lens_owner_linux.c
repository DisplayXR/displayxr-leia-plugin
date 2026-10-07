// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the Linux lens-ownership rules (leia_lens_owner_linux.h).
 *
 * No SDK, no Vulkan, no panel: the rules are a small state machine, and the
 * point of keeping them out of leia_sr_linux_sdk.c is that they can be checked
 * on any box. Each case replays the SDK calls leiasr_lnx_request_display_mode()
 * and the context bring-up would make, against a model of LeiaSR #266's
 * ownership flag, and asserts both the calls and who owns the lens after them.
 */

#include "leia_lens_owner_linux.h"

#include <stdio.h>
#include <stdlib.h>

static int g_failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

/*! Model of one SR context on the SDK side (LeiaSR #266). */
struct fake_ctx
{
	bool app_owns; //!< SwitchableLensHintClient::applicationOwnsPreference
	int enables, disables;
	bool pref_on;   //!< the context's lens preference slot
	bool pref_set;  //!< false = never written by anyone
};

static void
fake_new_ctx(struct fake_ctx *c)
{
	*c = (struct fake_ctx){0};
}

/*! The weaver's write: ignored once the application owns the preference. */
static void
fake_weaver_vote(struct fake_ctx *c, bool on)
{
	if (!c->app_owns) {
		c->pref_on = on;
		c->pref_set = true;
	}
}

static void
fake_send(struct fake_ctx *c, struct leia_lens_owner *o, enum leia_lens_action a)
{
	if (a == LEIA_LENS_ACTION_NONE) {
		return;
	}
	c->app_owns = true; // first srLensEnable/Disable marks the context
	if (a == LEIA_LENS_ACTION_ENABLE) {
		c->enables++;
		c->pref_on = true;
	} else {
		c->disables++;
		c->pref_on = false;
	}
	c->pref_set = true;
	leia_lens_owner_commit(o, a);
}

static void
request(struct fake_ctx *c, struct leia_lens_owner *o, bool want_3d)
{
	fake_send(c, o, leia_lens_owner_on_request(o, want_3d));
}

static void
new_context(struct fake_ctx *c, struct leia_lens_owner *o)
{
	fake_new_ctx(c);
	fake_send(c, o, leia_lens_owner_on_new_context(o));
}

/* Startup makes no lens call; the session-begin 3D request is left to the weaver. */
static void
test_untoggled_session_leaves_the_weaver_in_charge(void)
{
	struct leia_lens_owner o = {0};
	struct fake_ctx c;
	new_context(&c, &o);
	CHECK(c.enables == 0 && c.disables == 0);
	CHECK(!c.app_owns);

	request(&c, &o, true); // xrBeginSession asks for the session's (3D) mode
	CHECK(c.enables == 0);
	CHECK(!c.app_owns);
	CHECK(!o.ctx_app_owned);

	// So the weaver keeps its off-panel release.
	fake_weaver_vote(&c, true);
	CHECK(c.pref_on);
	fake_weaver_vote(&c, false); // 500 ms off the panel
	CHECK(!c.pref_on);
}

/* The first 2D takes ownership; 3D afterwards must be SENT, the weaver will not. */
static void
test_first_2d_takes_ownership_and_3d_is_then_sent(void)
{
	struct leia_lens_owner o = {0};
	struct fake_ctx c;
	new_context(&c, &o);
	request(&c, &o, true);

	request(&c, &o, false);
	CHECK(c.disables == 1);
	CHECK(c.app_owns && o.ctx_app_owned);
	CHECK(o.last_sent == LEIA_LENS_REQ_2D);

	fake_weaver_vote(&c, true); // would re-light it pre-#266; now ignored
	CHECK(!c.pref_on);

	request(&c, &o, true);
	CHECK(c.enables == 1);
	CHECK(c.pref_on);
	CHECK(o.last_sent == LEIA_LENS_REQ_3D);
}

/* A new context re-applies the last request sent, whichever it was. */
static void
test_new_context_reapplies_last_request(void)
{
	struct leia_lens_owner o = {0};
	struct fake_ctx c;
	new_context(&c, &o);
	request(&c, &o, false);

	new_context(&c, &o); // SRService restarted: context invalidated + replaced
	CHECK(c.disables == 1 && c.enables == 0);
	CHECK(c.app_owns && o.ctx_app_owned);
	CHECK(!c.pref_on);
	fake_weaver_vote(&c, true);
	CHECK(!c.pref_on); // the app's 2D survives the restart

	request(&c, &o, true);
	new_context(&c, &o);
	CHECK(c.enables == 1 && c.disables == 0);
	CHECK(c.pref_on);
}

/* Nothing sent before the restart: nothing re-applied, the new weaver owns it. */
static void
test_new_context_without_history_stays_weaver_owned(void)
{
	struct leia_lens_owner o = {0};
	struct fake_ctx c;
	new_context(&c, &o);
	request(&c, &o, true);
	new_context(&c, &o);
	CHECK(c.enables == 0 && c.disables == 0);
	CHECK(!c.app_owns && !o.ctx_app_owned);
}

/* A failed SDK call is not remembered as sent. */
static void
test_uncommitted_call_is_not_recorded(void)
{
	struct leia_lens_owner o = {0};
	enum leia_lens_action a = leia_lens_owner_on_request(&o, false);
	CHECK(a == LEIA_LENS_ACTION_DISABLE);
	// (srLensDisable failed: no commit)
	CHECK(o.last_sent == LEIA_LENS_REQ_NONE);
	CHECK(leia_lens_owner_on_request(&o, true) == LEIA_LENS_ACTION_NONE);
	CHECK(leia_lens_owner_on_new_context(&o) == LEIA_LENS_ACTION_NONE);
}

/* Multi-screen M4: an EXTERNAL-routed weaver never votes, so the plug-in turns
 * the lens on at its creation and off after the last one is gone. */
static void
external_created(struct fake_ctx *c, struct leia_lens_owner *o)
{
	fake_send(c, o, leia_lens_owner_on_external_weaver_created(o));
}

static void
external_destroyed(struct fake_ctx *c, struct leia_lens_owner *o)
{
	const enum leia_lens_action a = leia_lens_owner_on_external_weaver_destroyed(o);
	if (a == LEIA_LENS_ACTION_NONE) {
		return;
	}
	CHECK(a == LEIA_LENS_ACTION_DISABLE);
	c->app_owns = true;
	c->disables++;
	c->pref_on = false;
	c->pref_set = true;
	leia_lens_owner_commit_release(o);
}

static void
test_external_weaver_turns_the_lens_on_and_releases_it(void)
{
	struct leia_lens_owner o = {0};
	struct fake_ctx c;
	new_context(&c, &o);

	external_created(&c, &o);
	CHECK(c.enables == 1 && c.pref_on && c.app_owns);
	CHECK(o.external_weavers == 1);

	// The session-begin 3D request: the context is ours now, so it is sent
	// (every request is, once owned — redundant, harmless).
	request(&c, &o, true);
	CHECK(c.enables == 2 && c.pref_on);

	// A second EXTERNAL weaver (a segment DP next to the window's own DP).
	external_created(&c, &o);
	CHECK(c.enables == 2 && o.external_weavers == 2); // already on from us: no call
	external_destroyed(&c, &o);
	CHECK(c.disables == 0 && c.pref_on); // one still weaving

	external_destroyed(&c, &o);
	CHECK(c.disables == 1 && !c.pref_on);
	CHECK(o.external_weavers == 0);
	// A release is not a 2D wish: nothing to re-apply to a later context ...
	CHECK(o.last_sent == LEIA_LENS_REQ_NONE);
	// ... and the next session's EXTERNAL weaver turns the lens back on.
	external_created(&c, &o);
	CHECK(c.enables == 3 && c.pref_on);
	external_destroyed(&c, &o);
	CHECK(c.disables == 2);
}

/* With an EXTERNAL weaver alive a 3D wish is sent even before any 2D. */
static void
test_external_weaver_3d_request_is_sent(void)
{
	struct leia_lens_owner o = {0};
	o.external_weavers = 1; // created while the enable failed (not committed)
	CHECK(leia_lens_owner_on_request(&o, true) == LEIA_LENS_ACTION_ENABLE);
}

/* An app 2D wish survives a new EXTERNAL weaver; the 3D request restores it. */
static void
test_external_weaver_respects_2d(void)
{
	struct leia_lens_owner o = {0};
	struct fake_ctx c;
	new_context(&c, &o);
	external_created(&c, &o);
	request(&c, &o, false); // V toggle
	CHECK(c.disables == 1 && !c.pref_on);

	external_created(&c, &o); // a segment DP comes up mid-2D
	CHECK(c.enables == 1 && !c.pref_on);

	request(&c, &o, true);
	CHECK(c.enables == 2 && c.pref_on);

	// While the app is in 2D at teardown, the release has nothing to do.
	request(&c, &o, false);
	external_destroyed(&c, &o);
	external_destroyed(&c, &o);
	CHECK(c.disables == 2);
	CHECK(o.last_sent == LEIA_LENS_REQ_2D);
}

/* Review 4: the last EXTERNAL weaver dies after SRService invalidated the
 * context, so srLensDisable fails on the dead lens. The release is recorded
 * anyway, so the NEXT context does not re-apply ENABLE with no weaver alive. */
static void
test_failed_release_is_still_recorded(void)
{
	struct leia_lens_owner o = {0};
	struct fake_ctx c;
	new_context(&c, &o);
	external_created(&c, &o);
	CHECK(o.last_sent == LEIA_LENS_REQ_3D);

	// SRService restarts: the context is invalid, the disable fails.
	CHECK(leia_lens_owner_on_external_weaver_destroyed(&o) == LEIA_LENS_ACTION_DISABLE);
	leia_lens_owner_commit_release(&o); // recorded despite the failure
	CHECK(o.external_weavers == 0 && o.last_sent == LEIA_LENS_REQ_NONE);

	// The replacement context gets nothing re-applied: the lens stays off.
	new_context(&c, &o);
	CHECK(c.enables == 0 && c.disables == 0 && !c.app_owns);
}

int
main(void)
{
	test_untoggled_session_leaves_the_weaver_in_charge();
	test_first_2d_takes_ownership_and_3d_is_then_sent();
	test_new_context_reapplies_last_request();
	test_new_context_without_history_stays_weaver_owned();
	test_uncommitted_call_is_not_recorded();
	test_external_weaver_turns_the_lens_on_and_releases_it();
	test_external_weaver_3d_request_is_sent();
	test_external_weaver_respects_2d();
	test_failed_release_is_still_recorded();
	if (g_failures != 0) {
		fprintf(stderr, "test_lens_owner_linux: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	printf("test_lens_owner_linux: all checks passed\n");
	return EXIT_SUCCESS;
}
