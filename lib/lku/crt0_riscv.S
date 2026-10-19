/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

#if __riscv_xlen == 64
#define REG_L ld
#define SZREG 8
#else
#define REG_L lw
#define SZREG 4
#endif

// Process entry: sp points at the startup block (struct lk_proc_args), 16-byte aligned.
// _start_c(const struct lk_proc_args *) never returns.

.section .text.startup

.globl _start
_start:
.option push
.option norelax
    la      gp, __global_pointer$
.option pop

    mv      a0, sp
    call    _start_c
    unimp

// Entry of a thread lk_thread_start made: sp points at {fn, arg}, every other register is
// zero. Calls fn(arg); the thread leaves when it returns.

.text

.globl _lk_thread_entry
_lk_thread_entry:
.option push
.option norelax
    la      gp, __global_pointer$
.option pop

    REG_L   t0, 0(sp)
    REG_L   a0, SZREG(sp)
    jalr    t0
    call    lk_thread_exit
    unimp
