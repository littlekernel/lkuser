/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Native API test with no libc: calls the kernel services over the connections the process is
// born with, and checks handles and their rights, endpoints, and how the call path treats
// every malformed call and bad buffer it can be given. Progress lines and CALLTEST: PASS go
// through the console service; each failed check prints a CALLTEST: FAIL line through
// debug_write and the exit code is 1.

#define TEST_NAME "CALLTEST"
#include <lk/services.h>
#include <lktest.h>
#include <sys/svc/echo.h>

#define PAGE 4096
#define BIG (64 * 1024)
// slots in the kernel's handle table in this build
#define HANDLE_SLOTS 64

static lk_handle_t console_h, vm_h, time_h, process_h, echo_h;
static uint32_t startup_handles;

static void say(const char *s) {
    CHECK_EQ(lk_console_write(console_h, s, test_strlen(s)), NO_ERROR);
}

static void fill(uint8_t *p, size_t len, unsigned seed) {
    for (size_t i = 0; i < len; i++) {
        p[i] = (uint8_t)(i * 7 + seed);
    }
}

static int all_bytes(const uint8_t *p, size_t len, uint8_t v) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] != v) {
            return 0;
        }
    }
    return 1;
}

static int same(const uint8_t *a, const uint8_t *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

// A call with the caller's own iovecs: the transport's status, and the furthest reply byte.
static lk_status_t call_iov(lk_handle_t conn, const struct lk_iov *req, size_t nreq,
                            const struct lk_iov *reply, size_t nreply, size_t *reply_len) {
    struct lk_call_args args = {
        .req = req,
        .nreq = nreq,
        .reply = reply,
        .nreply = nreply,
        .timeout_ns = LK_TIME_INFINITE,
        .reply_len = 0xdead,
    };
    lk_status_t err = lk_call(conn, &args);
    if (reply_len) {
        *reply_len = args.reply_len;
    }
    return err;
}

// an address inside the address space with nothing mapped there, for now
static void *unmapped_page(void) {
    void *p = NULL;
    CHECK_EQ(lk_vm_alloc(vm_h, PAGE, LK_VM_PROT_READ, &p), NO_ERROR);
    CHECK_EQ(lk_vm_unmap(vm_h, p), NO_ERROR);
    return p;
}

static void test_startup(const struct lk_proc_args *args) {
    console_h = lk_proc_args_find(args, LK_TAG_CONSOLE);
    vm_h = lk_proc_args_find(args, LK_TAG_VM);
    time_h = lk_proc_args_find(args, LK_TAG_TIME);
    process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    echo_h = lk_proc_args_find(args, LK_TAG_TEST_ECHO);
    startup_handles = args->nhandles;
    CHECK(console_h != LK_HANDLE_INVALID);
    CHECK(vm_h != LK_HANDLE_INVALID);
    CHECK(time_h != LK_HANDLE_INVALID);
    CHECK(process_h != LK_HANDLE_INVALID);
    CHECK(echo_h != LK_HANDLE_INVALID);
    test_check_failures();
}

static void test_console(void) {
    say("CALLTEST: console write ok\n");

    // a payload in three pieces, and a header that straddles two iovecs
    struct lk_msg_hdr hdr = {.op = LK_CONSOLE_WRITE, .len = 25};
    struct lk_msg_hdr rhdr = {0};
    const struct lk_iov req[] = {
        {&hdr, 5},
        {(char *)&hdr + 5, 11},
        {(void *)"CALLTEST: ", 10},
        {(void *)"iovec ", 6},
        {(void *)"write ok\n", 9},
    };
    const struct lk_iov reply[] = {{&rhdr, sizeof(rhdr)}};
    size_t reply_len;
    CHECK_EQ(call_iov(console_h, req, 5, reply, 1, &reply_len), NO_ERROR);
    CHECK_EQ(rhdr.status, NO_ERROR);
    CHECK_EQ(rhdr.op, LK_CONSOLE_WRITE);
    CHECK_EQ(rhdr.len, 0);
    CHECK_EQ(reply_len, sizeof(rhdr));

    // an op the service does not know: the call succeeds, the service says no
    struct lk_msg_hdr bad = {.op = 99};
    const struct lk_iov bad_req[] = {{&bad, sizeof(bad)}};
    rhdr.status = 1;
    CHECK_EQ(call_iov(console_h, bad_req, 1, reply, 1, &reply_len), NO_ERROR);
    CHECK_EQ(rhdr.status, ERR_NOT_SUPPORTED);
    CHECK_EQ(rhdr.op, 99);
    CHECK_EQ(reply_len, sizeof(rhdr));
    CHECK_EQ(lk_msg_call(console_h, 99, NULL, 0, NULL, 0, NULL), ERR_NOT_SUPPORTED);
}

static void test_handles(void) {
    lk_handle_t h, h2;

    // no such handle
    CHECK_EQ(lk_console_write(LK_HANDLE_INVALID, "x", 1), ERR_BAD_HANDLE);
    CHECK_EQ(lk_console_write(HANDLE_SLOTS + 1, "x", 1), ERR_BAD_HANDLE);
    CHECK_EQ(lk_console_write(1000, "x", 1), ERR_BAD_HANDLE);
    CHECK_EQ(lk_handle_close(LK_HANDLE_INVALID), ERR_BAD_HANDLE);
    CHECK_EQ(lk_handle_close(1000), ERR_BAD_HANDLE);
    CHECK_EQ(lk_handle_dup(LK_HANDLE_INVALID, LK_RIGHTS_ALL, &h), ERR_BAD_HANDLE);

    // a copy works like the original; once closed it is no handle at all
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL, &h), NO_ERROR);
    CHECK(h != console_h);
    CHECK_EQ(lk_console_write(h, "CALLTEST: dup write ok\n", 23), NO_ERROR);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);
    CHECK_EQ(lk_handle_close(h), ERR_BAD_HANDLE);
    CHECK_EQ(lk_console_write(h, "x", 1), ERR_BAD_HANDLE);

    // without CALL; and a dup asking for every right still lacks it
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL & ~LK_RIGHT_CALL, &h), NO_ERROR);
    CHECK_EQ(lk_console_write(h, "x", 1), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_dup(h, LK_RIGHTS_ALL, &h2), NO_ERROR);
    CHECK_EQ(lk_console_write(h2, "x", 1), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h2), NO_ERROR);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    // without DUP
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL & ~LK_RIGHT_DUP, &h), NO_ERROR);
    CHECK_EQ(lk_handle_dup(h, LK_RIGHTS_ALL, &h2), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    // a handle that cannot be written out is closed again: the next dup gets its slot
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL, &h), NO_ERROR);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL, (lk_handle_t *)0x1000), ERR_FAULT);
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL, &h2), NO_ERROR);
    CHECK_EQ(h2, h);
    CHECK_EQ(lk_handle_close(h2), NO_ERROR);

    // a full table refuses rather than grows, and works again once emptied
    lk_handle_t dups[HANDLE_SLOTS];
    uint32_t n = 0;
    lk_status_t err = NO_ERROR;
    while (n < HANDLE_SLOTS && (err = lk_handle_dup(console_h, LK_RIGHTS_ALL, &dups[n])) == NO_ERROR) {
        n++;
    }
    CHECK_EQ(err, ERR_NO_RESOURCES);
    CHECK_EQ(n + startup_handles, HANDLE_SLOTS);
    while (n > 0) {
        CHECK_EQ(lk_handle_close(dups[--n]), NO_ERROR);
    }
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL, &h), NO_ERROR);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    say("CALLTEST: handles ok\n");
}

static void test_endpoints(void) {
    struct lk_endpoint_args args = {.flags = 0, .max_active = 1, .cookie = 0x1234};
    lk_handle_t ep, conn, h;

    CHECK_EQ(lk_endpoint_create(NULL, &ep), ERR_FAULT);
    CHECK_EQ(lk_endpoint_create(&args, (lk_handle_t *)0x1000), ERR_FAULT);
    args.max_active = 0;
    CHECK_EQ(lk_endpoint_create(&args, &ep), ERR_INVALID_ARGS);
    args.max_active = 1;
    args.flags = 1u << 7;
    CHECK_EQ(lk_endpoint_create(&args, &ep), ERR_INVALID_ARGS);
    args.flags = LK_EP_FLAG_CONNECT_NOTIFY;
    CHECK_EQ(lk_endpoint_create(&args, &ep), ERR_NOT_SUPPORTED);
    args.flags = 0;

    CHECK_EQ(lk_endpoint_create(&args, &ep), NO_ERROR);
    CHECK_EQ(lk_connect(ep, (lk_handle_t *)0x1000), ERR_FAULT);
    CHECK_EQ(lk_connect(ep, &conn), NO_ERROR);

    // each side's rights: an endpoint cannot be called, a connection cannot be connected to
    CHECK_EQ(lk_msg_call(ep, 1, NULL, 0, NULL, 0, NULL), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_connect(conn, &h), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_dup(ep, LK_RIGHTS_ALL & ~LK_RIGHT_CONNECT, &h), NO_ERROR);
    CHECK_EQ(lk_connect(h, &conn), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    // nothing receives on a user endpoint yet
    CHECK_EQ(lk_msg_call(conn, 1, NULL, 0, NULL, 0, NULL), ERR_NOT_SUPPORTED);
    // with the endpoint's last handle gone its connections are dead
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    CHECK_EQ(lk_msg_call(conn, 1, NULL, 0, NULL, 0, NULL), ERR_CHANNEL_CLOSED);
    CHECK_EQ(lk_handle_close(conn), NO_ERROR);

    say("CALLTEST: endpoints ok\n");
}

static void test_transport(void) {
    struct lk_msg_hdr hdr = {.op = LK_ECHO_ECHO};
    struct lk_msg_hdr rhdr;
    uint8_t buf[300];
    uint8_t rbuf[400];
    struct lk_iov req[LK_IOV_MAX + 1];
    struct lk_iov reply[LK_IOV_MAX + 1];
    for (int i = 0; i <= LK_IOV_MAX; i++) {
        req[i] = (struct lk_iov){&hdr, sizeof(hdr)};
        reply[i] = (struct lk_iov){&rhdr, sizeof(rhdr)};
    }

    // the arguments and the iovec arrays must be readable, the ranges inside the aspace
    CHECK_EQ(lk_call(echo_h, (struct lk_call_args *)0x1000), ERR_FAULT);
    CHECK_EQ(call_iov(echo_h, (const struct lk_iov *)0x1000, 1, reply, 1, NULL), ERR_FAULT);
    const struct lk_iov outside[] = {{(void *)0x1000, sizeof(hdr)}};
    CHECK_EQ(call_iov(echo_h, outside, 1, reply, 1, NULL), ERR_FAULT);
    CHECK_EQ(call_iov(echo_h, req, 1, outside, 1, NULL), ERR_FAULT);

    // too many iovecs; no room for a header either way
    CHECK_EQ(call_iov(echo_h, req, LK_IOV_MAX + 1, reply, 1, NULL), ERR_INVALID_ARGS);
    CHECK_EQ(call_iov(echo_h, req, 1, reply, LK_IOV_MAX + 1, NULL), ERR_INVALID_ARGS);
    const struct lk_iov short_hdr[] = {{&hdr, 8}};
    CHECK_EQ(call_iov(echo_h, short_hdr, 1, reply, 1, NULL), ERR_INVALID_ARGS);
    CHECK_EQ(call_iov(echo_h, req, 1, short_hdr, 1, NULL), ERR_INVALID_ARGS);
    CHECK_EQ(call_iov(echo_h, req, 0, reply, 1, NULL), ERR_INVALID_ARGS);
    CHECK_EQ(call_iov(echo_h, req, 1, reply, 0, NULL), ERR_INVALID_ARGS);

    // a header that disagrees with the iovecs, and an op only the kernel may send
    hdr.len = 5;
    CHECK_EQ(call_iov(echo_h, req, 1, reply, 1, NULL), ERR_INVALID_ARGS);
    hdr.len = 0;
    hdr.op = LK_OP_CONNECT;
    CHECK_EQ(call_iov(echo_h, req, 1, reply, 1, NULL), ERR_INVALID_ARGS);
    hdr.op = LK_ECHO_ECHO;
    hdr.nhandles = 1;
    CHECK_EQ(call_iov(echo_h, req, 1, reply, 1, NULL), ERR_INVALID_ARGS);
    hdr.nhandles = 0;

    // carrying handles comes later; asking for more reply handles than a message can carry
    struct lk_call_args args = {
        .req = req, .nreq = 1, .reply = reply, .nreply = 1, .timeout_ns = LK_TIME_INFINITE,
    };
    args.handles = &console_h;
    args.nhandles = 1;
    CHECK_EQ(lk_call(echo_h, &args), ERR_NOT_SUPPORTED);
    args.handles = NULL;
    args.nhandles = 0;
    args.reply_nhandles = LK_MSG_MAX_HANDLES + 1;
    CHECK_EQ(lk_call(echo_h, &args), ERR_INVALID_ARGS);
    args.reply_nhandles = LK_MSG_MAX_HANDLES;
    CHECK_EQ(lk_call(echo_h, &args), NO_ERROR);
    CHECK_EQ(args.reply_len, sizeof(rhdr));

    // a fault on the caller's buffers while the service runs fails the call: past the
    // prefix in the request, in the reply's payload, in the reply's header
    void *hole = unmapped_page();
    fill(buf, sizeof(buf), 1);
    hdr.len = sizeof(buf) + 16;
    const struct lk_iov late_fault[] = {{&hdr, sizeof(hdr)}, {buf, sizeof(buf)}, {hole, 16}};
    const struct lk_iov roomy[] = {{&rhdr, sizeof(rhdr)}, {rbuf, sizeof(rbuf)}};
    CHECK_EQ(call_iov(echo_h, late_fault, 3, roomy, 2, NULL), ERR_FAULT);
    hdr.len = 32;
    const struct lk_iov small[] = {{&hdr, sizeof(hdr)}, {buf, 32}};
    const struct lk_iov payload_fault[] = {{&rhdr, sizeof(rhdr)}, {hole, 64}};
    CHECK_EQ(call_iov(echo_h, small, 2, payload_fault, 2, NULL), ERR_FAULT);
    hdr.len = 0;
    const struct lk_iov header_fault[] = {{hole, sizeof(rhdr)}};
    CHECK_EQ(call_iov(echo_h, req, 1, header_fault, 1, NULL), ERR_FAULT);

    // the kernel honors page protections: no reply into read-only memory
    void *ro = NULL;
    CHECK_EQ(lk_vm_alloc(vm_h, PAGE, LK_VM_PROT_READ, &ro), NO_ERROR);
    hdr.len = 32;
    const struct lk_iov ro_reply[] = {{&rhdr, sizeof(rhdr)}, {ro, 64}};
    CHECK_EQ(call_iov(echo_h, small, 2, ro_reply, 2, NULL), ERR_FAULT);
    CHECK(all_bytes(ro, PAGE, 0));
    CHECK_EQ(lk_vm_unmap(vm_h, ro), NO_ERROR);

    // no call is in flight, so no token names one
    CHECK_EQ(lk_msg_read(1, 0, NULL, 0), ERR_NOT_FOUND);
    CHECK_EQ(lk_msg_write(1, 0, NULL, 0), ERR_NOT_FOUND);

    say("CALLTEST: transport checks ok\n");
}

static void test_echo(void) {
    uint8_t *mem = NULL;
    CHECK_EQ(lk_vm_alloc(vm_h, 2 * BIG, LK_VM_PROT_READ | LK_VM_PROT_WRITE, (void **)&mem),
             NO_ERROR);
    if (!mem) {
        return;
    }
    uint8_t *req = mem;
    uint8_t *rep = mem + BIG;
    CHECK(all_bytes(mem, 2 * BIG, 0));
    fill(req, BIG, 3);

    // 64KB out and back: the service reads and writes it piece by piece
    struct lk_msg_hdr hdr = {.op = LK_ECHO_ECHO, .len = BIG};
    struct lk_msg_hdr rhdr = {0};
    const struct lk_iov big_req[] = {{&hdr, sizeof(hdr)}, {req, BIG}};
    const struct lk_iov big_reply[] = {{&rhdr, sizeof(rhdr)}, {rep, BIG}};
    size_t reply_len;
    CHECK_EQ(call_iov(echo_h, big_req, 2, big_reply, 2, &reply_len), NO_ERROR);
    CHECK_EQ(rhdr.status, NO_ERROR);
    CHECK_EQ(rhdr.len, BIG);
    CHECK_EQ(reply_len, sizeof(rhdr) + BIG);
    CHECK(same(req, rep, BIG));

    // the same bytes cut at odd places on both sides, with an empty piece and both headers
    // split
    for (size_t i = 0; i < BIG; i++) {
        rep[i] = 0;
    }
    rhdr = (struct lk_msg_hdr){0};
    const struct lk_iov cut_req[] = {
        {&hdr, 6}, {(char *)&hdr + 6, 10}, {req, 1000}, {req, 0}, {req + 1000, BIG - 1000},
    };
    const struct lk_iov cut_reply[] = {
        {&rhdr, 10}, {(char *)&rhdr + 10, 6}, {rep, 333}, {rep + 333, BIG - 333},
    };
    CHECK_EQ(call_iov(echo_h, cut_req, 5, cut_reply, 4, &reply_len), NO_ERROR);
    CHECK_EQ(rhdr.status, NO_ERROR);
    CHECK_EQ(rhdr.len, BIG);
    CHECK_EQ(reply_len, sizeof(rhdr) + BIG);
    CHECK(same(req, rep, BIG));

    // a reply buffer smaller than the request: what fits, and the service says so
    for (size_t i = 0; i < 1100; i++) {
        rep[i] = 0xee;
    }
    hdr.len = 4096;
    const struct lk_iov trunc_req[] = {{&hdr, sizeof(hdr)}, {req, 4096}};
    const struct lk_iov trunc_reply[] = {{&rhdr, sizeof(rhdr)}, {rep, 1000}};
    CHECK_EQ(call_iov(echo_h, trunc_req, 2, trunc_reply, 2, &reply_len), NO_ERROR);
    CHECK_EQ(rhdr.status, ERR_NOT_ENOUGH_BUFFER);
    CHECK_EQ(rhdr.len, 1000);
    CHECK_EQ(reply_len, sizeof(rhdr) + 1000);
    CHECK(same(req, rep, 1000));
    CHECK(all_bytes(rep + 1000, 100, 0xee));

    CHECK_EQ(lk_vm_unmap(vm_h, mem), NO_ERROR);
    say("CALLTEST: 64KB echo ok\n");
}

static void test_vm(void) {
    // fresh memory reads as zero and holds what is written
    uint8_t *p = NULL;
    CHECK_EQ(lk_vm_alloc(vm_h, 3 * PAGE, LK_VM_PROT_READ | LK_VM_PROT_WRITE, (void **)&p),
             NO_ERROR);
    if (p) {
        CHECK(((uintptr_t)p & (PAGE - 1)) == 0);
        CHECK(all_bytes(p, 3 * PAGE, 0));
        fill(p, 3 * PAGE, 5);
        uint8_t expect[64];
        fill(expect, sizeof(expect), 5);
        CHECK(same(p, expect, sizeof(expect)));
        CHECK_EQ(p[3 * PAGE - 1], (uint8_t)((3 * PAGE - 1) * 7 + 5));
        CHECK_EQ(lk_vm_unmap(vm_h, p + PAGE), NO_ERROR);
        CHECK_EQ(lk_vm_unmap(vm_h, p), ERR_NOT_FOUND);
    }
    // the pmm hands the pages just freed straight back, still holding the pattern unless
    // the service zeroes them
    p = NULL;
    CHECK_EQ(lk_vm_alloc(vm_h, 3 * PAGE, LK_VM_PROT_READ | LK_VM_PROT_WRITE, (void **)&p),
             NO_ERROR);
    if (p) {
        CHECK(all_bytes(p, 3 * PAGE, 0));
        CHECK_EQ(lk_vm_unmap(vm_h, p), NO_ERROR);
    }

    // what the service refuses
    void *q;
    CHECK_EQ(lk_vm_alloc(vm_h, 0, LK_VM_PROT_READ, &q), ERR_INVALID_ARGS);
    CHECK_EQ(lk_vm_alloc(vm_h, PAGE, 0, &q), ERR_INVALID_ARGS);
    CHECK_EQ(lk_vm_alloc(vm_h, PAGE, LK_VM_PROT_WRITE, &q), ERR_INVALID_ARGS);
    CHECK_EQ(lk_vm_alloc(vm_h, PAGE, LK_VM_PROT_READ | (1u << 3), &q), ERR_INVALID_ARGS);
    struct lk_vm_alloc_req req = {.size = PAGE, .prot = LK_VM_PROT_READ, .flags = 1};
    struct lk_vm_alloc_reply reply;
    CHECK_EQ(lk_msg_call(vm_h, LK_VM_ALLOC, &req, sizeof(req), &reply, sizeof(reply), NULL),
             ERR_INVALID_ARGS);
    req.flags = 0;
    CHECK_EQ(lk_msg_call(vm_h, LK_VM_ALLOC, &req, 8, &reply, sizeof(reply), NULL),
             ERR_INVALID_ARGS);
    // where the address could not be returned nothing is allocated
    CHECK_EQ(lk_msg_call(vm_h, LK_VM_ALLOC, &req, sizeof(req), &reply, 4, NULL),
             ERR_NOT_ENOUGH_BUFFER);
    CHECK_EQ(lk_vm_unmap(vm_h, unmapped_page()), ERR_NOT_FOUND);

    // more than the machine has, and more than any address space holds: refused, and the
    // kernel carries on
    CHECK_EQ(lk_vm_alloc(vm_h, (size_t)1 << 30, LK_VM_PROT_READ, &q), ERR_NO_MEMORY);
    CHECK_EQ(lk_vm_alloc(vm_h, (size_t)1 << 62, LK_VM_PROT_READ, &q), ERR_NO_MEMORY);
    q = NULL;
    CHECK_EQ(lk_vm_alloc(vm_h, PAGE, LK_VM_PROT_READ, &q), NO_ERROR);
    CHECK_EQ(lk_vm_unmap(vm_h, q), NO_ERROR);

    say("CALLTEST: vm ok\n");
}

static void test_time(void) {
    const uint64_t sleep_ns = 10 * 1000 * 1000;
    uint64_t t0 = 0, t1 = 0;
    CHECK_EQ(lk_time_clock_read(time_h, LK_CLOCK_MONOTONIC, &t0), NO_ERROR);
    CHECK_EQ(lk_time_sleep(time_h, sleep_ns), NO_ERROR);
    CHECK_EQ(lk_time_clock_read(time_h, LK_CLOCK_MONOTONIC, &t1), NO_ERROR);
    // only a lower bound: an emulator's timing jitters
    CHECK(t1 > t0 && t1 - t0 >= sleep_ns);
    // and not a whole number of LK's milliseconds
    const uint64_t odd_ns = 1500 * 1000;
    CHECK_EQ(lk_time_clock_read(time_h, LK_CLOCK_MONOTONIC, &t0), NO_ERROR);
    CHECK_EQ(lk_time_sleep(time_h, odd_ns), NO_ERROR);
    CHECK_EQ(lk_time_clock_read(time_h, LK_CLOCK_MONOTONIC, &t1), NO_ERROR);
    CHECK(t1 - t0 >= odd_ns);

    CHECK_EQ(lk_time_clock_read(time_h, 7, &t1), ERR_NOT_SUPPORTED);
    CHECK_EQ(lk_msg_call(time_h, LK_TIME_SLEEP, &sleep_ns, 4, NULL, 0, NULL), ERR_INVALID_ARGS);

    say("CALLTEST: time ok\n");
}

static void test_process(void) {
    struct lk_process_info info = {0};
    CHECK_EQ(lk_process_info(process_h, &info), NO_ERROR);
    CHECK(info.pid > 0);
    CHECK_EQ(info.state, LK_PROCESS_RUNNING);

    CHECK_EQ(lk_msg_call(process_h, LK_PROCESS_INFO, &info, 4, &info, sizeof(info), NULL),
             ERR_INVALID_ARGS);
    CHECK_EQ(lk_msg_call(process_h, LK_PROCESS_INFO, NULL, 0, &info, 8, NULL),
             ERR_NOT_ENOUGH_BUFFER);

    say("CALLTEST: process ok\n");
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    test_put("CALLTEST: start\n");

    test_startup(args);
    test_console();
    test_handles();
    test_endpoints();
    test_transport();
    test_echo();
    test_vm();
    test_time();
    test_process();

    test_check_failures();
    say("CALLTEST: PASS\n");
    lk_exit(0);
}
