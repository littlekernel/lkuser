/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The process service: processes and threads, made and ended.

#include <lib/fs.h>
#include <lktl/auto_call.h>
#include <stdlib.h>
#include <string.h>
#include <sys/svc/process.h>

#include "../endpoint.h"
#include "../proc.h"
#include "../thread.h"
#include "../user.h"
#include "svc.h"

namespace lkuser {

namespace {

static_assert(LK_PROCESS_KILLED == ERR_CANCELLED, "a killed process's exit code is LK's");

constexpr uint32_t kProcessRights =
    LK_RIGHT_WAIT | LK_RIGHT_INFO | LK_RIGHT_KILL | LK_RIGHT_DUP | LK_RIGHT_TRANSFER;
constexpr uint32_t kThreadRights = LK_RIGHT_WAIT | LK_RIGHT_DUP | LK_RIGHT_TRANSFER;

constexpr uint32_t kMaxArgs = LK_PROCESS_SPAWN_NARGS_MAX;
constexpr uint32_t kMaxArgsLen = LK_PROCESS_SPAWN_ARGS_MAX;

// The process named by the request's one handle, with need, or with none the caller.
lk_status_t target(kcall &c, uint32_t need, lk::ref_ptr<proc> *out) {
    if (c.nhandles() == 0) {
        *out = lk::ref_ptr<proc>(c.caller());
        return NO_ERROR;
    }
    lk_status_t err = c.expect_handles(1);
    if (err < 0) {
        return err;
    }
    return c.handle_as(0, need, out);
}

lk_status_t process_info(kcall &c) {
    if (c.len() != 0) {
        return ERR_INVALID_ARGS;
    }
    lk::ref_ptr<proc> p;
    lk_status_t err = target(c, LK_RIGHT_INFO, &p);
    if (err < 0) {
        return err;
    }
    int code = 0;
    const bool exited = p->exited(&code);
    const lk_process_info info = {
        .pid = p->pid(),
        .state = exited ? LK_PROCESS_EXITED : LK_PROCESS_RUNNING,
        .exit_code = code,
        .reserved = 0,
    };
    return c.reply(info);
}

lk_status_t process_kill(kcall &c) {
    if (c.len() != 0 || c.nhandles() != 1) {
        return ERR_INVALID_ARGS;
    }
    lk::ref_ptr<proc> p;
    lk_status_t err = c.handle_as(0, LK_RIGHT_KILL, &p);
    if (err < 0) {
        return err;
    }
    // a process that has exited keeps its code
    p->exit(LK_PROCESS_KILLED);
    return NO_ERROR;
}

lk_status_t process_thread_create(kcall &c) {
    lk_process_thread_create_req req;
    lk_status_t err = c.args(&req);
    if (err == NO_ERROR) {
        err = c.expect_handles(0);
    }
    if (err < 0) {
        return err;
    }
    proc *p = c.caller();
    const vmm_aspace_t *aspace = p->aspace();
    if (req.entry > UINTPTR_MAX || req.sp > UINTPTR_MAX || (req.sp & 15) ||
        !user_range_valid(aspace, static_cast<vaddr_t>(req.entry), 1) ||
        !user_range_valid(aspace, static_cast<vaddr_t>(req.sp) - 1, 1)) {
        return ERR_INVALID_ARGS;
    }
    if (c.reply_capacity() < sizeof(lk_process_thread_create_reply) ||
        c.reply_handles_capacity() < 1) {
        return ERR_NOT_ENOUGH_BUFFER;
    }

    lk::ref_ptr<thread> t;
    err = thread::create(p, static_cast<vaddr_t>(req.entry), static_cast<vaddr_t>(req.sp), &t);
    if (err < 0) {
        return err;
    }
    // the handle first: a full table refuses before anything runs
    err = c.reply_handle(t, kThreadRights);
    if (err == NO_ERROR) {
        err = c.reply(lk_process_thread_create_reply{.tid = t->tid(), .reserved = 0});
    }
    if (err == NO_ERROR) {
        err = p->start_thread(t.get());
    }
    return err;
}

// The arguments of a spawn request: nargs NUL-terminated strings filling buf exactly.
lk_status_t split_args(char *buf, size_t len, uint32_t nargs, const char **argv) {
    size_t pos = 0;
    for (uint32_t i = 0; i < nargs; i++) {
        const char *end = static_cast<const char *>(memchr(buf + pos, '\0', len - pos));
        if (!end) {
            return ERR_INVALID_ARGS;
        }
        argv[i] = buf + pos;
        pos = end - buf + 1;
    }
    return pos == len ? NO_ERROR : ERR_INVALID_ARGS;
}

// Everything is read and checked, the child made and loaded and the parent's handle to it
// installed before the child runs, so a refusal starts nothing. The carried handles, all the
// child is born with, go into its table now and leave the caller's once this returns success.
lk_status_t process_spawn(kcall &c) {
    lk_process_spawn_req req;
    if (c.len() < sizeof(req)) {
        return ERR_INVALID_ARGS;
    }
    ssize_t n = c.read(0, &req, sizeof(req));
    if (n < 0) {
        return static_cast<lk_status_t>(n);
    }
    if (req.flags != 0 || req.nargs == 0 || req.nargs > kMaxArgs ||
        req.args_len > kMaxArgsLen ||
        c.len() - sizeof(req) != uint64_t{req.path_len} + req.args_len) {
        return ERR_INVALID_ARGS;
    }
    if (req.path_len >= FS_MAX_PATH_LEN) {
        return ERR_TOO_BIG;
    }
    if (c.reply_capacity() < sizeof(lk_process_spawn_reply) || c.reply_handles_capacity() < 1) {
        return ERR_NOT_ENOUGH_BUFFER;
    }

    // the child's startup handles: moved, never lent, since a lent one ends with this call;
    // and perhaps the connection its exit is told over, which the kernel keeps
    startup_handle extra[LK_MSG_MAX_HANDLES];
    size_t nextra = 0;
    const carried_handle *notify = nullptr;
    for (size_t i = 0; i < c.nhandles(); i++) {
        const carried_handle &h = c.handle(i);
        if (h.lent || req.tags[i] == 0) {
            return ERR_INVALID_ARGS;
        }
        for (size_t j = 0; j < i; j++) {
            if (req.tags[j] == req.tags[i]) {
                return ERR_INVALID_ARGS;
            }
        }
        if (req.tags[i] == LK_PROCESS_SPAWN_TAG_EXIT_NOTIFY) {
            lk::ref_ptr<connection> conn;
            lk_status_t err = c.handle_as(i, LK_RIGHT_IMPULSE, &conn);
            if (err < 0) {
                return err;
            }
            if (conn->ep()->is_kernel()) {
                return ERR_NOT_SUPPORTED;
            }
            notify = &h;
            continue;
        }
        extra[nextra++] = {req.tags[i], h.obj, h.rights};
    }
    for (size_t i = c.nhandles(); i < LK_MSG_MAX_HANDLES; i++) {
        if (req.tags[i] != 0) {
            return ERR_INVALID_ARGS;
        }
    }

    char path_arg[FS_MAX_PATH_LEN];
    n = c.read(sizeof(req), path_arg, req.path_len);
    if (n < 0) {
        return static_cast<lk_status_t>(n);
    }
    char path[FS_MAX_PATH_LEN];
    lk_status_t err = resolve_user_path(path_arg, req.path_len, path);
    if (err < 0) {
        return err;
    }

    char *args = static_cast<char *>(malloc(req.args_len ? req.args_len : 1));
    if (!args) {
        return ERR_NO_MEMORY;
    }
    auto free_args = lk::make_auto_call([args]() { free(args); });
    n = c.read(sizeof(req) + req.path_len, args, req.args_len);
    if (n < 0) {
        return static_cast<lk_status_t>(n);
    }
    const char *argv[kMaxArgs];
    err = split_args(args, req.args_len, req.nargs, argv);
    if (err < 0) {
        return err;
    }

    lk::ref_ptr<proc> child;
    err = proc::create(req.nargs, argv, false, extra, nextra, &child);
    if (err < 0) {
        return err;
    }
    if (notify) {
        child->set_exit_notify(notify->obj, notify->rights);
    }
    vaddr_t entry;
    err = load_image(child.get(), path, &entry);
    if (err < 0) {
        return err;
    }
    err = c.reply_handle(child, kProcessRights);
    if (err == NO_ERROR) {
        err = c.reply(lk_process_spawn_reply{.pid = child->pid(), .reserved = 0});
    }
    if (err == NO_ERROR) {
        err = child->start(entry);
    }
    return err;
}

lk_status_t process_handler(kcall &c) {
    switch (c.op()) {
        case LK_PROCESS_INFO:
            return process_info(c);
        case LK_PROCESS_SPAWN:
            return process_spawn(c);
        case LK_PROCESS_THREAD_CREATE:
            return process_thread_create(c);
        case LK_PROCESS_KILL:
            return process_kill(c);
        default:
            return ERR_NOT_SUPPORTED;
    }
}

} // namespace

} // namespace lkuser

LKUSER_SERVICE(process, LK_TAG_PROCESS, &lkuser::process_handler);
