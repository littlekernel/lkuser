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
#include <lktl/list.h>

#include "endpoint.h"
#include "object.h"

namespace lkuser {

// Endpoints received on together: recv on a group takes an impulse from any member first, then
// a call from a member below its max_active. Each member sits on exactly one of three lists,
// kept by endpoint::changed_locked, so picking one is O(1):
//   IMPULSES  it has impulses queued
//   CALLS     no impulses, and calls queued while active < max_active
//   IDLE      neither
// and goes to the back of its list whenever it changes, so busy members take turns. A member is
// still receivable on its own; a thread blocked on it is woken before one blocked on the group.
class group : public object {
public:
    static constexpr obj_kind kKind = obj_kind::GROUP;

    enum list_id : uint32_t { IMPULSES, CALLS, IDLE, kLists };

    static lk::ref_ptr<group> create();

    // The last handle has gone: every member leaves, and threads blocked in recv on the group
    // get ERR_CHANNEL_CLOSED.
    void close();

    // under LKUSER_IPC_LOCK
    bool closed_locked() const { return closed_; }
    // the member to take work from next, or null
    endpoint *pick_locked();
    // ERR_BAD_STATE if ep is in a group, ERR_CHANNEL_CLOSED if either has closed
    status_t add_locked(endpoint *ep);
    // ERR_NOT_FOUND unless ep is a member
    status_t remove_locked(endpoint *ep);
    // where ep belongs now, at the back
    void place_locked(endpoint *ep);

    wait_queue_t receivers;

private:
    friend class object;

    group();
    ~group();
    DISALLOW_COPY_ASSIGN_AND_MOVE(group);

    using member_list = lk::list<endpoint, lk::base_hook_traits<endpoint, group_tag>>;

    void unlink_locked(endpoint *ep);

    member_list lists_[kLists];
    bool closed_ = false;
};

} // namespace lkuser
