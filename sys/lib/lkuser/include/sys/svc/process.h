/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// The process protocol (LK_TAG_PROCESS): processes as the kernel sees them.

#include <sys/lk_msg.h>

// request: no payload, asking about the caller; reply: struct lk_process_info
#define LK_PROCESS_INFO 1u

#define LK_PROCESS_RUNNING 1u
#define LK_PROCESS_EXITED  2u

struct lk_process_info {
    uint32_t pid;
    uint32_t state;    // LK_PROCESS_*
    int32_t exit_code; // once exited
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_process_info) == 16);
LK_ABI_ASSERT(offsetof(struct lk_process_info, pid) == 0);
LK_ABI_ASSERT(offsetof(struct lk_process_info, state) == 4);
LK_ABI_ASSERT(offsetof(struct lk_process_info, exit_code) == 8);
LK_ABI_ASSERT(offsetof(struct lk_process_info, reserved) == 12);
