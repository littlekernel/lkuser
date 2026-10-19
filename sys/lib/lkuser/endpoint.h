/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <kernel/wait.h>
#include <lk/cpp.h>
#include <lk/err.h>
#include <lktl/list.h>
#include <lktl/ref_ptr.h>
#include <string.h>
#include <sys/lk_msg.h>
#include <sys/types.h>

#include "impulse.h"
#include "msg.h"
#include "object.h"

namespace lkuser {

class connection;
class group;
class kcall;
class proc;
class thread;

// the hooks of a group's member lists and of an endpoint's list of connections
struct group_tag {};
struct conn_tag {};

// A kernel endpoint's handler: runs on the calling thread, in the caller's address space,
// with interrupts enabled, and returns the service's status, which the reply header carries.
using kernel_handler_t = lk_status_t (*)(kcall &call);

// A kernel endpoint's per-connection cleanup: called with a connection's cookie when the
// connection is destroyed, on whichever thread drops the last reference, with no lock held and
// perhaps no user address space. State a service keeps per connection (an open file) thus
// lives as long as the connection object, which a call in progress keeps alive past its last
// handle.
using kernel_release_t = void (*)(uint64_t cookie);

// The receiving side of a service: a handler in the kernel, or the threads of a user server
// receiving on it, directly or through a group.
class endpoint : public object, public lk::list_hook<group_tag> {
public:
    static constexpr obj_kind kKind = obj_kind::ENDPOINT;

    // name is static text, for the console; release_hook may be null
    static lk::ref_ptr<endpoint> create_kernel(const char *name, kernel_handler_t handler,
                                               kernel_release_t release_hook);
    // a user server's, with LK_EP_FLAG_* flags; empty if out of memory
    static lk::ref_ptr<endpoint> create_user(uint32_t flags, uint32_t max_active, uint64_t cookie);

    bool is_kernel() const { return handler_ != nullptr; }
    kernel_handler_t handler() const { return handler_; }
    kernel_release_t release_hook() const { return release_hook_; }
    const char *name() const { return name_; }
    uint32_t flags() const { return flags_; }
    uint32_t max_active() const { return max_active_; }
    uint64_t cookie() const { return cookie_; }
    uint32_t active_debug() const { return __atomic_load_n(&active, __ATOMIC_RELAXED); }

    // Set when the last handle to a user endpoint closes: callers queued on it and threads
    // blocked in recv on it get ERR_CHANNEL_CLOSED, queued impulses are dropped, it leaves its
    // group, and its connections raise PEER_CLOSED; every later call or impulse over them fails
    // with ERR_CHANNEL_CLOSED. Calls already received can still be replied to. A kernel endpoint
    // has no handles and never closes.
    bool closed() const { return __atomic_load_n(&closed_, __ATOMIC_ACQUIRE); }
    void close();

    // A user endpoint's queues, under LKUSER_IPC_LOCK, worked by ipc.cpp, impulse.cpp and
    // group.cpp. pending holds calls not yet received, impulses the impulses, and active counts
    // the calls received and not yet ended, at most max_active.
    bool closed_locked() const { return closed_; }
    bool has_call_locked() const { return !pending.is_empty() && active < max_active_; }
    bool has_work_locked() const { return !impulses.is_empty() || has_call_locked(); }
    // after the queues or active changed: the READABLE signal, and the endpoint's place in its
    // group's lists, at the back of the one it now belongs on
    void changed_locked();
    // wakes a thread blocked in recv on the endpoint, or failing that one on its group
    void wake_receiver_locked();

    lk::list<pending_call> pending;
    lk::list<impulse_node> impulses;
    wait_queue_t receivers;
    lk::list<connection, lk::base_hook_traits<connection, conn_tag>> conns;
    group *grp = nullptr;
    uint32_t active = 0;

private:
    friend class object;

    endpoint(const char *name, kernel_handler_t fn, kernel_release_t rel, uint32_t flags,
             uint32_t max_active, uint64_t cookie);
    ~endpoint();
    DISALLOW_COPY_ASSIGN_AND_MOVE(endpoint);

    const char *const name_;
    const kernel_handler_t handler_;
    const kernel_release_t release_hook_;
    const uint32_t flags_;
    const uint32_t max_active_;
    const uint64_t cookie_;
    bool closed_ = false;
};

// A client's way to an endpoint, made by connect, or by a kernel service with a cookie naming
// its state for the connection (namespace.OPEN's open file). One object with two views: the
// client's handles, and with CONNECT_NOTIFY the server's peer handles (kRightPeerView), from
// connection_accept, which set the cookie and the READABLE/WRITABLE/ERROR signals the client
// waits on. The last client handle closing sends a user endpoint CONN_CLOSED with the cookie;
// peer operations fail with ERR_CHANNEL_CLOSED from then on.
class connection : public object, public lk::list_hook<conn_tag> {
public:
    static constexpr obj_kind kKind = obj_kind::CONNECTION;

    endpoint *ep() const { return ep_.get(); }
    // zero-extended to the 64 bits of the wire fields it is reported in
    uint64_t cookie() const { return __atomic_load_n(&cookie_, __ATOMIC_RELAXED); }
    uint32_t client_handles_debug() const { return __atomic_load_n(&client_handles_, __ATOMIC_RELAXED); }

    // under LKUSER_IPC_LOCK
    void set_cookie_locked(uintptr_t cookie) { __atomic_store_n(&cookie_, cookie, __ATOMIC_RELAXED); }
    bool client_closed_locked() const { return client_closed_; }

    // a client view came or went (object::handle_added/handle_removed)
    void client_handle_added() { __atomic_fetch_add(&client_handles_, 1, __ATOMIC_RELAXED); }
    void client_handle_removed();
    // The client side has gone, or a handshake the server accepted never reached the client:
    // a user endpoint gets CONN_CLOSED. Once only.
    void client_closed();

private:
    friend class object;

    explicit connection(lk::ref_ptr<endpoint> ep, uintptr_t cookie = 0);
    // runs a kernel endpoint's release hook
    ~connection();
    DISALLOW_COPY_ASSIGN_AND_MOVE(connection);

    const lk::ref_ptr<endpoint> ep_;
    // register width, like every value it is set from (a row's cookie argument, a kernel
    // pointer), so it reads and writes atomically on a 32-bit build too
    uintptr_t cookie_;
    uint32_t client_handles_ = 0;
    // under LKUSER_IPC_LOCK
    bool client_closed_ = false;
    impulse_node closed_impulse_;
};

// A connection to ep installed in p's table with the default rights.
status_t connect(proc *p, lk::ref_ptr<endpoint> ep, lk_handle_t *out);

// A call to a kernel endpoint as its handler sees it. The request header has been read and
// checked before the handler runs, and the reply header is written after it returns, with the
// handler's status and as many payload bytes as it wrote. Offsets count from the end of the
// header on both sides.
//
// A fault on the caller's buffers is the caller's mistake, not the service's: the call then
// returns ERR_FAULT whatever the handler returns.
class kcall {
public:
    uint32_t op() const { return hdr_.op; }
    size_t len() const { return hdr_.len; }  // request payload
    size_t reply_capacity() const;           // reply payload
    size_t reply_handles_capacity() const { return pc_.reply_handles_cap; }
    proc *caller() const;
    thread *caller_thread() const { return thread_; }
    const connection &conn() const { return *conn_; }

    // the handles the request carries
    size_t nhandles() const { return nhandles_; }
    const carried_handle &handle(size_t i) const { return handles_[i]; }

    // Handle i as a T with every right in need: ERR_INVALID_ARGS past the last handle, then
    // ERR_ACCESS_DENIED for a missing right, then ERR_BAD_HANDLE for another kind of object.
    template <typename T>
    lk_status_t handle_as(size_t i, uint32_t need, lk::ref_ptr<T> *out) const {
        if (i >= nhandles_) {
            return ERR_INVALID_ARGS;
        }
        const carried_handle &ch = handles_[i];
        if ((ch.rights & need) != need) {
            return ERR_ACCESS_DENIED;
        }
        if (ch.obj->kind() != T::kKind) {
            return ERR_BAD_HANDLE;
        }
        *out = lk::static_ref_cast<T>(ch.obj);
        return NO_ERROR;
    }

    // ERR_INVALID_ARGS unless the request carries exactly n handles, for the ops that take
    // none or a fixed number, so that nothing is moved to a service that ignores it.
    lk_status_t expect_handles(size_t n) const { return nhandles_ == n ? NO_ERROR : ERR_INVALID_ARGS; }

    // The request's payload as a T: ERR_INVALID_ARGS unless it is exactly that long.
    template <typename T>
    lk_status_t args(T *out) const {
        static_assert(sizeof(lk_msg_hdr) + sizeof(T) <= kRecvPrefix, "fits in the prefix");
        if (len() != sizeof(T)) {
            return ERR_INVALID_ARGS;
        }
        memcpy(out, pc_.prefix + sizeof(lk_msg_hdr), sizeof(T));
        return NO_ERROR;
    }

    // Request payload [off, off + len) into buf, clipped at its end: the count, or ERR_FAULT.
    ssize_t read(size_t off, void *buf, size_t len);
    // buf into reply payload [off, off + len), clipped at the capacity: the count, or
    // ERR_FAULT.
    ssize_t write(size_t off, const void *buf, size_t len);

    // The reply's payload as a whole T at offset 0: ERR_NOT_ENOUGH_BUFFER if it does not fit.
    template <typename T>
    lk_status_t reply(const T &r) {
        if (reply_capacity() < sizeof(T)) {
            return ERR_NOT_ENOUGH_BUFFER;
        }
        ssize_t n = write(0, &r, sizeof(T));
        return n < 0 ? static_cast<lk_status_t>(n) : NO_ERROR;
    }

    // A handle to obj with rights, carried by the reply. It goes into the caller's table now,
    // so a full table is the handler's ERR_NO_RESOURCES to report, and is closed again if the
    // handler returns an error or the call fails. ERR_NOT_ENOUGH_BUFFER past the caller's room
    // for reply handles; a handler checks reply_handles_capacity() before doing work it would
    // have to undo.
    lk_status_t reply_handle(lk::ref_ptr<object> obj, uint32_t rights);

private:
    friend status_t call_kernel(thread *t, const connection &conn, pending_call &pc,
                                const lk_msg_hdr &hdr, const carried_handle *handles,
                                size_t nhandles);

    kcall(thread *t, const connection &conn, pending_call &pc, const lk_msg_hdr &hdr,
          const carried_handle *handles, size_t nhandles)
        : thread_(t), conn_(&conn), pc_(pc), hdr_(hdr), handles_(handles), nhandles_(nhandles) {}
    DISALLOW_COPY_ASSIGN_AND_MOVE(kcall);

    ssize_t fault_check(ssize_t r) {
        if (r == ERR_FAULT) {
            faulted_ = true;
        }
        return r;
    }

    thread *const thread_;
    const connection *const conn_;
    pending_call &pc_;
    const lk_msg_hdr hdr_;
    const carried_handle *const handles_;
    const size_t nhandles_;
    bool faulted_ = false;
};

} // namespace lkuser
