/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// The native API: one lk_<name> function per row of <sys/_syscalls.h>, each returning
// NO_ERROR or a negative ERR_* from <sys/lk_err.h>.

#include <stddef.h>
#include <stdint.h>
#include <sys/lk_abi.h>
#include <sys/lk_err.h>

#ifdef __cplusplus
extern "C" {
#endif

void lk_exit(int code) __attribute__((noreturn));
void lk_thread_exit(void) __attribute__((noreturn));

#define LK_SYSCALL_DEF(n, ret, name, args...) ret lk_##name(args);
#include <sys/_syscalls.h>
#undef LK_SYSCALL_DEF

#ifdef __cplusplus
}
#endif
