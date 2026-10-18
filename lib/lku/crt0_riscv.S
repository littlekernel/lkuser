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
_start:
.option push
.option norelax
    la      gp, __global_pointer$
.option pop

    mv      a0, sp
    call    _start_c
    unimp
