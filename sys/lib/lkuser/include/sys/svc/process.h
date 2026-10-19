/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// The process protocol (LK_TAG_PROCESS): processes and threads as the kernel sees them.

#include <sys/lk_msg.h>

// request: no payload; with no handle it asks about the caller, with one process handle
// (lent, LK_RIGHT_INFO) about that process. reply: struct lk_process_info
#define LK_PROCESS_INFO 1u
// request: struct lk_process_spawn_req, the image's path, then the arguments; the handles it
// carries are moved, never lent, and the child is born with them under the request's tags.
// reply: struct lk_process_spawn_reply and a handle to the child (WAIT INFO KILL DUP
// TRANSFER), which wait() sees LK_SIGNAL_TERMINATED on once every thread has ended
#define LK_PROCESS_SPAWN 2u
// request: struct lk_process_thread_create_req; reply: struct lk_process_thread_create_reply
// and a handle to the thread (WAIT DUP TRANSFER). The thread starts at entry with sp and
// every other register zero.
#define LK_PROCESS_THREAD_CREATE 3u
// request: no payload, one process handle (lent, LK_RIGHT_KILL); reply: no payload. Every
// thread of the process ends at its next way out of the kernel or interruptible wait, and the
// process exits with LK_PROCESS_KILLED unless it had exited already.
#define LK_PROCESS_KILL 4u

#define LK_PROCESS_RUNNING 1u
#define LK_PROCESS_EXITED  2u

// the exit code of a killed process: ERR_CANCELLED
#define LK_PROCESS_KILLED (-26)

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

// Connect the child to every kernel service that has a startup tag, as the processes the
// console starts are; a carried handle under one of those tags takes its place.
#define LK_PROCESS_SPAWN_SERVICES (1u << 0)

// the most arguments a spawn passes, and their bytes
#define LK_PROCESS_SPAWN_NARGS_MAX 64
#define LK_PROCESS_SPAWN_ARGS_MAX  4096

struct lk_process_spawn_req {
    uint32_t flags;    // LK_PROCESS_SPAWN_*
    uint32_t path_len; // the absolute path follows, without a NUL
    uint32_t nargs;    // then argv, argv[0] included: nargs NUL-terminated strings
    uint32_t args_len; // their bytes, NULs included
    uint32_t tags[LK_MSG_MAX_HANDLES]; // the startup tag of each carried handle, nonzero, distinct
};
LK_ABI_ASSERT(sizeof(struct lk_process_spawn_req) == 32);
LK_ABI_ASSERT(offsetof(struct lk_process_spawn_req, flags) == 0);
LK_ABI_ASSERT(offsetof(struct lk_process_spawn_req, path_len) == 4);
LK_ABI_ASSERT(offsetof(struct lk_process_spawn_req, nargs) == 8);
LK_ABI_ASSERT(offsetof(struct lk_process_spawn_req, args_len) == 12);
LK_ABI_ASSERT(offsetof(struct lk_process_spawn_req, tags) == 16);

struct lk_process_spawn_reply {
    uint32_t pid;
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_process_spawn_reply) == 8);
LK_ABI_ASSERT(offsetof(struct lk_process_spawn_reply, pid) == 0);
LK_ABI_ASSERT(offsetof(struct lk_process_spawn_reply, reserved) == 4);

struct lk_process_thread_create_req {
    uint64_t entry; // user addresses in the caller's address space
    uint64_t sp;    // 16-byte aligned
};
LK_ABI_ASSERT(sizeof(struct lk_process_thread_create_req) == 16);
LK_ABI_ASSERT(offsetof(struct lk_process_thread_create_req, entry) == 0);
LK_ABI_ASSERT(offsetof(struct lk_process_thread_create_req, sp) == 8);

struct lk_process_thread_create_reply {
    uint32_t tid;
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_process_thread_create_reply) == 8);
LK_ABI_ASSERT(offsetof(struct lk_process_thread_create_reply, tid) == 0);
LK_ABI_ASSERT(offsetof(struct lk_process_thread_create_reply, reserved) == 4);
