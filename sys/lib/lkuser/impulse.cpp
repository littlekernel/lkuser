/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// Impulses: datagrams queued on a user endpoint and received ahead of its calls, so that
// notifications are never starved by request load.

#include "impulse.h"

#include <lk/err.h>

#include "endpoint.h"
#include "handle.h"
#include "proc.h"
#include "syscalls.h"
#include "thread.h"

namespace lkuser {

status_t queue_impulse_locked(endpoint *ep, impulse_node *n, object *holder) {
    if (ep->is_kernel()) {
        return ERR_NOT_SUPPORTED;
    }
    if (ep->closed_locked()) {
        return ERR_CHANNEL_CLOSED;
    }
    holder->add_ref();
    n->holder = holder;
    ep->impulses.push_back(n);
    ep->changed_locked();
    ep->wake_receiver_locked();
    return NO_ERROR;
}

void release_impulse(impulse_node *n) {
    object *holder = n->holder;
    {
        LKUSER_IPC_LOCK(state);
        n->holder = nullptr;
        if (n->pool) {
            n->pool->free_impulses.push_back(n);
        }
        LKUSER_IPC_UNLOCK(state);
    }
    // the pool's process may go with this, but nothing touches the entry after it is back
    holder->release();
}

// The datagram lands on the connection's endpoint with the connection's cookie, whatever the
// sender put there. Types below LK_IMPULSE_USER_BASE are the kernel's own.
status_t sys_impulse_send(lk_handle_t conn_h, user_ptr<const lk_impulse> uimp) {
    thread *t = thread::current();
    proc *p = t->process();

    lk::ref_ptr<connection> conn;
    status_t err = p->handles().get(conn_h, LK_RIGHT_IMPULSE, &conn);
    if (err < 0) {
        return err;
    }
    lk_impulse imp;
    err = copy_from_user(p->aspace(), &imp, uimp, sizeof(imp));
    if (err < 0) {
        return err;
    }
    if (imp.type < LK_IMPULSE_USER_BASE || imp.flags != 0) {
        return ERR_INVALID_ARGS;
    }
    endpoint *ep = conn->ep();
    if (ep->is_kernel()) {
        return ERR_NOT_SUPPORTED;
    }

    LKUSER_IPC_LOCK(state);
    impulse_node *n = nullptr;
    if (ep->closed_locked()) {
        err = ERR_CHANNEL_CLOSED;
    } else if (!(n = p->free_impulses.pop_front())) {
        err = ERR_NO_RESOURCES;
    } else {
        n->imp = imp;
        n->imp.cookie = conn->cookie();
        n->sender_pid = p->pid();
        n->sender_tid = t->tid();
        err = queue_impulse_locked(ep, n, p);
    }
    LKUSER_IPC_UNLOCK(state);
    return err;
}

} // namespace lkuser
