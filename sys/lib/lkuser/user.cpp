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
// The ELF loader and the console's lkuser command.

#include <kernel/vm.h>
#include <lib/elf.h>
#include <lib/fs.h>
#include <lk/err.h>
#include <lk/trace.h>
#include <lktl/auto_call.h>
#include <lktl/ref_ptr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>

#include "copy.h"
#include "object.h"
#include "proc.h"

#ifndef FS_MAX_PATH_LEN
#define FS_MAX_PATH_LEN 256
#endif

#define LOCAL_TRACE 0

namespace lkuser {

namespace {

char g_lkuser_fs_root[FS_MAX_PATH_LEN] = "/";

const char* get_fs_root(void) {
    return g_lkuser_fs_root;
}

status_t set_fs_root(const char* path) {
    if (!path || path[0] == '\0') {
        return ERR_INVALID_ARGS;
    }

    char normalized[FS_MAX_PATH_LEN];
    size_t len = strlcpy(normalized, path, sizeof(normalized));
    if (len >= sizeof(normalized)) {
        return ERR_TOO_BIG;
    }

    if (normalized[0] != '/') {
        return ERR_INVALID_ARGS;
    }

    while (len > 1 && normalized[len - 1] == '/') {
        normalized[len - 1] = '\0';
        len--;
    }

    strlcpy(g_lkuser_fs_root, normalized, sizeof(g_lkuser_fs_root));
    return NO_ERROR;
}

status_t resolve_fs_path(const char* path, char* out_path, size_t out_path_len) {
    if (!path || !out_path || out_path_len == 0) {
        return ERR_INVALID_ARGS;
    }

    const char* root = get_fs_root();
    const bool root_is_slash = (root[0] == '/' && root[1] == '\0');

    const char* relative = path;
    while (*relative == '/') {
        relative++;
    }

    if (root_is_slash) {
        if (relative != path) {
            if (snprintf(out_path, out_path_len, "/%s", relative) >= (int)out_path_len) {
                return ERR_TOO_BIG;
            }
        } else {
            if (strlcpy(out_path, path, out_path_len) >= out_path_len) {
                return ERR_TOO_BIG;
            }
        }
    } else {
        if (*relative == '\0') {
            if (strlcpy(out_path, root, out_path_len) >= out_path_len) {
                return ERR_TOO_BIG;
            }
        } else if (snprintf(out_path, out_path_len, "%s/%s", root, relative) >= (int)out_path_len) {
            return ERR_TOO_BIG;
        }
    }

    return NO_ERROR;
}

ssize_t elf_read_hook_file(struct elf_handle *handle, void *buf, uint64_t offset, size_t len) {
    filehandle *fhandle = (filehandle *)handle->read_hook_arg;

    LTRACEF("handle %p, buf %p, offset %llu, len %#zx\n", handle, buf, offset, len);

    return fs_read_file(fhandle, buf, offset, len);
}

// The loader's segment allocator: a user mapping at the address the segment asks for.
status_t elf_mem_alloc(struct elf_handle *handle, void **ptr, size_t len, uint num, uint flags) {
    LTRACEF("handle %p, ptr %p [%p], size %#zx, num %u, flags %#x\n", handle, ptr, *ptr, len, num, flags);

    proc *p = (proc *)handle->mem_alloc_hook_arg;

    char name[16];
    snprintf(name, sizeof(name), "image%u", num);

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
    // RWX: elf_load copies the segment in through this mapping, and lib/elf doesn't pass the segment's p_flags
    status_t err = vmm_alloc(p->aspace(), name, len, &vaptr, 0, VMM_FLAG_VALLOC_SPECIFIC,
                             ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_RWX_MASK);
    LTRACEF("vmm_alloc returns %d, ptr %p\n", err, vaptr);
    if (err < 0) {
        return err;
    }
    // elf_load fills only the segment, not the rest of its first and last pages
    err = zero_user_pages(p->aspace(), (vaddr_t)vaptr, len);
    if (err < 0) {
        return err;
    }

    *ptr = (void *)((uintptr_t)vaptr + aligndiff);
    LTRACEF("returning ptr %p\n", *ptr);

    return err;
}

// Loads the ELF image at path into p's address space and returns its entry point.
status_t load_image(proc *p, const char *path, vaddr_t *entry) {
    char resolved_path[FS_MAX_PATH_LEN];
    status_t err = resolve_fs_path(path, resolved_path, sizeof(resolved_path));
    if (err < 0) {
        return err;
    }

    filehandle *fhandle;
    err = fs_open_file(resolved_path, &fhandle);
    if (err < 0) {
        return err;
    }
    auto close_file = lk::make_auto_call([fhandle]() { fs_close_file(fhandle); });

    elf_handle_t elf;
    err = elf_open_handle(&elf, &elf_read_hook_file, fhandle, false);
    if (err < 0) {
        return err;
    }
    auto close_elf = lk::make_auto_call([&elf]() { elf_close_handle(&elf); });
    elf.mem_alloc_hook = &elf_mem_alloc;
    elf.mem_alloc_hook_arg = p;

    // elf_load writes the segments through their user mappings, so it runs on p's aspace
    vmm_aspace_t *prev = vmm_set_active_aspace(p->aspace());
    err = elf_load(&elf);
    vmm_set_active_aspace(prev);
    if (err < 0) {
        return err;
    }

    if (!is_user_address(elf.entry)) {
        return ERR_NOT_VALID;
    }
    *entry = elf.entry;
    return NO_ERROR;
}

} // namespace

} // namespace lkuser

#if defined(WITH_LIB_CONSOLE)
#include <lib/console.h>

namespace {

// loaded by "lkuser load", started by "lkuser run"
struct {
    lk::ref_ptr<lkuser::proc> p;
    vaddr_t entry;
} loaded;

int cmd_lkuser(int argc, const console_cmd_args *argv) {
    if (argc < 2) {
notenoughargs:
        printf("not enough arguments:\n");
usage:
        printf("%s load <path to binary> [args...]\n", argv[0].str);
        printf("%s run [&]\n", argv[0].str);
        printf("%s root <path>\n", argv[0].str);
        printf("%s objs [-f]          every object (-f: take no locks)\n", argv[0].str);
        printf("%s handles <pid> [-f] a process's handles\n", argv[0].str);
        return -1;
    }

    if (!strcmp(argv[1].str, "load")) {
        if (argc < 3) {
            goto notenoughargs;
        }

        // the process's arguments: the path and whatever follows it
        constexpr int kMaxArgs = 16;
        const char *args[kMaxArgs];
        const int nargs = MIN(argc - 2, kMaxArgs);
        for (int i = 0; i < nargs; i++) {
            args[i] = argv[i + 2].str;
        }

        lk::ref_ptr<lkuser::proc> p;
        status_t err = lkuser::proc::create(nargs, args, &p);
        if (err < 0) {
            printf("lkuser: creating a process for %s failed: %d\n", args[0], err);
            return err;
        }
        vaddr_t entry;
        err = lkuser::load_image(p.get(), args[0], &entry);
        if (err < 0) {
            printf("lkuser: loading %s failed: %d\n", args[0], err);
            return err;
        }
        printf("lkuser: loaded %s as process %u, entry %#lx\n", args[0], p->pid(), entry);
        loaded.p = std::move(p);
        loaded.entry = entry;
    } else if (!strcmp(argv[1].str, "root")) {
        if (argc < 3) {
            printf("lkuser root is '%s'\n", lkuser::get_fs_root());
            printf("usage: %s root <path>\n", argv[0].str);
            return -1;
        }

        status_t err = lkuser::set_fs_root(argv[2].str);
        if (err < 0) {
            printf("failed to set lkuser root to '%s': %d\n", argv[2].str, err);
            return err;
        }

        printf("lkuser root set to '%s'\n", lkuser::get_fs_root());
    } else if (!strcmp(argv[1].str, "run")) {
        if (!loaded.p) {
            printf("no loaded binary\n");
            return -1;
        }

        const bool wait = !(argc > 2 && !strcmp(argv[2].str, "&"));

        // the reference keeps the process readable after it exits, until the wait is over
        lk::ref_ptr<lkuser::proc> p = std::move(loaded.p);
        status_t err = p->start(loaded.entry);
        if (err < 0) {
            printf("lkuser: starting process %u failed: %d\n", p->pid(), err);
            return err;
        }
        if (wait) {
            p->wait();
        }
    } else if (!strcmp(argv[1].str, "objs")) {
        const bool lock = !(argc > 2 && !strcmp(argv[2].str, "-f"));
        lkuser::object::for_each([](lkuser::object &o) { o.dump(); }, lock);
    } else if (!strcmp(argv[1].str, "handles")) {
        if (argc < 3) {
            goto notenoughargs;
        }
        const uint32_t pid = argv[2].u;
        const bool lock = !(argc > 3 && !strcmp(argv[3].str, "-f"));
        bool found = false;
        // with the registry locked the process cannot be destroyed while its table is read
        lkuser::object::for_each(
            [pid, lock, &found](lkuser::object &o) {
                if (o.kind() != lkuser::obj_kind::PROCESS) {
                    return;
                }
                auto &p = static_cast<lkuser::proc &>(o);
                if (p.pid() == pid) {
                    printf("handles of process %u (%s):\n", p.pid(), p.name());
                    p.handles().dump(lock);
                    found = true;
                }
            },
            lock);
        if (!found) {
            printf("no process %u\n", pid);
            return ERR_NOT_FOUND;
        }
    } else {
        printf("unrecognized subcommand\n");
        goto usage;
    }

    return 0;
}

} // namespace

STATIC_COMMAND_START
STATIC_COMMAND("lkuser", "user space routines", &cmd_lkuser)
STATIC_COMMAND_END(lkuser);

#endif
