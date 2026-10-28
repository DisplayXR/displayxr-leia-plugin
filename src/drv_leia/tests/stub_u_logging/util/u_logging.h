// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  test_sr_delayload_win's stand-in for the runtime's util/u_logging.h:
 *         captures U_LOG_W so the test can assert on the bind WARNs.
 */

#pragma once

void
test_log_w(const char *fmt, ...);

#define U_LOG_W(...) test_log_w(__VA_ARGS__)
