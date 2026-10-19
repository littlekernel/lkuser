/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Calls built from payloads, and the clients of the kernel services on top. Nothing here uses
// the C library, so programs without one can call it.

#include <lk/ipc.h>
#include <lk/services.h>

static size_t msg_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

lk_status_t lk_msg_callv(lk_handle_t conn, struct lk_msg *m) {
    if (m->nreq > LK_IOV_MAX - 1 || m->nreply > LK_IOV_MAX - 1) {
        return ERR_INVALID_ARGS;
    }

    struct lk_msg_hdr hdr = {.op = m->op, .nhandles = (uint32_t)m->ncarry};
    struct lk_msg_hdr reply_hdr = {0};
    struct lk_iov req[LK_IOV_MAX];
    struct lk_iov reply[LK_IOV_MAX];
    req[0] = (struct lk_iov){&hdr, sizeof(hdr)};
    reply[0] = (struct lk_iov){&reply_hdr, sizeof(reply_hdr)};
    size_t req_len = 0;
    for (size_t i = 0; i < m->nreq; i++) {
        req[i + 1] = m->req[i];
        req_len += m->req[i].len;
        if (req_len < m->req[i].len || req_len > UINT32_MAX) {
            return ERR_TOO_BIG;
        }
    }
    for (size_t i = 0; i < m->nreply; i++) {
        reply[i + 1] = m->reply[i];
    }
    hdr.len = (uint32_t)req_len;

    struct lk_call_args args = {
        .req = req,
        .nreq = m->nreq + 1,
        .handles = m->carry,
        .nhandles = m->ncarry,
        .reply = reply,
        .nreply = m->nreply + 1,
        .reply_handles = m->handles,
        .reply_nhandles = m->handles_max,
        .handle_flags = m->carry_flags,
        .timeout_ns = LK_TIME_INFINITE,
    };
    lk_status_t err = lk_call(conn, &args);
    if (err < 0) {
        return err;
    }
    m->reply_len = reply_hdr.len;
    m->nhandles = reply_hdr.nhandles;
    return reply_hdr.status;
}

lk_status_t lk_msg_call(lk_handle_t conn, uint32_t op, const void *req, size_t req_len,
                        void *reply, size_t reply_cap, size_t *reply_len) {
    const struct lk_iov req_iov = {(void *)req, req_len};
    const struct lk_iov reply_iov = {reply, reply_cap};
    struct lk_msg m = {
        .op = op,
        .req = &req_iov,
        .nreq = req_len ? 1 : 0,
        .reply = &reply_iov,
        .nreply = reply_cap ? 1 : 0,
    };
    lk_status_t err = lk_msg_callv(conn, &m);
    if (reply_len) {
        *reply_len = m.reply_len;
    }
    return err;
}

// One call with a contiguous request payload and reply, lending lend if it is a handle, and
// taking at most one handle from the reply into *out.
static lk_status_t call_lend(lk_handle_t conn, uint32_t op, const void *req, size_t req_len,
                             void *reply, size_t reply_cap, lk_handle_t lend, lk_handle_t *out) {
    const struct lk_iov req_iov = {(void *)req, req_len};
    const struct lk_iov reply_iov = {reply, reply_cap};
    const int lending = lend != LK_HANDLE_INVALID;
    struct lk_msg m = {
        .op = op,
        .req = &req_iov,
        .nreq = req_len ? 1 : 0,
        .reply = &reply_iov,
        .nreply = reply_cap ? 1 : 0,
        .carry = &lend,
        .ncarry = lending ? 1 : 0,
        .carry_flags = lending ? LK_HANDLE_LEND(0) : 0,
        .handles = out,
        .handles_max = out ? 1 : 0,
    };
    lk_status_t err = lk_msg_callv(conn, &m);
    if (err == NO_ERROR && out && m.nhandles != 1) {
        return ERR_BAD_STATE;
    }
    return err;
}

lk_status_t lk_wait_one(lk_handle_t h, uint32_t mask, uint64_t timeout_ns, uint32_t *signals) {
    const struct lk_wait_item item = {.h = h, .mask = mask};
    struct lk_wait_args args = {.items = &item, .n = 1, .timeout_ns = timeout_ns};
    lk_status_t err = lk_wait(&args);
    if (signals) {
        *signals = args.signals;
    }
    return err;
}

lk_status_t lk_console_write(lk_handle_t console, const void *buf, size_t len) {
    return lk_msg_call(console, LK_CONSOLE_WRITE, buf, len, NULL, 0, NULL);
}

lk_status_t lk_console_read(lk_handle_t console, void *buf, size_t max, size_t *len) {
    const struct lk_console_read_req req = {.max = max > UINT32_MAX ? UINT32_MAX : (uint32_t)max};
    return lk_msg_call(console, LK_CONSOLE_READ, &req, sizeof(req), buf, max, len);
}

lk_status_t lk_console_winsize(lk_handle_t console, uint32_t *cols, uint32_t *rows) {
    struct lk_console_winsize ws;
    lk_status_t err = lk_msg_call(console, LK_CONSOLE_WINSIZE, NULL, 0, &ws, sizeof(ws), NULL);
    if (err < 0) {
        return err;
    }
    *cols = ws.cols;
    *rows = ws.rows;
    return NO_ERROR;
}

lk_status_t lk_process_info(lk_handle_t process, struct lk_process_info *info) {
    return lk_msg_call(process, LK_PROCESS_INFO, NULL, 0, info, sizeof(*info), NULL);
}

lk_status_t lk_process_info_of(lk_handle_t process, lk_handle_t proc, struct lk_process_info *info) {
    return call_lend(process, LK_PROCESS_INFO, NULL, 0, info, sizeof(*info), proc, NULL);
}

lk_status_t lk_process_kill(lk_handle_t process, lk_handle_t proc) {
    return call_lend(process, LK_PROCESS_KILL, NULL, 0, NULL, 0, proc, NULL);
}

lk_status_t lk_process_spawn(lk_handle_t process, const char *path, int argc,
                             const char *const *argv, uint32_t flags, const lk_handle_t *handles,
                             const uint32_t *tags, size_t nhandles, lk_handle_t *child,
                             uint32_t *pid) {
    if (argc < 1 || argc > LK_PROCESS_SPAWN_NARGS_MAX || nhandles > LK_MSG_MAX_HANDLES) {
        return ERR_INVALID_ARGS;
    }
    // the arguments travel as one block of NUL-terminated strings
    char args[LK_PROCESS_SPAWN_ARGS_MAX];
    size_t args_len = 0;
    for (int i = 0; i < argc; i++) {
        const size_t n = msg_strlen(argv[i]) + 1;
        if (n > sizeof(args) - args_len) {
            return ERR_TOO_BIG;
        }
        for (size_t j = 0; j < n; j++) {
            args[args_len + j] = argv[i][j];
        }
        args_len += n;
    }
    const size_t path_len = msg_strlen(path);
    if (path_len > UINT32_MAX) {
        return ERR_TOO_BIG;
    }

    struct lk_process_spawn_req req = {
        .flags = flags,
        .path_len = (uint32_t)path_len,
        .nargs = (uint32_t)argc,
        .args_len = (uint32_t)args_len,
    };
    for (size_t i = 0; i < nhandles; i++) {
        req.tags[i] = tags[i];
    }
    struct lk_process_spawn_reply reply;
    lk_handle_t h = LK_HANDLE_INVALID;
    const struct lk_iov req_iov[3] = {
        {&req, sizeof(req)}, {(void *)path, path_len}, {args, args_len},
    };
    const struct lk_iov reply_iov = {&reply, sizeof(reply)};
    struct lk_msg m = {
        .op = LK_PROCESS_SPAWN,
        .req = req_iov,
        .nreq = 3,
        .reply = &reply_iov,
        .nreply = 1,
        .carry = handles,
        .ncarry = nhandles,
        .handles = &h,
        .handles_max = 1,
    };
    lk_status_t err = lk_msg_callv(process, &m);
    if (err < 0) {
        return err;
    }
    if (pid) {
        *pid = reply.pid;
    }
    if (child) {
        *child = h;
    } else {
        lk_handle_close(h);
    }
    return NO_ERROR;
}

lk_status_t lk_process_thread_create(lk_handle_t process, uintptr_t entry, uintptr_t sp,
                                     lk_handle_t *thread, uint32_t *tid) {
    const struct lk_process_thread_create_req req = {.entry = entry, .sp = sp};
    struct lk_process_thread_create_reply reply;
    lk_status_t err = call_lend(process, LK_PROCESS_THREAD_CREATE, &req, sizeof(req), &reply,
                                sizeof(reply), LK_HANDLE_INVALID, thread);
    if (err == NO_ERROR && tid) {
        *tid = reply.tid;
    }
    return err;
}

// in crt0: loads fn and arg from the top of the new stack, calls fn(arg), then lk_thread_exit
void _lk_thread_entry(void);

lk_status_t lk_thread_start(lk_handle_t process, void (*fn)(void *), void *arg, void *stack,
                            size_t stack_size, lk_handle_t *thread) {
    // arch_enter_uspace passes no argument, so they wait in a 16-byte frame at the top of the
    // stack, where sp starts, aligned as the calling convention asks
    const uintptr_t top = ((uintptr_t)stack + stack_size) & ~(uintptr_t)15;
    if (top - (uintptr_t)stack < 16 || top < (uintptr_t)stack) {
        return ERR_INVALID_ARGS;
    }
    void **frame = (void **)(top - 16);
    frame[0] = (void *)fn;
    frame[1] = arg;
    return lk_process_thread_create(process, (uintptr_t)&_lk_thread_entry, (uintptr_t)frame,
                                    thread, NULL);
}

lk_status_t lk_time_sleep(lk_handle_t time, uint64_t ns) {
    const struct lk_time_sleep_req req = {.ns = ns};
    return lk_msg_call(time, LK_TIME_SLEEP, &req, sizeof(req), NULL, 0, NULL);
}

lk_status_t lk_time_clock_read(lk_handle_t time, uint32_t clock, uint64_t *ns) {
    const struct lk_time_clock_read_req req = {.clock = clock};
    struct lk_time_clock_read_reply reply;
    lk_status_t err = lk_msg_call(time, LK_TIME_CLOCK_READ, &req, sizeof(req), &reply,
                                  sizeof(reply), NULL);
    if (err < 0) {
        return err;
    }
    *ns = reply.ns;
    return NO_ERROR;
}

lk_status_t lk_vm_alloc(lk_handle_t vm, size_t size, uint32_t prot, void **va) {
    const struct lk_vm_alloc_req req = {.size = size, .prot = prot};
    struct lk_vm_alloc_reply reply;
    lk_status_t err = lk_msg_call(vm, LK_VM_ALLOC, &req, sizeof(req), &reply, sizeof(reply), NULL);
    if (err < 0) {
        return err;
    }
    *va = (void *)(uintptr_t)reply.va;
    return NO_ERROR;
}

lk_status_t lk_vm_unmap(lk_handle_t vm, void *va) {
    const struct lk_vm_unmap_req req = {.va = (uintptr_t)va};
    return lk_msg_call(vm, LK_VM_UNMAP, &req, sizeof(req), NULL, 0, NULL);
}

lk_status_t lk_vm_memobj_create(lk_handle_t vm, size_t size, uint32_t flags, lk_handle_t *memobj,
                                size_t *actual) {
    const struct lk_vm_memobj_create_req req = {.size = size, .flags = flags};
    struct lk_vm_memobj_create_reply reply;
    lk_status_t err = call_lend(vm, LK_VM_MEMOBJ_CREATE, &req, sizeof(req), &reply,
                                sizeof(reply), LK_HANDLE_INVALID, memobj);
    if (err == NO_ERROR && actual) {
        *actual = (size_t)reply.size;
    }
    return err;
}

lk_status_t lk_vm_memobj_map(lk_handle_t vm, lk_handle_t memobj, size_t off, size_t len,
                             uint32_t prot, void **va) {
    const struct lk_vm_memobj_map_req req = {.off = off, .len = len, .prot = prot};
    struct lk_vm_alloc_reply reply;
    lk_status_t err = call_lend(vm, LK_VM_MEMOBJ_MAP, &req, sizeof(req), &reply, sizeof(reply),
                                memobj, NULL);
    if (err == NO_ERROR) {
        *va = (void *)(uintptr_t)reply.va;
    }
    return err;
}

// A namespace request: flags and path, then whatever reply and handles the op returns.
static lk_status_t ns_call(lk_handle_t ns, struct lk_msg *m, uint32_t flags, const char *path) {
    const size_t len = msg_strlen(path);
    if (len > UINT32_MAX) {
        return ERR_TOO_BIG;
    }
    const struct lk_ns_path_req req = {.flags = flags, .path_len = (uint32_t)len};
    const struct lk_iov req_iov[2] = {{(void *)&req, sizeof(req)}, {(void *)path, len}};
    m->req = req_iov;
    m->nreq = 2;
    return lk_msg_callv(ns, m);
}

lk_status_t lk_ns_open(lk_handle_t ns, const char *path, uint32_t flags, lk_handle_t *file,
                       struct lk_file_stat *st) {
    struct lk_file_stat reply;
    lk_handle_t h = LK_HANDLE_INVALID;
    const struct lk_iov reply_iov = {&reply, sizeof(reply)};
    struct lk_msg m = {
        .op = LK_NS_OPEN, .reply = &reply_iov, .nreply = 1, .handles = &h, .handles_max = 1,
    };
    lk_status_t err = ns_call(ns, &m, flags, path);
    if (err < 0) {
        return err;
    }
    // an open that succeeded hands over exactly one connection
    if (m.nhandles != 1) {
        return ERR_BAD_STATE;
    }
    *file = h;
    if (st) {
        *st = reply;
    }
    return NO_ERROR;
}

lk_status_t lk_ns_stat(lk_handle_t ns, const char *path, struct lk_file_stat *st) {
    const struct lk_iov reply_iov = {st, sizeof(*st)};
    struct lk_msg m = {.op = LK_NS_STAT, .reply = &reply_iov, .nreply = 1};
    return ns_call(ns, &m, 0, path);
}

lk_status_t lk_ns_mkdir(lk_handle_t ns, const char *path) {
    struct lk_msg m = {.op = LK_NS_MKDIR};
    return ns_call(ns, &m, 0, path);
}

lk_status_t lk_ns_unlink(lk_handle_t ns, const char *path) {
    struct lk_msg m = {.op = LK_NS_UNLINK};
    return ns_call(ns, &m, 0, path);
}

lk_status_t lk_ns_rmdir(lk_handle_t ns, const char *path) {
    struct lk_msg m = {.op = LK_NS_RMDIR};
    return ns_call(ns, &m, 0, path);
}

lk_status_t lk_file_read(lk_handle_t file, uint64_t off, void *buf, size_t len, size_t *nread) {
    if (len > UINT32_MAX) {
        len = UINT32_MAX;
    }
    const struct lk_file_read_req req = {.off = off, .len = (uint32_t)len};
    return lk_msg_call(file, LK_FILE_READ, &req, sizeof(req), buf, len, nread);
}

lk_status_t lk_file_write(lk_handle_t file, uint64_t off, uint32_t flags, const void *buf,
                          size_t len, size_t *nwritten, uint64_t *end) {
    const struct lk_file_write_req req = {.off = off, .flags = flags};
    // the payload's length fits 32 bits
    if (len > UINT32_MAX - sizeof(req)) {
        len = UINT32_MAX - sizeof(req);
    }
    struct lk_file_write_reply reply;
    const struct lk_iov req_iov[2] = {{(void *)&req, sizeof(req)}, {(void *)buf, len}};
    const struct lk_iov reply_iov = {&reply, sizeof(reply)};
    struct lk_msg m = {
        .op = LK_FILE_WRITE, .req = req_iov, .nreq = len ? 2 : 1, .reply = &reply_iov, .nreply = 1,
    };
    lk_status_t err = lk_msg_callv(file, &m);
    if (err < 0) {
        return err;
    }
    if (nwritten) {
        *nwritten = reply.written;
    }
    if (end) {
        *end = reply.end;
    }
    return NO_ERROR;
}

lk_status_t lk_file_fstat(lk_handle_t file, struct lk_file_stat *st) {
    return lk_msg_call(file, LK_FILE_STAT, NULL, 0, st, sizeof(*st), NULL);
}

lk_status_t lk_file_truncate(lk_handle_t file, uint64_t size) {
    const struct lk_file_truncate_req req = {.size = size};
    return lk_msg_call(file, LK_FILE_TRUNCATE, &req, sizeof(req), NULL, 0, NULL);
}

lk_status_t lk_file_readdir(lk_handle_t dir, uint32_t index, char *name, size_t cap,
                            size_t *name_len) {
    if (cap == 0) {
        return ERR_NOT_ENOUGH_BUFFER;
    }
    const struct lk_file_readdir_req req = {.index = index};
    struct lk_file_dirent ent;
    const struct lk_iov req_iov = {(void *)&req, sizeof(req)};
    const struct lk_iov reply_iov[2] = {{&ent, sizeof(ent)}, {name, cap - 1}};
    struct lk_msg m = {
        .op = LK_FILE_READDIR, .req = &req_iov, .nreq = 1, .reply = reply_iov, .nreply = 2,
    };
    lk_status_t err = lk_msg_callv(dir, &m);
    if (err < 0) {
        return err;
    }
    const size_t len = ent.name_len < cap - 1 ? ent.name_len : cap - 1;
    name[len] = '\0';
    if (name_len) {
        *name_len = len;
    }
    return NO_ERROR;
}

static lk_status_t file_memobj_io(lk_handle_t file, uint32_t op, uint64_t off, uint32_t flags,
                                  lk_handle_t memobj, size_t mem_off, size_t len, size_t *count,
                                  uint64_t *end) {
    const struct lk_file_io_req req = {
        .off = off, .mem_off = mem_off, .len = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len,
        .flags = flags,
    };
    struct lk_file_io_reply reply;
    lk_status_t err = call_lend(file, op, &req, sizeof(req), &reply, sizeof(reply), memobj, NULL);
    if (err < 0) {
        return err;
    }
    if (count) {
        *count = reply.count;
    }
    if (end) {
        *end = reply.end;
    }
    return NO_ERROR;
}

lk_status_t lk_file_read_into(lk_handle_t file, uint64_t off, lk_handle_t memobj, size_t mem_off,
                              size_t len, size_t *count) {
    return file_memobj_io(file, LK_FILE_READ_INTO, off, 0, memobj, mem_off, len, count, NULL);
}

lk_status_t lk_file_write_from(lk_handle_t file, uint64_t off, uint32_t flags, lk_handle_t memobj,
                               size_t mem_off, size_t len, size_t *count, uint64_t *end) {
    return file_memobj_io(file, LK_FILE_WRITE_FROM, off, flags, memobj, mem_off, len, count, end);
}
