/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// wait(): a thread blocking until an object raises a signal.

#include <kernel/thread.h>
#include <lk/err.h>
#include <sys/lk_msg.h>

#include "copy.h"
#include "object.h"
#include "proc.h"
#include "syscalls.h"
#include "thread.h"

namespace lkuser {

namespace {

// An ABI timeout in LK's milliseconds, rounded up so a wait is never shorter than asked.
lk_time_t to_lk_time(uint64_t ns) {
    if (ns == LK_TIME_INFINITE) {
        return INFINITE_TIME;
    }
    const uint64_t ms = ns / 1000000 + (ns % 1000000 != 0);
    return static_cast<lk_time_t>(MIN(ms, uint64_t{INFINITE_TIME - 1}));
}

} // namespace

// One item for now: the thread links an edge onto the object and blocks on its own queue, so
// waiting on several is the same with an edge per item. NO_ERROR once a signal in the mask is
// up (at once if it already is), ERR_TIMED_OUT if none rose in time; either way index and
// signals report what the object showed.
status_t sys_wait(user_ptr<lk_wait_args> uargs) {
    thread *t = thread::current();
    proc *p = t->process();

    lk_wait_args args;
    status_t err = copy_from_user(p->aspace(), &args, uargs, sizeof(args));
    if (err < 0) {
        return err;
    }
    if (args.n == 0) {
        return ERR_INVALID_ARGS;
    }
    if (args.n > 1) {
        return ERR_NOT_SUPPORTED;
    }
    lk_wait_item item;
    err = copy_from_user(p->aspace(), &item, reinterpret_cast<vaddr_t>(args.items), sizeof(item));
    if (err < 0) {
        return err;
    }
    lk::ref_ptr<object> obj;
    err = p->handles().get(item.h, LK_RIGHT_WAIT, &obj);
    if (err < 0) {
        return err;
    }

    wait_edge edge;
    edge.wq = t->wait_queue();
    edge.mask = item.mask;
    uint32_t seen;
    {
        LKUSER_IPC_LOCK(state);
        seen = obj->signals_locked();
        if (seen & item.mask) {
            err = NO_ERROR;
        } else {
            obj->add_waiter_locked(&edge);
            err = wait_queue_block_etc(t->wait_queue(), to_lk_time(args.timeout_ns),
                                       WAIT_QUEUE_INTERRUPTIBLE);
            obj->remove_waiter_locked(&edge);
            seen = obj->signals_locked();
        }
        LKUSER_IPC_UNLOCK(state);
    }
    if (err == ERR_INTERRUPTED) {
        return err;
    }
    if (seen & item.mask) {
        err = NO_ERROR;
    }

    // index and signals are adjacent out fields
    const size_t index = 0;
    status_t cerr = copy_to_user(p->aspace(), uargs.addr() + offsetof(lk_wait_args, index),
                                 &index, sizeof(index));
    if (cerr == NO_ERROR) {
        cerr = copy_to_user(p->aspace(), uargs.addr() + offsetof(lk_wait_args, signals), &seen,
                            sizeof(seen));
    }
    return cerr < 0 ? cerr : err;
}

} // namespace lkuser
