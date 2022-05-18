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
#pragma once

#include <lk/cpp.h>
#include <lk/err.h>
#include <kernel/mutex.h>
#include <lib/fs.h>

namespace lkuser {

// generic class for file handles
class file_handle {
protected:
    file_handle() = default;
public:
    virtual ~file_handle() = default;

    DISALLOW_COPY_ASSIGN_AND_MOVE(file_handle);

    // status_t fs_create_file(const char *path, filehandle **handle, uint64_t len) __NONNULL();
    // status_t fs_open_file(const char *path, filehandle **handle) __NONNULL();
    // status_t fs_remove_file(const char *path) __NONNULL();
    // ssize_t fs_read_file(filehandle *handle, void *buf, off_t offset, size_t len) __NONNULL();
    // ssize_t fs_write_file(filehandle *handle, const void *buf, off_t offset, size_t len) __NONNULL();
    // status_t fs_close_file(filehandle *handle) __NONNULL();
    // status_t fs_stat_file(filehandle *handle, struct file_stat *) __NONNULL((1));
    // status_t fs_truncate_file(filehandle *handle, uint64_t len) __NONNULL((1));
    virtual ssize_t read(void *buf, size_t len) = 0;
    virtual ssize_t read_at(void *buf, off_t offset, size_t len) = 0;
    virtual status_t close() = 0;

    virtual bool is_open() const = 0;
};

class file_handle_null final : public file_handle {
public:
    explicit file_handle_null() : file_handle() {}
    virtual ~file_handle_null() {}

    ssize_t read(void *buf, size_t len) override { return ERR_NOT_SUPPORTED; }
    ssize_t read_at(void *buf, off_t offset, size_t len) override { return ERR_NOT_SUPPORTED; };
    status_t close() override { return ERR_NOT_SUPPORTED; }

    bool is_open() const override { return true; }
};

template <size_t size>
struct file_table {
    mutable Mutex lock_;
    file_handle *handles[size] = {};

    // searches for the first slot, sticking the handle in it
    int alloc_handle(file_handle *handle) {
        DEBUG_ASSERT(handle);

        AutoLock guard(lock_);
        for (size_t i = 0; i < size; i++) {
            if (!handles[i]) {
                handles[i] = handle;
                return i;
            }
        }
        return ERR_NOT_FOUND;
    }
    int alloc_specific_handle(file_handle *handle, int fd) {
        if (fd < 0) {
            return ERR_INVALID_ARGS;
        }
        if ((size_t)fd >= size) {
            return ERR_INVALID_ARGS;
        }

        DEBUG_ASSERT(handle);

        AutoLock guard(lock_);
        handles[fd] = handle;

        return fd;
    }

    // close the handle at the passed in slot, if opened.
    // zeros the slot
    status_t close_handle(int fd) {
        if (fd < 0) {
            return ERR_INVALID_ARGS;
        }
        if ((size_t)fd >= size) {
            return ERR_INVALID_ARGS;
        }


        file_handle *handle {};
        {
            AutoLock guard(lock_);
            handle = handles[fd];
            if (!handle) {
                return ERR_NOT_FOUND;
            }

            handles[fd] = nullptr;
        }

        DEBUG_ASSERT(handle);
        return handle->close();
    }

    file_handle *get_handle(int fd) const {
        if (fd < 0) {
            return nullptr;
        }
        if ((size_t)fd >= size) {
            return nullptr;
        }

        AutoLock guard(lock_);
        return handles[fd];
    }
};

// opens a file of some type and hands the handle back
status_t open_file(const char *path, file_handle **handle);

} // namespace lkuser

