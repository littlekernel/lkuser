/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// wait(): a thread blocking until one of several objects raises a signal.

#include <kernel/thread.h>
#include <lk/err.h>
#include <platform.h>
#include <sys/lk_msg.h>

#include "copy.h"
#include "msg.h"
#include "object.h"
#include "proc.h"
#include "syscalls.h"
#include "thread.h"

namespace lkuser {

namespace {

// The first item whose object shows a signal in its mask. Under LKUSER_IPC_LOCK.
bool first_signaled_locked(const lk::ref_ptr<object> *objs, const lk_wait_item *items, size_t n,
                           size_t *index, uint32_t *seen) {
    for (size_t i = 0; i < n; i++) {
        const uint32_t s = objs[i]->signals_locked();
        if (s & items[i].mask) {
            *index = i;
            *seen = s;
            return true;
        }
    }
    return false;
}

status_t write_result(vmm_aspace_t *aspace, user_ptr<lk_wait_args> uargs, size_t index,
                      uint32_t seen) {
    // index and signals are adjacent out fields
    status_t err = copy_to_user(aspace, uargs.addr() + offsetof(lk_wait_args, index), &index,
                                sizeof(index));
    if (err == NO_ERROR) {
        err = copy_to_user(aspace, uargs.addr() + offsetof(lk_wait_args, signals), &seen,
                           sizeof(seen));
    }
    return err;
}

} // namespace

// The thread links an edge onto every object and blocks on its own queue, so one signal from
// any of them wakes it. NO_ERROR once a signal in an item's mask is up (at once if one already
// is), and index and signals name the first such item and what its object showed; ERR_TIMED_OUT
// if none rose in time, reporting item 0. A handle lacking WAIT is ERR_ACCESS_DENIED, and index
// names it.
status_t sys_wait(user_ptr<lk_wait_args> uargs) {
    thread *t = thread::current();
    proc *p = t->process();

    lk_wait_args args;
    status_t err = copy_from_user(p->aspace(), &args, uargs, sizeof(args));
    if (err < 0) {
        return err;
    }
    if (args.n == 0 || args.n > LK_WAIT_MAX_ITEMS) {
        return ERR_INVALID_ARGS;
    }
    lk_wait_item items[LK_WAIT_MAX_ITEMS];
    err = copy_from_user(p->aspace(), items, reinterpret_cast<vaddr_t>(args.items),
                         args.n * sizeof(lk_wait_item));
    if (err < 0) {
        return err;
    }
    lk::ref_ptr<object> objs[LK_WAIT_MAX_ITEMS];
    for (size_t i = 0; i < args.n; i++) {
        err = p->handles().get(items[i].h, LK_RIGHT_WAIT, &objs[i]);
        if (err < 0) {
            const status_t cerr = write_result(p->aspace(), uargs, i, 0);
            return cerr < 0 ? cerr : err;
        }
    }

    wait_edge edges[LK_WAIT_MAX_ITEMS];
    size_t index = 0;
    uint32_t seen = 0;
    {
        LKUSER_IPC_LOCK(state);
        if (first_signaled_locked(objs, items, args.n, &index, &seen)) {
            err = NO_ERROR;
        } else {
            for (size_t i = 0; i < args.n; i++) {
                edges[i].wq = t->wait_queue();
                edges[i].mask = items[i].mask;
                objs[i]->add_waiter_locked(&edges[i]);
            }
            // a signal raised and cleared again before this thread ran wakes it for nothing
            const lk_time_t timeout = to_lk_time(args.timeout_ns);
            const lk_time_t start = current_time();
            bool found;
            for (;;) {
                lk_time_t left = INFINITE_TIME;
                if (timeout != INFINITE_TIME) {
                    const lk_time_t gone = current_time() - start;
                    left = gone < timeout ? timeout - gone : 0;
                }
                err = wait_queue_block_etc(t->wait_queue(), left, WAIT_QUEUE_INTERRUPTIBLE);
                found = first_signaled_locked(objs, items, args.n, &index, &seen);
                if (found || err != NO_ERROR) {
                    break;
                }
            }
            for (size_t i = 0; i < args.n; i++) {
                objs[i]->remove_waiter_locked(&edges[i]);
            }
            if (found) {
                err = NO_ERROR;
            } else {
                index = 0;
                seen = objs[0]->signals_locked();
            }
        }
        LKUSER_IPC_UNLOCK(state);
    }
    if (err == ERR_INTERRUPTED) {
        return err;
    }

    const status_t cerr = write_result(p->aspace(), uargs, index, seen);
    return cerr < 0 ? cerr : err;
}

} // namespace lkuser
