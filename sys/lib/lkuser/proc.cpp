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
#include "proc.h"

#include <lk/err.h>
#include <lk/init.h>
#include <lk/trace.h>
#include <kernel/vm.h>

#include "thread.h"
#include "lkuser_priv.h"

#define LOCAL_TRACE 0

namespace lkuser {

// event to kick off a pass of the reaper thread
event_t reap_event = EVENT_INITIAL_VALUE(reap_event, false, EVENT_FLAG_AUTOUNSIGNAL);

// list of processes
list_node proc_list = LIST_INITIAL_VALUE(proc_list);
Mutex proc_list_lock;

void add_to_global_list(proc *p) {
    AutoLock guard(proc_list_lock);
    list_add_head(&proc_list, &p->node);
}

proc::proc() = default;
proc::~proc() = default;

proc *proc::create() {
    proc *p = new proc;
    if (!p) {
        TRACEF("error allocating proc state\n");
        return NULL;
    }

    /* create an address space for it */
    if (vmm_create_aspace(&p->aspace_, "lkuser", 0) < 0) {
        TRACEF("error creating address space\n");
        delete p;
        return NULL;
    }

    /* add the process to the process list */
    add_to_global_list(p);

    return p;
}

status_t proc::add_thread(thread *t) {
    /* add the thread to the process */
    {
        AutoLock guard(thread_list_lock_);
        list_add_head(&thread_list_, &t->node);
    }

    return NO_ERROR;
}

void proc::destroy() {
    // TODO: formalize the state machine more
    DEBUG_ASSERT(state_ == PROC_STATE_DEAD);

    // clean up all the threads
    thread *t;
    while ((t = list_remove_head_type(&thread_list_, thread, node))) {
        t->join();

        delete t;
    }

    // free everything inside the address space
    vmm_free_aspace(aspace_);

    // no one should be waiting for to us
    event_destroy(&exit_event_);

    // free any resources in the elf handle
    elf_close_handle(&get_loader_state().elf);
}

status_t proc::wait() {
    return event_wait(&exit_event_);
}

void proc::start() {
    state_ = PROC_STATE_RUNNING;
}

void proc::exit(int retcode) {
    state_ = proc::PROC_STATE_DEAD;
    retcode = retcode;
    event_signal(&exit_event_, true);

    // TODO: only trigger the reaper when the last thread exits
    event_signal(&reap_event, true);
}

// reaper thread that cleans up dead processes
static int reaper(void *arg) {
    for (;;) {
        event_wait(&reap_event);

        for (;;) {
            proc *found = NULL;
            {
                AutoLock guard(proc_list_lock);

                proc *temp;
                list_for_every_entry(&proc_list, temp, proc, node) {
                    if (temp->get_state() == proc::PROC_STATE_DEAD) {
                        list_delete(&temp->node);
                        found = temp;
                        break;
                    }
                }
            }

            if (!found) {
                break;
            }

            TRACEF("going to reap process %p\n", found);

            found->destroy();

            // delete the process
            delete found;
        }
    }

    return 0;
}

void lkuser_init(uint level) {
    thread_detach_and_resume(thread_create("reaper", &reaper, NULL, HIGH_PRIORITY, DEFAULT_STACK_SIZE));
}

LK_INIT_HOOK(lkuser, lkuser_init, LK_INIT_LEVEL_THREADING);


} // namespace lkuser

