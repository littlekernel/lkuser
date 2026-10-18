/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lktl/ref_ptr.h>
#include <stdint.h>

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

// Base of every kernel object: a reference count and a kind. No virtual functions: the last
// release calls destroy(), which deletes the concrete type by kind. The hierarchy is not
// standard-layout, so a derived type is reached with static_cast after a kind check, never
// containerof.
class object : public lk::ref_counted<object> {
public:
    obj_kind kind() const { return kind_; }

    static void destroy(object *o);

protected:
    explicit constexpr object(obj_kind kind) : kind_(kind) {}
    ~object() = default;

private:
    const obj_kind kind_;
};

} // namespace lkuser
