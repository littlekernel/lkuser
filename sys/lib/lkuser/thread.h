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
#pragma once

#include <lk/list.h>
#include <kernel/thread.h>
#include <sys/lkuser_syscalls.h>

namespace lkuser {

class proc;

class thread {
private:
    explicit thread(proc *p);

    DISALLOW_COPY_ASSIGN_AND_MOVE(thread);
public:
    ~thread();

    // factory to build threads
    static thread *create(proc *p, vaddr_t entry);

    // accessors
    proc *get_proc() const { return proc_; }
    vaddr_t get_entry() const { return entry_; }
    void *get_stack() const { return user_stack_; }

    // operations on our thread
    void resume() { thread_resume(&lkthread); }
    void join() { thread_join(&lkthread, NULL, INFINITE_TIME); }

    // public for proc to maintain a list
    list_node node = LIST_INITIAL_CLEARED_VALUE;

private:
    proc *proc_ = nullptr;
    vaddr_t entry_ {};

    void *user_stack_ = nullptr;

    thread_t lkthread {};
};

static inline thread *get_lkuser_thread() {
    thread *t = (thread *)tls_get(TLS_ENTRY_LKUSER);
    DEBUG_ASSERT(t);
    return t;
}

} // namespace lkuser
