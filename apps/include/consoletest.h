/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// What apps/consoled speaks besides the console protocol of <sys/svc/console.h>: the ops
// consoletest drives it with, which the kernel console refuses as ERR_NOT_SUPPORTED.

#include <sys/lk_msg.h>
#include <sys/svc/console.h>

// request: no payload; reply: struct consoletest_stats
#define CONSOLETEST_STATS 0x100u
// request: no payload; the server replies, then exits
#define CONSOLETEST_QUIT  0x101u

struct consoletest_stats {
    uint32_t writes;      // WRITE calls relayed
    uint32_t write_bytes; // bytes they carried
    uint32_t reads;       // READ calls answered
    uint32_t winsizes;    // WINSIZE calls relayed
    uint32_t departed;    // READ calls whose callers left before an answer
    uint32_t errors;      // calls that failed on the server's side
};
