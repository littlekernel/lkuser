/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// What apps/echoserver speaks besides the echo protocol of <sys/svc/echo.h>: the ops that
// echotest drives it with, which the kernel's echo service refuses as ERR_NOT_SUPPORTED.

#include <sys/lk_msg.h>
#include <sys/svc/echo.h>

// where echoserver mounts its two endpoints: the first takes up to 4 calls at once, the second
// one at a time; one group, two server threads
#define ECHOTEST_PATH        "/srv/echo"
#define ECHOTEST_SINGLE_PATH "/srv/echo1"

// request: struct echotest_memobj_req, and a memory object (lent, MAP READ); reply: struct
// echotest_memobj_reply, the sum of its first len bytes
#define ECHOTEST_MEMOBJ 0x100u
// request: no payload, and a connection (moved, IMPULSE) to the client's endpoint, over which
// the server sends an ECHOTEST_IMPULSE every tenth echo from then on, payload[0..3] the count
#define ECHOTEST_NOTIFY 0x101u
// request: no payload; reply: struct echotest_stats
#define ECHOTEST_STATS  0x102u
// the server replies, then exits
#define ECHOTEST_QUIT   0x103u

#define ECHOTEST_IMPULSE (LK_IMPULSE_USER_BASE + 0x10)

struct echotest_memobj_req {
    uint32_t len;
};

struct echotest_memobj_reply {
    uint32_t sum;
};

struct echotest_stats {
    uint32_t echoes;          // echo calls served
    uint32_t single_calls;    // calls on the one-at-a-time endpoint
    uint32_t single_overlaps; // calls on it that began while another was in progress
    uint32_t multi_max;       // the most calls in progress at once on the other
    uint32_t conn_closed;     // CONN_CLOSED impulses received
    uint32_t reserved;
};
