/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Raw ABI test with no libc, run as "rawtest a b c": user entry, the startup block, the trap
// ABI, debug_write and the kernel's checks on the user pointers it is given, a row with no
// handler, a number past the table, and exit. Prints RAWTEST: PASS and exits with 0 when
// every check holds; each failed check prints a RAWTEST: FAIL line and the exit code is 1.

#include <lk/ipc.h>

static int failures;

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int ends_with(const char *s, const char *suffix) {
    const size_t n = str_len(s);
    const size_t m = str_len(suffix);
    return n >= m && str_eq(s + n - m, suffix);
}

static void put(const char *s) {
    lk_debug_write(s, str_len(s));
}

static void put_dec(unsigned v) {
    char buf[12];
    char *p = buf + sizeof(buf);
    *--p = '\0';
    do {
        *--p = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    put(p);
}

static void fail(int line, const char *what) {
    put("RAWTEST: FAIL line ");
    put_dec((unsigned)line);
    put(": ");
    put(what);
    put("\n");
    failures++;
}

#define CHECK(cond)                    \
    do {                               \
        if (!(cond)) {                 \
            fail(__LINE__, #cond);     \
        }                              \
    } while (0)

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
    CHECK(args->nargs == 4);
    CHECK(args->nhandles == 0);
    if (args->nargs == 4) {
        CHECK(ends_with(lk_proc_args_arg(args, 0), "/rawtest"));
        CHECK(str_eq(lk_proc_args_arg(args, 1), "a"));
        CHECK(str_eq(lk_proc_args_arg(args, 2), "b"));
        CHECK(str_eq(lk_proc_args_arg(args, 3), "c"));
    }
    put("RAWTEST: startup block ok\n");
}

static void test_debug_write(void) {
    static const char msg[] = "RAWTEST: debug_write ok\n";
    CHECK(lk_debug_write(msg, sizeof(msg) - 1) == NO_ERROR);
    CHECK(lk_debug_write(msg, 0) == NO_ERROR);

    // below the user address space, and in the kernel's: refused by the range check
    CHECK(lk_debug_write((const char *)0x1000, 4) == ERR_FAULT);
    CHECK(lk_debug_write((const char *)(UINTPTR_MAX - 0xfff), 4) == ERR_FAULT);
    // a length that runs off the end of the address space
    CHECK(lk_debug_write(msg, SIZE_MAX) == ERR_FAULT);
    // inside the address space but far above the image and stack near its base, so
    // unmapped: refused by the page walk
    CHECK(lk_debug_write((const char *)0x10000000, 4) == ERR_FAULT);
    put("RAWTEST: bad pointers refused\n");
}

static void test_unserved(void) {
    // a row with no handler, the first number past the table, and one far past it
    CHECK(lk_handle_close(1) == ERR_NOT_SUPPORTED);
    CHECK(raw_syscall(21) == ERR_NOT_SUPPORTED);
    CHECK(raw_syscall(~0ul) == ERR_NOT_SUPPORTED);
    put("RAWTEST: unserved syscalls ok\n");
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    put("RAWTEST: start\n");

    test_startup_block(args);
    test_debug_write();
    test_unserved();

    if (failures) {
        put("RAWTEST: FAIL (");
        put_dec((unsigned)failures);
        put(" checks)\n");
        lk_exit(1);
    }
    put("RAWTEST: PASS\n");
    lk_exit(0);
}
