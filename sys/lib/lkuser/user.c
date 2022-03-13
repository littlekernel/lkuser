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
#include "lkuser_priv.h"
#include <lib/lkuser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lk/list.h>
#include <lk/trace.h>
#include <lk/err.h>
#include <kernel/vm.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <lib/cksum.h>
#include <lib/elf.h>
#include <lib/bio.h>
#include <lk/init.h>
#include <sys/lkuser_syscalls.h>

#define LOCAL_TRACE 0

// list of processes
struct list_node proc_list = LIST_INITIAL_VALUE(proc_list);
static mutex_t proc_lock = MUTEX_INITIAL_VALUE(proc_lock);
event_t reap_event = EVENT_INITIAL_VALUE(reap_event, false, EVENT_FLAG_AUTOUNSIGNAL);

static status_t lkuser_reap(void);

static ssize_t elf_read_hook_bio(struct elf_handle *handle, void *buf, uint64_t offset, size_t len) {
    bdev_t *bdev = (bdev_t *)handle->read_hook_arg;

    LTRACEF("handle %p, buf %p, offset %llu, len %#zx\n", handle, buf, offset, len);

    return bio_read(bdev, buf, offset, len);
}

static status_t elf_mem_alloc(struct elf_handle *handle, void **ptr, size_t len, uint num, uint flags) {
    LTRACEF("handle %p, ptr %p [%p], size %#zx, num %u, flags %#x\n", handle, ptr, *ptr, len, num, flags);

    lkuser_proc_t *p = (lkuser_proc_t *)handle->mem_alloc_hook_arg;

    char name[16];
    snprintf(name, sizeof(name), "lkuser%u", num);

    // round the allocations down and up to page boundaries
    uintptr_t va = (uintptr_t)*ptr;
    uintptr_t newva = ROUNDDOWN(va, PAGE_SIZE);
    ptrdiff_t aligndiff = va - newva;
    len += aligndiff;
    va = newva;

    // round the size up to the next boundary
    len = ROUNDUP(len, PAGE_SIZE);

    LTRACEF("aligned va %#lx size %#zx\n", va, len);

    void *vaptr = (void *)va;
    status_t err = vmm_alloc(p->aspace, name, len, &vaptr, 0, VMM_FLAG_VALLOC_SPECIFIC, ARCH_MMU_FLAG_PERM_USER);
    LTRACEF("vmm_alloc returns %d, ptr %p\n", err, vaptr);

    *ptr = (void *)((uintptr_t)vaptr + aligndiff);
    LTRACEF("returning ptr %p\n", *ptr);

    return err;
}

static status_t lkuser_load_bio(lkuser_proc_t *proc, const char *bio_name) {
    status_t err;

    LTRACE;

    /* switch to the address space we're loading into */
    vmm_set_active_aspace(proc->aspace);

    /* open the block device we're looking for binaries on */
    bdev_t *bdev = bio_open(bio_name);
    if (!bdev) {
        TRACEF("failed to open block device '%s'\n", bio_name);
        return ERR_NOT_FOUND;
    }

    /* create an elf handle */
    err = elf_open_handle(&proc->elf, &elf_read_hook_bio, bdev, false);
    LTRACEF("elf_open_handle returns %d\n", err);
    if (err < 0) {
        TRACEF("failed to open elf handle\n");
        goto err;
    }

    /* register a memory allocation callback */
    proc->elf.mem_alloc_hook = &elf_mem_alloc;
    proc->elf.mem_alloc_hook_arg = proc;

    /* try to load the binary */
    err = elf_load(&proc->elf);
    LTRACEF("elf_load returns %d\n", err);
    if (err < 0) {
        TRACEF("failed to load elf file\n");
        goto err;
    }

    /* the binary loaded properly */
    proc->entry = (void *)proc->elf.entry;

    bio_close(bdev);
    vmm_set_active_aspace(NULL);

    return NO_ERROR;

err:
    if (bdev)
        bio_close(bdev);
    vmm_set_active_aspace(NULL);
    return err;
}

static lkuser_proc_t *create_proc(void) {
    lkuser_proc_t *p;
    p = calloc(1, sizeof(lkuser_proc_t));
    if (!p) {
        TRACEF("error allocating proc state\n");
        return NULL;
    }

    list_initialize(&p->thread_list);
    mutex_init(&p->thread_list_lock);

    p->state = PROC_STATE_INITIAL;

    event_init(&p->event, false, 0);

    /* create an address space for it */
    if (vmm_create_aspace(&p->aspace, "lkuser", 0) < 0) {
        TRACEF("error creating address space\n");
        free(p);
        return NULL;
    }

    return p;
}

static lkuser_thread_t *create_thread(lkuser_proc_t *p, void *entry) {
    lkuser_thread_t *t;
    t = calloc(1, sizeof(lkuser_thread_t));
    if (!t) {
        TRACEF("error allocating thread state\n");
        return NULL;
    }

    t->entry = entry;
    t->proc = p;

    return t;
}

static int lkuser_start_routine(void *arg) {
    lkuser_thread_t *t = (lkuser_thread_t *)arg;

    /* set our per-thread pointer */
    __tls_set(TLS_ENTRY_LKUSER, (uintptr_t)t);

    /* create a user stack for the new thread */
    status_t err = vmm_alloc(t->proc->aspace, "lkuser_user_stack", PAGE_SIZE, &t->user_stack, PAGE_SIZE_SHIFT,
                             0, ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_NO_EXECUTE);
    LTRACEF("vmm_alloc returns %d, stack at %p\n", err, t->user_stack);

    /* XXX put bits on the user stack */

    /* switch to user mode and start the process */
    arch_enter_uspace((vaddr_t)t->entry,
                      (uintptr_t)t->user_stack + PAGE_SIZE);

#if 0
    LTRACEF("calling into binary at %p\n", t->entry);
    int retcode = t->entry(&lkuser_syscalls);
    LTRACEF("binary returned to us %d\n", err);

    sys_exit(retcode);
#endif

    __UNREACHABLE;
}

status_t lkuser_start_binary(lkuser_proc_t *p, bool wait) {
    LTRACEF("proc %p, entry %p\n", p, p->entry);

    DEBUG_ASSERT(p);

    lkuser_thread_t *t = create_thread(p, p->entry);
    if (!t) {
        // XXX free proc
        return ERR_NO_MEMORY;
    }

    /* create a new thread */
    thread_t *lkthread = thread_create_etc(&t->thread, "lkuser", lkuser_start_routine, t, LOW_PRIORITY, NULL, DEFAULT_STACK_SIZE);
    if (!lkthread) {
        TRACEF("error creating thread\n");
        return ERR_NO_MEMORY;
    }
    DEBUG_ASSERT(lkthread == &t->thread);

    /* add the thread to the process */
    mutex_acquire(&p->thread_list_lock);
    list_add_head(&p->thread_list, &t->node);
    mutex_release(&p->thread_list_lock);

    /* set the address space for this thread */
    lkthread->aspace = p->aspace;

    /* we're ready to run now */
    t->proc->state = PROC_STATE_RUNNING;

    /* add the process to the process list */
    mutex_acquire(&proc_lock);
    list_add_head(&proc_list, &p->node);
    mutex_release(&proc_lock);

    /* resume */
    LTRACEF("resuming main thread\n");
    thread_resume(&t->thread);

    if (wait) {
        event_wait(&p->event);
    }

    return NO_ERROR;
}

static status_t lkuser_reap(void) {
    mutex_acquire(&proc_lock);

    lkuser_proc_t *found = NULL;
    lkuser_proc_t *temp;
    list_for_every_entry(&proc_list, temp, lkuser_proc_t, node) {
        if (temp->state == PROC_STATE_DEAD) {
            list_delete(&temp->node);
            found = temp;
            break;
        }
    }

    mutex_release(&proc_lock);

    if (!found)
        return ERR_NOT_FOUND;

    LTRACEF("going to reap %p\n", found);

    /* clean up all the threads */
    lkuser_thread_t *t;
    while ((t = list_remove_head_type(&found->thread_list, lkuser_thread_t, node))) {
        thread_join(&t->thread, NULL, INFINITE_TIME);

        free(t);
    }

    /* free everything inside the address space */
    vmm_free_aspace(found->aspace);

    /* no one should be waiting for to us */
    event_destroy(&found->event);

    /* free any resources in the elf handle */
    elf_close_handle(&found->elf);

    free(found);

    return NO_ERROR;
}

static int reaper(void *arg) {
    for (;;) {
        event_wait(&reap_event);

        while (lkuser_reap() >= 0)
            ;
    }

    return 0;
}

void lkuser_init(uint level) {
    thread_detach_and_resume(thread_create("reaper", &reaper, NULL, HIGH_PRIORITY, DEFAULT_STACK_SIZE));
}

LK_INIT_HOOK(lkuser, lkuser_init, LK_INIT_LEVEL_THREADING);

#if defined(WITH_LIB_CONSOLE)
#include <lib/console.h>

static int cmd_lkuser(int argc, const console_cmd_args *argv) {
    int rc = 0;

    if (argc < 2) {
notenoughargs:
        printf("not enough arguments:\n");
usage:
        printf("%s load\n", argv[0].str);
        printf("%s run [&]\n", argv[0].str);
        return -1;
    }

    static lkuser_proc_t *proc;
    if (!strcmp(argv[1].str, "load")) {
        if (!proc)
            proc = create_proc();
        status_t err = lkuser_load_bio(proc, "virtio0");
        printf("lkuser_load_bio() returns %d, entry at %p\n", err, proc->entry);
    } else if (!strcmp(argv[1].str, "run")) {
        if (!proc) {
            printf("no loaded binary\n");
            return -1;
        }

        bool wait = true;
        if (argc > 2 && !strcmp(argv[2].str, "&")) {
            wait = false;
        }

        status_t err = lkuser_start_binary(proc, wait);
        printf("lkuser_start_binary() returns %d\n", err);
        proc = NULL;
    } else {
        printf("unrecognized subcommand\n");
        goto usage;
    }

    return rc;
}

STATIC_COMMAND_START
STATIC_COMMAND("lkuser", "user space routines", &cmd_lkuser)
STATIC_COMMAND_END(lkuser);

#endif

