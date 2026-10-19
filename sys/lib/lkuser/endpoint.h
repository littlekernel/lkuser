/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lk/cpp.h>
#include <lk/err.h>
#include <lktl/ref_ptr.h>
#include <string.h>
#include <sys/lk_msg.h>
#include <sys/types.h>

#include "msg.h"
#include "object.h"

namespace lkuser {

class kcall;
class proc;
class thread;

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
// receiving on it.
class endpoint : public object {
public:
    static constexpr obj_kind kKind = obj_kind::ENDPOINT;

    // name is static text, for the console; release_hook may be null
    static lk::ref_ptr<endpoint> create_kernel(const char *name, kernel_handler_t handler,
                                               kernel_release_t release_hook);

    bool is_kernel() const { return handler_ != nullptr; }
    kernel_handler_t handler() const { return handler_; }
    kernel_release_t release_hook() const { return release_hook_; }
    const char *name() const { return name_; }
    uint32_t flags() const { return flags_; }
    uint32_t max_active() const { return max_active_; }
    uint64_t cookie() const { return cookie_; }

    // Set when the last handle to the endpoint closes; calls over its connections then fail
    // with ERR_CHANNEL_CLOSED. A kernel endpoint has no handles and never closes.
    bool closed() const { return __atomic_load_n(&closed_, __ATOMIC_ACQUIRE); }
    void close() { __atomic_store_n(&closed_, true, __ATOMIC_RELEASE); }

private:
    friend class object;

    endpoint(const char *name, kernel_handler_t fn, kernel_release_t rel, uint32_t flags,
             uint32_t max_active, uint64_t cookie)
        : object(kKind), name_(name), handler_(fn), release_hook_(rel), flags_(flags),
          max_active_(max_active), cookie_(cookie) {}
    ~endpoint() = default;
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
// its state for the connection (namespace.OPEN's open file).
class connection : public object {
public:
    static constexpr obj_kind kKind = obj_kind::CONNECTION;

    endpoint *ep() const { return ep_.get(); }
    uint64_t cookie() const { return cookie_; }

private:
    friend class object;

    explicit connection(lk::ref_ptr<endpoint> ep, uint64_t cookie = 0)
        : object(kKind), ep_(std::move(ep)), cookie_(cookie) {}
    // runs a kernel endpoint's release hook
    ~connection();
    DISALLOW_COPY_ASSIGN_AND_MOVE(connection);

    const lk::ref_ptr<endpoint> ep_;
    uint64_t cookie_;
};

// A connection to ep installed in p's table with the default rights.
status_t connect(proc *p, lk::ref_ptr<endpoint> ep, lk_handle_t *out);

// A handle a request carries, as the service sees it. A moved handle (the default) stays in
// the caller's table while the service runs and leaves it once the service returns success; a
// lent one never leaves, and the service sees its rights without DUP and TRANSFER. A service
// keeps a carried object only by reference or by putting it in a table, never a lent one.
struct carried_handle {
    lk_handle_t h; // in the caller's table
    lk::ref_ptr<object> obj;
    uint32_t rights;
    bool lent;
};

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
