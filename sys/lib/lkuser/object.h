/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lktl/function_ref.h>
#include <lktl/list.h>
#include <lktl/ref_ptr.h>
#include <new>
#include <stdint.h>
#include <utility>

namespace lkuser {

// every kind of kernel object
enum class obj_kind : uint8_t {
    ENDPOINT,
    CONNECTION,
    GROUP,
    MEMOBJ,
    PROCESS,
    THREAD,
};

const char *kind_name(obj_kind kind);

// the hook of the registry list every object is on
struct registry_tag {};

// Base of every kernel object: a reference count, a kind and the number of handles naming it.
// No virtual functions: the last release calls destroy(), which deletes the concrete type by
// kind. The hierarchy is not standard-layout, so a derived type is reached with static_cast
// after a kind check, never containerof.
//
// Every object is made by make() and stays on a registry list, for the console, until its last
// release. That release takes the registry mutex and frees memory, so a reference is never
// dropped with a handle table's mutex, a spinlock or THREAD_LOCK held.
class object : public lk::ref_counted<object>, public lk::list_hook<registry_tag> {
public:
    obj_kind kind() const { return kind_; }

    // A new T with its first reference, on the registry; empty if out of memory. T makes
    // object a friend so this can reach its constructor.
    template <typename T, typename... Args>
    static lk::ref_ptr<T> make(Args &&...args) {
        lk::ref_ptr<T> o = lk::adopt_ref(new (std::nothrow) T(std::forward<Args>(args)...));
        if (o) {
            add_to_registry(o.get());
        }
        return o;
    }

    static void destroy(object *o);

    // A handle naming this object was added to or removed from a table, in any process.
    // Removing the last one runs the kind's last-handle action (an endpoint closes).
    void handle_added();
    void handle_removed();
    uint32_t handle_count_debug() const { return __atomic_load_n(&handle_count_, __ATOMIC_RELAXED); }

    // Calls fn on every object, holding the registry mutex unless lock is false (for the
    // console's -f forms). With the mutex held no object can be destroyed meanwhile; fn must
    // not make or release one.
    static void for_each(lk::function_ref<void(object &)> fn, bool lock);

    // one line describing the object, for the console
    void dump() const;

protected:
    explicit constexpr object(obj_kind kind) : kind_(kind) {}
    ~object() = default;

private:
    static void add_to_registry(object *o);
    static void last_handle_removed(object *o);

    const obj_kind kind_;
    uint32_t handle_count_ = 0;
};

} // namespace lkuser
