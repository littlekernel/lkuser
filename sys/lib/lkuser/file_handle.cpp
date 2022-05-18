/*
 * Copyright (c) 2022 Travis Geiselbrecht
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
#include "file_handle.h"

#include <lk/err.h>
#include <lk/trace.h>

#define LOCAL_TRACE 0

namespace lkuser {

// overridden version for files that are implemented in LK's lib/fs layer
class file_handle_fs final : public file_handle {
public:
    explicit file_handle_fs(filehandle *lk_handle) : file_handle(), lk_fhandle_(lk_handle) {};
    virtual ~file_handle_fs();

    // status_t fs_create_file(const char *path, filehandle **handle, uint64_t len) __NONNULL();
    // status_t fs_open_file(const char *path, filehandle **handle) __NONNULL();
    // status_t fs_remove_file(const char *path) __NONNULL();
    // ssize_t fs_write_file(filehandle *handle, const void *buf, off_t offset, size_t len) __NONNULL();
    // status_t fs_close_file(filehandle *handle) __NONNULL();
    // status_t fs_stat_file(filehandle *handle, struct file_stat *) __NONNULL((1));
    // status_t fs_truncate_file(filehandle *handle, uint64_t len) __NONNULL((1));

    ssize_t read(void *buf, size_t len) override;
    ssize_t read_at(void *buf, off_t offset, size_t len) override;
    status_t close() override;

    bool is_open() const override { return lk_fhandle_ != nullptr; }

private:
    Mutex lock_;
    filehandle *lk_fhandle_ = nullptr;
    off_t pos_ {};
};

file_handle_fs::~file_handle_fs() {
    LTRACEF("%p\n", this);
    close();
}

status_t file_handle_fs::close() {
    LTRACEF("%p\n", this);

    AutoLock guard(lock_);

    if (!is_open()) {
        return ERR_BAD_STATE;
    }

    status_t err = fs_close_file(lk_fhandle_);
    lk_fhandle_ = nullptr;
    return err;
}

ssize_t file_handle_fs::read(void *buf, size_t len) {

    AutoLock guard(lock_);

    LTRACEF("len %zu, cursor is %lld\n", len, pos_);

    ssize_t ret = fs_read_file(lk_fhandle_, buf, pos_, len);
    if (ret >= 0) {
        pos_ += ret;
    }
    LTRACEF("fs_read_file returns %zd, cursor is now %llu\n", ret, pos_);

    return ret;
}

ssize_t file_handle_fs::read_at(void *buf, off_t offset, size_t len) {
    LTRACEF("offset %lld, len %zu\n", offset, len);

    AutoLock guard(lock_);

    ssize_t ret = fs_read_file(lk_fhandle_, buf, offset, len);
    LTRACEF("fs_read_file returns %zd\n", ret);

    return ret;
}

// file handle factory
status_t open_file(const char *path, file_handle **out_handle) {
    LTRACEF("path '%s'\n", path);

    // TODO: look for some special cases

    // did it open?
    filehandle *lk_handle {};
    status_t err = fs_open_file(path, &lk_handle);
    if (err < 0) {
        return err;
    }

    // it did, create a file_handle_fs
    auto *handle = new file_handle_fs(lk_handle);
    if (!handle) {
        fs_close_file(lk_handle);
        return ERR_NO_MEMORY;
    }

    // return the handle
    *out_handle = handle;

    return NO_ERROR;
}

} // namespace lkuser
