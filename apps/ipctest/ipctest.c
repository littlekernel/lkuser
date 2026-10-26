/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Calls to user endpoints, with no libc: the server and its clients are threads of this
// process, and /bin/child is a client or a server that dies mid-call. recv, msg_read, msg_write
// and reply; stale and forged tokens; timeouts; an endpoint closing under queued callers and a
// blocked receiver; a client killed and a server exiting mid-call; handles carried both ways
// (moved, lent, in transit, into a full table); impulses, served before calls, until the pool
// runs dry; CONNECT_NOTIFY with accept, refuse, peer handles, cookies and signals; CONN_CLOSED
// and PROCESS_EXITED; groups and max_active; wait on several objects; namespace mounts; and each
// IPC row with a handle lacking its right. Prints IPCTEST: PASS; each failed check prints an
// IPCTEST: FAIL line and the exit code is 1.

#define TEST_NAME "IPCTEST"
#include <lk/services.h>
#include <lktest.h>

#define PAGE 4096
#define MS 1000000ull
#define SEC (1000 * MS)
#define STACK_SIZE 8192
#define NTHREADS 4
#define CHILD "/bin/child"
#define HDR sizeof(struct lk_msg_hdr)
// the impulses a process can have queued at once (LKUSER_IMPULSE_POOL)
#define IMPULSE_POOL 32
// the namespace's mount table (kMaxMounts)
#define MOUNTS 8

static lk_handle_t console_h, process_h, vm_h, time_h, ns_h;
static uint32_t my_pid;
static uint8_t stacks[NTHREADS][STACK_SIZE] __attribute__((aligned(16)));

static void say(const char *s) {
    CHECK_EQ(lk_console_write(console_h, s, test_strlen(s)), NO_ERROR);
}

static uint64_t now_ns(void) {
    uint64_t ns = 0;
    CHECK_EQ(lk_time_clock_read(time_h, LK_CLOCK_MONOTONIC, &ns), NO_ERROR);
    return ns;
}

static void sleep_ms(uint64_t ms) {
    CHECK_EQ(lk_time_sleep(time_h, ms * MS), NO_ERROR);
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

static void *unmapped_page(void) {
    void *p = NULL;
    CHECK_EQ(lk_vm_alloc(vm_h, PAGE, LK_VM_PROT_READ | LK_VM_PROT_WRITE, &p), NO_ERROR);
    CHECK_EQ(lk_vm_unmap(vm_h, p), NO_ERROR);
    return p;
}

static lk_handle_t endpoint(uint32_t flags, uint32_t max_active, uint64_t cookie) {
    const struct lk_endpoint_args a = {.flags = flags, .max_active = max_active, .cookie = cookie};
    lk_handle_t ep = LK_HANDLE_INVALID;
    CHECK_EQ(lk_endpoint_create(&a, &ep), NO_ERROR);
    return ep;
}

static lk_handle_t connection(lk_handle_t ep) {
    lk_handle_t c = LK_HANDLE_INVALID;
    CHECK_EQ(lk_connect(ep, &c), NO_ERROR);
    return c;
}

// h with every right but those in drop
static lk_handle_t without(lk_handle_t h, uint32_t drop) {
    lk_handle_t d = LK_HANDLE_INVALID;
    CHECK_EQ(lk_handle_dup(h, LK_RIGHTS_ALL & ~drop, &d), NO_ERROR);
    return d;
}

static lk_handle_t run_thread(void (*fn)(void *), void *arg, int slot) {
    lk_handle_t t = LK_HANDLE_INVALID;
    CHECK_EQ(lk_thread_start(process_h, fn, arg, stacks[slot], STACK_SIZE, &t), NO_ERROR);
    return t;
}

static void join(lk_handle_t t) {
    CHECK_EQ(lk_wait_one(t, LK_SIGNAL_TERMINATED, 10 * SEC, NULL), NO_ERROR);
    CHECK_EQ(lk_handle_close(t), NO_ERROR);
}

// One call as lk_call makes it: a payload, room for a reply payload and reply handles, handles
// carried, a timeout. Made here or by a thread of its own, so that this thread can serve it.
struct client {
    lk_handle_t conn;
    uint32_t op;
    const void *req;
    size_t req_len;
    size_t reply_cap;
    void *reply_buf; // where the reply payload goes, if not reply[]
    lk_handle_t carry[LK_MSG_MAX_HANDLES];
    size_t ncarry;
    uint32_t carry_flags;
    size_t reply_handles;
    uint64_t timeout_ns;
    // what came back
    lk_status_t status;
    struct lk_msg_hdr rhdr;
    uint8_t reply[64];
    lk_handle_t rh[LK_MSG_MAX_HANDLES];
    size_t reply_len;
    lk_handle_t thread;
};

static lk_status_t raw_call(struct client *c) {
    struct lk_msg_hdr hdr = {.op = c->op, .len = (uint32_t)c->req_len, .nhandles = (uint32_t)c->ncarry};
    const struct lk_iov req[2] = {{&hdr, HDR}, {(void *)c->req, c->req_len}};
    const struct lk_iov reply[2] = {{&c->rhdr, HDR},
                                    {c->reply_buf ? c->reply_buf : c->reply, c->reply_cap}};
    struct lk_call_args a = {
        .req = req,
        .nreq = c->req_len ? 2 : 1,
        .handles = c->carry,
        .nhandles = c->ncarry,
        .reply = reply,
        .nreply = c->reply_cap ? 2 : 1,
        .reply_handles = c->rh,
        .reply_nhandles = c->reply_handles,
        .handle_flags = c->carry_flags,
        .timeout_ns = c->timeout_ns,
    };
    const lk_status_t err = lk_call(c->conn, &a);
    c->reply_len = a.reply_len;
    return err;
}

static void client_body(void *arg) {
    struct client *c = arg;
    c->status = raw_call(c);
}

static void start(struct client *c, int slot) {
    c->status = 1; // not a status
    c->thread = run_thread(client_body, c, slot);
}

static lk_status_t finish(struct client *c) {
    join(c->thread);
    return c->status;
}

static lk_status_t recv_on(lk_handle_t h, void *buf, size_t cap, uint64_t timeout_ns,
                           struct lk_recv_info *info) {
    struct lk_iov iov = {buf, cap};
    struct lk_recv_args a = {.buf = &iov, .nbuf = cap ? 1 : 0, .timeout_ns = timeout_ns};
    const lk_status_t err = lk_recv(h, &a);
    *info = a.info;
    return err;
}

// the next thing on h, which must be an impulse: its datagram
static struct lk_impulse next_impulse(lk_handle_t h, uint64_t timeout_ns) {
    struct lk_recv_info info;
    struct lk_impulse imp = {0};
    CHECK_EQ(recv_on(h, NULL, 0, timeout_ns, &info), NO_ERROR);
    CHECK_EQ(info.token, 0);
    for (size_t i = 0; i < sizeof(imp); i++) {
        ((uint8_t *)&imp)[i] = info.impulse[i];
    }
    return imp;
}

static lk_status_t reply_with(lk_token_t token, int32_t status, const void *payload, size_t len,
                              const lk_handle_t *hs, size_t nh) {
    struct lk_msg_hdr hdr = {.status = status};
    const struct lk_iov iov[2] = {{&hdr, HDR}, {(void *)payload, len}};
    const struct lk_reply_args a = {.iov = iov, .n = len ? 2 : 1, .handles = hs, .nhandles = nh};
    return lk_reply(token, &a);
}

static lk_status_t read_at(lk_token_t t, size_t off, void *buf, size_t len) {
    const struct lk_iov iov = {buf, len};
    return lk_msg_read(t, off, &iov, 1);
}

static lk_status_t write_at(lk_token_t t, size_t off, const void *buf, size_t len) {
    const struct lk_iov iov = {(void *)buf, len};
    return lk_msg_write(t, off, &iov, 1);
}

// /bin/child <mode>, handed the services its modes use and h under tag, moved: the status
static lk_status_t spawn_with(const char *mode, lk_handle_t h, uint32_t tag, lk_handle_t *p,
                              uint32_t *pid) {
    const char *argv[] = {CHILD, mode};
    const struct test_handoff down[] = {
        {process_h, LK_TAG_PROCESS, LK_RIGHTS_ALL},
        {time_h, LK_TAG_TIME, LK_RIGHTS_ALL},
        {console_h, LK_TAG_CONSOLE, LK_RIGHTS_ALL},
        {vm_h, LK_TAG_VM, LK_RIGHTS_ALL},
    };
    return test_spawn(process_h, CHILD, 2, argv, down, 4, &h, &tag, 1, p, pid);
}

// /bin/child <mode>, with h under LK_TAG_USER_BASE
static lk_handle_t spawn_child(const char *mode, lk_handle_t h, uint32_t *pid) {
    lk_handle_t p = LK_HANDLE_INVALID;
    CHECK_EQ(spawn_with(mode, h, LK_TAG_USER_BASE, &p, pid), NO_ERROR);
    return p;
}

// waits for the process to end, closes the handle and returns its exit code
static int child_exit(lk_handle_t p) {
    CHECK_EQ(lk_wait_one(p, LK_SIGNAL_TERMINATED, 10 * SEC, NULL), NO_ERROR);
    struct lk_process_info info = {0};
    CHECK_EQ(lk_process_info_of(process_h, p, &info), NO_ERROR);
    CHECK_EQ(lk_handle_close(p), NO_ERROR);
    return info.exit_code;
}

static void test_basic(void) {
    const lk_handle_t ep = endpoint(0, 1, 0xe1);
    struct client c = {.conn = connection(ep), .op = 7, .req = "hello", .req_len = 5,
                       .reply_cap = 32, .timeout_ns = LK_TIME_INFINITE};
    start(&c, 0);

    uint8_t buf[64];
    struct lk_recv_info info;
    CHECK_EQ(recv_on(ep, buf, sizeof(buf), 5 * SEC, &info), NO_ERROR);
    const lk_token_t t = info.token;
    CHECK(t != 0);
    CHECK_EQ(info.op, 7);
    CHECK_EQ(info.ep_cookie, 0xe1);
    CHECK_EQ(info.conn_cookie, 0);
    CHECK_EQ(info.sender_pid, my_pid);
    CHECK_EQ(info.req_len, HDR + 5);
    CHECK_EQ(info.reply_max, HDR + 32);
    CHECK_EQ(info.nhandles, 0);
    const struct lk_msg_hdr *h = (const struct lk_msg_hdr *)buf;
    CHECK_EQ(h->op, 7);
    CHECK_EQ(h->len, 5);
    CHECK(bytes_eq(buf + HDR, "hello", 5));

    // the request read in place, only inside it; a fault on this side is this side's alone
    char got[8] = {0};
    CHECK_EQ(read_at(t, HDR + 1, got, 4), NO_ERROR);
    CHECK(bytes_eq(got, "ello", 4));
    CHECK_EQ(read_at(t, HDR + 1, got, 5), ERR_OUT_OF_RANGE);
    CHECK_EQ(read_at(t, HDR + 6, got, 0), ERR_OUT_OF_RANGE);
    void *hole = unmapped_page();
    CHECK_EQ(read_at(t, HDR, hole, 1), ERR_FAULT);
    // the reply written in place, past its header, inside the caller's room
    CHECK_EQ(write_at(t, HDR + 2, "LLO", 3), NO_ERROR);
    CHECK_EQ(write_at(t, 0, "X", 1), ERR_INVALID_ARGS);
    CHECK_EQ(write_at(t, HDR + 30, "XYZ", 3), ERR_NOT_ENOUGH_BUFFER);
    uint8_t big[40] = {0};
    CHECK_EQ(reply_with(t, 5, big, sizeof(big), NULL, 0), ERR_NOT_ENOUGH_BUFFER);
    CHECK_EQ(reply_with(t, 5, hole, 2, NULL, 0), ERR_FAULT);
    // a call's token answers no handshake
    lk_handle_t peer;
    CHECK_EQ(lk_connection_accept(t, 1, &peer), ERR_BAD_STATE);
    CHECK_EQ(lk_connection_refuse(t), ERR_BAD_STATE);
    // the kernel writes the reply's header around the status
    CHECK_EQ(reply_with(t, 5, "HE", 2, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&c), NO_ERROR);
    CHECK_EQ(c.rhdr.status, 5);
    CHECK_EQ(c.rhdr.op, 7);
    CHECK_EQ(c.rhdr.len, 5);
    CHECK_EQ(c.rhdr.nhandles, 0);
    CHECK_EQ(c.reply_len, HDR + 5);
    CHECK(bytes_eq(c.reply, "HELLO", 5));

    // the token ended with the reply, and no other names a call
    CHECK_EQ(reply_with(t, 0, NULL, 0, NULL, 0), ERR_NOT_FOUND);
    CHECK_EQ(read_at(t, HDR, got, 1), ERR_NOT_FOUND);
    CHECK_EQ(write_at(t, HDR, got, 1), ERR_NOT_FOUND);
    const lk_token_t forged[] = {0, 1, t + 1, t ^ 0x100, t + 0x100, ~(lk_token_t)0};
    for (size_t i = 0; i < sizeof(forged) / sizeof(forged[0]); i++) {
        CHECK_EQ(reply_with(forged[i], 0, NULL, 0, NULL, 0), ERR_NOT_FOUND);
    }

    // a fault on the caller's side fails the caller's call, and the token goes with it
    c.reply_buf = NULL;
    CHECK_EQ(lk_vm_alloc(vm_h, PAGE, LK_VM_PROT_READ | LK_VM_PROT_WRITE, &c.reply_buf), NO_ERROR);
    start(&c, 0);
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    CHECK_EQ(lk_vm_unmap(vm_h, c.reply_buf), NO_ERROR);
    CHECK_EQ(write_at(info.token, HDR, "x", 1), ERR_FAULT);
    CHECK_EQ(finish(&c), ERR_FAULT);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), ERR_NOT_FOUND);

    CHECK_EQ(lk_handle_close(c.conn), NO_ERROR);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    say("IPCTEST: recv, msg_read, msg_write, reply ok\n");
}

static void test_timeouts(void) {
    const lk_handle_t ep = endpoint(0, 1, 0);
    struct client c = {.conn = connection(ep), .op = 1, .timeout_ns = 20 * MS};
    struct lk_recv_info info;

    // nobody receives: the call waits out its timeout and leaves the queue
    uint64_t t0 = now_ns();
    CHECK_EQ(raw_call(&c), ERR_TIMED_OUT);
    CHECK(now_ns() - t0 >= 20 * MS);
    CHECK_EQ(recv_on(ep, NULL, 0, 0, &info), ERR_TIMED_OUT);
    t0 = now_ns();
    CHECK_EQ(recv_on(ep, NULL, 0, 10 * MS, &info), ERR_TIMED_OUT);
    CHECK(now_ns() - t0 >= 10 * MS);

    // received and not answered in time: the caller leaves, and the server keeps the call's
    // place on the endpoint (max_active 1) until it next uses the token, which fails
    c.timeout_ns = 30 * MS;
    start(&c, 0);
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    const lk_token_t stale = info.token;
    CHECK_EQ(finish(&c), ERR_TIMED_OUT);
    c.timeout_ns = LK_TIME_INFINITE;
    start(&c, 0);
    CHECK_EQ(recv_on(ep, NULL, 0, 20 * MS, &info), ERR_TIMED_OUT);
    char x;
    CHECK_EQ(read_at(stale, HDR, &x, 1), ERR_NOT_FOUND);
    CHECK_EQ(reply_with(stale, 0, NULL, 0, NULL, 0), ERR_NOT_FOUND);
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&c), NO_ERROR);

    CHECK_EQ(lk_handle_close(c.conn), NO_ERROR);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    say("IPCTEST: timeouts ok\n");
}

struct receiver {
    lk_handle_t h;
    lk_status_t status;
    struct lk_recv_info info;
};

static void receiver_body(void *arg) {
    struct receiver *r = arg;
    r->status = recv_on(r->h, NULL, 0, LK_TIME_INFINITE, &r->info);
}

static void test_close(void) {
    // a caller queued when the endpoint's last handle goes, and every call after
    lk_handle_t ep = endpoint(0, 1, 0);
    struct client c = {.conn = connection(ep), .op = 1, .timeout_ns = LK_TIME_INFINITE};
    start(&c, 0);
    uint32_t sig = 0;
    CHECK_EQ(lk_wait_one(ep, LK_SIGNAL_READABLE, 5 * SEC, &sig), NO_ERROR);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    CHECK_EQ(finish(&c), ERR_CHANNEL_CLOSED);
    CHECK_EQ(lk_wait_one(c.conn, LK_SIGNAL_PEER_CLOSED, 0, &sig), NO_ERROR);
    CHECK_EQ(raw_call(&c), ERR_CHANNEL_CLOSED);
    const struct lk_impulse imp = {.type = LK_IMPULSE_USER_BASE};
    CHECK_EQ(lk_impulse_send(c.conn, &imp), ERR_CHANNEL_CLOSED);
    CHECK_EQ(lk_handle_close(c.conn), NO_ERROR);

    // a thread blocked in recv
    ep = endpoint(0, 1, 0);
    struct receiver r = {.h = ep};
    const lk_handle_t rt = run_thread(receiver_body, &r, 1);
    sleep_ms(50);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    join(rt);
    CHECK_EQ(r.status, ERR_CHANNEL_CLOSED);

    // a call already received can still be replied to
    ep = endpoint(0, 1, 0);
    c.conn = connection(ep);
    start(&c, 0);
    struct lk_recv_info info;
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&c), NO_ERROR);
    CHECK_EQ(lk_handle_close(c.conn), NO_ERROR);
    say("IPCTEST: endpoint close ok\n");
}

static void test_dying(void) {
    // a client killed while its call is queued: the call leaves the queue, and its
    // connection's CONN_CLOSED comes instead
    lk_handle_t ep = endpoint(0, 1, 0);
    uint32_t pid;
    lk_handle_t p = spawn_child("call", connection(ep), &pid);
    CHECK_EQ(lk_wait_one(ep, LK_SIGNAL_READABLE, 5 * SEC, NULL), NO_ERROR);
    CHECK_EQ(lk_process_kill(process_h, p), NO_ERROR);
    CHECK_EQ(child_exit(p), LK_PROCESS_KILLED);
    struct lk_impulse imp = next_impulse(ep, 0);
    CHECK_EQ(imp.type, LK_IMPULSE_CONN_CLOSED);
    struct lk_recv_info info;
    CHECK_EQ(recv_on(ep, NULL, 0, 0, &info), ERR_TIMED_OUT);

    // one killed after its call was received: the reply finds nobody
    p = spawn_child("call", connection(ep), &pid);
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    CHECK(info.token != 0);
    CHECK_EQ(info.sender_pid, pid);
    CHECK_EQ(lk_process_kill(process_h, p), NO_ERROR);
    CHECK_EQ(child_exit(p), LK_PROCESS_KILLED);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), ERR_NOT_FOUND);
    imp = next_impulse(ep, 0);
    CHECK_EQ(imp.type, LK_IMPULSE_CONN_CLOSED);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);

    // a server that exits holding the call: the caller gets ERR_CHANNEL_CLOSED, and with the
    // server's handles gone the endpoint has closed
    ep = endpoint(0, 1, 0);
    struct client c = {.conn = connection(ep), .op = 1, .timeout_ns = 5 * SEC};
    p = spawn_child("serve", ep, &pid);
    CHECK_EQ(raw_call(&c), ERR_CHANNEL_CLOSED);
    CHECK_EQ(child_exit(p), 4);
    CHECK_EQ(lk_wait_one(c.conn, LK_SIGNAL_PEER_CLOSED, 0, NULL), NO_ERROR);
    CHECK_EQ(lk_handle_close(c.conn), NO_ERROR);
    say("IPCTEST: dying clients and servers ok\n");
}

static void test_carried(void) {
    const lk_handle_t ep = endpoint(0, 1, 0);
    const lk_handle_t conn = connection(ep);
    lk_handle_t m = LK_HANDLE_INVALID;
    CHECK_EQ(lk_vm_memobj_create(vm_h, PAGE, 0, &m, NULL), NO_ERROR);
    lk_handle_t moved = without(m, 0);
    struct lk_recv_info info;
    lk_handle_t d;

    // moved and lent: the server sees both, the lent one without DUP and TRANSFER; the moved
    // one left the caller at recv, the lent one is taken back at the reply
    struct client c = {.conn = conn, .op = 2, .carry = {moved, m}, .ncarry = 2,
                       .carry_flags = LK_HANDLE_LEND(1), .timeout_ns = LK_TIME_INFINITE};
    start(&c, 0);
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    CHECK_EQ(info.nhandles, 2);
    const lk_handle_t got_moved = info.handles[0];
    const lk_handle_t got_lent = info.handles[1];
    CHECK_EQ(lk_handle_close(moved), ERR_BAD_HANDLE);
    CHECK_EQ(lk_handle_dup(got_lent, LK_RIGHTS_ALL, &d), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_dup(got_moved, LK_RIGHTS_ALL, &d), NO_ERROR);
    CHECK_EQ(lk_handle_close(d), NO_ERROR);
    uint8_t *va = NULL;
    CHECK_EQ(lk_vm_memobj_map(vm_h, got_lent, 0, 0, LK_VM_PROT_READ | LK_VM_PROT_WRITE, (void **)&va),
             NO_ERROR);
    if (va) {
        va[0] = 0x5a;
    }
    CHECK_EQ(lk_vm_unmap(vm_h, va), NO_ERROR);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&c), NO_ERROR);
    CHECK_EQ(lk_handle_close(got_lent), ERR_BAD_HANDLE);
    CHECK_EQ(lk_handle_close(got_moved), NO_ERROR);
    va = NULL;
    CHECK_EQ(lk_vm_memobj_map(vm_h, m, 0, 0, LK_VM_PROT_READ, (void **)&va), NO_ERROR);
    CHECK(va && va[0] == 0x5a);
    CHECK_EQ(lk_vm_unmap(vm_h, va), NO_ERROR);

    // the reply's handles move to the caller; more than its room, or one without TRANSFER, is
    // refused and the call stays
    struct client r = {.conn = conn, .op = 3, .reply_handles = 1, .timeout_ns = LK_TIME_INFINITE};
    start(&r, 0);
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    const lk_handle_t give[2] = {without(m, 0), without(m, 0)};
    const lk_handle_t weak = without(m, LK_RIGHT_TRANSFER);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, give, 2), ERR_NOT_ENOUGH_BUFFER);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, &weak, 1), ERR_ACCESS_DENIED);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, give, 1), NO_ERROR);
    CHECK_EQ(finish(&r), NO_ERROR);
    CHECK_EQ(r.rhdr.nhandles, 1);
    CHECK_EQ(lk_handle_close(give[0]), ERR_BAD_HANDLE);
    CHECK_EQ(lk_handle_close(r.rh[0]), NO_ERROR);
    CHECK_EQ(lk_handle_close(give[1]), NO_ERROR);
    CHECK_EQ(lk_handle_close(weak), NO_ERROR);

    // In transit a moved handle still counts as one: the endpoint it names stays open while
    // the call is queued, though the slot it left has been closed.
    const lk_handle_t other = endpoint(0, 1, 0);
    struct client t = {.conn = conn, .op = 4, .carry = {other}, .ncarry = 1,
                       .timeout_ns = LK_TIME_INFINITE};
    start(&t, 0);
    CHECK_EQ(lk_wait_one(ep, LK_SIGNAL_READABLE, 5 * SEC, NULL), NO_ERROR);
    CHECK_EQ(lk_handle_close(other), NO_ERROR);
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    lk_handle_t oc = LK_HANDLE_INVALID;
    CHECK_EQ(lk_connect(info.handles[0], &oc), NO_ERROR);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&t), NO_ERROR);
    CHECK_EQ(lk_handle_close(oc), NO_ERROR);
    CHECK_EQ(lk_handle_close(info.handles[0]), NO_ERROR);

    // a call that ends unreceived keeps what it would have moved
    moved = without(m, 0);
    struct client u = {.conn = conn, .op = 5, .carry = {moved}, .ncarry = 1, .timeout_ns = 10 * MS};
    CHECK_EQ(raw_call(&u), ERR_TIMED_OUT);
    CHECK_EQ(lk_handle_close(moved), NO_ERROR);

    // with no room in the receiver's table the call stays queued
    u.carry[0] = without(m, 0);
    u.timeout_ns = LK_TIME_INFINITE;
    start(&u, 0);
    CHECK_EQ(lk_wait_one(ep, LK_SIGNAL_READABLE, 5 * SEC, NULL), NO_ERROR);
    lk_handle_t fill[64];
    size_t nfill = 0;
    while (nfill < 64 && lk_handle_dup(m, LK_RIGHTS_ALL, &fill[nfill]) == NO_ERROR) {
        nfill++;
    }
    CHECK_EQ(recv_on(ep, NULL, 0, 0, &info), ERR_NO_RESOURCES);
    CHECK(nfill > 0);
    CHECK_EQ(lk_handle_close(fill[--nfill]), NO_ERROR);
    CHECK_EQ(recv_on(ep, NULL, 0, 0, &info), NO_ERROR);
    CHECK_EQ(info.nhandles, 1);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&u), NO_ERROR);
    CHECK_EQ(lk_handle_close(info.handles[0]), NO_ERROR);
    while (nfill > 0) {
        CHECK_EQ(lk_handle_close(fill[--nfill]), NO_ERROR);
    }

    CHECK_EQ(lk_handle_close(m), NO_ERROR);
    CHECK_EQ(lk_handle_close(conn), NO_ERROR);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    say("IPCTEST: carried handles ok\n");
}

static void test_impulses(void) {
    const lk_handle_t ep = endpoint(0, 1, 0x1f);
    const lk_handle_t conn = connection(ep);
    struct lk_impulse imp = {.type = LK_IMPULSE_USER_BASE + 1, .cookie = 0xdead};
    struct lk_recv_info info;

    // types below LK_IMPULSE_USER_BASE are the kernel's, no flags exist, and a kernel service
    // takes none
    // no flag is defined yet; known only at run time, so that the compiler makes no 8-byte
    // constant of the impulse's start, which the default linker script would put in a
    // writable and executable segment
    imp.flags = my_pid != 0;
    CHECK_EQ(lk_impulse_send(conn, &imp), ERR_INVALID_ARGS);
    imp.flags = 0;
    imp.type = LK_IMPULSE_CONN_CLOSED;
    CHECK_EQ(lk_impulse_send(conn, &imp), ERR_INVALID_ARGS);
    imp.type = LK_IMPULSE_USER_BASE + 1;
    CHECK_EQ(lk_impulse_send(time_h, &imp), ERR_NOT_SUPPORTED);

    // a call queued first, then the pool's worth of impulses, which all come before it
    struct client c = {.conn = conn, .op = 9, .timeout_ns = LK_TIME_INFINITE};
    start(&c, 0);
    CHECK_EQ(lk_wait_one(ep, LK_SIGNAL_READABLE, 5 * SEC, NULL), NO_ERROR);
    int sent = 0;
    for (int i = 0; i < 2 * IMPULSE_POOL; i++) {
        imp.payload[0] = (uint8_t)i;
        if (lk_impulse_send(conn, &imp) != NO_ERROR) {
            break;
        }
        sent++;
    }
    CHECK_EQ(sent, IMPULSE_POOL);
    CHECK_EQ(lk_impulse_send(conn, &imp), ERR_NO_RESOURCES);
    for (int i = 0; i < sent; i++) {
        CHECK_EQ(recv_on(ep, NULL, 0, 0, &info), NO_ERROR);
        CHECK_EQ(info.token, 0);
        CHECK_EQ(info.op, 0);
        CHECK_EQ(info.ep_cookie, 0x1f);
        CHECK_EQ(info.sender_pid, my_pid);
        struct lk_impulse got;
        for (size_t j = 0; j < sizeof(got); j++) {
            ((uint8_t *)&got)[j] = info.impulse[j];
        }
        CHECK_EQ(got.type, LK_IMPULSE_USER_BASE + 1);
        // the connection's cookie, whatever the sender wrote
        CHECK_EQ(got.cookie, 0);
        CHECK_EQ(got.payload[0], i);
    }
    CHECK_EQ(recv_on(ep, NULL, 0, 0, &info), NO_ERROR);
    CHECK(info.token != 0);
    CHECK_EQ(info.op, 9);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&c), NO_ERROR);

    // every entry is back in the pool
    for (int i = 0; i < IMPULSE_POOL; i++) {
        CHECK_EQ(lk_impulse_send(conn, &imp), NO_ERROR);
    }
    for (int i = 0; i < IMPULSE_POOL; i++) {
        next_impulse(ep, 0);
    }

    // the endpoint's last handle drops what is queued, and the entries come back
    const lk_handle_t ep2 = endpoint(0, 1, 0);
    const lk_handle_t conn2 = connection(ep2);
    for (int i = 0; i < IMPULSE_POOL; i++) {
        CHECK_EQ(lk_impulse_send(conn2, &imp), NO_ERROR);
    }
    CHECK_EQ(lk_handle_close(ep2), NO_ERROR);
    CHECK_EQ(lk_impulse_send(conn, &imp), NO_ERROR);
    next_impulse(ep, 0);
    CHECK_EQ(lk_handle_close(conn2), NO_ERROR);

    CHECK_EQ(lk_handle_close(conn), NO_ERROR);
    // the connection's own CONN_CLOSED
    imp = next_impulse(ep, 0);
    CHECK_EQ(imp.type, LK_IMPULSE_CONN_CLOSED);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    say("IPCTEST: impulses ok\n");
}

struct connector {
    lk_handle_t ep;
    lk_handle_t conn;
    lk_status_t status;
};

static void connector_body(void *arg) {
    struct connector *k = arg;
    k->status = lk_connect(k->ep, &k->conn);
}

struct waiter {
    struct lk_wait_args args;
    lk_status_t status;
};

static void waiter_body(void *arg) {
    struct waiter *w = arg;
    w->status = lk_wait(&w->args);
}

static void test_notify(void) {
    const lk_handle_t ep = endpoint(LK_EP_FLAG_CONNECT_NOTIFY, 1, 0);
    struct lk_recv_info info;
    uint8_t buf[16];

    // connect is a call the server answers, here with a refusal
    struct connector k = {.ep = ep};
    lk_handle_t kt = run_thread(connector_body, &k, 0);
    CHECK_EQ(recv_on(ep, buf, sizeof(buf), 5 * SEC, &info), NO_ERROR);
    CHECK(info.token != 0);
    CHECK_EQ(info.op, LK_OP_CONNECT);
    CHECK_EQ(info.req_len, 0);
    CHECK_EQ(info.reply_max, 0);
    CHECK_EQ(read_at(info.token, 0, buf, 1), ERR_BAD_STATE);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), ERR_BAD_STATE);
    CHECK_EQ(lk_connection_refuse(info.token), NO_ERROR);
    join(kt);
    CHECK_EQ(k.status, ERR_NOT_ALLOWED);
    CHECK_EQ(lk_connection_refuse(info.token), ERR_NOT_FOUND);

    // accepted with a cookie: the client has its connection, the server a peer handle
    kt = run_thread(connector_body, &k, 0);
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    lk_handle_t peer = LK_HANDLE_INVALID;
    CHECK_EQ(lk_connection_accept(info.token, 0x77, &peer), NO_ERROR);
    join(kt);
    CHECK_EQ(k.status, NO_ERROR);
    const lk_handle_t client = k.conn;
    lk_handle_t extra;
    CHECK_EQ(lk_connection_accept(info.token, 0x77, &extra), ERR_NOT_FOUND);
    struct client c = {.conn = client, .op = 1, .timeout_ns = LK_TIME_INFINITE};
    start(&c, 0);
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    CHECK_EQ(info.conn_cookie, 0x77);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&c), NO_ERROR);

    // the server raises signals on the client's connection, and a thread waiting on several
    // objects wakes for that one
    CHECK_EQ(lk_connection_signal(peer, LK_SIGNAL_PEER_CLOSED, 0), ERR_INVALID_ARGS);
    CHECK_EQ(lk_connection_signal(client, LK_SIGNAL_READABLE, 0), ERR_ACCESS_DENIED);
    const struct lk_wait_item items[3] = {
        {ep, LK_SIGNAL_READABLE},
        {client, LK_SIGNAL_READABLE | LK_SIGNAL_ERROR},
        {console_h, LK_SIGNAL_TERMINATED},
    };
    struct waiter w = {.args = {.items = items, .n = 3, .timeout_ns = 0}};
    CHECK_EQ(lk_wait(&w.args), ERR_TIMED_OUT);
    w.args.timeout_ns = 5 * SEC;
    const lk_handle_t wt = run_thread(waiter_body, &w, 1);
    sleep_ms(20);
    CHECK_EQ(lk_connection_signal(peer, LK_SIGNAL_ERROR, 0), NO_ERROR);
    join(wt);
    CHECK_EQ(w.status, NO_ERROR);
    CHECK_EQ(w.args.index, 1);
    CHECK(w.args.signals & LK_SIGNAL_ERROR);
    CHECK_EQ(lk_connection_signal(peer, LK_SIGNAL_WRITABLE, LK_SIGNAL_ERROR), NO_ERROR);
    w.args.timeout_ns = 0;
    CHECK_EQ(lk_wait(&w.args), ERR_TIMED_OUT);
    CHECK_EQ(lk_wait_one(client, LK_SIGNAL_WRITABLE, 0, NULL), NO_ERROR);

    // a new cookie, then the client's last handle goes: CONN_CLOSED with that cookie, and the
    // peer handle can do nothing more
    CHECK_EQ(lk_connection_set_cookie(peer, 0x88), NO_ERROR);
    CHECK_EQ(lk_handle_close(client), NO_ERROR);
    struct lk_impulse imp = next_impulse(ep, 0);
    CHECK_EQ(imp.type, LK_IMPULSE_CONN_CLOSED);
    CHECK_EQ(imp.cookie, 0x88);
    CHECK_EQ(lk_connection_signal(peer, LK_SIGNAL_READABLE, 0), ERR_CHANNEL_CLOSED);
    CHECK_EQ(lk_connection_set_cookie(peer, 1), ERR_CHANNEL_CLOSED);
    CHECK_EQ(lk_handle_close(peer), NO_ERROR);

    // a client in another process, which exits
    uint32_t pid;
    const lk_handle_t p =
        spawn_child("connect", without(ep, LK_RIGHTS_ALL & ~(LK_RIGHT_CONNECT | LK_RIGHT_TRANSFER)), &pid);
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    CHECK_EQ(info.op, LK_OP_CONNECT);
    CHECK_EQ(info.sender_pid, pid);
    CHECK_EQ(lk_connection_accept(info.token, 0x99, &peer), NO_ERROR);
    CHECK_EQ(child_exit(p), 6);
    imp = next_impulse(ep, 5 * SEC);
    CHECK_EQ(imp.type, LK_IMPULSE_CONN_CLOSED);
    CHECK_EQ(imp.cookie, 0x99);
    CHECK_EQ(lk_handle_close(peer), NO_ERROR);

    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    say("IPCTEST: connect notify, peer handles, signals ok\n");
}

static void test_groups(void) {
    lk_handle_t g = LK_HANDLE_INVALID, g2 = LK_HANDLE_INVALID;
    CHECK_EQ(lk_group_create(&g), NO_ERROR);
    CHECK_EQ(lk_group_create(&g2), NO_ERROR);
    const lk_handle_t e1 = endpoint(0, 1, 1);
    const lk_handle_t e2 = endpoint(0, 2, 2);
    const lk_handle_t e3 = endpoint(0, 1, 3);
    const lk_handle_t c1 = connection(e1);
    const lk_handle_t c2 = connection(e2);
    struct lk_recv_info info;

    // an endpoint joins one group at most
    CHECK_EQ(lk_group_add(g, e1), NO_ERROR);
    CHECK_EQ(lk_group_add(g, e2), NO_ERROR);
    CHECK_EQ(lk_group_add(g, e1), ERR_BAD_STATE);
    CHECK_EQ(lk_group_add(g2, e1), ERR_BAD_STATE);
    CHECK_EQ(lk_group_remove(g, e3), ERR_NOT_FOUND);
    CHECK_EQ(lk_group_add(g, c1), ERR_ACCESS_DENIED);

    // impulses first, from any member, then calls
    struct client a = {.conn = c2, .op = 21, .timeout_ns = LK_TIME_INFINITE};
    start(&a, 0);
    CHECK_EQ(lk_wait_one(e2, LK_SIGNAL_READABLE, 5 * SEC, NULL), NO_ERROR);
    const struct lk_impulse imp = {.type = LK_IMPULSE_USER_BASE};
    CHECK_EQ(lk_impulse_send(c1, &imp), NO_ERROR);
    CHECK_EQ(recv_on(g, NULL, 0, 0, &info), NO_ERROR);
    CHECK_EQ(info.token, 0);
    CHECK_EQ(info.ep_cookie, 1);
    CHECK_EQ(recv_on(g, NULL, 0, 0, &info), NO_ERROR);
    CHECK(info.token != 0);
    CHECK_EQ(info.ep_cookie, 2);
    CHECK_EQ(info.op, 21);
    const lk_token_t ta = info.token;

    // e1 takes one call at a time: its second waits for the first's reply
    struct client b1 = {.conn = c1, .op = 11, .timeout_ns = LK_TIME_INFINITE};
    struct client b2 = {.conn = c1, .op = 12, .timeout_ns = LK_TIME_INFINITE};
    start(&b1, 1);
    CHECK_EQ(lk_wait_one(e1, LK_SIGNAL_READABLE, 5 * SEC, NULL), NO_ERROR);
    start(&b2, 2);
    sleep_ms(20);
    CHECK_EQ(recv_on(g, NULL, 0, 5 * SEC, &info), NO_ERROR);
    CHECK_EQ(info.ep_cookie, 1);
    const lk_token_t tb = info.token;
    const uint32_t first_op = info.op;
    CHECK_EQ(recv_on(g, NULL, 0, 20 * MS, &info), ERR_TIMED_OUT);
    CHECK_EQ(reply_with(tb, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(recv_on(g, NULL, 0, 5 * SEC, &info), NO_ERROR);
    CHECK_EQ(info.ep_cookie, 1);
    CHECK_EQ(info.op + first_op, 11 + 12);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(reply_with(ta, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&b1), NO_ERROR);
    CHECK_EQ(finish(&b2), NO_ERROR);
    CHECK_EQ(finish(&a), NO_ERROR);

    // a member leaves with its queued work, which its own receivers then get
    start(&a, 0);
    CHECK_EQ(lk_wait_one(e2, LK_SIGNAL_READABLE, 5 * SEC, NULL), NO_ERROR);
    CHECK_EQ(lk_group_remove(g, e2), NO_ERROR);
    CHECK_EQ(recv_on(g, NULL, 0, 0, &info), ERR_TIMED_OUT);
    CHECK_EQ(recv_on(e2, NULL, 0, 0, &info), NO_ERROR);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&a), NO_ERROR);

    // the group's last handle wakes its receivers, and its members are on their own again
    struct receiver r = {.h = g};
    const lk_handle_t rt = run_thread(receiver_body, &r, 1);
    sleep_ms(50);
    CHECK_EQ(lk_handle_close(g), NO_ERROR);
    join(rt);
    CHECK_EQ(r.status, ERR_CHANNEL_CLOSED);
    CHECK_EQ(lk_group_add(g2, e1), NO_ERROR);
    CHECK_EQ(lk_group_remove(g2, e1), NO_ERROR);
    b1.timeout_ns = LK_TIME_INFINITE;
    start(&b1, 1);
    CHECK_EQ(recv_on(e1, NULL, 0, 5 * SEC, &info), NO_ERROR);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&b1), NO_ERROR);

    CHECK_EQ(lk_handle_close(g2), NO_ERROR);
    CHECK_EQ(lk_handle_close(c1), NO_ERROR);
    CHECK_EQ(lk_handle_close(c2), NO_ERROR);
    CHECK_EQ(lk_handle_close(e1), NO_ERROR);
    CHECK_EQ(lk_handle_close(e2), NO_ERROR);
    CHECK_EQ(lk_handle_close(e3), NO_ERROR);
    say("IPCTEST: groups ok\n");
}

static void test_exit_notify(void) {
    const lk_handle_t ep = endpoint(0, 1, 0);
    const uint32_t tag = LK_PROCESS_SPAWN_TAG_EXIT_NOTIFY;
    lk_handle_t notify = connection(ep);
    lk_handle_t p = LK_HANDLE_INVALID;
    uint32_t pid = 0;

    // the connection stays with the kernel, not the child, until the child has ended
    CHECK_EQ(spawn_with("fault", notify, tag, &p, &pid), NO_ERROR);
    CHECK_EQ(lk_handle_close(notify), ERR_BAD_HANDLE);
    CHECK_EQ(child_exit(p), ERR_FAULT);
    struct lk_impulse imp = next_impulse(ep, 5 * SEC);
    CHECK_EQ(imp.type, LK_IMPULSE_PROCESS_EXITED);
    const struct lk_impulse_process_exited *e = (const void *)imp.payload;
    CHECK_EQ(e->pid, pid);
    CHECK_EQ(e->exit_code, ERR_FAULT);
    // the kernel's was the last client handle
    imp = next_impulse(ep, 5 * SEC);
    CHECK_EQ(imp.type, LK_IMPULSE_CONN_CLOSED);

    // a connection with IMPULSE, to a user endpoint
    const lk_handle_t c = connection(ep);
    notify = without(c, LK_RIGHT_IMPULSE);
    CHECK_EQ(spawn_with("fault", notify, tag, &p, NULL), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(notify), NO_ERROR);
    notify = without(time_h, 0);
    CHECK_EQ(spawn_with("fault", notify, tag, &p, NULL), ERR_NOT_SUPPORTED);
    CHECK_EQ(lk_handle_close(notify), NO_ERROR);
    CHECK_EQ(lk_handle_close(c), NO_ERROR);
    next_impulse(ep, 0);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    say("IPCTEST: exit notify ok\n");
}

static void test_namespace(void) {
    lk_handle_t ep = endpoint(0, 1, 0);
    const lk_handle_t notify_ep = endpoint(LK_EP_FLAG_CONNECT_NOTIFY, 1, 0);
    const uint32_t connect_only = LK_RIGHTS_ALL & ~(LK_RIGHT_CONNECT | LK_RIGHT_TRANSFER);
    lk_handle_t spare = without(ep, connect_only);
    lk_handle_t conn = LK_HANDLE_INVALID;
    char rest[32];
    size_t rest_len = 99;
    struct lk_recv_info info;

    CHECK_EQ(lk_ns_mount(ns_h, "/srv/ipctest", spare), NO_ERROR);
    CHECK_EQ(lk_handle_close(spare), ERR_BAD_HANDLE);
    // a lookup normalizes, and hands out a connection with the rest of the path
    CHECK_EQ(lk_ns_lookup(ns_h, "/srv/./ipctest//a/b/", &conn, rest, sizeof(rest), &rest_len),
             NO_ERROR);
    CHECK(test_streq(rest, "/a/b"));
    CHECK_EQ(rest_len, 4);
    struct client c = {.conn = conn, .op = 1, .timeout_ns = LK_TIME_INFINITE};
    start(&c, 0);
    CHECK_EQ(recv_on(ep, NULL, 0, 5 * SEC, &info), NO_ERROR);
    CHECK_EQ(reply_with(info.token, 0, NULL, 0, NULL, 0), NO_ERROR);
    CHECK_EQ(finish(&c), NO_ERROR);
    CHECK_EQ(lk_handle_close(conn), NO_ERROR);
    CHECK_EQ(lk_ns_lookup(ns_h, "/srv/ipctest", &conn, rest, sizeof(rest), &rest_len), NO_ERROR);
    CHECK_EQ(rest_len, 0);
    CHECK_EQ(lk_handle_close(conn), NO_ERROR);
    CHECK_EQ(lk_ns_lookup(ns_h, "/srv/ipctestx", &conn, rest, sizeof(rest), NULL), ERR_NOT_FOUND);
    CHECK_EQ(lk_ns_lookup(ns_h, "/srv", &conn, rest, sizeof(rest), NULL), ERR_NOT_FOUND);
    CHECK_EQ(lk_ns_lookup(ns_h, "/srv/ipctest/abc", &conn, rest, 3, NULL), ERR_NOT_ENOUGH_BUFFER);

    // the path is taken; "/" covers too much; a handshake cannot run inside LOOKUP; MOUNT needs
    // CONNECT
    spare = without(ep, connect_only);
    CHECK_EQ(lk_ns_mount(ns_h, "/srv/ipctest", spare), ERR_ALREADY_MOUNTED);
    CHECK_EQ(lk_ns_mount(ns_h, "/", spare), ERR_INVALID_ARGS);
    lk_handle_t h = without(notify_ep, 0);
    CHECK_EQ(lk_ns_mount(ns_h, "/srv/notify", h), ERR_NOT_SUPPORTED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);
    h = without(ep, LK_RIGHT_CONNECT);
    CHECK_EQ(lk_ns_mount(ns_h, "/srv/weak", h), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    // unmounting takes the mounted endpoint
    CHECK_EQ(lk_ns_unmount(ns_h, "/srv/ipctest", notify_ep), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_ns_unmount(ns_h, "/srv/ipctest/a", ep), ERR_NOT_FOUND);
    CHECK_EQ(lk_ns_unmount(ns_h, "/srv/ipctest", ep), NO_ERROR);
    CHECK_EQ(lk_ns_lookup(ns_h, "/srv/ipctest", &conn, rest, sizeof(rest), NULL), ERR_NOT_FOUND);

    // a mount goes with its server's handles
    CHECK_EQ(lk_ns_mount(ns_h, "/srv/ipctest", spare), NO_ERROR);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    CHECK_EQ(lk_ns_lookup(ns_h, "/srv/ipctest", &conn, rest, sizeof(rest), NULL), ERR_NOT_FOUND);
    ep = endpoint(0, 1, 0);
    spare = without(ep, connect_only);
    CHECK_EQ(lk_ns_mount(ns_h, "/srv/ipctest", spare), NO_ERROR);
    CHECK_EQ(lk_ns_unmount(ns_h, "/srv/ipctest", ep), NO_ERROR);

    // the table holds MOUNTS live mounts, and a dead one makes room wherever it is
    char path[] = "/srv/ipctest0";
    lk_handle_t eps[MOUNTS];
    for (int i = 0; i < MOUNTS; i++) {
        eps[i] = endpoint(0, 1, 0);
        path[sizeof(path) - 2] = (char)('0' + i);
        CHECK_EQ(lk_ns_mount(ns_h, path, without(eps[i], connect_only)), NO_ERROR);
    }
    spare = without(ep, connect_only);
    CHECK_EQ(lk_ns_mount(ns_h, "/srv/ipctest", spare), ERR_NO_RESOURCES);
    for (int i = 0; i < MOUNTS; i++) {
        CHECK_EQ(lk_handle_close(eps[i]), NO_ERROR);
    }
    CHECK_EQ(lk_ns_mount(ns_h, "/srv/ipctest", spare), NO_ERROR);
    CHECK_EQ(lk_ns_lookup(ns_h, "/srv/ipctest0", &conn, rest, sizeof(rest), NULL), ERR_NOT_FOUND);
    CHECK_EQ(lk_ns_unmount(ns_h, "/srv/ipctest", ep), NO_ERROR);

    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    CHECK_EQ(lk_handle_close(notify_ep), NO_ERROR);
    say("IPCTEST: namespace mounts ok\n");
}

// Each IPC row refuses a handle lacking the right it needs (process, vm and file rights are
// calltest's and spawntest's).
static void test_rights(void) {
    const lk_handle_t ep = endpoint(0, 1, 0);
    const lk_handle_t conn = connection(ep);
    lk_handle_t g = LK_HANDLE_INVALID;
    CHECK_EQ(lk_group_create(&g), NO_ERROR);
    lk_handle_t h, d;
    struct lk_recv_info info;

    h = without(conn, LK_RIGHT_DUP);
    CHECK_EQ(lk_handle_dup(h, LK_RIGHTS_ALL, &d), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    h = without(conn, LK_RIGHT_CALL);
    struct client c = {.conn = h, .op = 1, .timeout_ns = 10 * MS};
    CHECK_EQ(raw_call(&c), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    // a handle moved to a user endpoint needs TRANSFER
    h = without(conn, LK_RIGHT_TRANSFER);
    c.conn = conn;
    c.carry[0] = h;
    c.ncarry = 1;
    CHECK_EQ(raw_call(&c), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    h = without(ep, LK_RIGHT_RECV);
    CHECK_EQ(recv_on(h, NULL, 0, 0, &info), ERR_ACCESS_DENIED);
    CHECK_EQ(recv_on(conn, NULL, 0, 0, &info), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_group_add(g, h), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_group_remove(g, h), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);
    h = without(g, LK_RIGHT_RECV);
    CHECK_EQ(lk_group_add(h, ep), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_group_remove(h, ep), ERR_ACCESS_DENIED);
    CHECK_EQ(recv_on(h, NULL, 0, 0, &info), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    h = without(conn, LK_RIGHT_IMPULSE);
    const struct lk_impulse imp = {.type = LK_IMPULSE_USER_BASE};
    CHECK_EQ(lk_impulse_send(h, &imp), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    h = without(ep, LK_RIGHT_CONNECT);
    CHECK_EQ(lk_connect(h, &d), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    // SIGNAL, which only a peer handle has
    CHECK_EQ(lk_connection_set_cookie(conn, 1), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_connection_signal(conn, LK_SIGNAL_READABLE, 0), ERR_ACCESS_DENIED);

    // WAIT, and the item that lacks it is named
    h = without(conn, LK_RIGHT_WAIT);
    CHECK_EQ(lk_wait_one(h, LK_SIGNAL_READABLE, 0, NULL), ERR_ACCESS_DENIED);
    const struct lk_wait_item items[2] = {{conn, LK_SIGNAL_READABLE}, {h, LK_SIGNAL_READABLE}};
    struct lk_wait_args w = {.items = items, .n = 2, .timeout_ns = 0};
    CHECK_EQ(lk_wait(&w), ERR_ACCESS_DENIED);
    CHECK_EQ(w.index, 1);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);

    CHECK_EQ(lk_handle_close(g), NO_ERROR);
    CHECK_EQ(lk_handle_close(conn), NO_ERROR);
    CHECK_EQ(lk_handle_close(ep), NO_ERROR);
    say("IPCTEST: rights ok\n");
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    test_put("IPCTEST: start\n");

    console_h = lk_proc_args_find(args, LK_TAG_CONSOLE);
    process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    vm_h = lk_proc_args_find(args, LK_TAG_VM);
    time_h = lk_proc_args_find(args, LK_TAG_TIME);
    ns_h = lk_proc_args_find(args, LK_TAG_NAMESPACE);
    CHECK(console_h && process_h && vm_h && time_h && ns_h);
    struct lk_process_info me = {0};
    CHECK_EQ(lk_process_info(process_h, &me), NO_ERROR);
    my_pid = me.pid;
    test_check_failures();

    test_basic();
    test_timeouts();
    test_close();
    test_dying();
    test_carried();
    test_impulses();
    test_notify();
    test_groups();
    test_exit_notify();
    test_namespace();
    test_rights();

    test_check_failures();
    say("IPCTEST: PASS\n");
    lk_exit(0);
}
