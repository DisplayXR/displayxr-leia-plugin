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

int
main(void)
{
	test_untoggled_session_leaves_the_weaver_in_charge();
	test_first_2d_takes_ownership_and_3d_is_then_sent();
	test_new_context_reapplies_last_request();
	test_new_context_without_history_stays_weaver_owned();
	test_uncommitted_call_is_not_recorded();
	if (g_failures != 0) {
		fprintf(stderr, "test_lens_owner_linux: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	printf("test_lens_owner_linux: all checks passed\n");
	return EXIT_SUCCESS;
}
