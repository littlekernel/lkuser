/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// The console protocol (LK_TAG_CONSOLE), served by the kernel console.

#include <sys/lk_msg.h>

// request payload: the bytes, written as they are; reply: no payload
#define LK_CONSOLE_WRITE 1u
