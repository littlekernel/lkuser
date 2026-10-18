/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// The time protocol (LK_TAG_TIME). Times are nanoseconds.

#include <sys/lk_msg.h>

// request: struct lk_time_sleep_req; reply: no payload, after at least that long
#define LK_TIME_SLEEP      1u
// request: struct lk_time_clock_read_req; reply: struct lk_time_clock_read_reply
#define LK_TIME_CLOCK_READ 2u

#define LK_CLOCK_MONOTONIC 0u // since boot

struct lk_time_sleep_req {
    uint64_t ns;
};
LK_ABI_ASSERT(sizeof(struct lk_time_sleep_req) == 8);
LK_ABI_ASSERT(offsetof(struct lk_time_sleep_req, ns) == 0);

struct lk_time_clock_read_req {
    uint32_t clock; // LK_CLOCK_*
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_time_clock_read_req) == 8);
LK_ABI_ASSERT(offsetof(struct lk_time_clock_read_req, clock) == 0);
LK_ABI_ASSERT(offsetof(struct lk_time_clock_read_req, reserved) == 4);

struct lk_time_clock_read_reply {
    uint64_t ns;
};
LK_ABI_ASSERT(sizeof(struct lk_time_clock_read_reply) == 8);
LK_ABI_ASSERT(offsetof(struct lk_time_clock_read_reply, ns) == 0);
