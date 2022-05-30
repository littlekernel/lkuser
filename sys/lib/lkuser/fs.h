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

#include "handle.h"

namespace lkuser {

// generic class for handles to things that act like files
class file_handle : public handle {
protected:
    file_handle() : handle(type::FILE) {}
public:
    virtual ~file_handle() = default;

    DISALLOW_COPY_ASSIGN_AND_MOVE(file_handle);

    // see if we can dynamically cast to a file handle from a plain handle
    static file_handle *dynamic_cast_from_handle(handle *h) {
        if (h->is_type(type::FILE)) {
            return static_cast<file_handle *>(h);
        }
        return nullptr;
    }

    virtual ssize_t read(void *buf, size_t len) = 0;
    virtual ssize_t read_at(void *buf, off_t offset, size_t len) = 0;
    virtual ssize_t write(const void *buf, size_t len) = 0;
    virtual ssize_t write_at(const void *buf, off_t offset, size_t len) = 0;
    virtual status_t close() = 0;
};

// opens a file of some type and hands the handle back
status_t open_file(const char *path, file_handle **handle);

} // namespace lkuser
