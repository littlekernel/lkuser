/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The namespace service: one tree of paths over LK's file systems, under the root the console
// sets with "lkuser root". OPEN hands back a connection to the file service.

#include <lib/fs.h>
#include <string.h>
#include <sys/svc/namespace.h>

#include "../handle.h"
#include "../user.h"
#include "open_file.h"
#include "svc.h"

namespace lkuser {

namespace {

constexpr uint32_t kOpenFlags = LK_NS_OPEN_READ | LK_NS_OPEN_WRITE | LK_NS_OPEN_CREATE |
                                LK_NS_OPEN_EXCL | LK_NS_OPEN_TRUNC | LK_NS_OPEN_DIR;

// The request's flags and its path as a path in the kernel's tree, out holding
// FS_MAX_PATH_LEN bytes. The path is normalized before the root goes in front of it, so ".."
// cannot climb out of the root.
lk_status_t request_path(kcall &c, uint32_t *flags, char *out) {
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

    char path[FS_MAX_PATH_LEN];
    n = c.read(sizeof(req), path, req.path_len);
    if (n < 0) {
        return static_cast<lk_status_t>(n);
    }
    *flags = req.flags;
    return resolve_user_path(path, req.path_len, out);
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

lk_status_t ns_handler(kcall &c) {
    // no op takes a handle
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
