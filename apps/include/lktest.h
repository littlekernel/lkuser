/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// Helpers for the tests that run without a libc: string basics, output through debug_write,
// and CHECK/CHECK_EQ, which count failures and print each one. Define TEST_NAME, the
// upper-cased app name, before including.

#include <lk/ipc.h>

#ifndef TEST_NAME
#error define TEST_NAME before including lktest.h
#endif

static int test_failures;

static size_t test_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int test_streq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int test_ends_with(const char *s, const char *suffix) {
    const size_t n = test_strlen(s);
    const size_t m = test_strlen(suffix);
    return n >= m && test_streq(s + n - m, suffix);
}

static void test_put(const char *s) {
    lk_debug_write(s, test_strlen(s));
}

static void test_put_dec(long v) {
    char buf[24];
    char *p = buf + sizeof(buf);
    unsigned long u = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v;
    *--p = '\0';
    do {
        *--p = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0) {
        *--p = '-';
    }
    test_put(p);
}

static void test_fail_line(int line, const char *what) {
    test_put(TEST_NAME ": FAIL line ");
    test_put_dec(line);
    test_put(": ");
    test_put(what);
    test_failures++;
}

#define CHECK(cond)                           \
    do {                                      \
        if (!(cond)) {                        \
            test_fail_line(__LINE__, #cond);  \
            test_put("\n");                   \
        }                                     \
    } while (0)

// for status codes and counts: prints what it got
#define CHECK_EQ(got, want)                                      \
    do {                                                         \
        const long got_ = (long)(got);                           \
        const long want_ = (long)(want);                         \
        if (got_ != want_) {                                     \
            test_fail_line(__LINE__, #got " == " #want ", got "); \
            test_put_dec(got_);                                  \
            test_put("\n");                                      \
        }                                                        \
    } while (0)

// Exits with 1 after a FAIL line if a check failed, else returns, leaving the PASS line and
// the exit to the test.
static void test_check_failures(void) {
    if (test_failures) {
        test_put(TEST_NAME ": FAIL (");
        test_put_dec(test_failures);
        test_put(" checks)\n");
        lk_exit(1);
    }
}
