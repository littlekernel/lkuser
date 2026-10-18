/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// The echo test protocol (LK_TAG_TEST_ECHO): a service that returns what it is sent, for
// exercising the message path. Test builds only.

#include <sys/lk_msg.h>

// request payload: any bytes; reply payload: the same bytes, or as many as fit with the
// status ERR_NOT_ENOUGH_BUFFER
#define LK_ECHO_ECHO 1u
