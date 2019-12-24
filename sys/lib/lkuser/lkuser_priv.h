/*
 * Copyright (c) 2015 Travis Geiselbrecht
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
#include <assert.h>
#include <lib/elf.h>
#include <lib/lkuser.h>
#include <kernel/event.h>
#include <kernel/mutex.h>
#include <kernel/thread.h>
#include <sys/lkuser_syscalls.h>

#include <kernel/vm.h>

typedef struct lkuser_proc {
    struct list_node node;

    /* list of threads */
    struct list_node thread_list;
    mutex_t thread_list_lock;

    /* our address space */
    vmm_aspace_t *aspace;

    int retcode;
    enum {
        PROC_STATE_INITIAL,
        PROC_STATE_RUNNING,
        PROC_STATE_DEAD,
    } state;
    event_t event;

    /* binary information from where we run */
    elf_handle_t elf;
    int (*entry)(const struct lkuser_syscall_table *table);

    /* sbrk information */
    uintptr_t last_sbrk;
    uintptr_t last_sbrk_top;
} lkuser_proc_t;

typedef struct lkuser_thread {
    struct list_node node;

    int (*entry)(const struct lkuser_syscall_table *table);

    lkuser_proc_t *proc;

    thread_t thread;
    void *user_stack;
} lkuser_thread_t;

/* defined in syscalls.c */
extern const struct lkuser_syscall_table lkuser_syscalls;

static inline lkuser_thread_t *get_lkuser_thread(void)
{
    lkuser_thread_t *t = (lkuser_thread_t *)tls_get(TLS_ENTRY_LKUSER);
    DEBUG_ASSERT(t);
    return t;
}

/* one of the syscalls */
void sys_exit(int retcode) __NO_RETURN;

/* hit this to signal the reaper should run */
extern event_t reap_event;

