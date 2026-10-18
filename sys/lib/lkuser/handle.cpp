/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "handle.h"

#include <stdio.h>

#include "proc.h"
#include "syscalls.h"
#include "thread.h"

namespace lkuser {

handle_table::slot *handle_table::find_locked(lk_handle_t h) {
    if (h == LK_HANDLE_INVALID || h > LKUSER_HANDLES) {
        return nullptr;
    }
    slot *s = &slots_[h - 1];
    return s->obj ? s : nullptr;
}

lk_handle_t handle_table::free_handle_locked() const {
    for (size_t i = 0; i < LKUSER_HANDLES; i++) {
        if (!slots_[i].obj) {
            return static_cast<lk_handle_t>(i + 1);
        }
    }
    return LK_HANDLE_INVALID;
}

status_t handle_table::add(lk::ref_ptr<object> obj, uint32_t rights, lk_handle_t *out) {
    {
        AutoLock guard(lock_);
        if (closed_) {
            return ERR_BAD_STATE;
        }
        const lk_handle_t h = free_handle_locked();
        if (h == LK_HANDLE_INVALID) {
            return ERR_NO_RESOURCES;
        }
        obj->handle_added();
        slots_[h - 1] = {std::move(obj), rights};
        *out = h;
    }
    return NO_ERROR;
}

status_t handle_table::add_for_user(vmm_aspace_t *aspace, lk::ref_ptr<object> obj,
                                    uint32_t rights, user_ptr<lk_handle_t> out) {
    lk_handle_t h;
    status_t err = add(std::move(obj), rights, &h);
    if (err < 0) {
        return err;
    }
    err = copy_to_user(aspace, out, &h, sizeof(h));
    if (err < 0) {
        close(h);
        return err;
    }
    return NO_ERROR;
}

status_t handle_table::get(lk_handle_t h, uint32_t need, lk::ref_ptr<object> *out) {
    lk::ref_ptr<object> obj;
    {
        AutoLock guard(lock_);
        slot *s = find_locked(h);
        if (!s) {
            return ERR_BAD_HANDLE;
        }
        if ((s->rights & need) != need) {
            return ERR_ACCESS_DENIED;
        }
        obj = s->obj;
    }
    // whatever *out held is released here, outside the lock
    *out = std::move(obj);
    return NO_ERROR;
}

status_t handle_table::close(lk_handle_t h) {
    lk::ref_ptr<object> obj;
    {
        AutoLock guard(lock_);
        slot *s = find_locked(h);
        if (!s) {
            return ERR_BAD_HANDLE;
        }
        obj = std::move(s->obj);
        s->rights = 0;
    }
    obj->handle_removed();
    // obj's reference goes here, outside the lock
    return NO_ERROR;
}

status_t handle_table::dup(lk_handle_t h, uint32_t mask, lk_handle_t *out) {
    AutoLock guard(lock_);
    slot *s = find_locked(h);
    if (!s) {
        return ERR_BAD_HANDLE;
    }
    if (!(s->rights & LK_RIGHT_DUP)) {
        return ERR_ACCESS_DENIED;
    }
    if (closed_) {
        return ERR_BAD_STATE;
    }
    const lk_handle_t nh = free_handle_locked();
    if (nh == LK_HANDLE_INVALID) {
        return ERR_NO_RESOURCES;
    }
    s->obj->handle_added();
    slots_[nh - 1] = {s->obj, s->rights & mask};
    *out = nh;
    return NO_ERROR;
}

void handle_table::close_all() {
    {
        AutoLock guard(lock_);
        closed_ = true;
    }
    // one slot at a time, so no reference is dropped under the lock
    for (size_t i = 0; i < LKUSER_HANDLES; i++) {
        lk::ref_ptr<object> obj;
        {
            AutoLock guard(lock_);
            obj = std::move(slots_[i].obj);
            slots_[i].rights = 0;
        }
        if (obj) {
            obj->handle_removed();
        }
    }
}

void handle_table::dump_locked() const {
    for (size_t i = 0; i < LKUSER_HANDLES; i++) {
        const slot &s = slots_[i];
        if (s.obj) {
            printf("  %3zu: rights %#06x ", i + 1, s.rights);
            s.obj->dump();
        }
    }
}

void handle_table::dump(bool lock) {
    if (lock) {
        AutoLock guard(lock_);
        dump_locked();
    } else {
        dump_locked();
    }
}

// Closing is always allowed: no right is needed to give a handle up.
status_t sys_handle_close(lk_handle_t h) {
    return thread::current()->process()->handles().close(h);
}

status_t sys_handle_dup(lk_handle_t h, uint32_t rights_mask, user_ptr<lk_handle_t> out) {
    proc *p = thread::current()->process();
    lk_handle_t nh;
    status_t err = p->handles().dup(h, rights_mask, &nh);
    if (err < 0) {
        return err;
    }
    err = copy_to_user(p->aspace(), out, &nh, sizeof(nh));
    if (err < 0) {
        p->handles().close(nh);
        return err;
    }
    return NO_ERROR;
}

} // namespace lkuser
