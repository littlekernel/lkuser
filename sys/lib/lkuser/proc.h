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
#include <lib/elf.h>
#include <kernel/mutex.h>
#include <kernel/event.h>
#include <kernel/vm.h>

#include "file_handle.h"

namespace lkuser {

class thread;

class proc {
private:
    proc();

    DISALLOW_COPY_ASSIGN_AND_MOVE(proc);

public:
    ~proc();

    static proc *create();
    void destroy();

    status_t add_thread(thread *t);
    status_t wait(); // wait for process to exit
    void start();
    void exit(int retcode); // must be called by a thread in the process

    // accessors
    vmm_aspace_t *get_aspace() const { return aspace_; }

    // state of the loader
    struct loader_state {
        elf_handle_t elf;
        vaddr_t entry; // entry point to the binary
        bool loaded;
    };
    loader_state &get_loader_state() { return loader_; }
    const loader_state &get_loader_state() const { return loader_; }

    // process state
    enum state {
        PROC_STATE_INITIAL,
        PROC_STATE_RUNNING,
        PROC_STATE_DEAD,
    };
    state get_state() const { return state_; }

    // sbrk state
    struct sbrk_state {
        uintptr_t last_sbrk;
        uintptr_t last_sbrk_top;
    };
    sbrk_state &get_sbrk_state() { return sbrk_state_; }
    const sbrk_state &get_sbrk_state() const { return sbrk_state_; }

    // the file descriptor table
    auto const &get_file_table() const { return files_; }
    auto &get_file_table() { return files_; }

    // list node for the process list
    list_node node = LIST_INITIAL_CLEARED_VALUE;

private:
    loader_state loader_ {};

    // our address space
    vmm_aspace_t *aspace_ = nullptr;

    // list of threads
    list_node thread_list_ = LIST_INITIAL_VALUE(thread_list_);
    Mutex thread_list_lock_;

    int retcode_;
    state state_ = PROC_STATE_INITIAL;

    event_t exit_event_ = EVENT_INITIAL_VALUE(exit_event_, false, 0);

    // sbrk information
    sbrk_state sbrk_state_;

    // our file table
    file_table<64> files_;
};

void add_to_global_list(proc *p);

} // namespace lkuser

