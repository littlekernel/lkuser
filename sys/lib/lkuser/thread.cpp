/*
 * Copyright (c) 2015-2022 Travis Geiselbrecht
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "thread.h"

#include <lk/err.h>
#include <lk/trace.h>
#include <kernel/thread.h>
#include <kernel/vm.h>

#include "proc.h"

#define LOCAL_TRACE 0

namespace lkuser {

thread::thread(proc *p) : proc_(p) {}
thread::~thread() = default;

int lkuser_start_routine(void *arg) {
    thread *t = (thread *)arg;

    /* set our per-thread pointer */
    __tls_set(TLS_ENTRY_LKUSER, (uintptr_t)t);

    /* switch to user mode and start the thread */
    arch_enter_uspace(t->get_entry(),
                      (uintptr_t)t->get_stack() + PAGE_SIZE);

    __UNREACHABLE;
}

thread *thread::create(proc *p, vaddr_t entry) {
    thread *t;
    t = new thread(p);
    if (!t) {
        TRACEF("error allocating thread state\n");
        return NULL;
    }

    t->entry_ = entry;

    // create the lk side of the thread
    thread_t *lkthread = thread_create_etc(&t->lkthread, "lkuser", lkuser_start_routine, t, LOW_PRIORITY, NULL, DEFAULT_STACK_SIZE);
    if (!lkthread) {
        TRACEF("error creating thread\n");
        delete t;
        return nullptr;
    }
    DEBUG_ASSERT(lkthread == &t->lkthread);

    // set the address space for this thread
    t->lkthread.aspace = p->get_aspace();

    // create a user stack for the new thread
    // TODO: move this logic elsewhere and/or generally make it a user space problem
    status_t err = vmm_alloc(p->get_aspace(), "lkuser_user_stack", PAGE_SIZE, &t->user_stack_, PAGE_SIZE_SHIFT,
                             0, ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_NO_EXECUTE);
    LTRACEF("vmm_alloc returns %d, stack at %p\n", err, t->user_stack_);
    DEBUG_ASSERT(err >= NO_ERROR);

    // add ourselves to the parent process
    err = p->add_thread(t);
    if (err < 0) {
        // XXX clean up the LK thread
        panic("failed addition of thread to process\n");
        return nullptr;
    }

    return t;
}

} // namespace lkuser

