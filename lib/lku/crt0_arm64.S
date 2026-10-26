/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Process entry: sp points at the startup block (struct lk_proc_args), 16-byte aligned.
// _start_c(const struct lk_proc_args *) never returns.

.section .text.startup

.globl _start
.type _start, %function
_start:
    mov     x0, sp
    bl      _start_c
    udf     #0

// Entry of a thread lk_thread_start made: sp points at {fn, arg}, every other register is
// zero. Calls fn(arg); the thread leaves when it returns.

.text

.globl _lk_thread_entry
.type _lk_thread_entry, %function
_lk_thread_entry:
    ldp     x1, x0, [sp]
    blr     x1
    bl      lk_thread_exit
    udf     #0
