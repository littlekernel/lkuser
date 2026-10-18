/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "msg.h"

#include <lk/err.h>
#include <stdint.h>
#include <sys/types.h>

namespace lkuser {

status_t msg_buffer::load(vmm_aspace_t *aspace, user_ptr<const lk_iov> iov, size_t n) {
    aspace_ = aspace;
    n_ = 0;
    len_ = 0;
    if (n > LK_IOV_MAX) {
        return ERR_INVALID_ARGS;
    }

    lk_iov user[LK_IOV_MAX];
    status_t err = copy_from_user(aspace, user, iov, n * sizeof(lk_iov));
    if (err < 0) {
        return err;
    }

    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        const vaddr_t base = reinterpret_cast<vaddr_t>(user[i].base);
        const size_t len = user[i].len;
        if (!user_range_valid(aspace, base, len)) {
            return ERR_FAULT;
        }
        // only reachable on 32-bit, where a few ranges can cover more than SIZE_MAX
        if (len > SIZE_MAX - total) {
            return ERR_INVALID_ARGS;
        }
        total += len;
        if (len > 0) {
            ranges_[n_++] = {base, len};
        }
    }
    // the copies report counts as ssize_t, whose largest value this is
    if (total > SIZE_MAX / 2) {
        return ERR_INVALID_ARGS;
    }
    len_ = total;
    return NO_ERROR;
}

// Calls copy(user address, offset into the caller's buffer, length) for each piece of
// [off, off + len) clipped at the message's end, in order.
template <typename F>
ssize_t msg_buffer::walk(size_t off, size_t len, F &&copy) const {
    if (off >= len_) {
        return 0;
    }
    len = MIN(len, len_ - off);

    size_t done = 0;
    for (size_t i = 0; i < n_ && done < len; i++) {
        const range &r = ranges_[i];
        if (off >= r.len) {
            off -= r.len;
            continue;
        }
        const size_t chunk = MIN(r.len - off, len - done);
        status_t err = copy(r.base + off, done, chunk);
        if (err < 0) {
            return err;
        }
        done += chunk;
        off = 0;
    }
    return static_cast<ssize_t>(done);
}

ssize_t msg_buffer::read(size_t off, void *buf, size_t len) const {
    uint8_t *dst = static_cast<uint8_t *>(buf);
    return walk(off, len, [this, dst](vaddr_t uaddr, size_t pos, size_t n) {
        return copy_from_user(aspace_, dst + pos, uaddr, n);
    });
}

ssize_t msg_buffer::write(size_t off, const void *buf, size_t len) const {
    const uint8_t *src = static_cast<const uint8_t *>(buf);
    return walk(off, len, [this, src](vaddr_t uaddr, size_t pos, size_t n) {
        return copy_to_user(aspace_, uaddr, src + pos, n);
    });
}

} // namespace lkuser
