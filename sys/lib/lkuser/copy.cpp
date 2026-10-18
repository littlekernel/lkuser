/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "copy.h"

#include <arch/mmu.h>
#include <assert.h>
#include <lk/err.h>
#include <stdlib.h>
#include <string.h>

namespace lkuser {

namespace {

enum class direction { FROM_USER, TO_USER, ZERO };

status_t copy_user(vmm_aspace_t *aspace, vaddr_t uaddr, uint8_t *kbuf, size_t len, direction dir) {
    DEBUG_ASSERT(!(aspace->flags & VMM_ASPACE_FLAG_KERNEL));

    if (!user_range_valid(aspace, uaddr, len)) {
        return ERR_FAULT;
    }

    uint need = ARCH_MMU_FLAG_PERM_USER;
    if (dir == direction::TO_USER) {
        need |= ARCH_MMU_FLAG_PERM_WRITE;
    } else if (dir == direction::FROM_USER) {
        need |= ARCH_MMU_FLAG_PERM_READ;
    }
    while (len > 0) {
        // stay within one page: the next one may map anywhere, or nowhere
        const size_t chunk = MIN(len, PAGE_SIZE - (uaddr & (PAGE_SIZE - 1)));

        // the lock keeps the page mapped, and so not freed, while it is copied
        vmm_aspace_lock(aspace);
        paddr_t pa;
        uint flags;
        uint8_t *kva = nullptr;
        if (arch_mmu_query(&aspace->arch_aspace, uaddr, &pa, &flags) == NO_ERROR &&
            (flags & need) == need) {
            kva = static_cast<uint8_t *>(paddr_to_kvaddr(pa));
        }
        if (kva) {
            if (dir == direction::TO_USER) {
                memcpy(kva, kbuf, chunk);
            } else if (dir == direction::FROM_USER) {
                memcpy(kbuf, kva, chunk);
            } else {
                memset(kva, 0, chunk);
            }
        }
        vmm_aspace_unlock(aspace);

        if (!kva) {
            return ERR_FAULT;
        }
        uaddr += chunk;
        if (kbuf) {
            kbuf += chunk;
        }
        len -= chunk;
    }
    return NO_ERROR;
}

} // namespace

bool user_range_valid(const vmm_aspace_t *aspace, vaddr_t addr, size_t len) {
    if (len == 0) {
        return true;
    }
    // written so that nothing wraps: addr - base is only formed once addr >= base
    return addr >= aspace->base && len <= aspace->size && addr - aspace->base <= aspace->size - len;
}

status_t copy_from_user(vmm_aspace_t *aspace, void *dst, vaddr_t src, size_t len) {
    return copy_user(aspace, src, static_cast<uint8_t *>(dst), len, direction::FROM_USER);
}

status_t copy_to_user(vmm_aspace_t *aspace, vaddr_t dst, const void *src, size_t len) {
    // copy_user only reads kbuf in this direction
    return copy_user(aspace, dst, static_cast<uint8_t *>(const_cast<void *>(src)), len,
                     direction::TO_USER);
}

status_t zero_user_pages(vmm_aspace_t *aspace, vaddr_t addr, size_t len) {
    return copy_user(aspace, addr, nullptr, len, direction::ZERO);
}

} // namespace lkuser
