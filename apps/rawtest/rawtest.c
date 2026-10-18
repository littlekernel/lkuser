/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Raw ABI test with no libc, run as "rawtest a b c": user entry, the startup block and its
// handles, the trap ABI, debug_write and the kernel's checks on the user pointers it is given,
// a row with no handler, a number past the table, and exit. Prints RAWTEST: PASS and exits
// with 0 when every check holds; each failed check prints a RAWTEST: FAIL line and the exit
// code is 1.

#define TEST_NAME "RAWTEST"
#include <lktest.h>

// a syscall by number, bypassing the stubs
static lk_status_t raw_syscall(unsigned long num) {
    register long a0 __asm__("a0") = 0;
    register unsigned long a7 __asm__("a7") = num;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (lk_status_t)a0;
}

static void test_startup_block(const struct lk_proc_args *args) {
    // sp pointed at the block, aligned as the calling convention asks
    CHECK(((uintptr_t)args & 15) == 0);
    CHECK_EQ(args->nargs, 4);
    if (args->nargs == 4) {
        CHECK(test_ends_with(lk_proc_args_arg(args, 0), "/rawtest"));
        CHECK(test_streq(lk_proc_args_arg(args, 1), "a"));
        CHECK(test_streq(lk_proc_args_arg(args, 2), "b"));
        CHECK(test_streq(lk_proc_args_arg(args, 3), "c"));
    }

    // a handle per service the build has, each a real and distinct handle under its own tag;
    // every build has a console
    const struct lk_proc_handle *h = lk_proc_args_handles(args);
    CHECK(args->nhandles > 0);
    for (uint32_t i = 0; i < args->nhandles; i++) {
        CHECK(h[i].handle != LK_HANDLE_INVALID);
        for (uint32_t j = 0; j < i; j++) {
            CHECK(h[i].handle != h[j].handle);
            CHECK(h[i].tag != h[j].tag);
        }
    }
    CHECK(lk_proc_args_find(args, LK_TAG_CONSOLE) != LK_HANDLE_INVALID);
    CHECK(lk_proc_args_find(args, 0) == LK_HANDLE_INVALID);
    test_put("RAWTEST: startup block ok\n");
}

static void test_debug_write(void) {
    static const char msg[] = "RAWTEST: debug_write ok\n";
    CHECK_EQ(lk_debug_write(msg, sizeof(msg) - 1), NO_ERROR);
    CHECK_EQ(lk_debug_write(msg, 0), NO_ERROR);

    // below the user address space, and in the kernel's: refused by the range check
    CHECK_EQ(lk_debug_write((const char *)0x1000, 4), ERR_FAULT);
    CHECK_EQ(lk_debug_write((const char *)(UINTPTR_MAX - 0xfff), 4), ERR_FAULT);
    // a length that runs off the end of the address space
    CHECK_EQ(lk_debug_write(msg, SIZE_MAX), ERR_FAULT);
    // inside the address space but far above the image and stack near its base, so
    // unmapped: refused by the page walk
    CHECK_EQ(lk_debug_write((const char *)0x10000000, 4), ERR_FAULT);
    test_put("RAWTEST: bad pointers refused\n");
}

static void test_unserved(void) {
    // a row with no handler, the first number past the table, and one far past it
    CHECK_EQ(lk_recv(LK_HANDLE_INVALID, NULL), ERR_NOT_SUPPORTED);
    CHECK_EQ(raw_syscall(21), ERR_NOT_SUPPORTED);
    CHECK_EQ(raw_syscall(~0ul), ERR_NOT_SUPPORTED);
    test_put("RAWTEST: unserved syscalls ok\n");
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    test_put("RAWTEST: start\n");

    test_startup_block(args);
    test_debug_write();
    test_unserved();

    test_check_failures();
    test_put("RAWTEST: PASS\n");
    lk_exit(0);
}
