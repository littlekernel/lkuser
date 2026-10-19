/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// The client of apps/echoserver, with no libc. It finds the server with namespace.LOOKUP, then
// runs the same echo checks against it and against the kernel's echo service (a small request,
// 64KB requests and replies, a reply cut short, an unknown op), lends it a memory object, has it
// send an impulse every tenth call, calls its one-at-a-time endpoint from several threads while
// others call its other endpoint, drops a connection and sees the server count the CONN_CLOSED,
// and tells it to quit, after which its endpoints and mounts are gone. Prints ECHOTEST: PASS.

#define TEST_NAME "ECHOTEST"
#include <echotest.h>
#include <lk/services.h>
#include <lktest.h>

#define MS 1000000ull
#define SEC (1000 * MS)
#define PAGE 4096
#define BIG 65536
#define STACK_SIZE 8192
#define NCALLERS 4
#define CALLS 20

static lk_handle_t console_h, process_h, time_h, vm_h, ns_h, kecho_h;
static uint32_t my_pid;
static uint8_t big_req[BIG];
static uint8_t big_reply[BIG];
static uint8_t stacks[NCALLERS][STACK_SIZE] __attribute__((aligned(16)));

static void say(const char *s) {
    CHECK_EQ(lk_console_write(console_h, s, test_strlen(s)), NO_ERROR);
}

static int bytes_eq(const void *a, const void *b, size_t n) {
    const uint8_t *x = a;
    const uint8_t *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return 0;
        }
    }
    return 1;
}

// The server starts just before this program does, so its mount may not be there yet.
static lk_handle_t lookup(const char *path) {
    lk_handle_t conn = LK_HANDLE_INVALID;
    char rest[8];
    for (int i = 0; i < 200; i++) {
        const lk_status_t err = lk_ns_lookup(ns_h, path, &conn, rest, sizeof(rest), NULL);
        if (err == NO_ERROR) {
            CHECK_EQ(rest[0], '\0');
            return conn;
        }
        CHECK_EQ(err, ERR_NOT_FOUND);
        lk_time_sleep(time_h, 10 * MS);
    }
    test_put("ECHOTEST: FAIL no server at " ECHOTEST_PATH "\n");
    lk_exit(1);
}

static struct echotest_stats stats(lk_handle_t conn) {
    struct echotest_stats s = {0};
    size_t len = 0;
    CHECK_EQ(lk_msg_call(conn, ECHOTEST_STATS, NULL, 0, &s, sizeof(s), &len), NO_ERROR);
    CHECK_EQ(len, sizeof(s));
    return s;
}

// what an echo service, in the kernel or not, does with the echo protocol
static void echo_checks(lk_handle_t conn, uint32_t seed) {
    char r[16] = {0};
    size_t len = 0;
    CHECK_EQ(lk_msg_call(conn, LK_ECHO_ECHO, "hello", 5, r, sizeof(r), &len), NO_ERROR);
    CHECK_EQ(len, 5);
    CHECK(bytes_eq(r, "hello", 5));

    // far past what recv copies, so the server reads the rest in place
    for (size_t i = 0; i < BIG; i++) {
        big_req[i] = (uint8_t)(i * 7 + seed);
        big_reply[i] = 0;
    }
    CHECK_EQ(lk_msg_call(conn, LK_ECHO_ECHO, big_req, BIG, big_reply, BIG, &len), NO_ERROR);
    CHECK_EQ(len, BIG);
    CHECK(bytes_eq(big_reply, big_req, BIG));

    // less room than the request: as much as fits
    CHECK_EQ(lk_msg_call(conn, LK_ECHO_ECHO, big_req, 1000, big_reply, 300, &len),
             ERR_NOT_ENOUGH_BUFFER);
    CHECK_EQ(len, 300);

    CHECK_EQ(lk_msg_call(conn, 99, NULL, 0, NULL, 0, NULL), ERR_NOT_SUPPORTED);
}

// the server maps a lent memory object and sums it; the object stays here
static void test_memobj(lk_handle_t conn) {
    lk_handle_t m = LK_HANDLE_INVALID;
    CHECK_EQ(lk_vm_memobj_create(vm_h, PAGE, 0, &m, NULL), NO_ERROR);
    uint8_t *va = NULL;
    CHECK_EQ(lk_vm_memobj_map(vm_h, m, 0, 0, LK_VM_PROT_READ | LK_VM_PROT_WRITE, (void **)&va),
             NO_ERROR);
    uint32_t sum = 0;
    for (uint32_t i = 0; va && i < 1000; i++) {
        va[i] = (uint8_t)(i * 13);
        sum += va[i];
    }
    const struct echotest_memobj_req req = {.len = 1000};
    struct echotest_memobj_reply rep = {0};
    const struct lk_iov req_iov = {(void *)&req, sizeof(req)};
    const struct lk_iov rep_iov = {&rep, sizeof(rep)};
    struct lk_msg msg = {
        .op = ECHOTEST_MEMOBJ, .req = &req_iov, .nreq = 1, .reply = &rep_iov, .nreply = 1,
        .carry = &m, .ncarry = 1, .carry_flags = LK_HANDLE_LEND(0),
    };
    CHECK_EQ(lk_msg_callv(conn, &msg), NO_ERROR);
    CHECK_EQ(rep.sum, sum);
    CHECK_EQ(lk_vm_unmap(vm_h, va), NO_ERROR);
    CHECK_EQ(lk_handle_close(m), NO_ERROR);
    say("ECHOTEST: lent memory object ok\n");
}

// The server gets a connection to an endpoint of this process, and impulses it every tenth
// echo. Returns the endpoint, which stays open while the server may send to it.
static lk_handle_t test_impulses(lk_handle_t conn) {
    const struct lk_endpoint_args ea = {.flags = 0, .max_active = 1, .cookie = 0xec};
    lk_handle_t ep = LK_HANDLE_INVALID;
    lk_handle_t c = LK_HANDLE_INVALID;
    CHECK_EQ(lk_endpoint_create(&ea, &ep), NO_ERROR);
    CHECK_EQ(lk_connect(ep, &c), NO_ERROR);
    struct lk_msg msg = {.op = ECHOTEST_NOTIFY, .carry = &c, .ncarry = 1};
    CHECK_EQ(lk_msg_callv(conn, &msg), NO_ERROR);
    CHECK_EQ(lk_handle_close(c), ERR_BAD_HANDLE);

    for (uint32_t i = 1; i <= 30; i++) {
        char r[8];
        size_t len = 0;
        CHECK_EQ(lk_msg_call(conn, LK_ECHO_ECHO, "tick", 4, r, sizeof(r), &len), NO_ERROR);
        if (i % 10 != 0) {
            continue;
        }
        struct lk_recv_args a = {.timeout_ns = 5 * SEC};
        CHECK_EQ(lk_recv(ep, &a), NO_ERROR);
        CHECK_EQ(a.info.token, 0);
        CHECK_EQ(a.info.ep_cookie, 0xec);
        CHECK(a.info.sender_pid != my_pid);
        struct lk_impulse imp;
        for (size_t j = 0; j < sizeof(imp); j++) {
            ((uint8_t *)&imp)[j] = a.info.impulse[j];
        }
        CHECK_EQ(imp.type, ECHOTEST_IMPULSE);
        const uint32_t n = imp.payload[0] | imp.payload[1] << 8 | imp.payload[2] << 16 |
                           (uint32_t)imp.payload[3] << 24;
        CHECK_EQ(n, i);
    }
    struct lk_recv_args a = {.timeout_ns = 0};
    CHECK_EQ(lk_recv(ep, &a), ERR_TIMED_OUT);
    say("ECHOTEST: impulse every tenth call ok\n");
    return ep;
}

struct caller {
    lk_handle_t conn;
    lk_status_t status;
    lk_handle_t thread;
};

static void caller_body(void *arg) {
    struct caller *c = arg;
    for (int i = 0; i < CALLS; i++) {
        char r[8];
        size_t len = 0;
        const lk_status_t err = lk_msg_call(c->conn, LK_ECHO_ECHO, "abcd", 4, r, sizeof(r), &len);
        if (err != NO_ERROR || len != 4 || !bytes_eq(r, "abcd", 4)) {
            c->status = err != NO_ERROR ? err : ERR_BAD_LEN;
            return;
        }
    }
    c->status = NO_ERROR;
}

// callers[i] calls conns[i], all at once
static void call_together(const lk_handle_t *conns, size_t n) {
    struct caller callers[NCALLERS];
    for (size_t i = 0; i < n; i++) {
        callers[i] = (struct caller){.conn = conns[i], .status = 1};
        CHECK_EQ(lk_thread_start(process_h, caller_body, &callers[i], stacks[i], STACK_SIZE,
                                 &callers[i].thread),
                 NO_ERROR);
    }
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ(lk_wait_one(callers[i].thread, LK_SIGNAL_TERMINATED, 30 * SEC, NULL), NO_ERROR);
        CHECK_EQ(lk_handle_close(callers[i].thread), NO_ERROR);
        CHECK_EQ(callers[i].status, NO_ERROR);
    }
}

// Two server threads share one group: the endpoint taking 4 calls at once gets both, the
// one-at-a-time endpoint never more than one.
static void test_concurrency(lk_handle_t multi, lk_handle_t single) {
    const lk_handle_t two_multi[2] = {multi, multi};
    call_together(two_multi, 2);
    struct echotest_stats s = stats(multi);
    CHECK(s.multi_max >= 2);

    const uint32_t before = s.single_calls;
    const lk_handle_t mixed[NCALLERS] = {single, single, single, multi};
    call_together(mixed, NCALLERS);
    s = stats(multi);
    CHECK_EQ(s.single_calls - before, 3 * CALLS);
    CHECK_EQ(s.single_overlaps, 0);
    say("ECHOTEST: one group, two threads, max_active ok\n");
}

static void test_conn_closed(lk_handle_t multi) {
    const uint32_t before = stats(multi).conn_closed;
    CHECK_EQ(lk_handle_close(lookup(ECHOTEST_PATH)), NO_ERROR);
    uint32_t now = before;
    for (int i = 0; i < 100 && now == before; i++) {
        lk_time_sleep(time_h, 5 * MS);
        now = stats(multi).conn_closed;
    }
    CHECK_EQ(now, before + 1);
    say("ECHOTEST: CONN_CLOSED ok\n");
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    test_put("ECHOTEST: start\n");
    console_h = lk_proc_args_find(args, LK_TAG_CONSOLE);
    process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    time_h = lk_proc_args_find(args, LK_TAG_TIME);
    vm_h = lk_proc_args_find(args, LK_TAG_VM);
    ns_h = lk_proc_args_find(args, LK_TAG_NAMESPACE);
    kecho_h = lk_proc_args_find(args, LK_TAG_TEST_ECHO);
    CHECK(console_h && process_h && time_h && vm_h && ns_h && kecho_h);
    struct lk_process_info me = {0};
    CHECK_EQ(lk_process_info(process_h, &me), NO_ERROR);
    my_pid = me.pid;
    test_check_failures();

    const lk_handle_t multi = lookup(ECHOTEST_PATH);
    const lk_handle_t single = lookup(ECHOTEST_SINGLE_PATH);

    // one client, either service
    echo_checks(kecho_h, 1);
    echo_checks(multi, 2);
    echo_checks(single, 3);
    say("ECHOTEST: echo in the kernel and in a server ok\n");

    test_memobj(multi);
    const lk_handle_t ep = test_impulses(multi);
    test_concurrency(multi, single);
    test_conn_closed(multi);

    // the server goes, and with it its endpoints and its mounts
    CHECK_EQ(lk_msg_call(multi, ECHOTEST_QUIT, NULL, 0, NULL, 0, NULL), NO_ERROR);
    CHECK_EQ(lk_wait_one(multi, LK_SIGNAL_PEER_CLOSED, 5 * SEC, NULL), NO_ERROR);
    CHECK_EQ(lk_wait_one(single, LK_SIGNAL_PEER_CLOSED, 5 * SEC, NULL), NO_ERROR);
    CHECK_EQ(lk_msg_call(multi, LK_ECHO_ECHO, "x", 1, NULL, 0, NULL), ERR_CHANNEL_CLOSED);
    lk_handle_t gone;
    char rest[8];
    CHECK_EQ(lk_ns_lookup(ns_h, ECHOTEST_PATH, &gone, rest, sizeof(rest), NULL), ERR_NOT_FOUND);
    CHECK_EQ(lk_handle_close(multi), NO_ERROR);
    CHECK_EQ(lk_handle_close(single), NO_ERROR);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);

    test_check_failures();
    say("ECHOTEST: PASS\n");
    lk_exit(0);
}
