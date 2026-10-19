/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// A user-space echo server with no libc: two endpoints in one group, mounted at ECHOTEST_PATH
// (up to 4 calls at once) and ECHOTEST_SINGLE_PATH (one at a time), and two threads receiving on
// the group. It speaks the echo protocol of <sys/svc/echo.h>, the kernel echo service's, reading
// what recv did not copy with msg_read and writing the reply in place with msg_write, and the
// test ops of <echotest.h>. It ends on ECHOTEST_QUIT, printing ECHOSERVER: PASS if every call
// was served and none on the single endpoint ever overlapped another, or after IDLE_NS with
// nothing to do, printing why it failed.

#define TEST_NAME "ECHOSERVER"
#include <echotest.h>
#include <lk/services.h>
#include <lktest.h>

#define HDR sizeof(struct lk_msg_hdr)
#define MS 1000000ull
#define IDLE_NS (20000 * MS)
#define STACK_SIZE 8192
#define COOKIE_MULTI 1
#define COOKIE_SINGLE 2

static lk_handle_t console_h, time_h, vm_h, ns_h;
static lk_handle_t group_h, multi_h, single_h;
// the client's endpoint, once ECHOTEST_NOTIFY has given it, and the echoes since
static lk_handle_t notify_h;
static uint32_t notify_count;
// shared by the two threads
static struct echotest_stats stats;
static uint32_t single_active, multi_active, errors;
static uint8_t stack[STACK_SIZE] __attribute__((aligned(16)));

static void say(const char *s) {
    lk_console_write(console_h, s, test_strlen(s));
}

static void count(uint32_t *v) {
    __atomic_add_fetch(v, 1, __ATOMIC_RELAXED);
}

static void raise_max(uint32_t *max, uint32_t v) {
    uint32_t old = __atomic_load_n(max, __ATOMIC_RELAXED);
    while (v > old && !__atomic_compare_exchange_n(max, &old, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

// One line, from the first thread to fail; both serving threads can time out at once.
static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why) {
    static uint32_t failed;
    if (__atomic_exchange_n(&failed, 1, __ATOMIC_ACQ_REL)) {
        lk_thread_exit();
    }
    char line[96] = "ECHOSERVER: FAIL ";
    size_t n = test_strlen(line);
    for (size_t i = 0; why[i] && n < sizeof(line) - 2; i++) {
        line[n++] = why[i];
    }
    line[n++] = '\n';
    lk_console_write(console_h, line, n);
    lk_exit(1);
}

static lk_status_t write_reply(lk_token_t t, const void *p, size_t len) {
    const struct lk_iov iov = {(void *)p, len};
    return lk_msg_write(t, HDR, &iov, 1);
}

// ECHO: the payload back, as much as fits, from what recv copied and then read in place
static lk_status_t echo(const struct lk_recv_info *info, const uint8_t *start, size_t got) {
    const size_t len = info->req_len - HDR;
    const size_t room = info->reply_max - HDR;
    const size_t n = len < room ? len : room;
    uint8_t buf[512];
    for (size_t off = 0; off < n;) {
        size_t chunk = n - off < sizeof(buf) ? n - off : sizeof(buf);
        const uint8_t *src = buf;
        if (HDR + off < got) {
            if (chunk > got - HDR - off) {
                chunk = got - HDR - off;
            }
            src = start + HDR + off;
        } else {
            const struct lk_iov in = {buf, chunk};
            const lk_status_t err = lk_msg_read(info->token, HDR + off, &in, 1);
            if (err < 0) {
                return err;
            }
        }
        const struct lk_iov out = {(void *)src, chunk};
        const lk_status_t err = lk_msg_write(info->token, HDR + off, &out, 1);
        if (err < 0) {
            return err;
        }
        off += chunk;
    }
    count(&stats.echoes);

    // every tenth echo since the client asked, an impulse to it
    const lk_handle_t to = __atomic_load_n(&notify_h, __ATOMIC_ACQUIRE);
    if (to != LK_HANDLE_INVALID) {
        const uint32_t c = __atomic_add_fetch(&notify_count, 1, __ATOMIC_RELAXED);
        if (c % 10 == 0) {
            struct lk_impulse imp = {.type = ECHOTEST_IMPULSE};
            for (size_t i = 0; i < sizeof(c); i++) {
                imp.payload[i] = (uint8_t)(c >> (8 * i));
            }
            if (lk_impulse_send(to, &imp) != NO_ERROR) {
                count(&errors);
            }
        }
    }
    return n < len ? ERR_NOT_ENOUGH_BUFFER : NO_ERROR;
}

// ECHOTEST_MEMOBJ: the sum of the bytes of a lent memory object, mapped for the call
static lk_status_t sum_memobj(const struct lk_recv_info *info, const uint8_t *start, size_t got) {
    struct echotest_memobj_req req;
    if (info->req_len != HDR + sizeof(req) || got < HDR + sizeof(req) || info->nhandles != 1) {
        return ERR_INVALID_ARGS;
    }
    for (size_t i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = start[HDR + i];
    }
    const uint8_t *va = NULL;
    lk_status_t err = lk_vm_memobj_map(vm_h, info->handles[0], 0, 0, LK_VM_PROT_READ, (void **)&va);
    if (err < 0) {
        return err;
    }
    struct echotest_memobj_reply r = {.sum = 0};
    for (uint32_t i = 0; i < req.len; i++) {
        r.sum += va[i];
    }
    lk_vm_unmap(vm_h, (void *)va);
    return write_reply(info->token, &r, sizeof(r));
}

static void quit(void) __attribute__((noreturn));
static void quit(void) {
    lk_ns_unmount(ns_h, ECHOTEST_PATH, multi_h);
    lk_ns_unmount(ns_h, ECHOTEST_SINGLE_PATH, single_h);
    if (__atomic_load_n(&stats.single_overlaps, __ATOMIC_RELAXED) != 0) {
        fail("calls overlapped on the single endpoint");
    }
    if (__atomic_load_n(&errors, __ATOMIC_RELAXED) != 0) {
        fail("calls failed");
    }
    say("ECHOSERVER: PASS\n");
    lk_exit(0);
}

static void serve_call(const struct lk_recv_info *info, const uint8_t *start, size_t got) {
    // Counted from recv until just before the reply: the kernel hands the single endpoint's next
    // call out only after this one's reply, so two in this window on it would be an overlap.
    const int single = info->ep_cookie == COOKIE_SINGLE;
    const uint32_t active = __atomic_add_fetch(single ? &single_active : &multi_active, 1,
                                               __ATOMIC_ACQ_REL);
    if (single) {
        count(&stats.single_calls);
        if (active > 1) {
            count(&stats.single_overlaps);
        }
    } else {
        raise_max(&stats.multi_max, active);
    }

    lk_status_t status;
    int keep_handles = 0;
    switch (info->op) {
        case LK_ECHO_ECHO:
            // long enough for the other thread to take a call meanwhile, if it can
            lk_time_sleep(time_h, 2 * MS);
            status = info->nhandles ? ERR_INVALID_ARGS : echo(info, start, got);
            break;
        case ECHOTEST_MEMOBJ:
            status = sum_memobj(info, start, got);
            break;
        case ECHOTEST_NOTIFY:
            if (info->nhandles != 1 || info->req_len != HDR) {
                status = ERR_INVALID_ARGS;
                break;
            }
            __atomic_store_n(&notify_count, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&notify_h, info->handles[0], __ATOMIC_RELEASE);
            keep_handles = 1;
            status = NO_ERROR;
            break;
        case ECHOTEST_STATS: {
            const struct echotest_stats s = {
                .echoes = __atomic_load_n(&stats.echoes, __ATOMIC_RELAXED),
                .single_calls = __atomic_load_n(&stats.single_calls, __ATOMIC_RELAXED),
                .single_overlaps = __atomic_load_n(&stats.single_overlaps, __ATOMIC_RELAXED),
                .multi_max = __atomic_load_n(&stats.multi_max, __ATOMIC_RELAXED),
                .conn_closed = __atomic_load_n(&stats.conn_closed, __ATOMIC_RELAXED),
            };
            status = write_reply(info->token, &s, sizeof(s));
            break;
        }
        case ECHOTEST_QUIT:
            status = NO_ERROR;
            break;
        default:
            status = ERR_NOT_SUPPORTED;
            break;
    }
    // what a call carried and the op did not keep: a moved handle is this process's now, and a
    // lent one is taken back at the reply anyway
    if (!keep_handles) {
        for (uint32_t i = 0; i < info->nhandles; i++) {
            lk_handle_close(info->handles[i]);
        }
    }

    __atomic_sub_fetch(single ? &single_active : &multi_active, 1, __ATOMIC_ACQ_REL);
    struct lk_msg_hdr hdr = {.status = status};
    const struct lk_iov iov = {&hdr, HDR};
    const struct lk_reply_args a = {.iov = &iov, .n = 1};
    if (lk_reply(info->token, &a) != NO_ERROR) {
        count(&errors);
    }
    if (info->op == ECHOTEST_QUIT) {
        quit();
    }
}

static void serve(void *arg) {
    uint8_t start[256];
    for (;;) {
        struct lk_iov iov = {start, sizeof(start)};
        struct lk_recv_args a = {.buf = &iov, .nbuf = 1, .timeout_ns = IDLE_NS};
        const lk_status_t err = lk_recv(group_h, &a);
        if (err == ERR_TIMED_OUT) {
            fail("nothing to do for too long");
        }
        if (err < 0) {
            fail("recv");
        }
        if (a.info.token == 0) {
            struct lk_impulse imp;
            for (size_t i = 0; i < sizeof(imp); i++) {
                ((uint8_t *)&imp)[i] = a.info.impulse[i];
            }
            if (imp.type == LK_IMPULSE_CONN_CLOSED) {
                count(&stats.conn_closed);
            }
            continue;
        }
        serve_call(&a.info, start, a.info.req_len < sizeof(start) ? a.info.req_len : sizeof(start));
    }
}

static lk_handle_t endpoint(uint32_t max_active, uint64_t cookie) {
    const struct lk_endpoint_args a = {.flags = 0, .max_active = max_active, .cookie = cookie};
    lk_handle_t ep = LK_HANDLE_INVALID;
    if (lk_endpoint_create(&a, &ep) != NO_ERROR || lk_group_add(group_h, ep) != NO_ERROR) {
        fail("endpoint");
    }
    return ep;
}

// the namespace keeps no handle of its own, so it gets a spare that can only connect
static void mount(const char *path, lk_handle_t ep) {
    lk_handle_t spare;
    if (lk_handle_dup(ep, LK_RIGHT_CONNECT | LK_RIGHT_TRANSFER, &spare) != NO_ERROR ||
        lk_ns_mount(ns_h, path, spare) != NO_ERROR) {
        fail("mount");
    }
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    console_h = lk_proc_args_find(args, LK_TAG_CONSOLE);
    time_h = lk_proc_args_find(args, LK_TAG_TIME);
    vm_h = lk_proc_args_find(args, LK_TAG_VM);
    ns_h = lk_proc_args_find(args, LK_TAG_NAMESPACE);
    const lk_handle_t process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    say("ECHOSERVER: start\n");

    if (lk_group_create(&group_h) != NO_ERROR) {
        fail("group");
    }
    multi_h = endpoint(4, COOKIE_MULTI);
    single_h = endpoint(1, COOKIE_SINGLE);
    mount(ECHOTEST_PATH, multi_h);
    mount(ECHOTEST_SINGLE_PATH, single_h);

    lk_handle_t t;
    if (lk_thread_start(process_h, serve, NULL, stack, sizeof(stack), &t) != NO_ERROR) {
        fail("thread");
    }
    say("ECHOSERVER: serving\n");
    serve(NULL);
    lk_exit(1);
}
