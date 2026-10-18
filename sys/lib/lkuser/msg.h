/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <kernel/vm.h>
#include <sys/lk_msg.h>
#include <sys/types.h>

#include "copy.h"

namespace lkuser {

// One side of a call's message as the caller laid it out: up to LK_IOV_MAX ranges of user
// memory in one address space, read or written at any offset as if contiguous. Every copy
// goes through copy.cpp, so the aspace need not be the active one.
class msg_buffer {
public:
    // Copies n iovecs from user memory and checks that every range lies inside aspace:
    // ERR_INVALID_ARGS for more than LK_IOV_MAX or lengths that add up past SIZE_MAX,
    // ERR_FAULT for unreadable iovecs or a range outside the aspace. Empty ranges are dropped.
    status_t load(vmm_aspace_t *aspace, user_ptr<const lk_iov> iov, size_t n);

    size_t len() const { return len_; }

    // Bytes [off, off + len) of the message, clipped at its end: the number copied, or
    // ERR_FAULT for an unmapped page or one without the permission.
    ssize_t read(size_t off, void *buf, size_t len) const;
    ssize_t write(size_t off, const void *buf, size_t len) const;

private:
    struct range {
        vaddr_t base;
        size_t len;
    };

    template <typename F>
    ssize_t walk(size_t off, size_t len, F &&copy) const;

    vmm_aspace_t *aspace_ = nullptr;
    range ranges_[LK_IOV_MAX] = {};
    size_t n_ = 0;
    size_t len_ = 0;
};

// the start of a request, copied before anything reads it, header included
constexpr size_t kRecvPrefix = 256;

// The call a thread has in progress. Embedded in the calling thread, which owns it for its
// lifetime.
struct pending_call {
    msg_buffer req;
    msg_buffer reply;
    size_t reply_len = 0; // the furthest reply byte written, header included
    uint8_t prefix[kRecvPrefix];
};

} // namespace lkuser
