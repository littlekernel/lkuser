/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The namespace service: one tree of paths over LK's file systems, under the root the console
// sets with "lkuser root", where user servers can take over paths with MOUNT. OPEN hands back a
// connection to the file service, LOOKUP one to the server mounted over a path.

#include <kernel/mutex.h>
#include <lib/fs.h>
#include <string.h>
#include <sys/svc/namespace.h>

#include "../handle.h"
#include "../proc.h"
#include "../user.h"
#include "open_file.h"
#include "svc.h"

namespace lkuser {

namespace {

constexpr uint32_t kOpenFlags = LK_NS_OPEN_READ | LK_NS_OPEN_WRITE | LK_NS_OPEN_CREATE |
                                LK_NS_OPEN_EXCL | LK_NS_OPEN_TRUNC | LK_NS_OPEN_DIR;

// The paths user servers own, in the namespace's own terms (no "lkuser root" in front). A
// mount holds its endpoint without counting as a handle, so it is dead once the endpoint has
// closed, and is dropped when next seen.
constexpr size_t kMaxMounts = 8;
struct mount {
    char path[FS_MAX_PATH_LEN];
    size_t len;
    lk::ref_ptr<endpoint> ep;
};
mount mounts[kMaxMounts];
Mutex mounts_lock;

// The request's flags and the len bytes of its path, path holding FS_MAX_PATH_LEN.
lk_status_t request_raw_path(kcall &c, uint32_t *flags, char *path, size_t *len) {
    lk_ns_path_req req;
    if (c.len() < sizeof(req)) {
        return ERR_INVALID_ARGS;
    }
    ssize_t n = c.read(0, &req, sizeof(req));
    if (n < 0) {
        return static_cast<lk_status_t>(n);
    }
    if (c.len() - sizeof(req) != req.path_len) {
        return ERR_INVALID_ARGS;
    }
    if (req.path_len >= FS_MAX_PATH_LEN) {
        return ERR_TOO_BIG;
    }

    n = c.read(sizeof(req), path, req.path_len);
    if (n < 0) {
        return static_cast<lk_status_t>(n);
    }
    *flags = req.flags;
    *len = req.path_len;
    return NO_ERROR;
}

// The request's flags and its path as a path in the kernel's tree, out holding
// FS_MAX_PATH_LEN bytes. The path is normalized before the root goes in front of it, so ".."
// cannot climb out of the root.
lk_status_t request_path(kcall &c, uint32_t *flags, char *out) {
    char path[FS_MAX_PATH_LEN];
    size_t len;
    lk_status_t err = request_raw_path(c, flags, path, &len);
    if (err < 0) {
        return err;
    }
    return resolve_user_path(path, len, out);
}

// As request_path, for the mount table: normalized, with no root in front.
lk_status_t request_ns_path(kcall &c, uint32_t *flags, char *out) {
    char path[FS_MAX_PATH_LEN];
    size_t len;
    lk_status_t err = request_raw_path(c, flags, path, &len);
    if (err < 0) {
        return err;
    }
    return normalize_user_path(path, len, out);
}

lk_status_t open_file_at(const char *path, uint32_t flags, lk::ref_ptr<connection> *conn,
                         lk_file_stat *st) {
    filehandle *fh;
    lk_status_t err;
    if (flags & LK_NS_OPEN_CREATE) {
        err = fs_create_file(path, &fh, 0);
        if (err == ERR_ALREADY_EXISTS && !(flags & LK_NS_OPEN_EXCL)) {
            err = fs_open_file(path, &fh);
        }
    } else {
        err = fs_open_file(path, &fh);
    }
    if (err < 0) {
        return err;
    }

    // lib/fs opens a directory as readily as a file
    file_stat fst;
    err = fs_stat_file(fh, &fst);
    if (err == NO_ERROR && fst.is_dir) {
        err = ERR_NOT_FILE;
    }
    if (err == NO_ERROR && (flags & LK_NS_OPEN_TRUNC) && fst.size > 0) {
        err = fs_truncate_file(fh, 0);
        fst.size = 0;
    }
    if (err < 0) {
        fs_close_file(fh);
        return err;
    }

    *st = {.size = fst.size, .type = LK_FILE_TYPE_FILE, .reserved = 0};
    return make_file_connection(fh, flags, conn);
}

lk_status_t ns_open(kcall &c) {
    uint32_t flags;
    char path[FS_MAX_PATH_LEN];
    lk_status_t err = request_path(c, &flags, path);
    if (err < 0) {
        return err;
    }
    if (flags & ~kOpenFlags) {
        return ERR_INVALID_ARGS;
    }
    if (flags & LK_NS_OPEN_DIR) {
        if (flags != LK_NS_OPEN_DIR) {
            return ERR_INVALID_ARGS;
        }
    } else if (!(flags & (LK_NS_OPEN_READ | LK_NS_OPEN_WRITE)) ||
               ((flags & LK_NS_OPEN_EXCL) && !(flags & LK_NS_OPEN_CREATE)) ||
               ((flags & LK_NS_OPEN_TRUNC) && !(flags & LK_NS_OPEN_WRITE))) {
        return ERR_INVALID_ARGS;
    }
    // nothing is opened or created that the reply could not hand over
    if (c.reply_handles_capacity() < 1 || c.reply_capacity() < sizeof(lk_file_stat)) {
        return ERR_NOT_ENOUGH_BUFFER;
    }
    if ((flags & LK_NS_OPEN_CREATE) && !c.caller()->handles().has_free_slot()) {
        return ERR_NO_RESOURCES;
    }

    lk::ref_ptr<connection> conn;
    lk_file_stat st = {.size = 0, .type = LK_FILE_TYPE_DIR, .reserved = 0};
    if (flags & LK_NS_OPEN_DIR) {
        dirhandle *dh;
        err = fs_open_dir(path, &dh);
        if (err == NO_ERROR) {
            err = make_dir_connection(dh, path, &conn);
        }
    } else {
        err = open_file_at(path, flags, &conn, &st);
    }
    if (err < 0) {
        return err;
    }

    // with a full table the connection goes here, and the file closes with it
    err = c.reply_handle(std::move(conn), kConnectionRights);
    if (err < 0) {
        return err;
    }
    return c.reply(st);
}

lk_status_t ns_stat(kcall &c) {
    uint32_t flags;
    char path[FS_MAX_PATH_LEN];
    lk_status_t err = request_path(c, &flags, path);
    if (err < 0) {
        return err;
    }
    if (flags != 0) {
        return ERR_INVALID_ARGS;
    }

    lk_file_stat st = {.size = 0, .type = LK_FILE_TYPE_DIR, .reserved = 0};
    filehandle *fh;
    err = fs_open_file(path, &fh);
    if (err == NO_ERROR) {
        file_stat fst;
        err = fs_stat_file(fh, &fst);
        fs_close_file(fh);
        if (err < 0) {
            return err;
        }
        if (!fst.is_dir) {
            st.size = fst.size;
            st.type = LK_FILE_TYPE_FILE;
        }
    } else if (err == ERR_NOT_FOUND) {
        // a directory only the mount table knows, such as "/" above the mounts, is no file
        dirhandle *dh;
        if (fs_open_dir(path, &dh) < 0) {
            return err;
        }
        fs_close_dir(dh);
    } else {
        return err;
    }
    return c.reply(st);
}

// MKDIR, UNLINK and RMDIR: a path, no flags, and lib/fs's answer.
lk_status_t ns_path_op(kcall &c, status_t (*op)(const char *path)) {
    uint32_t flags;
    char path[FS_MAX_PATH_LEN];
    lk_status_t err = request_path(c, &flags, path);
    if (err < 0) {
        return err;
    }
    if (flags != 0) {
        return ERR_INVALID_ARGS;
    }
    return op(path);
}

// Whether path is the mount point at mp (of length len) or lies below it.
bool under(const char *path, const char *mp, size_t len) {
    return strncmp(path, mp, len) == 0 && (path[len] == '\0' || path[len] == '/');
}

lk_status_t ns_mount(kcall &c) {
    uint32_t flags;
    char path[FS_MAX_PATH_LEN];
    lk_status_t err = request_ns_path(c, &flags, path);
    if (err == NO_ERROR) {
        err = c.expect_handles(1);
    }
    if (err < 0) {
        return err;
    }
    // a lent handle ends with the call, and a mount of "/" would cover every path
    if (flags != 0 || c.handle(0).lent || strcmp(path, "/") == 0) {
        return ERR_INVALID_ARGS;
    }
    lk::ref_ptr<endpoint> ep;
    err = c.handle_as(0, LK_RIGHT_CONNECT, &ep);
    if (err < 0) {
        return err;
    }
    // LOOKUP connects on the caller's thread, inside its call, where no handshake can run
    if (ep->is_kernel() || (ep->flags() & LK_EP_FLAG_CONNECT_NOTIFY)) {
        return ERR_NOT_SUPPORTED;
    }

    lk::ref_ptr<endpoint> dead;
    {
        AutoLock guard(mounts_lock);
        mount *slot = nullptr;
        for (mount &m : mounts) {
            if (m.ep && strcmp(m.path, path) == 0) {
                if (!m.ep->closed()) {
                    return ERR_ALREADY_MOUNTED;
                }
                slot = &m;
                break;
            }
            if (!m.ep && !slot) {
                slot = &m;
            }
        }
        if (!slot) {
            return ERR_NO_RESOURCES;
        }
        dead = std::move(slot->ep);
        strlcpy(slot->path, path, sizeof(slot->path));
        slot->len = strlen(path);
        slot->ep = std::move(ep);
    }
    // a dead mount's endpoint reference goes here, outside the lock
    return NO_ERROR;
}

lk_status_t ns_unmount(kcall &c) {
    uint32_t flags;
    char path[FS_MAX_PATH_LEN];
    lk_status_t err = request_ns_path(c, &flags, path);
    if (err == NO_ERROR) {
        err = c.expect_handles(1);
    }
    if (err < 0) {
        return err;
    }
    if (flags != 0) {
        return ERR_INVALID_ARGS;
    }
    if (c.handle(0).obj->kind() != obj_kind::ENDPOINT) {
        return ERR_BAD_HANDLE;
    }

    lk::ref_ptr<endpoint> gone;
    {
        AutoLock guard(mounts_lock);
        for (mount &m : mounts) {
            if (m.ep && strcmp(m.path, path) == 0) {
                if (m.ep.get() != c.handle(0).obj.get()) {
                    return ERR_ACCESS_DENIED;
                }
                gone = std::move(m.ep);
                return NO_ERROR;
            }
        }
    }
    return ERR_NOT_FOUND;
}

lk_status_t ns_lookup(kcall &c) {
    uint32_t flags;
    char path[FS_MAX_PATH_LEN];
    lk_status_t err = request_ns_path(c, &flags, path);
    if (err == NO_ERROR) {
        err = c.expect_handles(0);
    }
    if (err < 0) {
        return err;
    }
    if (flags != 0) {
        return ERR_INVALID_ARGS;
    }
    if (c.reply_handles_capacity() < 1 || c.reply_capacity() < sizeof(lk_ns_lookup_reply)) {
        return ERR_NOT_ENOUGH_BUFFER;
    }

    lk::ref_ptr<endpoint> ep;
    size_t len = 0;
    lk::ref_ptr<endpoint> dead[kMaxMounts];
    {
        AutoLock guard(mounts_lock);
        for (size_t i = 0; i < kMaxMounts; i++) {
            mount &m = mounts[i];
            if (!m.ep) {
                continue;
            }
            if (m.ep->closed()) {
                dead[i] = std::move(m.ep);
                continue;
            }
            if (m.len > len && under(path, m.path, m.len)) {
                ep = m.ep;
                len = m.len;
            }
        }
    }
    if (!ep) {
        return ERR_NOT_FOUND;
    }

    const char *rest = path + len;
    const lk_ns_lookup_reply r = {.rest_len = static_cast<uint32_t>(strlen(rest)), .reserved = 0};
    if (c.reply_capacity() < sizeof(r) + r.rest_len) {
        return ERR_NOT_ENOUGH_BUFFER;
    }
    lk::ref_ptr<connection> conn = object::make<connection>(std::move(ep));
    if (!conn) {
        return ERR_NO_MEMORY;
    }
    err = c.reply_handle(std::move(conn), kConnectionRights);
    if (err < 0) {
        return err;
    }
    ssize_t n = c.write(0, &r, sizeof(r));
    if (n >= 0) {
        n = c.write(sizeof(r), rest, r.rest_len);
    }
    return n < 0 ? static_cast<lk_status_t>(n) : NO_ERROR;
}

lk_status_t ns_handler(kcall &c) {
    switch (c.op()) {
        case LK_NS_MOUNT:
            return ns_mount(c);
        case LK_NS_UNMOUNT:
            return ns_unmount(c);
        case LK_NS_LOOKUP:
            return ns_lookup(c);
        default:
            break;
    }
    // no other op takes a handle
    if (c.nhandles() != 0) {
        return ERR_INVALID_ARGS;
    }
    switch (c.op()) {
        case LK_NS_OPEN:
            return ns_open(c);
        case LK_NS_STAT:
            return ns_stat(c);
        case LK_NS_MKDIR:
            return ns_path_op(c, &fs_make_dir);
        case LK_NS_UNLINK:
            return ns_path_op(c, &fs_remove_file);
        case LK_NS_RMDIR:
            return ns_path_op(c, &fs_remove_dir);
        default:
            return ERR_NOT_SUPPORTED;
    }
}

} // namespace

} // namespace lkuser

LKUSER_SERVICE(namespace, LK_TAG_NAMESPACE, &lkuser::ns_handler);
