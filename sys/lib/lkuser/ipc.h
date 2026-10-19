/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lktl/ref_ptr.h>
#include <sys/lk_msg.h>
#include <sys/types.h>

#include "msg.h"

#ifndef LKUSER_INFLIGHT
#define LKUSER_INFLIGHT 16
#endif

namespace lkuser {

class connection;
class endpoint;
class proc;
class thread;

// a handle the kernel put in a table and may have to take out again
struct installed_handle {
    lk_handle_t h;
    const object *obj; // what h named when added, so taking it out closes nothing else
};

// A call a process has received and not yet ended: the authority behind its token, which is
// the slot's index in the low 8 bits and its generation above them. Generations start at 1, so
// no live token is small. Under LKUSER_IPC_LOCK.
struct inflight_slot {
    bool used = false;
    uintptr_t gen = 0;
    pending_call *pc = nullptr; // null once the caller has left the call
    endpoint *ep = nullptr;     // holds a reference while used, for its active count
    // the lent handles the call put in this process's table, taken out when the call ends
    installed_handle lent[LK_MSG_MAX_HANDLES];
    size_t nlent = 0;
};

// A call over conn to a user endpoint, on t's pending_call, whose req and reply buffers, prefix
// and reply handle room are loaded: queued, received by a server and replied to, or ended
// another way. The transport's status: NO_ERROR once a reply arrived, ERR_TIMED_OUT,
// ERR_INTERRUPTED (a kill), ERR_CHANNEL_CLOSED (the endpoint or the server went), ERR_FAULT (the
// caller's buffers faulted while the server copied).
status_t call_user(thread *t, lk::ref_ptr<connection> conn, const lk_msg_hdr &hdr,
                   carried_handle *carried, size_t ncarried, uint64_t timeout_ns);

// The CONNECT_NOTIFY handshake for conn, a new connection: a call the server receives with
// op LK_OP_CONNECT and ends with connection_accept (NO_ERROR) or connection_refuse
// (ERR_NOT_ALLOWED).
status_t connect_notify(thread *t, lk::ref_ptr<connection> conn);

// p is exiting: every call it has received ends with ERR_CHANNEL_CLOSED, and it receives no
// more.
void cancel_inflight(proc *p);

} // namespace lkuser
