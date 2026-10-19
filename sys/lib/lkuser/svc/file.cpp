/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The file service: one kernel endpoint behind every open file and directory. Each is a
// connection to it, made by namespace.OPEN, whose cookie is its open_file record, freed with
// the connection. No process is born connected to it.

#include <kernel/mutex.h>
#include <lib/fs.h>
#include <lk/cpp.h>
#include <new>
#include <stdint.h>
#include <string.h>
#include <sys/svc/file.h>
#include <sys/svc/namespace.h>

#include "open_file.h"
#include "svc.h"

namespace lkuser {

namespace {

// The file system runs on the caller's kernel stack too, so data moves in pieces this big.
constexpr size_t kChunk = 512;

// off_t is signed
constexpr uint64_t kMaxOffset = INT64_MAX;

struct open_file {
    open_file(filehandle *f, uint32_t fl) : fh(f), flags(fl) {}
    open_file(dirhandle *d, const char *p) : dh(d) { strlcpy(path, p, sizeof(path)); }
    ~open_file() {
        if (fh) {
            fs_close_file(fh);
        }
        if (dh) {
            fs_close_dir(dh);
        }
    }
    DISALLOW_COPY_ASSIGN_AND_MOVE(open_file);

    bool is_dir() const { return !fh; }

    // a file
    filehandle *fh = nullptr;
    const uint32_t flags = 0; // LK_NS_OPEN_*

    // A directory. lib/fs lists one through a cursor that only moves forward, so READDIR keeps
    // the index the cursor is at and opens the directory again to go back.
    Mutex lock;
    dirhandle *dh = nullptr; // null after a failed reopen
    uint32_t next = 0;
    char path[FS_MAX_PATH_LEN] = {};
};

lk::ref_ptr<endpoint> file_ep;

open_file &record(kcall &c) {
    return *reinterpret_cast<open_file *>(static_cast<uintptr_t>(c.conn().cookie()));
}

lk_status_t file_read(kcall &c, open_file &f) {
    lk_file_read_req req;
    lk_status_t err = c.args(&req);
    if (err < 0) {
        return err;
    }
    if (f.is_dir()) {
        return ERR_NOT_FILE;
    }
    if (!(f.flags & LK_NS_OPEN_READ)) {
        return ERR_ACCESS_DENIED;
    }
    if (req.off > kMaxOffset) {
        return ERR_INVALID_ARGS;
    }

    const uint64_t len = MIN(MIN(uint64_t{req.len}, uint64_t{c.reply_capacity()}),
                             kMaxOffset - req.off);
    uint8_t buf[kChunk];
    for (size_t done = 0; done < len;) {
        const size_t chunk = MIN(static_cast<size_t>(len) - done, sizeof(buf));
        ssize_t n = fs_read_file(f.fh, buf, static_cast<off_t>(req.off + done), chunk);
        if (n < 0) {
            // what came before the error is the reply; the next read gets the error
            return done ? NO_ERROR : static_cast<lk_status_t>(n);
        }
        ssize_t w = c.write(done, buf, n);
        if (w < 0) {
            return static_cast<lk_status_t>(w);
        }
        done += n;
        if (static_cast<size_t>(n) < chunk) {
            break;
        }
    }
    return NO_ERROR;
}

lk_status_t file_write(kcall &c, open_file &f) {
    lk_file_write_req req;
    if (c.len() < sizeof(req)) {
        return ERR_INVALID_ARGS;
    }
    ssize_t n = c.read(0, &req, sizeof(req));
    if (n < 0) {
        return static_cast<lk_status_t>(n);
    }
    if (f.is_dir()) {
        return ERR_NOT_FILE;
    }
    if (!(f.flags & LK_NS_OPEN_WRITE)) {
        return ERR_ACCESS_DENIED;
    }
    if (req.flags & ~LK_FILE_WRITE_APPEND) {
        return ERR_INVALID_ARGS;
    }
    // where the count would be lost, write nothing
    if (c.reply_capacity() < sizeof(lk_file_write_reply)) {
        return ERR_NOT_ENOUGH_BUFFER;
    }

    uint64_t off = req.off;
    if (req.flags & LK_FILE_WRITE_APPEND) {
        file_stat st;
        lk_status_t err = fs_stat_file(f.fh, &st);
        if (err < 0) {
            return err;
        }
        off = st.size;
    }
    const size_t len = c.len() - sizeof(req);
    if (off > kMaxOffset || len > kMaxOffset - off) {
        return ERR_INVALID_ARGS;
    }

    uint8_t buf[kChunk];
    size_t done = 0;
    while (done < len) {
        const size_t chunk = MIN(len - done, sizeof(buf));
        n = c.read(sizeof(req) + done, buf, chunk);
        if (n < 0) {
            return static_cast<lk_status_t>(n);
        }
        ssize_t w = fs_write_file(f.fh, buf, static_cast<off_t>(off + done), n);
        if (w < 0) {
            // as for a read: what was written is the reply, the next write gets the error
            if (done == 0) {
                return static_cast<lk_status_t>(w);
            }
            break;
        }
        done += w;
        if (w < n) {
            break;
        }
    }
    // a request payload's length fits 32 bits
    return c.reply(lk_file_write_reply{
        .end = off + done, .written = static_cast<uint32_t>(done), .reserved = 0});
}

lk_status_t file_stat_op(kcall &c, open_file &f) {
    if (c.len() != 0) {
        return ERR_INVALID_ARGS;
    }
    lk_file_stat st = {.size = 0, .type = LK_FILE_TYPE_DIR, .reserved = 0};
    if (!f.is_dir()) {
        file_stat fst;
        lk_status_t err = fs_stat_file(f.fh, &fst);
        if (err < 0) {
            return err;
        }
        st.size = fst.size;
        st.type = LK_FILE_TYPE_FILE;
    }
    return c.reply(st);
}

lk_status_t file_truncate(kcall &c, open_file &f) {
    lk_file_truncate_req req;
    lk_status_t err = c.args(&req);
    if (err < 0) {
        return err;
    }
    if (f.is_dir()) {
        return ERR_NOT_FILE;
    }
    if (!(f.flags & LK_NS_OPEN_WRITE)) {
        return ERR_ACCESS_DENIED;
    }
    if (req.size > kMaxOffset) {
        return ERR_INVALID_ARGS;
    }
    return fs_truncate_file(f.fh, req.size);
}

lk_status_t file_readdir(kcall &c, open_file &f) {
    lk_file_readdir_req req;
    lk_status_t err = c.args(&req);
    if (err < 0) {
        return err;
    }
    if (!f.is_dir()) {
        return ERR_NOT_DIR;
    }

    struct dirent ent;
    {
        AutoLock guard(f.lock);
        if (!f.dh || req.index < f.next) {
            if (f.dh) {
                fs_close_dir(f.dh);
                f.dh = nullptr;
            }
            err = fs_open_dir(f.path, &f.dh);
            if (err < 0) {
                f.dh = nullptr;
                return err;
            }
            f.next = 0;
        }
        do {
            // ERR_NOT_FOUND past the last entry
            err = fs_read_dir(f.dh, &ent);
            if (err < 0) {
                return err;
            }
        } while (f.next++ != req.index);
    }

    // the entry is used up either way; asking again for the same index goes back for it
    const size_t name_len = strnlen(ent.name, sizeof(ent.name));
    if (c.reply_capacity() < sizeof(lk_file_dirent) + name_len) {
        return ERR_NOT_ENOUGH_BUFFER;
    }
    const lk_file_dirent hdr = {.name_len = static_cast<uint32_t>(name_len), .reserved = 0};
    ssize_t n = c.write(0, &hdr, sizeof(hdr));
    if (n >= 0) {
        n = c.write(sizeof(hdr), ent.name, name_len);
    }
    return n < 0 ? static_cast<lk_status_t>(n) : NO_ERROR;
}

lk_status_t file_handler(kcall &c) {
    open_file &f = record(c);
    switch (c.op()) {
        case LK_FILE_READ:
            return file_read(c, f);
        case LK_FILE_WRITE:
            return file_write(c, f);
        case LK_FILE_STAT:
            return file_stat_op(c, f);
        case LK_FILE_TRUNCATE:
            return file_truncate(c, f);
        case LK_FILE_READDIR:
            return file_readdir(c, f);
        default:
            return ERR_NOT_SUPPORTED;
    }
}

void file_release(uint64_t cookie) {
    delete reinterpret_cast<open_file *>(static_cast<uintptr_t>(cookie));
}

lk_status_t connect_record(open_file *f, lk::ref_ptr<connection> *out) {
    // registration at boot can fail for want of memory
    if (!file_ep) {
        delete f;
        return ERR_NOT_SUPPORTED;
    }
    lk::ref_ptr<connection> conn = object::make<connection>(file_ep, reinterpret_cast<uintptr_t>(f));
    if (!conn) {
        delete f;
        return ERR_NO_MEMORY;
    }
    *out = std::move(conn);
    return NO_ERROR;
}

void file_init(uint level) {
    file_ep = register_service(kNoTag, "file", &file_handler, &file_release);
}

} // namespace

lk_status_t make_file_connection(filehandle *fh, uint32_t flags, lk::ref_ptr<connection> *out) {
    auto *f = new (std::nothrow) open_file(fh, flags);
    if (!f) {
        fs_close_file(fh);
        return ERR_NO_MEMORY;
    }
    return connect_record(f, out);
}

lk_status_t make_dir_connection(dirhandle *dh, const char *path, lk::ref_ptr<connection> *out) {
    auto *f = new (std::nothrow) open_file(dh, path);
    if (!f) {
        fs_close_dir(dh);
        return ERR_NO_MEMORY;
    }
    return connect_record(f, out);
}

} // namespace lkuser

LK_INIT_HOOK(lkuser_service_file, lkuser::file_init, LK_INIT_LEVEL_THREADING);
