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
        obj->handle_added(rights);
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

status_t handle_table::get(lk_handle_t h, uint32_t need, lk::ref_ptr<object> *out,
                           uint32_t *rights) {
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
        if (rights) {
            *rights = s->rights;
        }
    }
    // whatever *out held is released here, outside the lock
    *out = std::move(obj);
    return NO_ERROR;
}

status_t handle_table::close(lk_handle_t h) {
    return close_if(h, nullptr);
}

status_t handle_table::close_if(lk_handle_t h, const object *o) {
    lk::ref_ptr<object> obj;
    uint32_t rights;
    {
        AutoLock guard(lock_);
        slot *s = find_locked(h);
        if (!s || (o && s->obj.get() != o)) {
            return ERR_BAD_HANDLE;
        }
        obj = std::move(s->obj);
        rights = s->rights;
        s->rights = 0;
    }
    obj->handle_removed(rights);
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
    const uint32_t rights = (s->rights & mask & LK_RIGHTS_ALL) | (s->rights & kRightPeerView);
    s->obj->handle_added(rights);
    slots_[nh - 1] = {s->obj, rights};
    *out = nh;
    return NO_ERROR;
}

bool handle_table::has_free_slot() {
    AutoLock guard(lock_);
    return !closed_ && free_handle_locked() != LK_HANDLE_INVALID;
}

void handle_table::close_all() {
    {
        AutoLock guard(lock_);
        closed_ = true;
    }
    // one slot at a time, so no reference is dropped under the lock
    for (size_t i = 0; i < LKUSER_HANDLES; i++) {
        lk::ref_ptr<object> obj;
        uint32_t rights;
        {
            AutoLock guard(lock_);
            obj = std::move(slots_[i].obj);
            rights = slots_[i].rights;
            slots_[i].rights = 0;
        }
        if (obj) {
            obj->handle_removed(rights);
        }
    }
}

void handle_table::dump_locked() const {
    for (size_t i = 0; i < LKUSER_HANDLES; i++) {
        const slot &s = slots_[i];
        if (s.obj) {
            printf("  %3zu: rights %#06x%s ", i + 1, s.rights & LK_RIGHTS_ALL,
                   (s.rights & kRightPeerView) ? " peer" : "");
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
