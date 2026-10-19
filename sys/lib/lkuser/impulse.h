/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lktl/list.h>
#include <sys/lk_msg.h>
#include <sys/types.h>

#include "object.h"

#ifndef LKUSER_IMPULSE_POOL
#define LKUSER_IMPULSE_POOL 32
#endif

namespace lkuser {

class endpoint;
class proc;

// An impulse on its way to an endpoint. One a process sends comes from that process's pool;
// the kernel's lifecycle impulses are embedded in the object they are about, one each, so
// sending them never fails for want of an entry. While queued it holds a reference to its
// holder (the sending process, or the object it is embedded in), dropped once it is received
// or thrown away.
struct impulse_node : lk::list_hook<> {
    lk_impulse imp = {};
    uint32_t sender_pid = 0;
    uint32_t sender_tid = 0;
    object *holder = nullptr;
    proc *pool = nullptr; // the process whose pool it goes back to, or null if embedded
};

// Queues n on ep, ahead of every call, with a reference to holder: ERR_CHANNEL_CLOSED if ep
// has closed. Under LKUSER_IPC_LOCK.
status_t queue_impulse_locked(endpoint *ep, impulse_node *n, object *holder);

// Gives back an impulse taken off a queue: to its pool if it has one, and its holder's
// reference. Without LKUSER_IPC_LOCK held.
void release_impulse(impulse_node *n);

} // namespace lkuser
