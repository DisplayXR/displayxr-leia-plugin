// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR display readiness — see leia_sr_readiness.h.
 */

#include "leia_sr_readiness.h"

#include <stddef.h>

uint32_t
leia_sr_pending_count(const struct leia_sr_readiness_entry *cur, uint32_t n, bool accept_edid_only)
{
	uint32_t pending = 0;
	for (uint32_t i = 0; cur != NULL && i < n; i++) {
		if (!leia_sr_display_ready(&cur[i], accept_edid_only)) {
			pending++;
		}
	}
	return pending;
}

bool
leia_sr_newly_ready(const struct leia_sr_readiness_entry *prev,
                    uint32_t np,
                    const struct leia_sr_readiness_entry *cur,
                    uint32_t nc,
                    bool accept_edid_only)
{
	for (uint32_t i = 0; cur != NULL && i < nc; i++) {
		if (!leia_sr_display_ready(&cur[i], accept_edid_only)) {
			continue;
		}
		bool was_ready = false;
		for (uint32_t j = 0; prev != NULL && j < np && !was_ready; j++) {
			was_ready = prev[j].sr_display_id == cur[i].sr_display_id &&
			            leia_sr_display_ready(&prev[j], accept_edid_only);
		}
		if (!was_ready) {
			return true;
		}
	}
	return false;
}
