/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "group.h"

#include <lk/err.h>

#include "handle.h"
#include "proc.h"
#include "syscalls.h"
#include "thread.h"

namespace lkuser {

namespace {

using member_hook = lk::list_hook<group_tag>;

bool is_member_listed(endpoint *ep) {
    return static_cast<member_hook *>(ep)->in_list();
}

} // namespace

group::group() : object(kKind) {
    wait_queue_init(&receivers);
}

group::~group() {
    // a member leaves at the group's last handle, which comes before its destruction
    for (const auto &l : lists_) {
        DEBUG_ASSERT(l.is_empty());
    }
    THREAD_LOCK(state);
    wait_queue_destroy(&receivers, false);
    THREAD_UNLOCK(state);
}

lk::ref_ptr<group> group::create() {
    return object::make<group>();
}

void group::close() {
    LKUSER_IPC_LOCK(state);
    closed_ = true;
    for (auto &l : lists_) {
        while (endpoint *ep = l.pop_front()) {
            ep->grp = nullptr;
            // receivable on its own again
            if (ep->has_work_locked()) {
                wait_queue_wake_one(&ep->receivers, false, NO_ERROR);
            }
        }
    }
    wait_queue_wake_all(&receivers, false, ERR_CHANNEL_CLOSED);
    LKUSER_IPC_UNLOCK(state);
}

endpoint *group::pick_locked() {
    if (endpoint *ep = lists_[IMPULSES].front()) {
        return ep;
    }
    return lists_[CALLS].front();
}

status_t group::add_locked(endpoint *ep) {
    if (closed_ || ep->closed_locked()) {
        return ERR_CHANNEL_CLOSED;
    }
    if (ep->grp) {
        return ERR_BAD_STATE;
    }
    ep->grp = this;
    place_locked(ep);
    if (ep->has_work_locked()) {
        wait_queue_wake_one(&receivers, false, NO_ERROR);
    }
    return NO_ERROR;
}

status_t group::remove_locked(endpoint *ep) {
    if (ep->grp != this) {
        return ERR_NOT_FOUND;
    }
    unlink_locked(ep);
    ep->grp = nullptr;
    // its queued work stays with it, for the threads receiving on it directly
    if (ep->has_work_locked()) {
        wait_queue_wake_one(&ep->receivers, false, NO_ERROR);
    }
    return NO_ERROR;
}

void group::place_locked(endpoint *ep) {
    const list_id id = !ep->impulses.is_empty() ? IMPULSES : ep->has_call_locked() ? CALLS : IDLE;
    unlink_locked(ep);
    lists_[id].push_back(ep);
}

void group::unlink_locked(endpoint *ep) {
    if (is_member_listed(ep)) {
        member_list::remove(ep);
    }
}

status_t sys_group_create(user_ptr<lk_handle_t> out) {
    proc *p = thread::current()->process();
    lk::ref_ptr<group> g = group::create();
    if (!g) {
        return ERR_NO_MEMORY;
    }
    return p->handles().add_for_user(p->aspace(), std::move(g), kGroupRights, out);
}

namespace {

// the group and the endpoint a membership change names, each with RECV
status_t group_and_endpoint(lk_handle_t group_h, lk_handle_t ep_h, lk::ref_ptr<group> *g,
                            lk::ref_ptr<endpoint> *ep) {
    proc *p = thread::current()->process();
    status_t err = p->handles().get(group_h, LK_RIGHT_RECV, g);
    if (err < 0) {
        return err;
    }
    return p->handles().get(ep_h, LK_RIGHT_RECV, ep);
}

} // namespace

// An endpoint joins at most one group; queued work comes along.
status_t sys_group_add(lk_handle_t group_h, lk_handle_t ep_h) {
    lk::ref_ptr<group> g;
    lk::ref_ptr<endpoint> ep;
    status_t err = group_and_endpoint(group_h, ep_h, &g, &ep);
    if (err < 0) {
        return err;
    }
    LKUSER_IPC_LOCK(state);
    err = g->add_locked(ep.get());
    LKUSER_IPC_UNLOCK(state);
    return err;
}

status_t sys_group_remove(lk_handle_t group_h, lk_handle_t ep_h) {
    lk::ref_ptr<group> g;
    lk::ref_ptr<endpoint> ep;
    status_t err = group_and_endpoint(group_h, ep_h, &g, &ep);
    if (err < 0) {
        return err;
    }
    LKUSER_IPC_LOCK(state);
    err = g->remove_locked(ep.get());
    LKUSER_IPC_UNLOCK(state);
    return err;
}

} // namespace lkuser
