/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Native API test with no libc: calls the kernel services over the connections the process is
// born with, and checks handles and their rights, endpoints, how the call path treats every
// malformed call and bad buffer it can be given, handles carried by requests and replies, files
// and directories through the namespace and file services, and memory objects. Progress lines and CALLTEST: PASS go
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
// made and removed again by the file tests, in the image's root
#define TMP_FILE "/calltest.tmp"
#define TMP_DIR  "/calltest.dir"
#define MEM_FILE "/calltest.mem"
// no table has this many slots
#define NOT_A_HANDLE 0xffffffffu

static lk_handle_t console_h, vm_h, time_h, process_h, echo_h, ns_h;
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

// equal ignoring ASCII case: FAT keeps names in either
static int name_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) {
            return 0;
        }
        if (!x) {
            return 1;
        }
    }
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
    ns_h = lk_proc_args_find(args, LK_TAG_NAMESPACE);
    startup_handles = args->nhandles;
    CHECK(console_h != LK_HANDLE_INVALID);
    CHECK(vm_h != LK_HANDLE_INVALID);
    CHECK(time_h != LK_HANDLE_INVALID);
    CHECK(process_h != LK_HANDLE_INVALID);
    CHECK(echo_h != LK_HANDLE_INVALID);
    CHECK(ns_h != LK_HANDLE_INVALID);
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

    // reading waits for a line, but not when there is no room for one; the gate's input is
    // closed, so the rest of READ is checked by hand
    char line[4];
    size_t len = 99;
    CHECK_EQ(lk_console_read(console_h, line, 0, &len), NO_ERROR);
    CHECK_EQ(len, 0);
    CHECK_EQ(lk_msg_call(console_h, LK_CONSOLE_READ, NULL, 0, line, sizeof(line), NULL),
             ERR_INVALID_ARGS);

    uint32_t cols = 0, rows = 0;
    CHECK_EQ(lk_console_winsize(console_h, &cols, &rows), NO_ERROR);
    CHECK(cols > 0 && rows > 0);
    CHECK_EQ(lk_msg_call(console_h, LK_CONSOLE_WINSIZE, line, 1, line, sizeof(line), NULL),
             ERR_INVALID_ARGS);
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

    // asking for more reply handles than a message can carry; room for reply handles that
    // cannot be written
    lk_handle_t rh[LK_MSG_MAX_HANDLES];
    struct lk_call_args args = {
        .req = req, .nreq = 1, .reply = reply, .nreply = 1, .timeout_ns = LK_TIME_INFINITE,
    };
    args.reply_nhandles = LK_MSG_MAX_HANDLES + 1;
    args.reply_handles = rh;
    CHECK_EQ(lk_call(echo_h, &args), ERR_INVALID_ARGS);
    args.reply_nhandles = LK_MSG_MAX_HANDLES;
    args.reply_handles = NULL;
    CHECK_EQ(lk_call(echo_h, &args), ERR_FAULT);
    // the room the reply did not use reads as no handle
    args.reply_handles = rh;
    for (int i = 0; i < LK_MSG_MAX_HANDLES; i++) {
        rh[i] = NOT_A_HANDLE;
    }
    CHECK_EQ(lk_call(echo_h, &args), NO_ERROR);
    CHECK_EQ(args.reply_len, sizeof(rhdr));
    CHECK_EQ(rhdr.nhandles, 0);
    for (int i = 0; i < LK_MSG_MAX_HANDLES; i++) {
        CHECK_EQ(rh[i], LK_HANDLE_INVALID);
    }

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

// A call with no payload carrying n handles with flags (LK_HANDLE_LEND): the transport's status,
// and the service's in *status.
static lk_status_t call_carrying(lk_handle_t conn, uint32_t op, const lk_handle_t *hs, size_t n,
                                 uint32_t flags, lk_status_t *status) {
    struct lk_msg_hdr hdr = {.op = op, .nhandles = (uint32_t)n};
    struct lk_msg_hdr rhdr = {.status = 1};
    const struct lk_iov req = {&hdr, sizeof(hdr)};
    const struct lk_iov reply = {&rhdr, sizeof(rhdr)};
    struct lk_call_args args = {
        .req = &req, .nreq = 1, .handles = hs, .nhandles = n, .reply = &reply, .nreply = 1,
        .handle_flags = flags, .timeout_ns = LK_TIME_INFINITE,
    };
    lk_status_t err = lk_call(conn, &args);
    *status = rhdr.status;
    return err;
}

// Handles a request carries: the transport looks each up before the service runs, a moved one
// needs TRANSFER, and a service that refuses the call moves nothing. The echo service takes no
// handles, so it refuses every request that carries one.
static void test_carried(void) {
    lk_handle_t dup, weak;
    lk_handle_t hs[LK_MSG_MAX_HANDLES + 1];
    lk_status_t st;
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL, &dup), NO_ERROR);
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL & ~LK_RIGHT_TRANSFER, &weak), NO_ERROR);
    for (int i = 0; i <= LK_MSG_MAX_HANDLES; i++) {
        hs[i] = dup;
    }

    // too many, a flag past the last, an unreadable array, no such handle
    CHECK_EQ(call_carrying(echo_h, LK_ECHO_ECHO, hs, LK_MSG_MAX_HANDLES + 1, ~0u, &st),
             ERR_INVALID_ARGS);
    CHECK_EQ(call_carrying(echo_h, LK_ECHO_ECHO, hs, 1, LK_HANDLE_LEND(0) | LK_HANDLE_LEND(1), &st),
             ERR_INVALID_ARGS);
    CHECK_EQ(call_carrying(echo_h, LK_ECHO_ECHO, (const lk_handle_t *)0x1000, 1, 0, &st), ERR_FAULT);
    const lk_handle_t bogus = NOT_A_HANDLE;
    CHECK_EQ(call_carrying(echo_h, LK_ECHO_ECHO, &bogus, 1, LK_HANDLE_LEND(0), &st), ERR_BAD_HANDLE);

    // moving needs TRANSFER, lending nothing
    CHECK_EQ(call_carrying(echo_h, LK_ECHO_ECHO, &weak, 1, 0, &st), ERR_ACCESS_DENIED);
    CHECK_EQ(call_carrying(echo_h, LK_ECHO_ECHO, &weak, 1, LK_HANDLE_LEND(0), &st), NO_ERROR);
    CHECK_EQ(st, ERR_INVALID_ARGS);

    // one handle moved and named again in the same request; lent twice is no move
    CHECK_EQ(call_carrying(echo_h, LK_ECHO_ECHO, hs, 2, 0, &st), ERR_INVALID_ARGS);
    CHECK_EQ(call_carrying(echo_h, LK_ECHO_ECHO, hs, 2, LK_HANDLE_LEND(0), &st), ERR_INVALID_ARGS);
    CHECK_EQ(call_carrying(echo_h, LK_ECHO_ECHO, hs, 2, LK_HANDLE_LEND(0) | LK_HANDLE_LEND(1), &st),
             NO_ERROR);
    CHECK_EQ(st, ERR_INVALID_ARGS);

    // refused, so still here
    CHECK_EQ(call_carrying(echo_h, LK_ECHO_ECHO, &dup, 1, 0, &st), NO_ERROR);
    CHECK_EQ(st, ERR_INVALID_ARGS);
    CHECK_EQ(lk_handle_close(dup), NO_ERROR);
    CHECK_EQ(lk_handle_close(weak), NO_ERROR);

    say("CALLTEST: carried handles ok\n");
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
    // about another process by a handle with INFO, which a connection lacks
    CHECK_EQ(lk_process_info_of(process_h, console_h, &info), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_process_kill(process_h, console_h), ERR_ACCESS_DENIED);

    say("CALLTEST: process ok\n");
}

// A namespace request built by hand: flags, the path_len it claims, and len bytes of path.
static lk_status_t ns_req(struct lk_msg *m, uint32_t flags, uint32_t path_len, const char *path,
                          size_t len) {
    const struct lk_ns_path_req req = {.flags = flags, .path_len = path_len};
    const struct lk_iov iov[2] = {{(void *)&req, sizeof(req)}, {(void *)path, len}};
    m->req = iov;
    m->nreq = len ? 2 : 1;
    return lk_msg_callv(ns_h, m);
}

static lk_status_t ns_open_path(struct lk_msg *m, uint32_t flags, const char *path) {
    const uint32_t len = (uint32_t)test_strlen(path);
    return ns_req(m, flags, len, path, len);
}

// what namespace.OPEN's flags and paths may be
static lk_status_t open_status(uint32_t flags, const char *path) {
    lk_handle_t h = LK_HANDLE_INVALID;
    lk_status_t err = lk_ns_open(ns_h, path, flags, &h, NULL);
    if (err == NO_ERROR) {
        lk_handle_close(h);
    }
    return err;
}

static void test_reply_handles(void) {
    struct lk_file_stat st;
    lk_handle_t rh[LK_MSG_MAX_HANDLES];
    lk_handle_t f;
    lk_ns_unlink(ns_h, TMP_FILE);

    // a reply's handle lands in the caller's array, the rest of which reads as no handle
    for (int i = 0; i < LK_MSG_MAX_HANDLES; i++) {
        rh[i] = NOT_A_HANDLE;
    }
    const struct lk_iov reply = {&st, sizeof(st)};
    struct lk_msg m = {
        .op = LK_NS_OPEN, .reply = &reply, .nreply = 1, .handles = rh,
        .handles_max = LK_MSG_MAX_HANDLES,
    };
    CHECK_EQ(ns_open_path(&m, LK_NS_OPEN_READ, "/bin/calltest"), NO_ERROR);
    CHECK_EQ(m.nhandles, 1);
    CHECK(rh[0] != LK_HANDLE_INVALID && rh[0] != NOT_A_HANDLE);
    for (int i = 1; i < LK_MSG_MAX_HANDLES; i++) {
        CHECK_EQ(rh[i], LK_HANDLE_INVALID);
    }
    CHECK_EQ(lk_file_fstat(rh[0], &st), NO_ERROR);
    CHECK_EQ(lk_handle_close(rh[0]), NO_ERROR);

    // a refusal carries none
    rh[0] = NOT_A_HANDLE;
    CHECK_EQ(ns_open_path(&m, LK_NS_OPEN_READ, "/no/such/file"), ERR_NOT_FOUND);
    CHECK_EQ(m.nhandles, 0);
    CHECK_EQ(rh[0], LK_HANDLE_INVALID);

    // with no room for the handle nothing is created
    m.handles_max = 0;
    CHECK_EQ(ns_open_path(&m, LK_NS_OPEN_CREATE | LK_NS_OPEN_WRITE, TMP_FILE),
             ERR_NOT_ENOUGH_BUFFER);
    CHECK_EQ(lk_ns_stat(ns_h, TMP_FILE, &st), ERR_NOT_FOUND);
    // nor where the room cannot be written: the call fails before the service runs
    void *ro = NULL;
    CHECK_EQ(lk_vm_alloc(vm_h, PAGE, LK_VM_PROT_READ, &ro), NO_ERROR);
    m.handles = ro;
    m.handles_max = 1;
    CHECK_EQ(ns_open_path(&m, LK_NS_OPEN_CREATE | LK_NS_OPEN_WRITE, TMP_FILE), ERR_FAULT);
    CHECK_EQ(lk_ns_stat(ns_h, TMP_FILE, &st), ERR_NOT_FOUND);
    CHECK_EQ(lk_vm_unmap(vm_h, ro), NO_ERROR);

    // a call that fails after the handle went into the table takes it back: the reply's
    // payload faults once the service has opened the file, and the slot is free again
    lk_handle_t probe;
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL, &probe), NO_ERROR);
    CHECK_EQ(lk_handle_close(probe), NO_ERROR);
    const struct lk_iov hole = {unmapped_page(), sizeof(st)};
    m.reply = &hole;
    m.handles = rh;
    CHECK_EQ(ns_open_path(&m, LK_NS_OPEN_CREATE | LK_NS_OPEN_WRITE, TMP_FILE), ERR_FAULT);
    m.reply = &reply;
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL, &f), NO_ERROR);
    CHECK_EQ(f, probe);
    CHECK_EQ(lk_handle_close(f), NO_ERROR);
    CHECK_EQ(lk_ns_unlink(ns_h, TMP_FILE), NO_ERROR);

    // with a full table the service says so, and the file it had opened is closed again:
    // unlink refuses a file that is open
    CHECK_EQ(lk_ns_open(ns_h, TMP_FILE, LK_NS_OPEN_CREATE | LK_NS_OPEN_WRITE, &f, NULL),
             NO_ERROR);
    CHECK_EQ(lk_ns_unlink(ns_h, TMP_FILE), ERR_BUSY);
    CHECK_EQ(lk_handle_close(f), NO_ERROR);
    lk_handle_t dups[HANDLE_SLOTS];
    uint32_t n = 0;
    while (n < HANDLE_SLOTS && lk_handle_dup(console_h, LK_RIGHTS_ALL, &dups[n]) == NO_ERROR) {
        n++;
    }
    m.handles = rh;
    rh[0] = NOT_A_HANDLE;
    CHECK_EQ(ns_open_path(&m, LK_NS_OPEN_READ, TMP_FILE), ERR_NO_RESOURCES);
    CHECK_EQ(m.nhandles, 0);
    CHECK_EQ(rh[0], LK_HANDLE_INVALID);
    while (n > 0) {
        CHECK_EQ(lk_handle_close(dups[--n]), NO_ERROR);
    }
    CHECK_EQ(lk_ns_unlink(ns_h, TMP_FILE), NO_ERROR);

    say("CALLTEST: reply handles ok\n");
}

static void test_files(void) {
    lk_handle_t f, h;
    struct lk_file_stat st;
    uint8_t buf[600];
    uint8_t back[700];
    char name[LK_NAME_MAX + 1];
    size_t n;
    uint64_t end;

    // what a run that died part way may have left
    lk_ns_unlink(ns_h, TMP_FILE);
    lk_ns_unlink(ns_h, TMP_DIR "/a");
    lk_ns_rmdir(ns_h, TMP_DIR);

    // an image file: at an offset, up to the end, past it
    CHECK_EQ(lk_ns_open(ns_h, "/bin/calltest", LK_NS_OPEN_READ, &f, &st), NO_ERROR);
    CHECK_EQ(st.type, LK_FILE_TYPE_FILE);
    CHECK(st.size > sizeof(buf));
    CHECK_EQ(lk_file_read(f, 0, buf, sizeof(buf), &n), NO_ERROR);
    CHECK_EQ(n, sizeof(buf));
    CHECK(buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F');
    CHECK_EQ(lk_file_read(f, 500, back, 100, &n), NO_ERROR);
    CHECK_EQ(n, 100);
    CHECK(same(buf + 500, back, 100));
    CHECK_EQ(lk_file_read(f, st.size - 10, back, sizeof(back), &n), NO_ERROR);
    CHECK_EQ(n, 10);
    CHECK_EQ(lk_file_read(f, st.size, back, sizeof(back), &n), NO_ERROR);
    CHECK_EQ(n, 0);
    // opened only for reading, and not a directory
    CHECK_EQ(lk_file_write(f, 0, 0, "x", 1, NULL, NULL), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_file_truncate(f, 0), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_file_readdir(f, 0, name, sizeof(name), NULL), ERR_NOT_DIR);
    CHECK_EQ(lk_msg_call(f, 99, NULL, 0, NULL, 0, NULL), ERR_NOT_SUPPORTED);
    // the connection's rights work like any other's
    CHECK_EQ(lk_handle_dup(f, LK_RIGHTS_ALL & ~LK_RIGHT_CALL, &h), NO_ERROR);
    CHECK_EQ(lk_file_fstat(h, &st), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);
    CHECK_EQ(lk_handle_close(f), NO_ERROR);

    // a file of its own: written, read back, appended to, cut short
    CHECK_EQ(lk_ns_open(ns_h, TMP_FILE,
                        LK_NS_OPEN_CREATE | LK_NS_OPEN_EXCL | LK_NS_OPEN_READ | LK_NS_OPEN_WRITE,
                        &f, &st),
             NO_ERROR);
    CHECK_EQ(st.size, 0);
    fill(buf, sizeof(buf), 9);
    CHECK_EQ(lk_file_write(f, 0, 0, buf, sizeof(buf), &n, &end), NO_ERROR);
    CHECK_EQ(n, sizeof(buf));
    CHECK_EQ(end, sizeof(buf));
    CHECK_EQ(lk_file_write(f, 0, LK_FILE_WRITE_APPEND, buf, 100, &n, &end), NO_ERROR);
    CHECK_EQ(n, 100);
    CHECK_EQ(end, sizeof(buf) + 100);
    CHECK_EQ(lk_file_read(f, 0, back, sizeof(back), &n), NO_ERROR);
    CHECK_EQ(n, sizeof(buf) + 100);
    CHECK(same(back, buf, sizeof(buf)));
    CHECK(same(back + sizeof(buf), buf, 100));
    CHECK_EQ(lk_file_truncate(f, 300), NO_ERROR);
    CHECK_EQ(lk_file_fstat(f, &st), NO_ERROR);
    CHECK_EQ(st.size, 300);
    CHECK_EQ(lk_file_write(f, 0, 1u << 5, buf, 1, NULL, NULL), ERR_INVALID_ARGS);
    CHECK_EQ(lk_file_read(f, (uint64_t)1 << 63, back, 1, &n), ERR_INVALID_ARGS);
    // a write whose count could not come back writes nothing
    CHECK_EQ(lk_msg_call(f, LK_FILE_WRITE, &(struct lk_file_write_req){.off = 0}, 16, NULL, 0,
                         NULL),
             ERR_NOT_ENOUGH_BUFFER);
    // in use, and there already
    CHECK_EQ(lk_ns_unlink(ns_h, TMP_FILE), ERR_BUSY);
    CHECK_EQ(open_status(LK_NS_OPEN_CREATE | LK_NS_OPEN_EXCL | LK_NS_OPEN_WRITE, TMP_FILE),
             ERR_ALREADY_EXISTS);
    CHECK_EQ(lk_handle_close(f), NO_ERROR);
    CHECK_EQ(lk_ns_stat(ns_h, TMP_FILE, &st), NO_ERROR);
    CHECK_EQ(st.type, LK_FILE_TYPE_FILE);
    CHECK_EQ(st.size, 300);
    CHECK_EQ(lk_ns_open(ns_h, TMP_FILE, LK_NS_OPEN_WRITE | LK_NS_OPEN_TRUNC, &f, &st), NO_ERROR);
    CHECK_EQ(st.size, 0);
    CHECK_EQ(lk_file_read(f, 0, back, 1, &n), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(f), NO_ERROR);
    CHECK_EQ(lk_ns_unlink(ns_h, TMP_FILE), NO_ERROR);
    CHECK_EQ(lk_ns_stat(ns_h, TMP_FILE, &st), ERR_NOT_FOUND);

    // a directory: listed, refused as a file and the other way around, removed once empty
    CHECK_EQ(lk_ns_mkdir(ns_h, TMP_DIR), NO_ERROR);
    CHECK_EQ(lk_ns_mkdir(ns_h, TMP_DIR), ERR_ALREADY_EXISTS);
    CHECK_EQ(lk_ns_stat(ns_h, TMP_DIR, &st), NO_ERROR);
    CHECK_EQ(st.type, LK_FILE_TYPE_DIR);
    CHECK_EQ(open_status(LK_NS_OPEN_CREATE | LK_NS_OPEN_WRITE, TMP_DIR "/a"), NO_ERROR);
    CHECK_EQ(open_status(LK_NS_OPEN_READ, TMP_DIR), ERR_NOT_FILE);
    CHECK_EQ(open_status(LK_NS_OPEN_DIR, TMP_DIR "/a"), ERR_NOT_DIR);
    CHECK_EQ(lk_ns_open(ns_h, TMP_DIR, LK_NS_OPEN_DIR, &h, &st), NO_ERROR);
    CHECK_EQ(st.type, LK_FILE_TYPE_DIR);
    int found = 0;
    uint32_t i = 0;
    lk_status_t err;
    while ((err = lk_file_readdir(h, i, name, sizeof(name), &n)) == NO_ERROR) {
        CHECK_EQ(n, test_strlen(name));
        if (name_eq(name, "a")) {
            found++;
        } else {
            CHECK(test_streq(name, ".") || test_streq(name, ".."));
        }
        i++;
    }
    CHECK_EQ(err, ERR_NOT_FOUND);
    CHECK_EQ(found, 1);
    // going back to an earlier entry, and a name with no room
    CHECK_EQ(lk_file_readdir(h, 0, name, sizeof(name), NULL), NO_ERROR);
    CHECK_EQ(lk_file_readdir(h, 0, name, 1, NULL), ERR_NOT_ENOUGH_BUFFER);
    CHECK_EQ(lk_file_read(h, 0, back, 1, &n), ERR_NOT_FILE);
    CHECK_EQ(lk_file_fstat(h, &st), NO_ERROR);
    CHECK_EQ(st.type, LK_FILE_TYPE_DIR);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);
    CHECK_EQ(lk_ns_rmdir(ns_h, TMP_DIR), ERR_NOT_ALLOWED);
    CHECK_EQ(lk_ns_unlink(ns_h, TMP_DIR), ERR_NOT_FILE);
    CHECK_EQ(lk_ns_rmdir(ns_h, TMP_DIR "/a"), ERR_NOT_DIR);
    CHECK_EQ(lk_ns_unlink(ns_h, TMP_DIR "/a"), NO_ERROR);
    CHECK_EQ(lk_ns_rmdir(ns_h, TMP_DIR), NO_ERROR);
    CHECK_EQ(lk_ns_stat(ns_h, TMP_DIR, &st), ERR_NOT_FOUND);

    say("CALLTEST: files ok\n");
}

static void test_paths(void) {
    struct lk_file_stat st;
    struct lk_msg m = {.op = LK_NS_STAT};
    const struct lk_iov reply = {&st, sizeof(st)};
    m.reply = &reply;
    m.nreply = 1;

    // normalized before the root goes in front: ".." stops at the root, so a path climbing
    // out of it and back in names nothing
    CHECK_EQ(lk_ns_stat(ns_h, "/bin/../bin/./calltest", &st), NO_ERROR);
    CHECK_EQ(lk_ns_stat(ns_h, "/../bin/calltest", &st), NO_ERROR);
    CHECK_EQ(lk_ns_stat(ns_h, "/../fs/bin/calltest", &st), ERR_NOT_FOUND);
    CHECK_EQ(lk_ns_stat(ns_h, "/", &st), NO_ERROR);
    CHECK_EQ(st.type, LK_FILE_TYPE_DIR);

    // relative, empty, holding a NUL, a length that disagrees, too long
    CHECK_EQ(lk_ns_stat(ns_h, "bin/calltest", &st), ERR_BAD_PATH);
    CHECK_EQ(ns_req(&m, 0, 0, "", 0), ERR_BAD_PATH);
    CHECK_EQ(ns_req(&m, 0, 6, "/bin\0x", 6), ERR_BAD_PATH);
    CHECK_EQ(ns_req(&m, 0, 5, "/bin", 4), ERR_INVALID_ARGS);
    char longpath[201];
    for (int i = 0; i < 200; i++) {
        longpath[i] = i % 8 ? 'a' : '/';
    }
    longpath[200] = '\0';
    CHECK_EQ(lk_ns_stat(ns_h, longpath, &st), ERR_TOO_BIG);
    CHECK_EQ(lk_msg_call(ns_h, LK_NS_STAT, "ab", 2, &st, sizeof(st), NULL), ERR_INVALID_ARGS);

    // flags: only OPEN takes them, and only these mixes
    CHECK_EQ(ns_req(&m, LK_NS_OPEN_READ, 4, "/bin", 4), ERR_INVALID_ARGS);
    CHECK_EQ(open_status(1u << 6 | LK_NS_OPEN_READ, "/bin/calltest"), ERR_INVALID_ARGS);
    CHECK_EQ(open_status(0, "/bin/calltest"), ERR_INVALID_ARGS);
    CHECK_EQ(open_status(LK_NS_OPEN_DIR | LK_NS_OPEN_READ, "/bin"), ERR_INVALID_ARGS);
    CHECK_EQ(open_status(LK_NS_OPEN_READ | LK_NS_OPEN_EXCL, "/bin/calltest"), ERR_INVALID_ARGS);
    CHECK_EQ(open_status(LK_NS_OPEN_READ | LK_NS_OPEN_TRUNC, "/bin/calltest"), ERR_INVALID_ARGS);
    CHECK_EQ(open_status(LK_NS_OPEN_DIR, "/bin"), NO_ERROR);
    CHECK_EQ(lk_msg_call(ns_h, 99, NULL, 0, NULL, 0, NULL), ERR_NOT_SUPPORTED);

    say("CALLTEST: paths ok\n");
}

static lk_status_t map_status(lk_handle_t m, size_t off, size_t len, uint32_t prot) {
    void *va;
    lk_status_t err = lk_vm_memobj_map(vm_h, m, off, len, prot, &va);
    if (err == NO_ERROR) {
        CHECK_EQ(lk_vm_unmap(vm_h, va), NO_ERROR);
    }
    return err;
}

// MEMOBJ_MAP moving the handle rather than lending it
static lk_status_t map_moved(lk_handle_t m, void **va) {
    const struct lk_vm_memobj_map_req req = {.prot = LK_VM_PROT_READ};
    struct lk_vm_alloc_reply reply = {0};
    const struct lk_iov req_iov = {(void *)&req, sizeof(req)};
    const struct lk_iov reply_iov = {&reply, sizeof(reply)};
    struct lk_msg msg = {
        .op = LK_VM_MEMOBJ_MAP, .req = &req_iov, .nreq = 1, .reply = &reply_iov, .nreply = 1,
        .carry = &m, .ncarry = 1,
    };
    lk_status_t err = lk_msg_callv(vm_h, &msg);
    *va = (void *)(uintptr_t)reply.va;
    return err;
}

static void test_memobj(void) {
    const uint32_t r = LK_VM_PROT_READ;
    const uint32_t rw_prot = LK_VM_PROT_READ | LK_VM_PROT_WRITE;
    lk_handle_t m, h, f;
    size_t size, n;
    uint64_t end;
    uint8_t *rw, *ro;
    void *va;

    // made in whole pages, reading as zero; refused when empty, too large or with unknown flags
    CHECK_EQ(lk_vm_memobj_create(vm_h, 3 * PAGE + 1, 0, &m, &size), NO_ERROR);
    CHECK_EQ(size, 4 * PAGE);
    CHECK_EQ(lk_vm_memobj_create(vm_h, 0, 0, &h, NULL), ERR_INVALID_ARGS);
    CHECK_EQ(lk_vm_memobj_create(vm_h, PAGE, 0x80, &h, NULL), ERR_INVALID_ARGS);
    CHECK_EQ(lk_vm_memobj_create(vm_h, LK_VM_MEMOBJ_MAX + 1, 0, &h, NULL), ERR_TOO_BIG);

    // two mappings of the same pages, one of them read only and offset
    CHECK_EQ(lk_vm_memobj_map(vm_h, m, 0, 0, rw_prot, (void **)&rw), NO_ERROR);
    CHECK(all_bytes(rw, 4 * PAGE, 0));
    CHECK_EQ(lk_vm_memobj_map(vm_h, m, PAGE, 2 * PAGE, r, (void **)&ro), NO_ERROR);
    fill(rw, 4 * PAGE, 3);
    CHECK(same(ro, rw + PAGE, 2 * PAGE));

    // whole pages inside the object, READ always
    CHECK_EQ(map_status(m, 1, PAGE, r), ERR_INVALID_ARGS);
    CHECK_EQ(map_status(m, 0, PAGE + 1, r), ERR_INVALID_ARGS);
    CHECK_EQ(map_status(m, 4 * PAGE, 0, r), ERR_INVALID_ARGS);
    CHECK_EQ(map_status(m, 2 * PAGE, 3 * PAGE, r), ERR_INVALID_ARGS);
    CHECK_EQ(map_status(m, 0, 0, LK_VM_PROT_WRITE), ERR_INVALID_ARGS);

    // MAP, and the right of each protection; EXEC only on an object made with it
    CHECK_EQ(map_status(m, 0, 0, r | LK_VM_PROT_EXEC), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_dup(m, LK_RIGHTS_ALL & ~LK_RIGHT_WRITE, &h), NO_ERROR);
    CHECK_EQ(map_status(h, 0, 0, rw_prot), ERR_ACCESS_DENIED);
    CHECK_EQ(map_status(h, 0, 0, r), NO_ERROR);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);
    CHECK_EQ(lk_handle_dup(m, LK_RIGHTS_ALL & ~LK_RIGHT_MAP, &h), NO_ERROR);
    CHECK_EQ(map_status(h, 0, 0, r), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);
    CHECK_EQ(map_status(console_h, 0, 0, r), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_vm_memobj_create(vm_h, PAGE, LK_VM_MEMOBJ_EXEC, &h, NULL), NO_ERROR);
    CHECK_EQ(map_status(h, 0, 0, r | LK_VM_PROT_EXEC), NO_ERROR);

    // a handle moved to a service that accepts the call is gone; the mapping keeps the object
    CHECK_EQ(map_moved(h, &va), NO_ERROR);
    CHECK_EQ(lk_handle_close(h), ERR_BAD_HANDLE);
    CHECK(all_bytes(va, PAGE, 0));
    CHECK_EQ(lk_vm_unmap(vm_h, va), NO_ERROR);

    // Call arguments the kernel can read but not write: reply_len is written before the
    // service runs, so the call fails and the directory is never made.
    lk_ns_rmdir(ns_h, TMP_DIR);
    const struct lk_ns_path_req mkdir_req = {.path_len = sizeof(TMP_DIR) - 1};
    struct lk_msg_hdr hdr = {.op = LK_NS_MKDIR, .len = sizeof(mkdir_req) + sizeof(TMP_DIR) - 1};
    struct lk_msg_hdr rhdr;
    const struct lk_iov req[3] = {
        {&hdr, sizeof(hdr)},
        {(void *)&mkdir_req, sizeof(mkdir_req)},
        {(void *)TMP_DIR, sizeof(TMP_DIR) - 1},
    };
    const struct lk_iov reply = {&rhdr, sizeof(rhdr)};
    const struct lk_call_args args = {
        .req = req, .nreq = 3, .reply = &reply, .nreply = 1, .timeout_ns = LK_TIME_INFINITE,
    };
    // ro shows rw from its second page on
    struct lk_call_args *args_rw = (struct lk_call_args *)(rw + PAGE);
    *args_rw = args;
    CHECK_EQ(lk_call(ns_h, (struct lk_call_args *)ro), ERR_FAULT);
    struct lk_file_stat st;
    CHECK_EQ(lk_ns_stat(ns_h, TMP_DIR, &st), ERR_NOT_FOUND);
    // the same arguments where they can be written make it
    CHECK_EQ(lk_call(ns_h, args_rw), NO_ERROR);
    CHECK_EQ(rhdr.status, NO_ERROR);
    CHECK_EQ(lk_ns_rmdir(ns_h, TMP_DIR), NO_ERROR);

    // a file written from the object and read back into it
    lk_ns_unlink(ns_h, MEM_FILE);
    CHECK_EQ(lk_ns_open(ns_h, MEM_FILE,
                        LK_NS_OPEN_CREATE | LK_NS_OPEN_READ | LK_NS_OPEN_WRITE, &f, NULL),
             NO_ERROR);
    fill(rw, 4 * PAGE, 9);
    CHECK_EQ(lk_file_write_from(f, 0, 0, m, PAGE, 2 * PAGE, &n, &end), NO_ERROR);
    CHECK_EQ(n, 2 * PAGE);
    CHECK_EQ(end, 2 * PAGE);
    CHECK_EQ(lk_file_write_from(f, 0, LK_FILE_WRITE_APPEND, m, 0, 100, &n, &end), NO_ERROR);
    CHECK_EQ(end, 2 * PAGE + 100);
    for (size_t i = 0; i < PAGE; i++) {
        rw[3 * PAGE + i] = 0;
    }
    CHECK_EQ(lk_file_read_into(f, PAGE, m, 3 * PAGE, PAGE, &n), NO_ERROR);
    CHECK_EQ(n, PAGE);
    CHECK(same(rw + 3 * PAGE, rw + 2 * PAGE, PAGE));
    // fewer at the end of the file
    CHECK_EQ(lk_file_read_into(f, 2 * PAGE, m, 3 * PAGE, PAGE, &n), NO_ERROR);
    CHECK_EQ(n, 100);
    CHECK(same(rw + 3 * PAGE, rw, 100));
    // inside the object only; READ_INTO writes it, WRITE_FROM reads it
    CHECK_EQ(lk_file_read_into(f, 0, m, 3 * PAGE, PAGE + 1, &n), ERR_INVALID_ARGS);
    CHECK_EQ(lk_file_read_into(f, 0, m, 5 * PAGE, 0, &n), ERR_INVALID_ARGS);
    CHECK_EQ(lk_handle_dup(m, LK_RIGHTS_ALL & ~LK_RIGHT_WRITE, &h), NO_ERROR);
    CHECK_EQ(lk_file_read_into(f, 0, h, 0, PAGE, &n), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_file_write_from(f, 0, 0, h, 0, PAGE, &n, NULL), NO_ERROR);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);
    CHECK_EQ(lk_handle_dup(m, LK_RIGHTS_ALL & ~LK_RIGHT_READ, &h), NO_ERROR);
    CHECK_EQ(lk_file_write_from(f, 0, 0, h, 0, PAGE, &n, NULL), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);
    CHECK_EQ(lk_file_read_into(f, 0, console_h, 0, PAGE, &n), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(f), NO_ERROR);
    CHECK_EQ(lk_ns_unlink(ns_h, MEM_FILE), NO_ERROR);
    // the open mode holds as for READ and WRITE
    CHECK_EQ(lk_ns_open(ns_h, "/bin/calltest", LK_NS_OPEN_READ, &f, NULL), NO_ERROR);
    CHECK_EQ(lk_file_write_from(f, 0, 0, m, 0, PAGE, &n, NULL), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(f), NO_ERROR);

    // its last handle closed, the object lives on in its mappings
    fill(rw, 4 * PAGE, 5);
    CHECK_EQ(lk_handle_close(m), NO_ERROR);
    CHECK(same(ro, rw + PAGE, 2 * PAGE));
    CHECK_EQ(lk_vm_unmap(vm_h, ro + PAGE), NO_ERROR);
    CHECK_EQ(lk_vm_unmap(vm_h, rw), NO_ERROR);

    say("CALLTEST: memory objects ok\n");
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    test_put("CALLTEST: start\n");

    test_startup(args);
    test_console();
    test_handles();
    test_endpoints();
    test_transport();
    test_carried();
    test_echo();
    test_vm();
    test_time();
    test_process();
    test_reply_handles();
    test_files();
    test_paths();
    test_memobj();

    test_check_failures();
    say("CALLTEST: PASS\n");
    lk_exit(0);
}
