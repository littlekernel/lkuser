/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Faults on purpose, so that a user fault is seen to end its process and nothing else: the
// kernel prints its "faulted" line and the process exits with -40 (ERR_FAULT), which the gate
// expects of it ("faulttest=-40" in TEST_APPS).

#define TEST_NAME "FAULTTEST"
#include <lktest.h>

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    test_put("FAULTTEST: storing to an unmapped address\n");
    // below the user address space, so never mapped; not null, which the compiler may turn
    // into a trap instruction of its own
    *(volatile int *)0x1000 = 1;
    test_put("FAULTTEST: FAIL, the store did not fault\n");
    lk_exit(1);
}
