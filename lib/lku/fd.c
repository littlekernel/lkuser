/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// The fd table and newlib's file system calls. An fd names a handle: 0, 1 and 2 share the
// console connection the process was born with, which closing them leaves open; any other is a
// file or directory connection from namespace.OPEN, which close() closes. File I/O is
// positional, so the offset lives here.

#include <errno.h>
#include <fcntl.h>
#include <lk/services.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "lku_priv.h"

#define MAX_FDS 32
// the longest path, made absolute, that a call sends
#define PATH_BUF 256

enum fd_kind {
    FD_FREE,
    FD_CONSOLE,
    FD_FILE,
    FD_DIR,
};

struct fd {
    enum fd_kind kind;
    int owned; // close() closes the handle
    int flags; // as open() was given them
    lk_handle_t h;
    uint64_t off;
};

static struct fd fds[MAX_FDS];

void lku_fd_init(void) {
    if (lku_console == LK_HANDLE_INVALID) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        fds[i] = (struct fd){
            .kind = FD_CONSOLE, .flags = i == 0 ? O_RDONLY : O_WRONLY, .h = lku_console,
        };
    }
}

static struct fd *get_fd(int fd) {
    if (fd < 0 || fd >= MAX_FDS || fds[fd].kind == FD_FREE) {
        errno = EBADF;
        return NULL;
    }
    return &fds[fd];
}

const char *lku_ns_path(const char *path, char *buf, size_t cap) {
    if (lku_namespace == LK_HANDLE_INVALID) {
        lku_fail(ERR_NOT_SUPPORTED);
        return NULL;
    }
    if (path[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }
    if (path[0] == '/') {
        return path;
    }
    const size_t len = strlen(path);
    if (len + 2 > cap) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    buf[0] = '/';
    memcpy(buf + 1, path, len + 1);
    return buf;
}

static void fill_stat(struct stat *st, const struct lk_file_stat *fst) {
    memset(st, 0, sizeof(*st));
    st->st_mode = fst->type == LK_FILE_TYPE_DIR ? S_IFDIR | 0755 : S_IFREG | 0644;
    st->st_nlink = 1;
    st->st_size = (off_t)fst->size;
    st->st_blksize = 512;
    st->st_blocks = (blkcnt_t)((fst->size + 511) / 512);
}

int _open(const char *name, int flags, int mode) {
    char buf[PATH_BUF];
    const char *path = lku_ns_path(name, buf, sizeof(buf));
    if (!path) {
        return -1;
    }
    int fd = 0;
    while (fd < MAX_FDS && fds[fd].kind != FD_FREE) {
        fd++;
    }
    if (fd == MAX_FDS) {
        errno = EMFILE;
        return -1;
    }

    uint32_t ns_flags;
    switch (flags & O_ACCMODE) {
        case O_RDONLY:
            ns_flags = LK_NS_OPEN_READ;
            break;
        case O_WRONLY:
            ns_flags = LK_NS_OPEN_WRITE;
            break;
        case O_RDWR:
            ns_flags = LK_NS_OPEN_READ | LK_NS_OPEN_WRITE;
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    // O_EXCL without O_CREAT, and O_TRUNC without writing, mean nothing
    if (flags & O_CREAT) {
        ns_flags |= LK_NS_OPEN_CREATE | ((flags & O_EXCL) ? LK_NS_OPEN_EXCL : 0);
    }
    if ((flags & O_TRUNC) && (ns_flags & LK_NS_OPEN_WRITE)) {
        ns_flags |= LK_NS_OPEN_TRUNC;
    }

    enum fd_kind kind = FD_FILE;
    lk_handle_t h;
    lk_status_t err = lk_ns_open(lku_namespace, path, ns_flags, &h, NULL);
    // a directory opened for reading, as fstat and fdopendir users do
    if (err == ERR_NOT_FILE && ns_flags == LK_NS_OPEN_READ) {
        kind = FD_DIR;
        err = lk_ns_open(lku_namespace, path, LK_NS_OPEN_DIR, &h, NULL);
    }
    if (err < 0) {
        return lku_fail(err);
    }
    fds[fd] = (struct fd){.kind = kind, .owned = 1, .flags = flags, .h = h, .off = 0};
    return fd;
}

int _close(int fd) {
    struct fd *f = get_fd(fd);
    if (!f) {
        return -1;
    }
    lk_status_t err = f->owned ? lk_handle_close(f->h) : NO_ERROR;
    f->kind = FD_FREE;
    return err < 0 ? lku_fail(err) : 0;
}

_ssize_t _read(int fd, void *buf, size_t len) {
    struct fd *f = get_fd(fd);
    if (!f) {
        return -1;
    }
    size_t n = 0;
    lk_status_t err;
    switch (f->kind) {
        case FD_CONSOLE:
            err = lk_console_read(f->h, buf, len, &n);
            break;
        case FD_FILE:
            if ((f->flags & O_ACCMODE) == O_WRONLY) {
                errno = EBADF;
                return -1;
            }
            err = lk_file_read(f->h, f->off, buf, len, &n);
            if (err == NO_ERROR) {
                f->off += n;
            }
            break;
        default:
            errno = EISDIR;
            return -1;
    }
    return err < 0 ? lku_fail(err) : (_ssize_t)n;
}

_ssize_t _write(int fd, const void *buf, size_t len) {
    struct fd *f = get_fd(fd);
    if (!f) {
        return -1;
    }
    size_t n = len;
    lk_status_t err;
    switch (f->kind) {
        case FD_CONSOLE:
            err = lk_console_write(f->h, buf, len);
            break;
        case FD_FILE: {
            if ((f->flags & O_ACCMODE) == O_RDONLY) {
                errno = EBADF;
                return -1;
            }
            const uint32_t wflags = (f->flags & O_APPEND) ? LK_FILE_WRITE_APPEND : 0;
            uint64_t end;
            err = lk_file_write(f->h, f->off, wflags, buf, len, &n, &end);
            if (err == NO_ERROR) {
                f->off = end;
            }
            break;
        }
        default:
            errno = EISDIR;
            return -1;
    }
    return err < 0 ? lku_fail(err) : (_ssize_t)n;
}

_off_t _lseek(int fd, _off_t off, int whence) {
    struct fd *f = get_fd(fd);
    if (!f) {
        return -1;
    }
    if (f->kind != FD_FILE) {
        errno = f->kind == FD_CONSOLE ? ESPIPE : EINVAL;
        return -1;
    }
    int64_t base;
    switch (whence) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = (int64_t)f->off;
            break;
        case SEEK_END: {
            struct lk_file_stat st;
            lk_status_t err = lk_file_fstat(f->h, &st);
            if (err < 0) {
                return lku_fail(err);
            }
            base = (int64_t)st.size;
            break;
        }
        default:
            errno = EINVAL;
            return -1;
    }
    if ((off < 0 && base + off < 0) || (off > 0 && base > INT64_MAX - off)) {
        errno = EINVAL;
        return -1;
    }
    f->off = (uint64_t)(base + off);
    return (_off_t)f->off;
}

int _fstat(int fd, struct stat *st) {
    struct fd *f = get_fd(fd);
    if (!f) {
        return -1;
    }
    struct lk_file_stat fst = {.type = LK_FILE_TYPE_DIR};
    switch (f->kind) {
        case FD_CONSOLE:
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFCHR | 0620;
            st->st_nlink = 1;
            return 0;
        case FD_FILE: {
            lk_status_t err = lk_file_fstat(f->h, &fst);
            if (err < 0) {
                return lku_fail(err);
            }
            break;
        }
        default:
            break;
    }
    fill_stat(st, &fst);
    return 0;
}

int _isatty(int fd) {
    struct fd *f = get_fd(fd);
    if (!f) {
        return 0;
    }
    if (f->kind != FD_CONSOLE) {
        errno = ENOTTY;
        return 0;
    }
    return 1;
}

int _stat(const char *name, struct stat *st) {
    char buf[PATH_BUF];
    const char *path = lku_ns_path(name, buf, sizeof(buf));
    if (!path) {
        return -1;
    }
    struct lk_file_stat fst;
    lk_status_t err = lk_ns_stat(lku_namespace, path, &fst);
    if (err < 0) {
        return lku_fail(err);
    }
    fill_stat(st, &fst);
    return 0;
}

int _unlink(const char *name) {
    char buf[PATH_BUF];
    const char *path = lku_ns_path(name, buf, sizeof(buf));
    if (!path) {
        return -1;
    }
    lk_status_t err = lk_ns_unlink(lku_namespace, path);
    return err < 0 ? lku_fail(err) : 0;
}

int mkdir(const char *name, mode_t mode) {
    char buf[PATH_BUF];
    const char *path = lku_ns_path(name, buf, sizeof(buf));
    if (!path) {
        return -1;
    }
    lk_status_t err = lk_ns_mkdir(lku_namespace, path);
    return err < 0 ? lku_fail(err) : 0;
}

int rmdir(const char *name) {
    char buf[PATH_BUF];
    const char *path = lku_ns_path(name, buf, sizeof(buf));
    if (!path) {
        return -1;
    }
    lk_status_t err = lk_ns_rmdir(lku_namespace, path);
    // the file systems refuse a directory with entries left this way
    if (err == ERR_NOT_ALLOWED) {
        errno = ENOTEMPTY;
        return -1;
    }
    return err < 0 ? lku_fail(err) : 0;
}

int ftruncate(int fd, off_t length) {
    struct fd *f = get_fd(fd);
    if (!f) {
        return -1;
    }
    if (f->kind != FD_FILE || length < 0) {
        errno = EINVAL;
        return -1;
    }
    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        errno = EBADF;
        return -1;
    }
    lk_status_t err = lk_file_truncate(f->h, (uint64_t)length);
    return err < 0 ? lku_fail(err) : 0;
}

int truncate(const char *name, off_t length) {
    char buf[PATH_BUF];
    const char *path = lku_ns_path(name, buf, sizeof(buf));
    if (!path) {
        return -1;
    }
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    lk_handle_t h;
    lk_status_t err = lk_ns_open(lku_namespace, path, LK_NS_OPEN_WRITE, &h, NULL);
    if (err < 0) {
        return lku_fail(err);
    }
    err = lk_file_truncate(h, (uint64_t)length);
    lk_handle_close(h);
    return err < 0 ? lku_fail(err) : 0;
}
