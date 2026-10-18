/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <kernel/vm.h>
#include <sys/types.h>

namespace lkuser {

// A user address. It has no dereference: a kernel-mode fault on a user address is fatal, so
// user memory is reached only through copy_from_user/copy_to_user, which check the mapping.
template <typename T>
class user_ptr {
public:
    constexpr user_ptr() = default;
    explicit constexpr user_ptr(vaddr_t addr) : addr_(addr) {}

    vaddr_t addr() const { return addr_; }

private:
    vaddr_t addr_ = 0;
};

// Whether [addr, addr + len) lies inside aspace without wrapping. A zero length always does.
bool user_range_valid(const vmm_aspace_t *aspace, vaddr_t addr, size_t len);

// Copies between kernel memory and the user mappings of aspace, which need not be the
// active one: each page is looked up with arch_mmu_query and copied through the physmap
// under the aspace lock, so no fault is ever taken. ERR_FAULT when a byte lies outside the
// aspace, is unmapped, or its page lacks USER plus READ (from) or WRITE (to); the bytes
// before it may have been copied. Takes the aspace lock, a mutex: call with interrupts
// enabled and without the lock held.
status_t copy_from_user(vmm_aspace_t *aspace, void *dst, vaddr_t src, size_t len);
status_t copy_to_user(vmm_aspace_t *aspace, vaddr_t dst, const void *src, size_t len);

template <typename T>
status_t copy_from_user(vmm_aspace_t *aspace, void *dst, user_ptr<T> src, size_t len) {
    return copy_from_user(aspace, dst, src.addr(), len);
}

template <typename T>
status_t copy_to_user(vmm_aspace_t *aspace, user_ptr<T> dst, const void *src, size_t len) {
    return copy_to_user(aspace, dst.addr(), src, len);
}

} // namespace lkuser
