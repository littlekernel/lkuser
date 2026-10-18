/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The vm service: anonymous memory in the caller's address space, which is the active one
// while the handler runs.

#include <arch/mmu.h>
#include <kernel/vm.h>
#include <sys/svc/vm.h>

#include "../copy.h"
#include "../proc.h"
#include "svc.h"

namespace lkuser {

namespace {

constexpr uint32_t kProtAll = LK_VM_PROT_READ | LK_VM_PROT_WRITE | LK_VM_PROT_EXEC;

lk_status_t vm_alloc(kcall &c) {
    lk_vm_alloc_req req;
    lk_status_t err = c.args(&req);
    if (err < 0) {
        return err;
    }
    if (req.size == 0 || req.flags != 0 || (req.prot & ~kProtAll) ||
        !(req.prot & LK_VM_PROT_READ)) {
        return ERR_INVALID_ARGS;
    }
    // where the address would be lost, allocate nothing
    if (c.reply_capacity() < sizeof(lk_vm_alloc_reply)) {
        return ERR_NOT_ENOUGH_BUFFER;
    }
    vmm_aspace_t *aspace = c.caller()->aspace();
    // vmm_alloc takes the pages before it looks for room, so a size no address space can
    // hold is refused first
    if (req.size > aspace->size) {
        return ERR_NO_MEMORY;
    }

    uint mmu_flags = ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_READ;
    if (req.prot & LK_VM_PROT_WRITE) {
        mmu_flags |= ARCH_MMU_FLAG_PERM_WRITE;
    }
    if (req.prot & LK_VM_PROT_EXEC) {
        mmu_flags |= ARCH_MMU_FLAG_PERM_EXECUTE;
    }
    const size_t size = ROUNDUP(static_cast<size_t>(req.size), PAGE_SIZE);
    void *ptr = nullptr;
    err = vmm_alloc(aspace, "vm alloc", size, &ptr, PAGE_SIZE_SHIFT, 0, mmu_flags);
    if (err < 0) {
        return err;
    }
    const vaddr_t va = reinterpret_cast<vaddr_t>(ptr);
    err = zero_user_pages(aspace, va, size);
    if (err == NO_ERROR) {
        err = c.reply(lk_vm_alloc_reply{.va = va});
    }
    if (err < 0) {
        vmm_free_region(aspace, va);
    }
    return err;
}

lk_status_t vm_unmap(kcall &c) {
    lk_vm_unmap_req req;
    lk_status_t err = c.args(&req);
    if (err < 0) {
        return err;
    }
    if (req.va > UINTPTR_MAX) {
        return ERR_NOT_FOUND;
    }
    return vmm_free_region(c.caller()->aspace(), static_cast<vaddr_t>(req.va));
}

lk_status_t vm_handler(kcall &c) {
    switch (c.op()) {
        case LK_VM_ALLOC:
            return vm_alloc(c);
        case LK_VM_UNMAP:
            return vm_unmap(c);
        default:
            return ERR_NOT_SUPPORTED;
    }
}

} // namespace

} // namespace lkuser

LKUSER_SERVICE(vm, LK_TAG_VM, &lkuser::vm_handler);
