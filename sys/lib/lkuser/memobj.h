/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <kernel/vm.h>
#include <lk/cpp.h>
#include <lk/list.h>
#include <lktl/ref_ptr.h>
#include <sys/types.h>

#include "object.h"

namespace lkuser {

// Memory a process can map and hand to others: one physically contiguous run of zeroed pages
// until the VM rework brings objects LK's regions know about. The kernel reaches it through
// the physmap. Each mapping holds a reference (proc's mapping list), so the pages go back to
// the pmm once no handle and no mapping remains.
class memobj : public object {
public:
    static constexpr obj_kind kKind = obj_kind::MEMOBJ;

    // size rounded up to whole pages; ERR_NO_MEMORY when no run that long is free
    static status_t create(size_t size, lk::ref_ptr<memobj> *out);

    size_t size() const { return size_; }
    paddr_t paddr() const { return paddr_; }
    // the kernel's view of byte off
    uint8_t *kva(size_t off) const;

private:
    friend class object;

    explicit memobj(size_t size) : object(kKind), size_(size) {}
    ~memobj();
    DISALLOW_COPY_ASSIGN_AND_MOVE(memobj);

    const size_t size_;
    paddr_t paddr_ = 0;
    list_node pages_ = LIST_INITIAL_VALUE(pages_);
};

} // namespace lkuser
