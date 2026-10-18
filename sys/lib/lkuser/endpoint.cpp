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

lk::ref_ptr<endpoint> endpoint::create_kernel(const char *name, kernel_handler_t handler) {
    return object::make<endpoint>(name, handler, 0u, 1u, uint64_t{0});
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

// Runs the handler of a kernel endpoint on the calling thread, then writes the reply header
// at offset 0. The request has been checked and its prefix copied.
status_t call_kernel(thread *t, const connection &conn, pending_call &pc, const lk_msg_hdr &hdr) {
    pc.reply_len = 0;
    kcall c(t, conn, pc, hdr);
    const lk_status_t status = conn.ep()->handler()(c);
    if (c.faulted_) {
        return ERR_FAULT;
    }

    const size_t written = MAX(pc.reply_len, sizeof(lk_msg_hdr));
    const lk_msg_hdr reply = {
        .op = hdr.op,
        .len = static_cast<uint32_t>(written - sizeof(lk_msg_hdr)),
        .nhandles = 0,
        .status = status,
    };
    if (pc.reply.write(0, &reply, sizeof(reply)) != sizeof(reply)) {
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
    // carrying handles: not yet
    if (args.nhandles > 0) {
        return ERR_NOT_SUPPORTED;
    }
    if (args.reply_nhandles > LK_MSG_MAX_HANDLES) {
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

    err = call_kernel(t, *conn, pc, hdr);
    if (err < 0) {
        return err;
    }

    const size_t reply_len = pc.reply_len;
    return copy_to_user(aspace, uargs.addr() + offsetof(lk_call_args, reply_len), &reply_len,
                        sizeof(reply_len));
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

    lk::ref_ptr<endpoint> ep =
        object::make<endpoint>(nullptr, nullptr, args.flags, args.max_active, args.cookie);
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
