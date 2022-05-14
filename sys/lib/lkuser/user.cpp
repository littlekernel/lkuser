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
#include <lk/cpp.h>
#include <lk/list.h>
#include <lk/trace.h>
#include <lk/err.h>
#include <kernel/vm.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <lib/bio.h>
#include <lib/cksum.h>
#include <lib/elf.h>
#include <lib/fs.h>
#include <lk/init.h>
#include <sys/lkuser_syscalls.h>

#define LOCAL_TRACE 0

namespace lkuser {

static ssize_t elf_read_hook_file(struct elf_handle *handle, void *buf, uint64_t offset, size_t len) {
    filehandle *fhandle = (filehandle *)handle->read_hook_arg;

    LTRACEF("handle %p, buf %p, offset %llu, len %#zx\n", handle, buf, offset, len);

    return fs_read_file(fhandle, buf, offset, len);
}

static status_t elf_mem_alloc(struct elf_handle *handle, void **ptr, size_t len, uint num, uint flags) {
    LTRACEF("handle %p, ptr %p [%p], size %#zx, num %u, flags %#x\n", handle, ptr, *ptr, len, num, flags);

    proc *p = (proc *)handle->mem_alloc_hook_arg;

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
    status_t err = vmm_alloc(p->get_aspace(), name, len, &vaptr, 0, VMM_FLAG_VALLOC_SPECIFIC, ARCH_MMU_FLAG_PERM_USER);
    LTRACEF("vmm_alloc returns %d, ptr %p\n", err, vaptr);

    *ptr = (void *)((uintptr_t)vaptr + aligndiff);
    LTRACEF("returning ptr %p\n", *ptr);

    return err;
}

static status_t lkuser_load_file(proc *proc, const char *file_name) {
    status_t err;

    LTRACE;

    /* switch to the address space we're loading into */
    vmm_set_active_aspace(proc->get_aspace());

    filehandle *handle;
    err = fs_open_file(file_name, &handle);
    if (err < 0) {
        TRACEF("failed to open file %s\n", file_name);
        return err;
    }

    proc::loader_state &ls = proc->get_loader_state();

    /* create an elf handle */
    err = elf_open_handle(&ls.elf, &elf_read_hook_file, handle, false);
    LTRACEF("elf_open_handle returns %d\n", err);
    if (err < 0) {
        TRACEF("failed to open elf handle\n");
        goto err;
    }

    /* register a memory allocation callback */
    ls.elf.mem_alloc_hook = &elf_mem_alloc;
    ls.elf.mem_alloc_hook_arg = proc;

    /* try to load the binary */
    err = elf_load(&ls.elf);
    LTRACEF("elf_load returns %d\n", err);
    if (err < 0) {
        TRACEF("failed to load elf file\n");
        goto err;
    }

    /* the binary loaded properly */
    ls.entry = ls.elf.entry;
    ls.loaded = true;

    fs_close_file(handle);
    vmm_set_active_aspace(NULL);

    return NO_ERROR;

err:
    if (handle) {
        fs_close_file(handle);
    }
    vmm_set_active_aspace(NULL);
    return err;
}

status_t lkuser_start_binary(proc *p, bool wait) {
    DEBUG_ASSERT(p);

    const auto ls = p->get_loader_state();
    LTRACEF("proc %p, entry %#lx\n", p, ls.entry);

    if (!ls.loaded) {
        return ERR_NOT_READY;
    }

    thread *t = thread::create(p, ls.entry);
    if (!t) {
        // XXX free proc
        return ERR_NO_MEMORY;
    }

    /* we're ready to run now */
    p->start();

    /* resume */
    LTRACEF("resuming main thread\n");
    t->resume();

    if (wait) {
        p->wait();
    }

    return NO_ERROR;
}

} // namespace lkuser

#if defined(WITH_LIB_CONSOLE)
#include <lib/console.h>

static int cmd_lkuser(int argc, const console_cmd_args *argv) {
    int rc = 0;

    if (argc < 2) {
notenoughargs:
        printf("not enough arguments:\n");
usage:
        printf("%s load <path to binary>\n", argv[0].str);
        printf("%s run [&]\n", argv[0].str);
        return -1;
    }

    static lkuser::proc *proc;
    if (!strcmp(argv[1].str, "load")) {
        if (argc < 3) {
            goto notenoughargs;
        }
        if (!proc) {
            proc = lkuser::proc::create();
        }
        status_t err;
        err = lkuser_load_file(proc, argv[2].str);
        printf("lkuser_load_file() returns %d, entry at %#lx\n", err, proc->get_loader_state().entry);
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
    } else if (!strcmp(argv[1].str, "test")) {
        // a whole sequence of commands
        status_t err = fs_mount("/lku", "fat", "virtio0");
        if (err < 0) {
            printf("fs mount returns %d\n", err);
            return -1;
        }

        console_run_script_locked(nullptr, "lkuser load /lku/bin/hello");
        console_run_script_locked(nullptr, "lkuser run");
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

