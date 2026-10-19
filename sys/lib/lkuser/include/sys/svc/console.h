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
#define LK_CONSOLE_WRITE   1u
// request: struct lk_console_read_req; reply payload: one line as typed, echoed and ending in
// '\n', or as much of it as max and the reply allow; nothing at end of input (^D on an empty
// line). Blocks until the line ends or the room is used up.
#define LK_CONSOLE_READ    2u
// request: no payload; reply: struct lk_console_winsize
#define LK_CONSOLE_WINSIZE 3u

struct lk_console_read_req {
    uint32_t max; // bytes at most
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_console_read_req) == 8);
LK_ABI_ASSERT(offsetof(struct lk_console_read_req, max) == 0);
LK_ABI_ASSERT(offsetof(struct lk_console_read_req, reserved) == 4);

struct lk_console_winsize {
    uint32_t cols;
    uint32_t rows;
};
LK_ABI_ASSERT(sizeof(struct lk_console_winsize) == 8);
LK_ABI_ASSERT(offsetof(struct lk_console_winsize, cols) == 0);
LK_ABI_ASSERT(offsetof(struct lk_console_winsize, rows) == 4);
