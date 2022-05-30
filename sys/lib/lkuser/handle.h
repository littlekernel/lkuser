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
#include <lk/trace.h>
#include <kernel/mutex.h>

namespace lkuser {

// generic handle to things, goes in the per process handle table
class handle {
public:
    // stores a type field, for hand rolled RTTI
    enum class type {
        EMPTY,
        FILE,
    };

protected:
    explicit handle(type t) : type_(t) {}
    DISALLOW_COPY_ASSIGN_AND_MOVE(handle);
public:
    virtual ~handle() = default;

    // only really supports being closed, which does nothing
    virtual status_t close() { return NO_ERROR; }

    type get_type() const { return type_; }
    bool is_type(type t) const { return type_ == t; }

private:
    type type_;
};

// A static fixed size table, one per process.
// Allocates first fit, posix style.
template <size_t size>
class handle_table {
public:
    handle_table() = default;
    ~handle_table() {
        // all handles must be closed before destruction
        for (size_t i = 0; i < size; i++) {
            DEBUG_ASSERT(handles[i] == nullptr);
        }
    }
    DISALLOW_COPY_ASSIGN_AND_MOVE(handle_table);

    // searches for the first slot, sticking the handle in it
    int alloc_handle(handle *h) {
        DEBUG_ASSERT(h);

        AutoLock guard(lock_);
        for (size_t i = 0; i < size; i++) {
            if (!handles[i]) {
                handles[i] = h;
                return i;
            }
        }
        return ERR_NOT_FOUND;
    }

    int alloc_specific_slot(handle *h, int fd) {
        if (fd < 0) {
            return ERR_INVALID_ARGS;
        }
        if ((size_t)fd >= size) {
            return ERR_INVALID_ARGS;
        }

        DEBUG_ASSERT(h);

        AutoLock guard(lock_);
        handles[fd] = h;

        return fd;
    }

    // close the handle at the passed in slot, if opened.
    // zeros the slot
    status_t close_handle(int fd) {
        //TRACEF("fd %d\n", fd);
        if (fd < 0) {
            return ERR_INVALID_ARGS;
        }
        if ((size_t)fd >= size) {
            return ERR_INVALID_ARGS;
        }

        handle *h = nullptr;
        {
            AutoLock guard(lock_);
            h = handles[fd];
            if (!h) {
                return ERR_NOT_FOUND;
            }

            handles[fd] = nullptr;
        }

        DEBUG_ASSERT(h);
        auto status = h->close();

        // TODO: have some sort of ref counter on the handle
        delete h;
        return status;
    }

    // used during cleanup to empty the table
    void close_all() {
        //TRACEF("%p\n", this);
        for (size_t i = 0; i < size; i++) {
            close_handle(i);
        }
    }

    handle *get_handle(int fd) const {
        if (fd < 0) {
            return nullptr;
        }
        if ((size_t)fd >= size) {
            return nullptr;
        }

        AutoLock guard(lock_);
        return handles[fd];
    }

private:
    mutable Mutex lock_;
    handle *handles[size] = {};
};

} // namespace lkuser

