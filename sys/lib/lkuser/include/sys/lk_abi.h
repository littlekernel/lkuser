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
// startup block, and later message headers, impulses and service payloads) uses fixed-width
// integers only, with static_asserts on its size and every field offset, so a 32-bit and a
// 64-bit build agree on it. Syscall argument structures never leave the calling process and
// use size_t and pointers freely. Every syscall argument fits one register on both widths:
// a 64-bit value travels in a structure, never as an argument.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define LK_ABI_ASSERT(e) static_assert(e, #e)
#else
#define LK_ABI_ASSERT(e) _Static_assert(e, #e)
#endif

// LK's status codes: NO_ERROR or a negative ERR_* (<sys/lk_err.h>), never an errno
typedef int lk_status_t;

// an index into the calling process's handle table
typedef uint32_t lk_handle_t;
#define LK_HANDLE_INVALID ((lk_handle_t)0)

// Authority over one received call, valid in the receiving process only, so it never
// crosses a process boundary and is register width.
typedef uintptr_t lk_token_t;

// syscall argument structures, defined with the rows that use them
struct lk_iov;
struct lk_call_args;
struct lk_recv_args;
struct lk_reply_args;
struct lk_impulse;
struct lk_endpoint_args;
struct lk_wait_args;

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

// startup handle tags
#define LK_TAG_PROCESS   1u
#define LK_TAG_VM        2u
#define LK_TAG_TIME      3u
#define LK_TAG_CONSOLE   4u
#define LK_TAG_NAMESPACE 5u

static inline const uint32_t *lk_proc_args_offsets(const struct lk_proc_args *a) {
    return (const uint32_t *)(a + 1);
}

static inline const struct lk_proc_handle *lk_proc_args_handles(const struct lk_proc_args *a) {
    return (const struct lk_proc_handle *)(lk_proc_args_offsets(a) + a->nargs);
}

static inline const char *lk_proc_args_arg(const struct lk_proc_args *a, uint32_t i) {
    return (const char *)a + lk_proc_args_offsets(a)[i];
}
