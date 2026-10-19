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
#include "../memobj.h"
#include "../proc.h"
#include "svc.h"

namespace lkuser {

namespace {

constexpr uint32_t kProtAll = LK_VM_PROT_READ | LK_VM_PROT_WRITE | LK_VM_PROT_EXEC;

constexpr uint32_t kMemobjRights =
    LK_RIGHT_MAP | LK_RIGHT_READ | LK_RIGHT_WRITE | LK_RIGHT_DUP | LK_RIGHT_TRANSFER;

uint mmu_flags_for(uint32_t prot) {
    uint flags = ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_READ;
    if (prot & LK_VM_PROT_WRITE) {
        flags |= ARCH_MMU_FLAG_PERM_WRITE;
    }
    if (prot & LK_VM_PROT_EXEC) {
        flags |= ARCH_MMU_FLAG_PERM_EXECUTE;
    }
    return flags;
}

lk_status_t vm_alloc(kcall &c) {
    lk_vm_alloc_req req;
    lk_status_t err = c.args(&req);
    if (err == NO_ERROR) {
        err = c.expect_handles(0);
    }
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

    const uint mmu_flags = mmu_flags_for(req.prot);
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
    if (err == NO_ERROR) {
        err = c.expect_handles(0);
    }
    if (err < 0) {
        return err;
    }
    if (req.va > UINTPTR_MAX) {
        return ERR_NOT_FOUND;
    }
    return c.caller()->unmap(static_cast<vaddr_t>(req.va));
}

lk_status_t vm_memobj_create(kcall &c) {
    lk_vm_memobj_create_req req;
    lk_status_t err = c.args(&req);
    if (err == NO_ERROR) {
        err = c.expect_handles(0);
    }
    if (err < 0) {
        return err;
    }
    if (req.size == 0 || (req.flags & ~LK_VM_MEMOBJ_EXEC) || req.reserved != 0) {
        return ERR_INVALID_ARGS;
    }
    if (req.size > LK_VM_MEMOBJ_MAX) {
        return ERR_TOO_BIG;
    }
    if (c.reply_capacity() < sizeof(lk_vm_memobj_create_reply) || c.reply_handles_capacity() < 1) {
        return ERR_NOT_ENOUGH_BUFFER;
    }

    lk::ref_ptr<memobj> m;
    err = memobj::create(static_cast<size_t>(req.size), &m);
    if (err < 0) {
        return err;
    }
    const uint64_t size = m->size();
    const uint32_t rights = kMemobjRights | ((req.flags & LK_VM_MEMOBJ_EXEC) ? LK_RIGHT_EXEC : 0);
    err = c.reply_handle(std::move(m), rights);
    if (err < 0) {
        return err;
    }
    return c.reply(lk_vm_memobj_create_reply{.size = size});
}

// The rights a mapping with prot needs on the memory object's handle.
uint32_t rights_for(uint32_t prot) {
    uint32_t need = LK_RIGHT_MAP | LK_RIGHT_READ;
    if (prot & LK_VM_PROT_WRITE) {
        need |= LK_RIGHT_WRITE;
    }
    if (prot & LK_VM_PROT_EXEC) {
        need |= LK_RIGHT_EXEC;
    }
    return need;
}

lk_status_t vm_memobj_map(kcall &c) {
    lk_vm_memobj_map_req req;
    lk_status_t err = c.args(&req);
    if (err == NO_ERROR) {
        err = c.expect_handles(1);
    }
    if (err < 0) {
        return err;
    }
    if (req.flags != 0 || (req.prot & ~kProtAll) || !(req.prot & LK_VM_PROT_READ)) {
        return ERR_INVALID_ARGS;
    }
    lk::ref_ptr<memobj> m;
    err = c.handle_as(0, rights_for(req.prot), &m);
    if (err < 0) {
        return err;
    }
    const uint64_t size = m->size();
    const uint64_t len = req.len ? req.len : size - MIN(req.off, size);
    if ((req.off | req.len) & (PAGE_SIZE - 1) || req.off >= size || len > size - req.off) {
        return ERR_INVALID_ARGS;
    }
    // where the address would be lost, map nothing
    if (c.reply_capacity() < sizeof(lk_vm_alloc_reply)) {
        return ERR_NOT_ENOUGH_BUFFER;
    }

    proc *p = c.caller();
    vaddr_t va;
    err = p->map(std::move(m), static_cast<size_t>(req.off), static_cast<size_t>(len),
                 mmu_flags_for(req.prot), &va);
    if (err < 0) {
        return err;
    }
    err = c.reply(lk_vm_alloc_reply{.va = va});
    if (err < 0) {
        p->unmap(va);
    }
    return err;
}

lk_status_t vm_handler(kcall &c) {
    switch (c.op()) {
        case LK_VM_ALLOC:
            return vm_alloc(c);
        case LK_VM_UNMAP:
            return vm_unmap(c);
        case LK_VM_MEMOBJ_CREATE:
            return vm_memobj_create(c);
        case LK_VM_MEMOBJ_MAP:
            return vm_memobj_map(c);
        default:
            return ERR_NOT_SUPPORTED;
    }
}

} // namespace

} // namespace lkuser

LKUSER_SERVICE(vm, LK_TAG_VM, &lkuser::vm_handler);
