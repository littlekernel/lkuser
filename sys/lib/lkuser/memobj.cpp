/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "memobj.h"

#include <lk/err.h>
#include <string.h>

namespace lkuser {

status_t memobj::create(size_t size, lk::ref_ptr<memobj> *out) {
    if (size == 0) {
        return ERR_INVALID_ARGS;
    }
    size = ROUNDUP(size, PAGE_SIZE);

    lk::ref_ptr<memobj> m = object::make<memobj>(size);
    if (!m) {
        return ERR_NO_MEMORY;
    }
    const size_t count = size / PAGE_SIZE;
    if (pmm_alloc_contiguous(count, PAGE_SIZE_SHIFT, &m->paddr_, &m->pages_) != count) {
        return ERR_NO_MEMORY;
    }
    // the pmm hands pages out as their last user left them
    memset(m->kva(0), 0, size);

    *out = std::move(m);
    return NO_ERROR;
}

memobj::~memobj() {
    pmm_free(&pages_);
}

uint8_t *memobj::kva(size_t off) const {
    return static_cast<uint8_t *>(paddr_to_kvaddr(paddr_)) + off;
}

} // namespace lkuser
