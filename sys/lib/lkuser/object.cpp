/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "object.h"

#include <kernel/mutex.h>
#include <lk/debug.h>
#include <stdio.h>

#include "endpoint.h"
#include "memobj.h"
#include "proc.h"
#include "thread.h"

namespace lkuser {

namespace {

lk::list<object, lk::base_hook_traits<object, registry_tag>> registry;
Mutex registry_lock;

} // namespace

const char *kind_name(obj_kind kind) {
    switch (kind) {
        case obj_kind::ENDPOINT:
            return "endpoint";
        case obj_kind::CONNECTION:
            return "connection";
        case obj_kind::GROUP:
            return "group";
        case obj_kind::MEMOBJ:
            return "memobj";
        case obj_kind::PROCESS:
            return "process";
        case obj_kind::THREAD:
            return "thread";
    }
    return "?";
}

void object::add_to_registry(object *o) {
    AutoLock guard(registry_lock);
    registry.push_back(o);
}

void object::destroy(object *o) {
    {
        AutoLock guard(registry_lock);
        registry.remove(o);
    }
    switch (o->kind()) {
        case obj_kind::ENDPOINT:
            delete static_cast<endpoint *>(o);
            return;
        case obj_kind::CONNECTION:
            delete static_cast<connection *>(o);
            return;
        case obj_kind::MEMOBJ:
            delete static_cast<memobj *>(o);
            return;
        case obj_kind::PROCESS:
            delete static_cast<proc *>(o);
            return;
        case obj_kind::THREAD:
            delete static_cast<thread *>(o);
            return;
        default:
            panic("lkuser: no destroy for object %p of kind %u\n", o, (unsigned)o->kind());
    }
}

void object::handle_added() {
    __atomic_fetch_add(&handle_count_, 1, __ATOMIC_RELAXED);
}

void object::handle_removed() {
    const uint32_t old = __atomic_fetch_add(&handle_count_, -1, __ATOMIC_ACQ_REL);
    DEBUG_ASSERT(old > 0);
    if (old == 1) {
        last_handle_removed(this);
    }
}

void object::signal(uint32_t set) {
    LKUSER_IPC_LOCK(state);
    signals_ |= set;
    for (wait_edge &e : waiters_) {
        if (signals_ & e.mask) {
            wait_queue_wake_all(e.wq, false, NO_ERROR);
        }
    }
    LKUSER_IPC_UNLOCK(state);
}

void object::last_handle_removed(object *o) {
    switch (o->kind()) {
        case obj_kind::ENDPOINT:
            static_cast<endpoint *>(o)->close();
            return;
        default:
            return;
    }
}

void object::for_each(lk::function_ref<void(object &)> fn, bool lock) {
    if (lock) {
        registry_lock.acquire();
    }
    for (object &o : registry) {
        fn(o);
    }
    if (lock) {
        registry_lock.release();
    }
}

void object::dump() const {
    printf("%-10s %p refs %d handles %u", kind_name(kind()), this, ref_count_debug(),
           handle_count_debug());
    if (signals_debug()) {
        printf(" signals %#x", signals_debug());
    }
    switch (kind()) {
        case obj_kind::ENDPOINT: {
            const auto *ep = static_cast<const endpoint *>(this);
            if (ep->is_kernel()) {
                printf(" kernel '%s'", ep->name());
            } else {
                printf(" user flags %#x max_active %u cookie %#llx%s", ep->flags(),
                       ep->max_active(), (unsigned long long)ep->cookie(),
                       ep->closed() ? " closed" : "");
            }
            break;
        }
        case obj_kind::CONNECTION: {
            const auto *conn = static_cast<const connection *>(this);
            printf(" to %p cookie %#llx", conn->ep(), (unsigned long long)conn->cookie());
            break;
        }
        case obj_kind::MEMOBJ: {
            const auto *m = static_cast<const memobj *>(this);
            printf(" size %#zx at %#lx", m->size(), (unsigned long)m->paddr());
            break;
        }
        case obj_kind::PROCESS: {
            const auto *p = static_cast<const proc *>(this);
            printf(" pid %u '%s'", p->pid(), p->name());
            break;
        }
        case obj_kind::THREAD: {
            const auto *t = static_cast<const thread *>(this);
            printf(" tid %u of pid %u", t->tid(), t->process()->pid());
            break;
        }
        default:
            break;
    }
    printf("\n");
}

} // namespace lkuser
