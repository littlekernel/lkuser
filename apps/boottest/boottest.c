/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Boot smoke test for a newlib program: static data, stdio, floating point, the heap behind
// malloc and sbrk, and the exit path. Reads no input. The verdict comes from an exit handler,
// so the gate sees BOOTTEST: PASS only if exit runs them: PASS with exit code 0 when every
// check holds; else each failed check prints a BOOTTEST: FAIL line and the code is 1.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            printf("BOOTTEST: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                                \
        }                                                                              \
    } while (0)

volatile int bss_var;
volatile int data_var = 9;

// out of line so the compiler cannot fold the arithmetic away
__attribute__((noinline)) static double fmul_add(double a, double b, double c) {
    return a * b + c;
}

static void fill(unsigned char *p, size_t len, unsigned seed) {
    for (size_t i = 0; i < len; i++) {
        p[i] = (unsigned char)(seed + i * 31);
    }
}

static int verify(const unsigned char *p, size_t len, unsigned seed) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] != (unsigned char)(seed + i * 31)) {
            return 0;
        }
    }
    return 1;
}

static void at_exit(void) {
    if (failures) {
        printf("BOOTTEST: FAIL (%d checks)\n", failures);
    } else {
        printf("BOOTTEST: PASS\n");
    }
}

static void test_static_data(void) {
    CHECK(bss_var == 0);
    CHECK(data_var == 9);
    printf("BOOTTEST: static data ok\n");
}

static void test_float(void) {
    // exact in binary, so the comparison is exact too
    CHECK(fmul_add(1.5, 0.25, 0.0625) == 0.4375);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", fmul_add(0.1234, 1.0, 0.123));
    CHECK(strcmp(buf, "0.2464") == 0);
    printf("BOOTTEST: float ok (%s)\n", buf);
}

static void test_malloc(void) {
    // small blocks, all live at once, filled before any is checked
    enum { NBLOCKS = 16 };
    unsigned char *blocks[NBLOCKS];
    for (int i = 0; i < NBLOCKS; i++) {
        size_t len = 16 + (size_t)i * 200;
        blocks[i] = malloc(len);
        CHECK(blocks[i] != NULL);
        if (blocks[i]) {
            fill(blocks[i], len, i);
        }
    }
    for (int i = 0; i < NBLOCKS; i++) {
        if (blocks[i]) {
            CHECK(verify(blocks[i], 16 + (size_t)i * 200, i));
        }
        free(blocks[i]);
    }

    // realloc keeps the contents when it grows a block
    unsigned char *p = malloc(100);
    CHECK(p != NULL);
    if (p) {
        fill(p, 100, 7);
        unsigned char *q = realloc(p, 10000);
        CHECK(q != NULL);
        if (q) {
            CHECK(verify(q, 100, 7));
            p = q;
        }
        free(p);
    }

    // a block spanning several pages
    const size_t big = 32 * 1024;
    unsigned char *b = malloc(big);
    CHECK(b != NULL);
    if (b) {
        fill(b, big, 3);
        CHECK(verify(b, big, 3));
        free(b);
    }
    printf("BOOTTEST: malloc ok\n");
}

// The heap grows in 64KB chunks of vm.ALLOC memory: blocks live in several chunks at once,
// some bigger than a chunk, all filled before any is checked.
static void test_heap_growth(void) {
    enum { NBLOCKS = 6 };
    static const size_t sizes[NBLOCKS] = {
        40 * 1024, 70 * 1024, 24 * 1024, 200 * 1024, 64 * 1024, 100 * 1024,
    };
    unsigned char *blocks[NBLOCKS];
    for (int i = 0; i < NBLOCKS; i++) {
        blocks[i] = malloc(sizes[i]);
        CHECK(blocks[i] != NULL);
        if (blocks[i]) {
            fill(blocks[i], sizes[i], 20 + i);
        }
    }
    for (int i = 0; i < NBLOCKS; i++) {
        if (blocks[i]) {
            CHECK(verify(blocks[i], sizes[i], 20 + i));
        }
    }

    // a block bigger than a chunk keeps its bytes when realloc grows it
    unsigned char *q = realloc(blocks[3], 300 * 1024);
    CHECK(q != NULL);
    if (q) {
        CHECK(verify(q, sizes[3], 23));
        blocks[3] = q;
    }
    for (int i = 0; i < NBLOCKS; i++) {
        free(blocks[i]);
    }
    printf("BOOTTEST: heap growth ok\n");
}

static void test_sbrk(void) {
    unsigned char *p = sbrk(8192);
    CHECK(p != NULL && p != (void *)-1);
    if (p && p != (void *)-1) {
        fill(p, 8192, 11);
        CHECK(verify(p, 8192, 11));
    }
    printf("BOOTTEST: sbrk ok\n");
}

int main(void) {
    printf("BOOTTEST: start\n");
    CHECK(atexit(at_exit) == 0);

    test_static_data();
    test_float();
    test_malloc();
    test_heap_growth();
    test_sbrk();

    return failures ? 1 : 0;
}
