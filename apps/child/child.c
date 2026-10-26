/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// The process spawntest spawns, with no libc: argv[1] says how it is to end, and the exit code
// says it got there. It is born with only the handles its spawner passed. Anything unexpected
// prints a CHILD: FAIL line through debug_write, which needs no handle, and exits 1.
//
//   count           exits with 100 plus the number of handles it was born with
//   impulse         sends an impulse on its console connection, exits with the status
//   memobj <text>   writes <text> into the memory object under LK_TAG_USER_BASE, exits 7
//   bare            born with a vm connection and that memory object only; writes "bare",
//                   exits 9
//   thread_exit     leaves by thread_exit from its only thread: exit code 0
//   fault           stores to an unmapped address: ERR_FAULT
//   sleep           sleeps a minute in time.SLEEP, to be killed
//   read            blocks in console.READ, to be killed
//   wait            blocks in wait() on a signal its console connection never raises, to be killed
//   threads         a second thread sleeps a minute; the first exits with 5
//   thread_fault    a second thread sleeps a minute; the first faults
//   spin            a second thread spins in user mode; the first exits with 3
//   last_thread     the first thread leaves by thread_exit, the second a little later: 0
//   call            calls the connection under LK_TAG_USER_BASE and waits, to be killed
//   serve           receives one call on the endpoint under LK_TAG_USER_BASE, then exits 4
//                   without replying
//   connect         connects to the endpoint under LK_TAG_USER_BASE, then exits 6, closing the
//                   connection

#define TEST_NAME "CHILD"
#include <lk/services.h>
#include <lktest.h>

#define STACK_SIZE 8192
#define MINUTE_NS 60000000000ull

static lk_handle_t process_h, time_h, console_h;
static uint8_t stack[STACK_SIZE] __attribute__((aligned(16)));

static void fail(const char *what) __attribute__((noreturn));
static void fail(const char *what) {
    test_put("CHILD: FAIL ");
    test_put(what);
    test_put("\n");
    lk_exit(1);
}

static void sleep_minute(void *arg) {
    lk_time_sleep(time_h, MINUTE_NS);
    // only a kill should end the sleep early, and a kill never returns here
    fail("the sleep ended");
}

static void spin(void *arg) {
    for (;;) {
    }
}

static void sleep_then_leave(void *arg) {
    lk_time_sleep(time_h, 20000000);
}

static void start_thread(void (*fn)(void *)) {
    lk_handle_t t;
    if (lk_thread_start(process_h, fn, NULL, stack, sizeof(stack), &t) != NO_ERROR) {
        fail("thread_start");
    }
}

// the memory object a spawner passed, mapped
static char *memobj(const struct lk_proc_args *args) {
    const lk_handle_t vm = lk_proc_args_find(args, LK_TAG_VM);
    const lk_handle_t m = lk_proc_args_find(args, LK_TAG_USER_BASE);
    void *va = NULL;
    if (m == LK_HANDLE_INVALID ||
        lk_vm_memobj_map(vm, m, 0, 0, LK_VM_PROT_READ | LK_VM_PROT_WRITE, &va) != NO_ERROR) {
        fail("no memory object");
    }
    return va;
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    time_h = lk_proc_args_find(args, LK_TAG_TIME);
    console_h = lk_proc_args_find(args, LK_TAG_CONSOLE);
    if (args->nargs < 2) {
        fail("no mode");
    }
    const char *mode = lk_proc_args_arg(args, 1);

    if (test_streq(mode, "count")) {
        lk_exit(100 + (int)args->nhandles);
    }
    if (test_streq(mode, "impulse")) {
        const struct lk_impulse imp = {.type = LK_IMPULSE_USER_BASE};
        lk_exit(lk_impulse_send(console_h, &imp));
    }
    if (test_streq(mode, "memobj") && args->nargs == 3) {
        const char *text = lk_proc_args_arg(args, 2);
        char *p = memobj(args);
        for (size_t i = 0; i <= test_strlen(text); i++) {
            p[i] = text[i];
        }
        lk_exit(7);
    }
    if (test_streq(mode, "bare")) {
        // a vm connection and the memory object, exactly as the spawner passed them
        if (args->nhandles != 2 || process_h != LK_HANDLE_INVALID) {
            fail("bare child has other handles");
        }
        char *p = memobj(args);
        p[0] = 'b';
        p[1] = 'a';
        p[2] = 'r';
        p[3] = 'e';
        p[4] = '\0';
        lk_exit(9);
    }
    if (test_streq(mode, "thread_exit")) {
        lk_thread_exit();
    }
    if (test_streq(mode, "fault")) {
        *(volatile int *)0x1000 = 1;
    }
    if (test_streq(mode, "sleep")) {
        sleep_minute(NULL);
    }
    if (test_streq(mode, "read")) {
        char buf[16];
        size_t n;
        lk_console_read(console_h, buf, sizeof(buf), &n);
        fail("the read ended");
    }
    if (test_streq(mode, "wait")) {
        lk_wait_one(console_h, LK_SIGNAL_TERMINATED, LK_TIME_INFINITE, NULL);
        fail("the wait ended");
    }
    if (test_streq(mode, "threads")) {
        start_thread(sleep_minute);
        lk_time_sleep(time_h, 1000000);
        lk_exit(5);
    }
    if (test_streq(mode, "thread_fault")) {
        start_thread(sleep_minute);
        lk_time_sleep(time_h, 1000000);
        *(volatile int *)0x1000 = 1;
    }
    if (test_streq(mode, "spin")) {
        start_thread(spin);
        lk_time_sleep(time_h, 10000000);
        lk_exit(3);
    }
    if (test_streq(mode, "last_thread")) {
        start_thread(sleep_then_leave);
        lk_thread_exit();
    }
    const lk_handle_t given = lk_proc_args_find(args, LK_TAG_USER_BASE);
    if (test_streq(mode, "call")) {
        lk_msg_call(given, 1, NULL, 0, NULL, 0, NULL);
        fail("the call ended");
    }
    if (test_streq(mode, "serve")) {
        struct lk_recv_args r = {.timeout_ns = LK_TIME_INFINITE};
        if (lk_recv(given, &r) != NO_ERROR || r.info.token == 0) {
            fail("recv");
        }
        lk_exit(4);
    }
    if (test_streq(mode, "connect")) {
        lk_handle_t conn;
        if (lk_connect(given, &conn) != NO_ERROR) {
            fail("connect");
        }
        lk_exit(6);
    }
    fail(mode);
}
