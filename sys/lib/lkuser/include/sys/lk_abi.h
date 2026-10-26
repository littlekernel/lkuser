/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// Types and layouts shared by the kernel and user space. Compiled by both sides, so it uses
// only <stdint.h>/<stddef.h> types, never LK's.
//
// Width rule: a structure that crosses a process boundary or is read by a service (the
// startup block, message headers, impulses and service payloads) uses fixed-width integers
// only, with static_asserts on its size and every field offset, so a 32-bit and a 64-bit
// build agree on it. Syscall argument structures never leave the calling process and use
// size_t and pointers freely.
//
// Argument widths: a syscall argument is register width by preference: handles, rights and
// counts are 32-bit, lengths and offsets size_t, tokens and cookies uintptr_t. An inherently
// 64-bit value (a file offset, a time) may take a register pair on 32-bit as the psABI
// prescribes, but neither the stubs nor the dispatcher map a pair yet (the dispatcher asserts
// that each argument fits one register), so the first such row has to add that mapping. No
// row has one: times travel in the argument structures.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define LK_ABI_ASSERT(e) static_assert(e, #e)
#else
#define LK_ABI_ASSERT(e) _Static_assert(e, #e)
#endif

// LK's status codes: NO_ERROR or a negative ERR_* (<sys/lk_err.h>), never an errno
typedef int lk_status_t;

// names a slot in the calling process's handle table; 0 is never a handle
typedef uint32_t lk_handle_t;
#define LK_HANDLE_INVALID ((lk_handle_t)0)

// What a handle lets its holder do with the object. Rights only ever shrink: the calls that
// create an object hand out the full set for its kind, handle_dup can drop rights, and
// carrying a handle in a message keeps them.
#define LK_RIGHT_DUP      (1u << 0)  // handle_dup
#define LK_RIGHT_TRANSFER (1u << 1)  // carried in a message to another process
#define LK_RIGHT_WAIT     (1u << 2)  // wait on the object's signals
#define LK_RIGHT_CALL     (1u << 3)  // call over a connection
#define LK_RIGHT_IMPULSE  (1u << 4)  // impulse_send over a connection
#define LK_RIGHT_SIGNAL   (1u << 5)  // connection_signal and connection_set_cookie (peer view)
#define LK_RIGHT_RECV     (1u << 6)  // recv on an endpoint or group
#define LK_RIGHT_CONNECT  (1u << 7)  // connect to an endpoint
#define LK_RIGHT_MAP      (1u << 8)  // map a memory object
#define LK_RIGHT_READ     (1u << 9)  // the protections a memory object's mappings may have
#define LK_RIGHT_WRITE    (1u << 10)
#define LK_RIGHT_EXEC     (1u << 11)
#define LK_RIGHT_INFO     (1u << 12) // process.INFO on a process
#define LK_RIGHT_KILL     (1u << 13) // process.KILL on a process
#define LK_RIGHTS_ALL     ((1u << 14) - 1)

// Authority over one received call, valid in the receiving process only, so it never
// crosses a process boundary and is register width.
typedef uintptr_t lk_token_t;

// Startup block: what a process is born with, at the top of its main thread's stack, sp
// pointing at it on entry. The header is followed by uint32_t arg_off[nargs], then
// struct lk_proc_handle handles[nhandles], then the NUL-terminated argument strings. Every
// offset counts from the start of the block.
struct lk_proc_args {
    uint32_t nargs;
    uint32_t nhandles;
};
LK_ABI_ASSERT(sizeof(struct lk_proc_args) == 8);
LK_ABI_ASSERT(offsetof(struct lk_proc_args, nargs) == 0);
LK_ABI_ASSERT(offsetof(struct lk_proc_args, nhandles) == 4);

// a handle the process is born with, named by what it is for
struct lk_proc_handle {
    uint32_t tag;
    lk_handle_t handle;
};
LK_ABI_ASSERT(sizeof(struct lk_proc_handle) == 8);
LK_ABI_ASSERT(offsetof(struct lk_proc_handle, tag) == 0);
LK_ABI_ASSERT(offsetof(struct lk_proc_handle, handle) == 4);

// Startup handle tags: connections to the services of the same name. Which a process gets
// is its build's and its spawner's decision.
#define LK_TAG_PROCESS   1u
#define LK_TAG_VM        2u
#define LK_TAG_TIME      3u
#define LK_TAG_CONSOLE   4u
#define LK_TAG_NAMESPACE 5u

// A process the kernel starts at boot to serve one of the services above (LKUSER_BOOT's
// path@service) is born with the endpoint it serves under this tag (RECV CONNECT WAIT DUP
// TRANSFER), and every process the kernel makes after it gets a connection to that endpoint
// under the service's tag in place of the kernel's.
#define LK_TAG_SERVE_ENDPOINT 0x100u

// tags from here up name test services, present only in builds that compile them in
#define LK_TAG_TEST_BASE 0x1000u
#define LK_TAG_TEST_ECHO (LK_TAG_TEST_BASE + 0)

// Tags from here up are the spawner's own: handles a program is born with by agreement with
// whoever spawns it (process.SPAWN), such as a memory object to work in.
#define LK_TAG_USER_BASE 0x10000u

// What wait() watches: bits an object raises as its state changes.
#define LK_SIGNAL_TERMINATED  (1u << 0) // a process or thread has ended; stays raised
#define LK_SIGNAL_READABLE    (1u << 1) // an endpoint has a call or impulse queued; a connection: server-set
#define LK_SIGNAL_WRITABLE    (1u << 2) // a connection: server-set
#define LK_SIGNAL_ERROR       (1u << 3) // a connection: server-set
#define LK_SIGNAL_PEER_CLOSED (1u << 4) // a connection's endpoint has closed; stays raised

// the signals a server sets and clears on its connections with connection_signal
#define LK_SIGNAL_CONN_SERVER (LK_SIGNAL_READABLE | LK_SIGNAL_WRITABLE | LK_SIGNAL_ERROR)

static inline const uint32_t *lk_proc_args_offsets(const struct lk_proc_args *a) {
    return (const uint32_t *)(a + 1);
}

static inline const struct lk_proc_handle *lk_proc_args_handles(const struct lk_proc_args *a) {
    return (const struct lk_proc_handle *)(lk_proc_args_offsets(a) + a->nargs);
}

static inline const char *lk_proc_args_arg(const struct lk_proc_args *a, uint32_t i) {
    return (const char *)a + lk_proc_args_offsets(a)[i];
}

// the handle the process was born with for tag, or LK_HANDLE_INVALID
static inline lk_handle_t lk_proc_args_find(const struct lk_proc_args *a, uint32_t tag) {
    const struct lk_proc_handle *h = lk_proc_args_handles(a);
    for (uint32_t i = 0; i < a->nhandles; i++) {
        if (h[i].tag == tag) {
            return h[i].handle;
        }
    }
    return LK_HANDLE_INVALID;
}
