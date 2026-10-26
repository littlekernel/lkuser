/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// Helpers for the tests that run without a libc: string basics, output through debug_write,
// CHECK/CHECK_EQ, which count failures and print each one, and test_spawn. Define TEST_NAME,
// the upper-cased app name, before including.

#include <lk/ipc.h>
#include <lk/services.h>

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

// A handle of the caller's that a child gets a dup of, under tag, with the caller's rights
// masked by rights: how a test hands its service connections down.
struct test_handoff {
    lk_handle_t h;
    uint32_t tag;
    uint32_t rights;
};

// Spawns path as argv[0..argc). The child is born with a dup of each of down[], made here, then
// hs[i] under tags[i], moved, and nothing else. A refusal closes the dups and leaves hs[] here,
// as lk_process_spawn does.
static lk_status_t test_spawn(lk_handle_t process, const char *path, int argc,
                              const char *const *argv, const struct test_handoff *down,
                              size_t ndown, const lk_handle_t *hs, const uint32_t *tags, size_t n,
                              lk_handle_t *child, uint32_t *pid) {
    lk_handle_t all[LK_MSG_MAX_HANDLES];
    uint32_t all_tags[LK_MSG_MAX_HANDLES];
    if (ndown > LK_MSG_MAX_HANDLES || n > LK_MSG_MAX_HANDLES - ndown) {
        return ERR_INVALID_ARGS;
    }
    size_t made = 0;
    lk_status_t err = NO_ERROR;
    while (made < ndown && err == NO_ERROR) {
        err = lk_handle_dup(down[made].h, down[made].rights, &all[made]);
        if (err == NO_ERROR) {
            all_tags[made] = down[made].tag;
            made++;
        }
    }
    if (err == NO_ERROR) {
        for (size_t i = 0; i < n; i++) {
            all[ndown + i] = hs[i];
            all_tags[ndown + i] = tags[i];
        }
        err = lk_process_spawn(process, path, argc, argv, 0, all, all_tags, ndown + n, child,
                               pid);
    }
    if (err < 0) {
        while (made > 0) {
            lk_handle_close(all[--made]);
        }
    }
    return err;
}
