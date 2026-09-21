// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Who owns the SR lens preference on Linux: the weaver, or us.
 *
 * Pure decision logic, no SDK calls, so it is unit-testable without hardware
 * (tests/test_lens_owner_linux.c). leia_sr_linux_sdk.c is the only caller.
 *
 * ## The SDK rule this encodes (LeiaSR #266)
 *
 * SRService keeps ONE lens preference per client connection, i.e. per SR
 * context, and the last writer wins: `srCreateLens` and the weaver share the
 * same SwitchableLensHint. The FIRST `srLensEnable` / `srLensDisable` on a
 * context makes the preference the application's for the rest of that
 * context's life, and from then on the Linux weaver never writes it again --
 * not on panel exit or re-entry, not after tracking loss, not from a weaver
 * recreated on the same context. Before that first call the weaver owns it:
 * it votes the lens on at the first woven frame and withdraws the vote after
 * 500 ms continuously off the panel. A NEW context starts over with the
 * weaver in charge.
 *
 * ## What that means for us
 *
 * The plug-in keeps one process-wide SR context, so the first lens call we
 * make owns the lens for the rest of the process's context, and nobody else
 * will ever turn it back on or off. Two consequences:
 *
 *  1. **3D before any 2D is delegated, not sent.** The runtime asks for 3D at
 *     every xrBeginSession (it requests the session's mode, which defaults to
 *     3D). Turning that into `srLensEnable` would take ownership at startup
 *     and throw away the weaver's automatic "lens off when the window leaves
 *     the panel" -- the Windows-parity behaviour an untoggled session should
 *     get. The weaver already turns the lens on at its first woven frame, so
 *     a 3D wish needs no call at all until something has turned the lens off.
 *  2. **The first 2D request takes ownership, deliberately.** From then on
 *     every request is sent, because the weaver will not act on our behalf
 *     any more. That is the reason every runtime path that asks for 2D must
 *     pair with one that asks for the previous state back.
 *
 * ## Across contexts
 *
 * The last request actually sent is remembered process-wide. When a NEW SR
 * context comes up (first creation, or a re-creation after the SR context was
 * invalidated by an SRService restart), that request is re-applied, so "the
 * application took control" survives the restart. If nothing was ever sent,
 * nothing is re-applied and the new context's weaver owns the lens, which is
 * what the SDK intends.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The last lens request actually sent to the SDK (process-wide).
enum leia_lens_req
{
	LEIA_LENS_REQ_NONE = 0, //!< never sent: the weaver owns the lens
	LEIA_LENS_REQ_2D,       //!< srLensDisable was the last call sent
	LEIA_LENS_REQ_3D,       //!< srLensEnable was the last call sent
};

//! What the caller must do with the SDK.
enum leia_lens_action
{
	LEIA_LENS_ACTION_NONE = 0, //!< make no call (leave it to the weaver / nothing to re-apply)
	LEIA_LENS_ACTION_DISABLE,  //!< call srLensDisable
	LEIA_LENS_ACTION_ENABLE,   //!< call srLensEnable
};

struct leia_lens_owner
{
	//! Last request sent through srLensEnable/srLensDisable, on ANY context.
	//! Survives context re-creation; NONE until the first one is sent.
	enum leia_lens_req last_sent;
	//! True once the CURRENT context has been sent a request, i.e. the SDK has
	//! marked this context's preference application-owned. Cleared by
	//! leia_lens_owner_on_new_context().
	bool ctx_app_owned;
};

/*!
 * What a request_display_mode(@p want_3d) must send. Does not change state:
 * call leia_lens_owner_commit() once the SDK call (if any) succeeded, so a
 * failed call is not remembered as sent.
 */
static inline enum leia_lens_action
leia_lens_owner_on_request(const struct leia_lens_owner *o, bool want_3d)
{
	if (want_3d && o->last_sent == LEIA_LENS_REQ_NONE) {
		// Never turned the lens off: the weaver still owns it and brings it
		// up when it weaves. Sending srLensEnable here would take ownership
		// for nothing and lose the weaver's off-panel release.
		return LEIA_LENS_ACTION_NONE;
	}
	return want_3d ? LEIA_LENS_ACTION_ENABLE : LEIA_LENS_ACTION_DISABLE;
}

//! Record that @p action was sent successfully (NONE is a no-op).
static inline void
leia_lens_owner_commit(struct leia_lens_owner *o, enum leia_lens_action action)
{
	switch (action) {
	case LEIA_LENS_ACTION_ENABLE:
		o->last_sent = LEIA_LENS_REQ_3D;
		o->ctx_app_owned = true;
		break;
	case LEIA_LENS_ACTION_DISABLE:
		o->last_sent = LEIA_LENS_REQ_2D;
		o->ctx_app_owned = true;
		break;
	case LEIA_LENS_ACTION_NONE:
	default: break;
	}
}

/*!
 * A new SR context (and lens handle) exists. Returns the request to re-apply
 * to it: the last one sent on any earlier context, or NONE if none was ever
 * sent (then the new context's weaver owns the lens). Commit the result the
 * same way as a request once the call succeeded.
 */
static inline enum leia_lens_action
leia_lens_owner_on_new_context(struct leia_lens_owner *o)
{
	o->ctx_app_owned = false;
	switch (o->last_sent) {
	case LEIA_LENS_REQ_3D: return LEIA_LENS_ACTION_ENABLE;
	case LEIA_LENS_REQ_2D: return LEIA_LENS_ACTION_DISABLE;
	case LEIA_LENS_REQ_NONE:
	default: return LEIA_LENS_ACTION_NONE;
	}
}

#ifdef __cplusplus
}
#endif
