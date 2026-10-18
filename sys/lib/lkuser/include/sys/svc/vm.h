/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// The vm protocol (LK_TAG_VM): the caller's own address space.

#include <sys/lk_msg.h>

// request: struct lk_vm_alloc_req; reply: struct lk_vm_alloc_reply. New memory reads as zero.
#define LK_VM_ALLOC 1u
// request: struct lk_vm_unmap_req, any address inside the region to remove; reply: no payload
#define LK_VM_UNMAP 2u

// protections; every mapping is readable, so READ is required
#define LK_VM_PROT_READ  (1u << 0)
#define LK_VM_PROT_WRITE (1u << 1)
#define LK_VM_PROT_EXEC  (1u << 2)

struct lk_vm_alloc_req {
    uint64_t size;  // rounded up to whole pages
    uint32_t prot;  // LK_VM_PROT_*
    uint32_t flags; // none defined: 0
};
LK_ABI_ASSERT(sizeof(struct lk_vm_alloc_req) == 16);
LK_ABI_ASSERT(offsetof(struct lk_vm_alloc_req, size) == 0);
LK_ABI_ASSERT(offsetof(struct lk_vm_alloc_req, prot) == 8);
LK_ABI_ASSERT(offsetof(struct lk_vm_alloc_req, flags) == 12);

struct lk_vm_alloc_reply {
    uint64_t va;
};
LK_ABI_ASSERT(sizeof(struct lk_vm_alloc_reply) == 8);
LK_ABI_ASSERT(offsetof(struct lk_vm_alloc_reply, va) == 0);

struct lk_vm_unmap_req {
    uint64_t va;
};
LK_ABI_ASSERT(sizeof(struct lk_vm_unmap_req) == 8);
LK_ABI_ASSERT(offsetof(struct lk_vm_unmap_req, va) == 0);
