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

#include <kernel/thread.h>
#include <lk/cpp.h>
#include <lktl/list.h>
#include <lktl/ref_ptr.h>

#include "object.h"

namespace lkuser {

class proc;

// A user thread: an LK thread that enters user space in its process's address space. It
// holds the LK thread's storage, so it must outlive the LK thread: the reaper joins the LK
// thread before dropping the last reference.
class thread : public object, public lk::list_hook<> {
public:
    // A suspended thread that will enter user space at entry with sp. Once created it must
    // be resumed: LK cannot free a thread that never ran.
    static status_t create(proc *p, vaddr_t entry, vaddr_t sp, lk::ref_ptr<thread> *out);

    // the lkuser thread on this cpu, or null on a kernel-only thread
    static thread *current() { return reinterpret_cast<thread *>(tls_get(TLS_ENTRY_LKUSER)); }

    proc *process() const { return proc_.get(); }

    void resume() { thread_resume(&lkthread_); }
    void join() { thread_join(&lkthread_, nullptr, INFINITE_TIME); }

    // set while the thread is inside a syscall
    bool in_syscall() const { return in_syscall_; }
    void set_in_syscall(bool in) { in_syscall_ = in; }

private:
    friend class object;

    thread(proc *p, vaddr_t entry, vaddr_t sp);
    ~thread();
    DISALLOW_COPY_ASSIGN_AND_MOVE(thread);

    static int start_routine(void *arg);

    lk::ref_ptr<proc> proc_;
    const vaddr_t entry_;
    const vaddr_t sp_;
    bool in_syscall_ = false;

    thread_t lkthread_ = {};
};

} // namespace lkuser
