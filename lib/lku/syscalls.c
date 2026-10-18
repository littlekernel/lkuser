/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// The raw syscall stubs, one per row of <sys/_syscalls.h>.

#include <lk/ipc.h>

#if ARCH_RISCV

// The arguments are already in a0-a5 where the calling convention put them; the number goes
// in a7 and the status comes back in a0.
#define LK_SYSCALL_DEF(n, ret, name, args...) \
    __attribute__((naked)) ret lk_##name(args) { \
        __asm__ volatile("li a7, " #n "\n" \
                         "ecall\n" \
                         "ret\n"); \
    }
#include <sys/_syscalls.h>
#undef LK_SYSCALL_DEF

#else
#error no syscall stubs for this architecture
#endif
