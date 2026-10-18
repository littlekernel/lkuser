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

#include <kernel/event.h>
#include <kernel/mutex.h>
#include <kernel/vm.h>
#include <lk/cpp.h>
#include <lktl/list.h>
#include <lktl/ref_ptr.h>
#include <stdint.h>

#include "handle.h"
#include "object.h"
#include "thread.h"

namespace lkuser {

// A user process: an address space, its handles, the threads that run in it, and how it
// ended.
class proc : public object, public lk::list_hook<> {
public:
    static constexpr obj_kind kKind = obj_kind::PROCESS;

    // A process with an empty address space, a connection to every kernel service, and a
    // startup block carrying argv (argv[0] is the image path) and those connections under
    // their tags. Nothing runs until start().
    static status_t create(int argc, const char *const *argv, lk::ref_ptr<proc> *out);

    // Starts the main thread at entry, on a new stack whose top holds the startup block.
    status_t start(vaddr_t entry);

    // Ends the process with code and closes its handles. The first call wins. Called by a
    // thread of the process, which then leaves with thread_exit().
    void exit(int code);

    // A thread is leaving without ending the process. Returns true if it was the last one,
    // in which case the caller ends the process.
    bool thread_exiting();

    // Blocks until the process has exited and returns its exit code.
    int wait();

    // Frees the threads and address spaces of exited processes. Run by the reaper thread.
    static void reap_exited();

    uint32_t pid() const { return pid_; }
    const char *name() const { return name_; }
    vmm_aspace_t *aspace() const { return aspace_; }
    handle_table &handles() { return handles_; }

private:
    friend class object;

    explicit proc(uint32_t pid);
    ~proc();
    DISALLOW_COPY_ASSIGN_AND_MOVE(proc);

    status_t build_args(int argc, const char *const *argv, const lk_proc_handle *handles,
                        size_t nhandles);
    bool exited();
    void teardown();
    void free_aspace();

    enum class state { CREATED, RUNNING, DEAD };

    const uint32_t pid_;
    char name_[32] = {};
    vmm_aspace_t *aspace_ = nullptr;
    // Emptied at exit rather than at destruction, so that a handle naming the process itself
    // cannot keep it alive.
    handle_table handles_;

    // the startup block until start() copies it out
    uint8_t *args_ = nullptr;
    size_t args_len_ = 0;

    // guards everything below
    Mutex lock_;
    state state_ = state::CREATED;
    // each thread on the list holds a reference, dropped by the reaper after joining it
    lk::list<thread> threads_;
    uint32_t live_threads_ = 0;
    int exit_code_ = 0;

    event_t exit_event_;
};

} // namespace lkuser
