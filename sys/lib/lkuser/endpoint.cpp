/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "endpoint.h"

#include <stddef.h>
#include <string.h>

#include "handle.h"
#include "proc.h"
#include "syscalls.h"
#include "thread.h"

namespace lkuser {

lk::ref_ptr<endpoint> endpoint::create_kernel(const char *name, kernel_handler_t handler,
                                              kernel_release_t release_hook) {
    return object::make<endpoint>(name, handler, release_hook, 0u, 1u, uint64_t{0});
}

connection::~connection() {
    if (ep_->release_hook()) {
        ep_->release_hook()(cookie_);
    }
}

status_t connect(proc *p, lk::ref_ptr<endpoint> ep, lk_handle_t *out) {
    lk::ref_ptr<connection> conn = object::make<connection>(std::move(ep));
    if (!conn) {
        return ERR_NO_MEMORY;
    }
    return p->handles().add(std::move(conn), kConnectionRights, out);
}

size_t kcall::reply_capacity() const {
    return pc_.reply.len() - sizeof(lk_msg_hdr);
}

proc *kcall::caller() const {
    return thread_->process();
}

ssize_t kcall::read(size_t off, void *buf, size_t len) {
    if (off > this->len()) {
        return 0;
    }
    len = MIN(len, this->len() - off);
    return fault_check(pc_.req.read(sizeof(lk_msg_hdr) + off, buf, len));
}

ssize_t kcall::write(size_t off, const void *buf, size_t len) {
    ssize_t n = fault_check(pc_.reply.write(sizeof(lk_msg_hdr) + off, buf, len));
    if (n > 0) {
        pc_.reply_len = MAX(pc_.reply_len, sizeof(lk_msg_hdr) + off + n);
    }
    return n;
}

lk_status_t kcall::reply_handle(lk::ref_ptr<object> obj, uint32_t rights) {
    if (pc_.nreply_handles == pc_.reply_handles_cap) {
        return ERR_NOT_ENOUGH_BUFFER;
    }
    const object *o = obj.get();
    lk_handle_t h;
    status_t err = caller()->handles().add(std::move(obj), rights, &h);
    if (err < 0) {
        return err;
    }
    pc_.reply_handles[pc_.nreply_handles++] = {h, o};
    return NO_ERROR;
}

namespace {

// Takes back the handles a reply was to carry, when the call cannot deliver them.
void drop_reply_handles(proc *p, pending_call &pc) {
    for (size_t i = 0; i < pc.nreply_handles; i++) {
        p->handles().close_if(pc.reply_handles[i].h, pc.reply_handles[i].obj);
    }
    pc.nreply_handles = 0;
}

// The handles a request carries, looked up in p's table: ERR_FAULT for an unreadable array,
// ERR_BAD_HANDLE for no such handle, ERR_ACCESS_DENIED for a moved one without TRANSFER, and
// ERR_INVALID_ARGS for a handle moved and named again in the same request.
status_t carry_request_handles(proc *p, const lk_call_args &args, carried_handle *out) {
    lk_handle_t hs[LK_MSG_MAX_HANDLES];
    status_t err = copy_from_user(p->aspace(), hs, reinterpret_cast<vaddr_t>(args.handles),
                                  args.nhandles * sizeof(lk_handle_t));
    if (err < 0) {
        return err;
    }
    for (size_t i = 0; i < args.nhandles; i++) {
        const bool lent = args.handle_flags & LK_HANDLE_LEND(i);
        for (size_t j = 0; j < i; j++) {
            if (hs[j] == hs[i] && !(lent && out[j].lent)) {
                return ERR_INVALID_ARGS;
            }
        }
        out[i].h = hs[i];
        out[i].lent = lent;
        err = p->handles().get(hs[i], lent ? 0 : LK_RIGHT_TRANSFER, &out[i].obj, &out[i].rights);
        if (err < 0) {
            return err;
        }
        if (lent) {
            out[i].rights &= ~(LK_RIGHT_DUP | LK_RIGHT_TRANSFER);
        }
    }
    return NO_ERROR;
}

} // namespace

// Runs the handler of a kernel endpoint on the calling thread, then writes the reply header
// at offset 0. The request has been checked and its prefix copied. A refusal carries no
// handles, so a client that ignores a failed reply leaks none, and moves none: the moved
// handles leave the caller only when the service accepted the call, even if writing the reply
// then fails. A killed thread's call (ERR_INTERRUPTED) gets no reply at all; the thread ends
// on its way out.
status_t call_kernel(thread *t, const connection &conn, pending_call &pc, const lk_msg_hdr &hdr,
                     const carried_handle *handles, size_t nhandles) {
    pc.reply_len = 0;
    pc.nreply_handles = 0;
    kcall c(t, conn, pc, hdr, handles, nhandles);
    const lk_status_t status = conn.ep()->handler()(c);
    if (status == ERR_INTERRUPTED) {
        drop_reply_handles(t->process(), pc);
        return ERR_INTERRUPTED;
    }
    if (status >= 0) {
        for (size_t i = 0; i < nhandles; i++) {
            if (!handles[i].lent) {
                t->process()->handles().close_if(handles[i].h, handles[i].obj.get());
            }
        }
    }
    if (c.faulted_ || status < 0) {
        drop_reply_handles(t->process(), pc);
    }
    if (c.faulted_) {
        return ERR_FAULT;
    }

    const size_t written = MAX(pc.reply_len, sizeof(lk_msg_hdr));
    const lk_msg_hdr reply = {
        .op = hdr.op,
        .len = static_cast<uint32_t>(written - sizeof(lk_msg_hdr)),
        .nhandles = static_cast<uint32_t>(pc.nreply_handles),
        .status = status,
    };
    if (pc.reply.write(0, &reply, sizeof(reply)) != sizeof(reply)) {
        drop_reply_handles(t->process(), pc);
        return ERR_FAULT;
    }
    pc.reply_len = written;
    return NO_ERROR;
}

// The transport checks every request before a service sees it: a header that fits, a reply
// buffer that can take one, an op the client may send and a length that matches the iovecs.
// Its status says whether a reply arrived; the service's verdict is in the reply header.
status_t sys_call(lk_handle_t conn_h, user_ptr<lk_call_args> uargs) {
    thread *t = thread::current();
    proc *p = t->process();
    vmm_aspace_t *aspace = p->aspace();

    lk_call_args args;
    status_t err = copy_from_user(aspace, &args, uargs, sizeof(args));
    if (err < 0) {
        return err;
    }

    lk::ref_ptr<connection> conn;
    err = p->handles().get(conn_h, LK_RIGHT_CALL, &conn);
    if (err < 0) {
        return err;
    }
    endpoint *ep = conn->ep();
    if (ep->closed()) {
        return ERR_CHANNEL_CLOSED;
    }
    // a user endpoint is served by recv, which no thread can call yet
    if (!ep->is_kernel()) {
        return ERR_NOT_SUPPORTED;
    }
    if (args.nhandles > LK_MSG_MAX_HANDLES || args.reply_nhandles > LK_MSG_MAX_HANDLES ||
        (args.handle_flags & ~((1u << args.nhandles) - 1))) {
        return ERR_INVALID_ARGS;
    }

    pending_call &pc = t->call();
    err = pc.req.load(aspace, user_ptr<const lk_iov>(reinterpret_cast<vaddr_t>(args.req)),
                      args.nreq);
    if (err < 0) {
        return err;
    }
    err = pc.reply.load(aspace, user_ptr<const lk_iov>(reinterpret_cast<vaddr_t>(args.reply)),
                        args.nreply);
    if (err < 0) {
        return err;
    }
    if (pc.req.len() < sizeof(lk_msg_hdr) || pc.reply.len() < sizeof(lk_msg_hdr)) {
        return ERR_INVALID_ARGS;
    }

    const size_t prefix_len = MIN(pc.req.len(), kRecvPrefix);
    ssize_t n = pc.req.read(0, pc.prefix, prefix_len);
    if (n < 0) {
        return static_cast<status_t>(n);
    }
    lk_msg_hdr hdr;
    memcpy(&hdr, pc.prefix, sizeof(hdr));
    if (hdr.op >= LK_OP_KERNEL_BASE || hdr.len != pc.req.len() - sizeof(hdr) ||
        hdr.nhandles != args.nhandles) {
        return ERR_INVALID_ARGS;
    }
    carried_handle carried[LK_MSG_MAX_HANDLES];
    err = carry_request_handles(p, args, carried);
    if (err < 0) {
        return err;
    }

    // Where the results go must be writable before the service runs, so a call that could
    // not report them has no effects: reply_len, and the reply handles, cleared to
    // LK_HANDLE_INVALID beyond those the reply carries.
    const user_ptr<size_t> ureply_len(uargs.addr() + offsetof(lk_call_args, reply_len));
    const user_ptr<lk_handle_t> ureply_handles(reinterpret_cast<vaddr_t>(args.reply_handles));
    size_t reply_len = 0;
    err = copy_to_user(aspace, ureply_len, &reply_len, sizeof(reply_len));
    if (err < 0) {
        return err;
    }
    lk_handle_t handles[LK_MSG_MAX_HANDLES] = {};
    err = copy_to_user(aspace, ureply_handles, handles, args.reply_nhandles * sizeof(lk_handle_t));
    if (err < 0) {
        return err;
    }
    pc.reply_handles_cap = args.reply_nhandles;

    err = call_kernel(t, *conn, pc, hdr, carried, args.nhandles);
    if (err < 0) {
        return err;
    }

    for (size_t i = 0; i < pc.nreply_handles; i++) {
        handles[i] = pc.reply_handles[i].h;
    }
    reply_len = pc.reply_len;
    err = copy_to_user(aspace, ureply_handles, handles, pc.nreply_handles * sizeof(lk_handle_t));
    if (err == NO_ERROR) {
        err = copy_to_user(aspace, ureply_len, &reply_len, sizeof(reply_len));
    }
    if (err < 0) {
        drop_reply_handles(p, pc);
    }
    return err;
}

status_t sys_endpoint_create(user_ptr<const lk_endpoint_args> uargs, user_ptr<lk_handle_t> out) {
    proc *p = thread::current()->process();

    lk_endpoint_args args;
    status_t err = copy_from_user(p->aspace(), &args, uargs, sizeof(args));
    if (err < 0) {
        return err;
    }
    if ((args.flags & ~LK_EP_FLAG_CONNECT_NOTIFY) || args.max_active == 0) {
        return ERR_INVALID_ARGS;
    }
    // the handshake is a call the server receives, so it waits for recv
    if (args.flags & LK_EP_FLAG_CONNECT_NOTIFY) {
        return ERR_NOT_SUPPORTED;
    }

    lk::ref_ptr<endpoint> ep = object::make<endpoint>(nullptr, nullptr, nullptr, args.flags,
                                                      args.max_active, args.cookie);
    if (!ep) {
        return ERR_NO_MEMORY;
    }
    return p->handles().add_for_user(p->aspace(), std::move(ep), kEndpointRights, out);
}

status_t sys_connect(lk_handle_t ep_h, user_ptr<lk_handle_t> out) {
    proc *p = thread::current()->process();

    lk::ref_ptr<endpoint> ep;
    status_t err = p->handles().get(ep_h, LK_RIGHT_CONNECT, &ep);
    if (err < 0) {
        return err;
    }
    lk::ref_ptr<connection> conn = object::make<connection>(std::move(ep));
    if (!conn) {
        return ERR_NO_MEMORY;
    }
    return p->handles().add_for_user(p->aspace(), std::move(conn), kConnectionRights, out);
}

// Tokens come only from recv, which no thread can call yet, so none names a live call.
status_t sys_msg_read(lk_token_t, size_t, user_ptr<const lk_iov>, size_t) {
    return ERR_NOT_FOUND;
}

status_t sys_msg_write(lk_token_t, size_t, user_ptr<const lk_iov>, size_t) {
    return ERR_NOT_FOUND;
}

} // namespace lkuser
