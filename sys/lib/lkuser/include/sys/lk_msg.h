/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// The message layer shared by the kernel and user space: the header that leads every request
// and reply, the impulse datagram, and the argument structures of the IPC rows of
// <sys/_syscalls.h>. lk_msg_hdr and lk_impulse are wire structures under the width rule of
// <sys/lk_abi.h>; the argument structures are per-ABI.

#include <sys/lk_abi.h>

// Leads every request and every reply. Handles travel beside the payload, never in it.
struct lk_msg_hdr {
    uint32_t op;       // service-defined, below LK_OP_KERNEL_BASE
    uint32_t len;      // payload bytes after the header
    uint32_t nhandles; // handles carried beside the message
    int32_t status;    // reply only: the service's verdict, NO_ERROR or a negative ERR_*
};
LK_ABI_ASSERT(sizeof(struct lk_msg_hdr) == 16);
LK_ABI_ASSERT(offsetof(struct lk_msg_hdr, op) == 0);
LK_ABI_ASSERT(offsetof(struct lk_msg_hdr, len) == 4);
LK_ABI_ASSERT(offsetof(struct lk_msg_hdr, nhandles) == 8);
LK_ABI_ASSERT(offsetof(struct lk_msg_hdr, status) == 12);

// ops the kernel uses for messages it makes itself; a client may not send them
#define LK_OP_KERNEL_BASE 0xffff0000u
#define LK_OP_CONNECT     (LK_OP_KERNEL_BASE + 1) // a CONNECT_NOTIFY handshake

#define LK_MSG_MAX_HANDLES 8 // handles carried by one request or reply
#define LK_IOV_MAX         8 // iovecs on each side of a call

// times in the ABI are nanoseconds; this one means no timeout
#define LK_TIME_INFINITE UINT64_MAX

// lk_call_args.handle_flags bit i: handles[i] is lent for the call rather than moved
#define LK_HANDLE_LEND(i) (1u << (i))

// one piece of a message in the caller's memory
struct lk_iov {
    void *base;
    size_t len;
};

// call(): the request is the concatenation of req[], the reply lands in reply[]
struct lk_call_args {
    const struct lk_iov *req;
    size_t nreq;
    const lk_handle_t *handles;
    size_t nhandles;
    const struct lk_iov *reply;
    size_t nreply;
    lk_handle_t *reply_handles; // room for the handles the reply carries, at most
    size_t reply_nhandles;      // LK_MSG_MAX_HANDLES; LK_HANDLE_INVALID beyond those it did
    uint32_t handle_flags;
    uint64_t timeout_ns;
    size_t reply_len; // out: the furthest reply byte the service wrote, header included
};

// What recv() reports about a call or an impulse. A token names the call in the receiving
// process until reply (connection_accept or connection_refuse for a handshake) ends it, or the
// caller leaves first, which makes every later use of the token ERR_NOT_FOUND.
struct lk_recv_info {
    lk_token_t token; // 0 for an impulse, else the call's authority for msg_read/msg_write/reply
    uint32_t op;      // LK_OP_CONNECT for a CONNECT_NOTIFY handshake, 0 for an impulse, else the request's
    uint64_t ep_cookie;
    uint64_t conn_cookie;
    uint32_t sender_pid;
    uint32_t sender_tid;
    size_t req_len;   // the whole request, header included
    size_t reply_max; // the caller's reply capacity
    lk_handle_t handles[LK_MSG_MAX_HANDLES]; // carried handles, now in the receiver's table
    uint32_t nhandles;
    uint8_t impulse[32]; // the datagram when token is 0; sizeof(struct lk_impulse)
};

// recv(): buf[] receives the start of the request, header included; msg_read reads the rest.
// Offsets of msg_read and msg_write count from the start of the request and of the reply, so
// a request's payload starts at sizeof(struct lk_msg_hdr), and so does a reply's: its header is
// the kernel's to write at reply.
struct lk_recv_args {
    struct lk_iov *buf;
    size_t nbuf;
    uint64_t timeout_ns;
    struct lk_recv_info info; // out
};

// reply(): iov[] starts with a struct lk_msg_hdr, of which the kernel keeps the status and
// fills in the rest, followed by payload bytes that land at sizeof(struct lk_msg_hdr) in the
// caller's reply. The handles are moved to the caller.
struct lk_reply_args {
    const struct lk_iov *iov;
    size_t n;
    const lk_handle_t *handles;
    size_t nhandles;
};

// endpoint_create()
#define LK_EP_FLAG_CONNECT_NOTIFY (1u << 0) // connect() is a call the server accepts or refuses
struct lk_endpoint_args {
    uint32_t flags;
    uint32_t max_active; // calls received and not yet replied to, at most; at least 1
    uint64_t cookie;     // returned with every call and impulse the endpoint receives
};

// wait(): at most LK_WAIT_MAX_ITEMS objects at once
#define LK_WAIT_MAX_ITEMS 8
struct lk_wait_item {
    lk_handle_t h;
    uint32_t mask;
};
struct lk_wait_args {
    const struct lk_wait_item *items;
    size_t n;
    uint64_t timeout_ns;
    size_t index;     // out: the item that woke the wait
    uint32_t signals; // out: what it showed
};

// A small asynchronous datagram to an endpoint, received through recv() with token 0, ahead
// of any queued call. impulse_send takes one from the sending process's fixed pool
// (ERR_NO_RESOURCES when all are queued) and gives it back once received.
#define LK_IMPULSE_SIZE 32
#define LK_IMPULSE_USER_BASE 0x1000u // types below are the kernel's
struct lk_impulse {
    uint32_t type;
    uint32_t flags;  // none defined yet: 0
    uint64_t cookie; // the sending connection's, or for the kernel's the affected object's
    uint8_t payload[16];
};
LK_ABI_ASSERT(sizeof(struct lk_impulse) == LK_IMPULSE_SIZE);
LK_ABI_ASSERT(offsetof(struct lk_impulse, type) == 0);
LK_ABI_ASSERT(offsetof(struct lk_impulse, flags) == 4);
LK_ABI_ASSERT(offsetof(struct lk_impulse, cookie) == 8);
LK_ABI_ASSERT(offsetof(struct lk_impulse, payload) == 16);
LK_ABI_ASSERT(sizeof(((struct lk_recv_info *)0)->impulse) == LK_IMPULSE_SIZE);

// The kernel's impulses. CONN_CLOSED: the last client handle of a connection to the endpoint
// has closed (its process may have exited); the cookie is the connection's, no payload.
// PROCESS_EXITED: a process spawned with this connection to notify has ended and its memory
// is freed; the cookie is the connection's, the payload a struct lk_impulse_process_exited.
#define LK_IMPULSE_CONN_CLOSED    1u
#define LK_IMPULSE_PROCESS_EXITED 2u

struct lk_impulse_process_exited {
    uint32_t pid;
    int32_t exit_code;
    uint8_t reserved[8];
};
LK_ABI_ASSERT(sizeof(struct lk_impulse_process_exited) == 16);
LK_ABI_ASSERT(offsetof(struct lk_impulse_process_exited, pid) == 0);
LK_ABI_ASSERT(offsetof(struct lk_impulse_process_exited, exit_code) == 4);
