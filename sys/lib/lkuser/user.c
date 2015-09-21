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

#include <trace.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <list.h>
#include <kernel/vm.h>
#include <kernel/thread.h>
#include <err.h>
#include <lib/elf.h>
#include <lib/bio.h>
#include <sys/lkuser_syscalls.h>

#define LOCAL_TRACE 0

// default virtio device to try to load from
static const char *bio_device = "virtio0";

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

    LTRACEF("calling into binary at %p\n", state->entry);
    int err = state->entry(&lkuser_syscalls);
    LTRACEF("binary returned %d\n", err);

    return err;
}

status_t lkuser_start_binary(void *entry)
{
    LTRACEF("entry %p\n", entry);

    lkuser_state_t *state;
    state = calloc(1, sizeof(lkuser_state_t));
    if (!state) {
        TRACEF("error allocating user state\n");
        return ERR_NO_MEMORY;
    }

    state->entry = entry;

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

    /* resume */
    LTRACEF("resuming thread\n");
    thread_resume(&state->main_thread);

    return NO_ERROR;
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
        printf("%s run\n", argv[0].str);
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

        status_t err = lkuser_start_binary(loaded_entry);
        printf("lkuser_start_binary() returns %d\n", err);
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

