/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Faults on purpose, for checking by hand that a user fault ends its process and nothing
// else: "make test-auto TEST_APPS=faulttest" shows the kernel's "faulted" line and exit code
// -40 (ERR_FAULT). The gate reports FAIL for it, as it cannot yet expect a non-zero exit code.

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
