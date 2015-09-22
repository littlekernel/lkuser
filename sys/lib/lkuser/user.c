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

#include <trace.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <list.h>
#include <kernel/vm.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <err.h>
#include <lib/cksum.h>
#include <lib/elf.h>
#include <lib/bio.h>
#include <sys/lkuser_syscalls.h>

#define LOCAL_TRACE 0

// default virtio device to try to load from
static const char *bio_device = "virtio0";

struct list_node proc_list = LIST_INITIAL_VALUE(proc_list);
static mutex_t proc_lock = MUTEX_INITIAL_VALUE(proc_lock);

static void lkuser_reap(void);

static ssize_t elf_read_hook_bio(struct elf_handle *handle, void *buf, uint64_t offset, size_t len)
{
    bdev_t *bdev = (bdev_t *)handle->read_hook_arg;

    LTRACEF("handle %p, buf %p, offset %llu, len %zu\n", handle, buf, offset, len);

    return bio_read(bdev, buf, offset, len);
}

static status_t elf_mem_alloc(struct elf_handle *handle, void **ptr, size_t len, uint num, uint flags)
{
    LTRACEF("handle %p, ptr %p [%p], size %zu, num %u, flags 0x%x\n", handle, ptr, *ptr, len, num, flags);

    char name[16];
    snprintf(name, sizeof(name), "lkuser%u", num);

    status_t err = vmm_alloc(vmm_get_kernel_aspace(), name, len, ptr, 0, VMM_FLAG_VALLOC_SPECIFIC, 0);
    LTRACEF("vmm_alloc returns %d\n", err);

    return err;
}

status_t lkuser_load(void **entry)
{
    status_t err;

    LTRACE;

    /* open the block device we're looking for binaries on */
    bdev_t *bdev = bio_open(bio_device);
    if (!bdev) {
        TRACEF("failed to open block device '%s'\n", bio_device);
        return ERR_NOT_FOUND;
    }

    /* create an elf handle */
    elf_handle_t elf;
    err = elf_open_handle(&elf, &elf_read_hook_bio, bdev, false);
    LTRACEF("elf_open_handle returns %d\n", err);
    if (err < 0) {
        TRACEF("failed to open elf handle\n");
        goto err;
    }

    /* register a memory allocation callback */
    elf.mem_alloc_hook = &elf_mem_alloc;

    /* try to load the binary */
    err = elf_load(&elf);
    LTRACEF("elf_load returns %d\n", err);
    if (err < 0) {
        TRACEF("failed to load elf file\n");
        goto err;
    }

    /* the binary loaded properly */
    *entry = (void *)elf.entry;

    elf_close_handle(&elf);
    bio_close(bdev);

    return NO_ERROR;

err:
    if (bdev)
        bio_close(bdev);
    return err;
}

static int lkuser_start_routine(void *arg)
{
    lkuser_state_t *state = (lkuser_state_t *)arg;

    /* set our per-thread pointer */
    __tls_set(TLS_ENTRY_LKUSER, (uintptr_t)state);

    state->state = STATE_RUNNING;

    LTRACEF("calling into binary at %p\n", state->entry);
    int err = state->entry(&lkuser_syscalls);
    LTRACEF("binary returned %d\n", err);

    return err;
}

status_t lkuser_start_binary(void *entry, bool wait)
{
    LTRACEF("entry %p\n", entry);

    lkuser_state_t *state;
    state = calloc(1, sizeof(lkuser_state_t));
    if (!state) {
        TRACEF("error allocating user state\n");
        return ERR_NO_MEMORY;
    }

    state->entry = entry;
    event_init(&state->event, false, 0);

    /* create a stack for the new thread */
    status_t err = vmm_alloc(vmm_get_kernel_aspace(), "lkuser_stack", PAGE_SIZE, &state->main_thread_stack, PAGE_SIZE_SHIFT, 0, 0);
    LTRACEF("vmm_alloc returns %d, stack at %p\n", err, state->main_thread_stack);

    /* create a new thread */
    thread_t *t = thread_create_etc(&state->main_thread, "lkuser", lkuser_start_routine, state, LOW_PRIORITY, state->main_thread_stack, PAGE_SIZE);
    if (!t) {
        TRACEF("error creating thread\n");
        return ERR_NO_MEMORY;
    }

    thread_detach(&state->main_thread);

    /* add the process to the list */
    mutex_acquire(&proc_lock);
    list_add_head(&proc_list, &state->node);
    mutex_release(&proc_lock);

    /* resume */
    LTRACEF("resuming thread\n");
    thread_resume(&state->main_thread);

    if (wait) {
        event_wait(&state->event);

        TRACEF("process stopped, retcode %d\n", state->retcode);
    }

    return NO_ERROR;
}

static void lkuser_reap(void)
{
    mutex_acquire(&proc_lock);

    lkuser_state_t *found = NULL;
    lkuser_state_t *temp;
    list_for_every_entry(&proc_list, temp, lkuser_state_t, node) {
        if (temp->state == STATE_DEAD) {
            list_delete(&temp->node);
            found = temp;
            break;
        }
    }

    mutex_release(&proc_lock);

    if (found) {
        TRACEF("going to reap %p\n", temp);

        /* free the stack and memory the loaded binary was using */
        vmm_free_region(vmm_get_kernel_aspace(), (vaddr_t)temp->entry);
        vmm_free_region(vmm_get_kernel_aspace(), (vaddr_t)temp->main_thread_stack);

        event_destroy(&temp->event);

        free(temp);
    }
}

#if defined(WITH_LIB_CONSOLE)
#include <lib/console.h>

static int cmd_lkuser(int argc, const cmd_args *argv)
{
    int rc = 0;

    if (argc < 2) {
notenoughargs:
        printf("not enough arguments:\n");
usage:
        printf("%s load\n", argv[0].str);
        printf("%s run [&]\n", argv[0].str);
        printf("%s reap\n", argv[0].str);
        return -1;
    }

    static void *loaded_entry;
    if (!strcmp(argv[1].str, "load")) {
        status_t err = lkuser_load(&loaded_entry);
        printf("lkuser_load() returns %d, entry at %p\n", err, loaded_entry);
    } else if (!strcmp(argv[1].str, "run")) {
        if (!loaded_entry) {
            printf("no loaded binary\n");
            return -1;
        }

        bool wait = true;
        if (argc > 2 && !strcmp(argv[2].str, "&")) {
            wait = false;
        }

        status_t err = lkuser_start_binary(loaded_entry, wait);
        printf("lkuser_start_binary() returns %d\n", err);
        loaded_entry = NULL;
    } else if (!strcmp(argv[1].str, "reap")) {
        lkuser_reap();
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

