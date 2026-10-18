/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <sys/lk_abi.h>

#include "copy.h"

namespace lkuser {

namespace internal {

// A row argument as its handler takes it: a user pointer becomes a user_ptr, which cannot
// be dereferenced; anything else is unchanged.
template <typename A>
struct handler_arg {
    using type = A;
};
template <typename T>
struct handler_arg<T *> {
    using type = user_ptr<T>;
};

template <typename F>
struct handler_fn;
template <typename R, typename... A>
struct handler_fn<R(A...)> {
    using type = R(typename handler_arg<A>::type...);
};

} // namespace internal

// One sys_<name> handler per row of <sys/_syscalls.h>, declared from the row itself, so a
// handler defined with any other signature leaves the declared one undefined at link time.
#define LK_SYSCALL_DEF(n, ret, name, args...) internal::handler_fn<ret(args)>::type sys_##name;
#include <sys/_syscalls.h>
#undef LK_SYSCALL_DEF

} // namespace lkuser
