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
#include "impulse.h"
#include "ipc.h"
#include "object.h"
#include "thread.h"

namespace lkuser {

class memobj;

// A handle a new process is born with: obj with rights, under tag in its startup block.
struct startup_handle {
    uint32_t tag;
    lk::ref_ptr<object> obj;
    uint32_t rights;
};

// A user process: an address space, its handles, the threads that run in it, and how it
// ended.
class proc : public object, public lk::list_hook<> {
public:
    static constexpr obj_kind kKind = obj_kind::PROCESS;

    // A process with an empty address space and a startup block carrying argv (argv[0] is the
    // image path) and its startup handles: a connection to every kernel service with a tag if
    // kernel_services is set (the processes the kernel makes itself, at boot and from the
    // console), then extra. Nothing runs until start().
    static status_t create(int argc, const char *const *argv, bool kernel_services,
                           const startup_handle *extra, size_t nextra, lk::ref_ptr<proc> *out);

    // Starts the main thread at entry, on a new stack whose top holds the startup block.
    status_t start(vaddr_t entry);

    // Makes t's LK thread and runs it: ERR_BAD_STATE once the process has exited.
    status_t start_thread(thread *t);

    // Ends the process with code: ends the calls it has received, closes its handles and kills
    // every thread, the caller's too if it is one of them, so each leaves at its next way out
    // of the kernel or interruptible wait. The first call wins. Callable from any thread.
    void exit(int code);

    // A thread is leaving. Returns true if it was the last one, in which case the caller ends
    // the process.
    bool thread_exiting();

    // Blocks until the process has exited and returns its exit code.
    int wait();

    // Whether the process has exited, and if so with what.
    bool exited(int *code);

    // Maps len bytes of m from off into the address space with arch_mmu_flags; the mapping
    // holds a reference to m until unmap() or the process's end.
    status_t map(lk::ref_ptr<memobj> m, size_t off, size_t len, uint arch_mmu_flags, vaddr_t *va);
    // Removes the region holding va, a memory object's mapping or not.
    status_t unmap(vaddr_t va);

    // Frees the threads and address spaces of exited processes, then raises their
    // LK_SIGNAL_TERMINATED and sends the PROCESS_EXITED impulse. Run by the reaper thread.
    static void reap_exited();

    // The connection to send PROCESS_EXITED over once the process has ended, which the kernel
    // holds as a handle with rights until then (process.SPAWN). Before start().
    void set_exit_notify(lk::ref_ptr<object> conn, uint32_t rights);

    // A boot process, which the reaper reports to boot_reaped() once freed. Before start().
    void set_boot() { boot_ = true; }

    uint32_t pid() const { return pid_; }
    const char *name() const { return name_; }
    // the main thread's stack, [main_stack(), main_stack() + kMainStackSize), and its tid; set
    // by start() before the thread runs
    vaddr_t main_stack() const { return main_stack_; }
    uint32_t main_tid() const { return main_tid_; }
    static constexpr size_t kMainStackSize = 64 * 1024;
    vmm_aspace_t *aspace() const { return aspace_; }
    handle_table &handles() { return handles_; }

    // IPC state, under LKUSER_IPC_LOCK, worked by ipc.cpp and impulse.cpp: the calls the
    // process has received, whether it can receive more (not once it has exited), and the pool
    // impulse_send takes from.
    inflight_slot inflight[LKUSER_INFLIGHT];
    uintptr_t inflight_gen = 0;
    bool ipc_dead = false;
    impulse_node impulse_pool[LKUSER_IMPULSE_POOL];
    lk::list<impulse_node> free_impulses;

private:
    friend class object;

    struct mapping;

    explicit proc(uint32_t pid);
    ~proc();
    DISALLOW_COPY_ASSIGN_AND_MOVE(proc);

    status_t build_args(int argc, const char *const *argv, const lk_proc_handle *handles,
                        size_t nhandles);
    bool is_dead();
    void teardown();
    void notify_exit();
    void free_aspace();
    void drop_mappings();

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

    vaddr_t main_stack_ = 0;
    uint32_t main_tid_ = 0;

    // guards everything below
    Mutex lock_;
    state state_ = state::CREATED;
    // each thread on the list holds a reference, dropped by the reaper after joining it
    lk::list<thread> threads_;
    uint32_t live_threads_ = 0;
    int exit_code_ = 0;

    // memory objects mapped into the address space, each holding a reference
    Mutex mappings_lock_;
    lk::list<mapping> mappings_;

    event_t exit_event_;

    lk::ref_ptr<object> exit_notify_;
    uint32_t exit_notify_rights_ = 0;
    impulse_node exit_impulse_;

    bool boot_ = false;
};

} // namespace lkuser
