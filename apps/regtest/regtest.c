/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// The registers a thread owns: the thread pointer (tp on riscv, tpidr_el0 on arm64) and the
// FP registers (none on riscv32, rv32imac). The main thread and every new one are born with
// them zero. Eight threads, and as many again in a copy of this program (argv[0] regchild),
// each put values of their own in them and check them after a spin long enough to be
// preempted, a sleep and a console call: the callee-saved FP registers (fs0-fs11, d8-d15)
// across all of it, a caller-saved one (ft0, d16) only across a spin with no call in it.
// Prints REGTEST: PASS. No libc: it starts threads.

#define TEST_NAME "REGTEST"
#include <lk/services.h>
#include <lktest.h>
#include <stdbool.h>

#define NTHREADS   8
#define ROUNDS     3
#define STACK_SIZE 4096
// three of LK's quanta (5 ticks of 10ms) of spinning per round, so each thread is preempted
#define SPIN_NS    (150ull * 1000 * 1000)
// how long one spin with no call in it takes alone, which the main thread calibrates
#define INNER_NS   (10ull * 1000 * 1000)

static lk_handle_t process_h;
static lk_handle_t time_h;
static lk_handle_t console_h;
static const char *who = "regtest";
// in every value, so the copy's differ from this one's
static uint32_t salt = 1;
static unsigned long inner_count;

static uint8_t stacks[NTHREADS][STACK_SIZE] __attribute__((aligned(16)));
// per thread, the last for the main thread
static int failures[NTHREADS + 1];

#if defined(__aarch64__)
#define HAVE_FP 1
#define TP_NAME "tpidr_el0"
#define TP_READ(v) __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v))
#define TP_WRITE(v) __asm__ volatile("msr tpidr_el0, %0" ::"r"(v))
// index, register, clobber
#define SAVED_FP(X) \
    X(0, "d8", "v8") X(1, "d9", "v9") X(2, "d10", "v10") X(3, "d11", "v11") \
    X(4, "d12", "v12") X(5, "d13", "v13") X(6, "d14", "v14") X(7, "d15", "v15")
#define NSAVED 8
#define FP_WRITE(r, clob, v) __asm__ volatile("fmov " r ", %x0" ::"r"(v) : clob)
#define FP_READ(r, v) __asm__ volatile("fmov %x0, " r : "=r"(v))
#define SCRATCH_FP "d16"
// writes d16, spins n times with no call, reads d16 back
#define SPIN_HOLDING(n, v, out)                                                      \
    __asm__ volatile("fmov d16, %x2\n"                                               \
                     "1: subs %x0, %x0, #1\n"                                        \
                     "b.ne 1b\n"                                                     \
                     "fmov %x1, d16\n"                                               \
                     : "+r"(n), "=r"(out)                                            \
                     : "r"(v)                                                        \
                     : "v16", "cc")
#elif defined(__riscv)
#define TP_NAME "tp"
#define TP_READ(v) __asm__ volatile("mv %0, tp" : "=r"(v))
#define TP_WRITE(v) __asm__ volatile("mv tp, %0" ::"r"(v))
#if defined(__riscv_flen) && __riscv_flen >= 64
#define HAVE_FP 1
#define SAVED_FP(X) \
    X(0, "fs0", "fs0") X(1, "fs1", "fs1") X(2, "fs2", "fs2") X(3, "fs3", "fs3") \
    X(4, "fs4", "fs4") X(5, "fs5", "fs5") X(6, "fs6", "fs6") X(7, "fs7", "fs7") \
    X(8, "fs8", "fs8") X(9, "fs9", "fs9") X(10, "fs10", "fs10") X(11, "fs11", "fs11")
#define NSAVED 12
#define FP_WRITE(r, clob, v) __asm__ volatile("fmv.d.x " r ", %0" ::"r"(v) : clob)
#define FP_READ(r, v) __asm__ volatile("fmv.x.d %0, " r : "=r"(v))
#define SCRATCH_FP "ft0"
#define SPIN_HOLDING(n, v, out)                                                      \
    __asm__ volatile("fmv.d.x ft0, %2\n"                                             \
                     "1: addi %0, %0, -1\n"                                          \
                     "bnez %0, 1b\n"                                                 \
                     "fmv.x.d %1, ft0\n"                                             \
                     : "+r"(n), "=r"(out)                                            \
                     : "r"(v)                                                        \
                     : "ft0")
#else
#define HAVE_FP 0
#define SPIN_HOLDING(n, v, out)                                                      \
    __asm__ volatile("1: addi %0, %0, -1\n"                                          \
                     "bnez %0, 1b\n"                                                 \
                     : "+r"(n)                                                       \
                     :                                                               \
                     : "memory");                                                    \
    (out) = (v)
#endif
#else
#error no registers to test on this architecture
#endif

#if HAVE_FP
#define SAVED_NAME(i, r, clob) r,
static const char *const saved_names[NSAVED] = {SAVED_FP(SAVED_NAME)};
#endif

static void put_hex(uint64_t v) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xf];
    }
    buf[18] = '\0';
    test_put(buf);
}

// thread NTHREADS is the main thread
static void put_thread(int t) {
    test_put(TEST_NAME ": FAIL ");
    test_put(who);
    if (t == NTHREADS) {
        test_put(" main thread");
    } else {
        test_put(" thread ");
        test_put_dec(t);
    }
}

static void born_nonzero(int t, const char *reg, uint64_t got) {
    put_thread(t);
    test_put(" born with ");
    test_put(reg);
    test_put(" = ");
    put_hex(got);
    test_put("\n");
    failures[t]++;
}

static void mismatch(int t, int round, const char *reg, const char *after, uint64_t got,
                     uint64_t want) {
    put_thread(t);
    test_put(" round ");
    test_put_dec(round);
    test_put(": ");
    test_put(reg);
    test_put(" after ");
    test_put(after);
    test_put(" is ");
    put_hex(got);
    test_put(", not ");
    put_hex(want);
    test_put("\n");
    failures[t]++;
}

// the value thread t keeps in register reg (NSAVED: the thread pointer, NSAVED + 1: the
// caller-saved FP register) in a round: never zero, and its low half unique too
static uint64_t value(int t, int round, int reg) {
    const uint32_t lo = (salt << 28) | ((uint32_t)t << 20) | ((uint32_t)round << 12) |
                        ((uint32_t)reg << 4) | 0x9u;
    return ((uint64_t)(lo ^ 0xa5a5a5a5u) << 32) | lo;
}

#ifndef NSAVED
#define NSAVED 0
#endif
#define REG_TP NSAVED
#define REG_SCRATCH (NSAVED + 1)

// What a thread is born with: the thread pointer and every FP register zero. Inline, so the
// registers are read before anything of the thread's own has run.
#define CHECK_BORN(t)                                                                   \
    do {                                                                                \
        uintptr_t tp_;                                                                  \
        TP_READ(tp_);                                                                   \
        if (tp_ != 0) {                                                                 \
            born_nonzero((t), TP_NAME, tp_);                                            \
        }                                                                               \
        CHECK_BORN_FP(t);                                                               \
    } while (0)

#if HAVE_FP
#define BORN_ONE(i, r, clob)                                                            \
    {                                                                                   \
        uint64_t v_;                                                                    \
        FP_READ(r, v_);                                                                 \
        if (v_ != 0) {                                                                  \
            born_nonzero(t_, r, v_);                                                    \
        }                                                                               \
    }
#define CHECK_BORN_FP(t)                                                                \
    do {                                                                                \
        const int t_ = (t);                                                             \
        uint64_t s_;                                                                    \
        __asm__ volatile(                                                               \
            HAVE_SCRATCH_READ : "=r"(s_));                                              \
        if (s_ != 0) {                                                                  \
            born_nonzero(t_, SCRATCH_FP, s_);                                           \
        }                                                                               \
        SAVED_FP(BORN_ONE)                                                              \
    } while (0)
#if defined(__aarch64__)
#define HAVE_SCRATCH_READ "fmov %x0, d16"
#else
#define HAVE_SCRATCH_READ "fmv.x.d %0, ft0"
#endif
#define WRITE_ONE(i, r, clob) FP_WRITE(r, clob, value(t, round, i));
#define CHECK_ONE(i, r, clob)                                                           \
    {                                                                                   \
        uint64_t v_;                                                                    \
        FP_READ(r, v_);                                                                 \
        if (v_ != value(t, round, i)) {                                                 \
            mismatch(t, round, saved_names[i], after, v_, value(t, round, i));          \
        }                                                                               \
    }
#else
#define CHECK_BORN_FP(t) \
    do {                 \
    } while (0)
#define WRITE_ONE(i, r, clob)
#define CHECK_ONE(i, r, clob)
#define SAVED_FP(X)
#endif

// Puts this round's values in the thread pointer and the callee-saved FP registers.
#define WRITE_ALL()                                                                     \
    do {                                                                                \
        const uintptr_t tp_ = (uintptr_t)value(t, round, REG_TP);                       \
        TP_WRITE(tp_);                                                                  \
        SAVED_FP(WRITE_ONE)                                                             \
    } while (0)

#define CHECK_ALL(what)                                                                 \
    do {                                                                                \
        const char *after = (what);                                                     \
        uintptr_t tp_;                                                                  \
        TP_READ(tp_);                                                                   \
        if (tp_ != (uintptr_t)value(t, round, REG_TP)) {                                \
            mismatch(t, round, TP_NAME, after, tp_, (uintptr_t)value(t, round, REG_TP)); \
        }                                                                               \
        SAVED_FP(CHECK_ONE)                                                             \
    } while (0)

static uint64_t now(void) {
    uint64_t ns = 0;
    lk_time_clock_read(time_h, LK_CLOCK_MONOTONIC, &ns);
    return ns;
}

// Spins at least SPIN_NS, each piece with no call in it, holding the caller-saved register.
static void spin(int t, int round) {
    const uint64_t want = value(t, round, REG_SCRATCH);
    const uint64_t start = now();
    do {
        unsigned long n = inner_count;
        uint64_t got;
        SPIN_HOLDING(n, want, got);
#if HAVE_FP
        if (got != want) {
            mismatch(t, round, SCRATCH_FP, "a spin with no call", got, want);
        }
#else
        (void)got;
#endif
        CHECK_ALL("a spin");
    } while (now() - start < SPIN_NS);
}

static void run_thread(void *arg) {
    const int t = (int)(uintptr_t)arg;
    CHECK_BORN(t);
    for (int round = 0; round < ROUNDS; round++) {
        WRITE_ALL();
        CHECK_ALL("writing them");
        spin(t, round);
        CHECK_ALL("the spin");
        const lk_status_t err = lk_time_sleep(time_h, 1000 * 1000);
        CHECK_ALL("time.SLEEP");
        uint32_t cols, rows;
        const lk_status_t werr = lk_console_winsize(console_h, &cols, &rows);
        CHECK_ALL("console.WINSIZE");
        if (err < 0 || werr < 0) {
            put_thread(t);
            test_put(" a call failed\n");
            failures[t]++;
        }
    }
}

// How many turns of the spin take INNER_NS, measured alone.
static void calibrate(void) {
    for (inner_count = 1ul << 14;; inner_count *= 2) {
        unsigned long n = inner_count;
        uint64_t got;
        const uint64_t start = now();
        SPIN_HOLDING(n, 1, got);
        (void)got;
        if (now() - start >= INNER_NS || inner_count >= (1ul << 30)) {
            return;
        }
    }
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    CHECK_BORN(NTHREADS);

    process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    time_h = lk_proc_args_find(args, LK_TAG_TIME);
    console_h = lk_proc_args_find(args, LK_TAG_CONSOLE);
    CHECK(process_h != LK_HANDLE_INVALID && time_h != LK_HANDLE_INVALID &&
          console_h != LK_HANDLE_INVALID);
    const bool child = args->nargs > 0 && test_ends_with(lk_proc_args_arg(args, 0), "regchild");
    if (child) {
        who = "regchild";
        salt = 2;
    }
    test_check_failures();
    calibrate();

    // the copy, so threads of two processes share the cpus
    lk_handle_t copy = LK_HANDLE_INVALID;
    if (!child) {
        const struct test_handoff down[] = {
            {process_h, LK_TAG_PROCESS, LK_RIGHTS_ALL},
            {time_h, LK_TAG_TIME, LK_RIGHTS_ALL},
            {console_h, LK_TAG_CONSOLE, LK_RIGHTS_ALL},
        };
        const char *argv[] = {"regchild"};
        CHECK_EQ(test_spawn(process_h, TEST_IMAGE "/bin/regtest", 1, argv, down, 3, NULL, NULL,
                            0, &copy, NULL),
                 NO_ERROR);
    }

    // the main thread keeps values of its own across the whole run
    {
        const int t = NTHREADS;
        const int round = 0;
        WRITE_ALL();
    }
    lk_handle_t threads[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        CHECK_EQ(lk_thread_start(process_h, run_thread, (void *)(uintptr_t)i, stacks[i],
                                 sizeof(stacks[i]), &threads[i]),
                 NO_ERROR);
    }
    for (int i = 0; i < NTHREADS; i++) {
        CHECK_EQ(lk_wait_one(threads[i], LK_SIGNAL_TERMINATED, LK_TIME_INFINITE, NULL),
                 NO_ERROR);
        lk_handle_close(threads[i]);
    }
    {
        const int t = NTHREADS;
        const int round = 0;
        CHECK_ALL("waiting for the threads");
    }
    if (copy != LK_HANDLE_INVALID) {
        CHECK_EQ(lk_wait_one(copy, LK_SIGNAL_TERMINATED, LK_TIME_INFINITE, NULL), NO_ERROR);
        struct lk_process_info info;
        CHECK_EQ(lk_process_info_of(process_h, copy, &info), NO_ERROR);
        CHECK_EQ(info.exit_code, 0);
        lk_handle_close(copy);
    }

    for (int i = 0; i <= NTHREADS; i++) {
        test_failures += failures[i];
    }
    test_check_failures();
    if (!child) {
        test_put("REGTEST: PASS\n");
    }
    lk_exit(0);
}
