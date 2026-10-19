/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <kernel/mutex.h>
#include <kernel/vm.h>
#include <lk/cpp.h>
#include <lk/err.h>
#include <lktl/ref_ptr.h>
#include <sys/lk_abi.h>

#include "copy.h"
#include "object.h"

// LKUSER_HANDLES, the slots of a handle table, comes from rules.mk
static_assert(LKUSER_HANDLES >= 8 && LKUSER_HANDLES <= 1024, "a usable handle table");

namespace lkuser {

// Not a right: marks a connection handle as the server's peer view, which no attenuation
// removes, so that the client views can be counted (the last one closing is CONN_CLOSED).
// handle_dup and every transfer keep it; user space never sees it.
constexpr uint32_t kRightPeerView = 1u << 31;

// The rights a handle gets from the call that creates its object, the only source of full
// rights.
constexpr uint32_t kEndpointRights =
    LK_RIGHT_RECV | LK_RIGHT_CONNECT | LK_RIGHT_WAIT | LK_RIGHT_DUP | LK_RIGHT_TRANSFER;
constexpr uint32_t kConnectionRights =
    LK_RIGHT_CALL | LK_RIGHT_IMPULSE | LK_RIGHT_WAIT | LK_RIGHT_DUP | LK_RIGHT_TRANSFER;
constexpr uint32_t kPeerRights = LK_RIGHT_SIGNAL | LK_RIGHT_DUP | LK_RIGHT_TRANSFER | kRightPeerView;
constexpr uint32_t kGroupRights = LK_RIGHT_RECV | LK_RIGHT_DUP | LK_RIGHT_TRANSFER;

// A process's handles: slots of {object, rights}. Handle h is slot h - 1, so 0 is never a
// handle, and a new handle takes the lowest free slot. No object is released with the mutex
// held (see object.h): a removed handle's reference is dropped after unlocking.
class handle_table {
public:
    constexpr handle_table() = default;
    ~handle_table() { close_all(); }
    DISALLOW_COPY_ASSIGN_AND_MOVE(handle_table);

    // A new handle naming obj. ERR_NO_RESOURCES when every slot is taken, ERR_BAD_STATE once
    // the table is closed.
    status_t add(lk::ref_ptr<object> obj, uint32_t rights, lk_handle_t *out);

    // As add(), then writes the handle to *out in aspace; if that faults the handle is closed
    // again and the result is ERR_FAULT.
    status_t add_for_user(vmm_aspace_t *aspace, lk::ref_ptr<object> obj, uint32_t rights,
                          user_ptr<lk_handle_t> out);

    // The object h names, if the handle has every right in need: ERR_BAD_HANDLE for no such
    // handle, then ERR_ACCESS_DENIED for a missing right. *rights, if not null, gets the
    // handle's rights.
    status_t get(lk_handle_t h, uint32_t need, lk::ref_ptr<object> *out, uint32_t *rights = nullptr);

    // As get(), and then ERR_BAD_HANDLE unless the object is a T.
    template <typename T>
    status_t get(lk_handle_t h, uint32_t need, lk::ref_ptr<T> *out) {
        lk::ref_ptr<object> o;
        status_t err = get(h, need, &o);
        if (err < 0) {
            return err;
        }
        if (o->kind() != T::kKind) {
            return ERR_BAD_HANDLE;
        }
        *out = lk::static_ref_cast<T>(std::move(o));
        return NO_ERROR;
    }

    status_t close(lk_handle_t h);

    // As close(), only while h still names o (any object if o is null): undoes an add()
    // without closing a handle that another thread has since closed and reused.
    // ERR_BAD_HANDLE if it does not.
    status_t close_if(lk_handle_t h, const object *o);

    // A second handle to h's object with h's rights & mask. Needs DUP; the rights can only
    // shrink.
    status_t dup(lk_handle_t h, uint32_t mask, lk_handle_t *out);

    // Whether a slot is free just now. Another thread may take it before the caller does, so
    // this only lets a service refuse early what add() would refuse later.
    bool has_free_slot();

    // Closes every handle and refuses new ones. A process's table closes when it exits.
    void close_all();

    // the handles, for the console; with lock false nothing is locked
    void dump(bool lock);

private:
    struct slot {
        lk::ref_ptr<object> obj;
        uint32_t rights = 0;
    };

    slot *find_locked(lk_handle_t h);
    // the lowest free slot as a handle, or LK_HANDLE_INVALID
    lk_handle_t free_handle_locked() const;
    void dump_locked() const;

    Mutex lock_;
    slot slots_[LKUSER_HANDLES];
    bool closed_ = false;
};

} // namespace lkuser
