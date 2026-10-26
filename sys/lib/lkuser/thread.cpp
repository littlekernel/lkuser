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

#include <arch.h>
#include <lk/err.h>
#include <sys/lk_abi.h>

#include "proc.h"

namespace lkuser {

namespace {

uint32_t next_tid = 1;

// A user thread's kernel stack: SPAWN reads the image through the file system and block
// drivers on it, which takes more than the one page arm64 gives a thread by default.
constexpr size_t kKernelStackSize = MAX(DEFAULT_STACK_SIZE, 8192ul);

} // namespace

thread::thread(proc *p, vaddr_t entry, vaddr_t sp)
    : object(kKind), proc_(p), tid_(__atomic_fetch_add(&next_tid, 1, __ATOMIC_RELAXED)),
      entry_(entry), sp_(sp), call_(this) {
    wait_queue_init(&wait_queue_);
}

thread::~thread() {
    THREAD_LOCK(state);
    wait_queue_destroy(&wait_queue_, false);
    THREAD_UNLOCK(state);
}

int thread::start_routine(void *arg) {
    thread *t = static_cast<thread *>(arg);

    tls_set(TLS_ENTRY_LKUSER, reinterpret_cast<uintptr_t>(t));
    // killed before it ever ran: user mode would only meet the kill at its first trap
    if (thread_signals(get_current_thread()) & THREAD_SIGNAL_KILL) {
        exit_current();
    }
    arch_enter_uspace(t->entry_, t->sp_);

    __UNREACHABLE;
}

status_t thread::create(proc *p, vaddr_t entry, vaddr_t sp, lk::ref_ptr<thread> *out) {
    lk::ref_ptr<thread> t = object::make<thread>(p, entry, sp);
    if (!t) {
        return ERR_NO_MEMORY;
    }
    *out = std::move(t);
    return NO_ERROR;
}

status_t thread::create_lk_thread() {
    if (!thread_create_etc(&lkthread_, proc_->name(), &start_routine, this, LOW_PRIORITY,
                           nullptr, kKernelStackSize)) {
        return ERR_NO_MEMORY;
    }
    // the LK thread runs in the process's address space from its first instruction
    lkthread_.aspace = proc_->aspace();
    return NO_ERROR;
}

void thread::exit_current() {
    thread *t = current();
    proc *p = t->process();

    if (p->thread_exiting()) {
        p->exit(0);
    }
    t->signal(LK_SIGNAL_TERMINATED);
    thread_exit(0);
}

} // namespace lkuser
