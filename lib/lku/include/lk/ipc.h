/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// The native API: one lk_<name> function per row of <sys/_syscalls.h>, each returning
// NO_ERROR or a negative ERR_* from <sys/lk_err.h>, and lk_msg_call for the common call.

#include <stddef.h>
#include <stdint.h>
#include <sys/lk_abi.h>
#include <sys/lk_err.h>
#include <sys/lk_msg.h>

#ifdef __cplusplus
extern "C" {
#endif

void lk_exit(int code) __attribute__((noreturn));
void lk_thread_exit(void) __attribute__((noreturn));

#define LK_SYSCALL_DEF(n, ret, name, args...) ret lk_##name(args);
#include <sys/_syscalls.h>
#undef LK_SYSCALL_DEF

// A call described by its payloads: lk_msg_callv puts the request header in front and takes
// the reply's off.
struct lk_msg {
    uint32_t op;
    const struct lk_iov *req;   // the request payload, at most LK_IOV_MAX - 1 pieces
    size_t nreq;
    const struct lk_iov *reply; // where the reply payload goes, likewise
    size_t nreply;
    const lk_handle_t *carry;   // handles the request carries, at most LK_MSG_MAX_HANDLES:
    size_t ncarry;              // moved, or lent where carry_flags has LK_HANDLE_LEND(i)
    uint32_t carry_flags;
    lk_handle_t *handles;       // room for the handles a reply carries
    size_t handles_max;
    size_t reply_len;           // out: the reply payload's length
    uint32_t nhandles;          // out: the handles the reply carried, now in handles[]
};

// One call. Returns the call's status if it failed, else the service's; the out fields are
// set once a reply arrived, whatever the service said.
lk_status_t lk_msg_callv(lk_handle_t conn, struct lk_msg *m);

// One call with a contiguous request payload and reply buffer, as lk_msg_callv. Once a reply
// arrived, *reply_len (if not NULL) gets its payload length.
lk_status_t lk_msg_call(lk_handle_t conn, uint32_t op, const void *req, size_t req_len,
                        void *reply, size_t reply_cap, size_t *reply_len);

// wait() on one object: NO_ERROR once a signal in mask is up, ERR_TIMED_OUT if none rose
// within timeout_ns (LK_TIME_INFINITE for no limit); *signals, if not NULL, gets what the
// object showed.
lk_status_t lk_wait_one(lk_handle_t h, uint32_t mask, uint64_t timeout_ns, uint32_t *signals);

#ifdef __cplusplus
}
#endif
