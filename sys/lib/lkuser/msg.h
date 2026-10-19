/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <kernel/vm.h>
#include <kernel/wait.h>
#include <lk/cpp.h>
#include <lk/err.h>
#include <lktl/list.h>
#include <lktl/ref_ptr.h>
#include <sys/lk_msg.h>
#include <sys/types.h>

#include "copy.h"
#include "object.h"

namespace lkuser {

// One side of a call's message as the caller laid it out: up to LK_IOV_MAX ranges of user
// memory in one address space, read or written at any offset as if contiguous. Every copy
// goes through copy.cpp, so the aspace need not be the active one.
class msg_buffer {
public:
    // Copies n iovecs from user memory and checks that every range lies inside aspace:
    // ERR_INVALID_ARGS for more than LK_IOV_MAX or lengths that add up past SIZE_MAX,
    // ERR_FAULT for unreadable iovecs or a range outside the aspace. Empty ranges are dropped.
    status_t load(vmm_aspace_t *aspace, user_ptr<const lk_iov> iov, size_t n);
    // no ranges at all
    void reset() {
        n_ = 0;
        len_ = 0;
    }

    size_t len() const { return len_; }

    // Bytes [off, off + len) of the message, clipped at its end: the number copied, or
    // ERR_FAULT for an unmapped page or one without the permission.
    ssize_t read(size_t off, void *buf, size_t len) const;
    ssize_t write(size_t off, const void *buf, size_t len) const;

private:
    struct range {
        vaddr_t base;
        size_t len;
    };

    template <typename F>
    ssize_t walk(size_t off, size_t len, F &&copy) const;

    vmm_aspace_t *aspace_ = nullptr;
    range ranges_[LK_IOV_MAX] = {};
    size_t n_ = 0;
    size_t len_ = 0;
};

// Which side of a copy between two messages faulted.
enum class copy_fault { NONE, SRC, DST };

// len bytes from src at src_off to dst at dst_off, through a kernel buffer, so the two may lie
// in different address spaces. Both ranges must lie inside their messages. ERR_FAULT with
// *fault naming the side.
status_t copy_msg(const msg_buffer &src, size_t src_off, const msg_buffer &dst, size_t dst_off,
                  size_t len, copy_fault *fault);

// the start of a request, copied before anything reads it, header included
constexpr size_t kRecvPrefix = 256;

class proc;
class thread;

// A handle a request carries, as the receiver sees it. A moved handle (the default) stays in
// the caller's table until a kernel service returns success or a user server receives the call;
// a lent one never leaves, and the receiver sees its rights without DUP and TRANSFER. A kernel
// service keeps a carried object only by reference or by putting it in a table, never a lent
// one.
struct carried_handle {
    lk_handle_t h; // in the caller's table
    lk::ref_ptr<object> obj;
    uint32_t rights;
    bool lent;
};

// Where a call to a user endpoint is: queued on the endpoint, received by a server (which
// holds an inflight slot naming it), or over, with result saying how.
enum class call_state : uint8_t { IDLE, QUEUED, RECEIVED, DONE };

// The call a thread has in progress. Embedded in the calling thread, which owns it for its
// lifetime.
//
// For a user endpoint the caller blocks while a server reads the request and writes the reply
// in place, from its own thread. Everything from state on is under LKUSER_IPC_LOCK. A server
// thread copying to or from the caller's buffers sets busy first, and the caller does not leave
// the call while busy is set, so the caller's address space outlives every copy.
struct pending_call : lk::list_hook<> {
    explicit pending_call(thread *owner);
    ~pending_call();
    DISALLOW_COPY_ASSIGN_AND_MOVE(pending_call);

    // a handle the reply gives the caller, already in its table
    struct reply_handle {
        lk_handle_t h;
        const object *obj; // what h named when added, so undoing it closes nothing else
    };

    thread *const owner;
    msg_buffer req;
    msg_buffer reply;
    size_t reply_len = 0; // the furthest reply byte written, header included
    reply_handle reply_handles[LK_MSG_MAX_HANDLES];
    size_t nreply_handles = 0;
    size_t reply_handles_cap = 0; // the caller's room for them
    uint8_t prefix[kRecvPrefix];

    // A call to a user endpoint, set before it is queued: what it is and what it carries. A
    // moved handle counts as a handle of its object until the call ends or a server receives
    // it, so the object's last handle cannot go while it is in transit.
    lk_msg_hdr hdr = {};
    bool connect = false; // a CONNECT_NOTIFY handshake: no request, no reply
    lk::ref_ptr<object> conn;
    carried_handle carried[LK_MSG_MAX_HANDLES];
    size_t ncarried = 0;
    bool carried_taken = false; // a server received the moved handles

    // under LKUSER_IPC_LOCK
    call_state state = call_state::IDLE;
    bool busy = false;
    status_t result = NO_ERROR;
    // while RECEIVED: the receiving process and its inflight slot
    proc *server = nullptr;
    uint32_t slot = 0;
    wait_queue_t done; // the caller, until state is DONE
    wait_queue_t idle; // the caller and other server threads, until busy clears
};

// An ABI timeout in LK's milliseconds, long enough that a wait never ends before timeout_ns has
// passed by the ns clock: rounded up, plus one for the truncated start. 0 stays 0.
lk_time_t to_lk_time(uint64_t ns);

} // namespace lkuser
