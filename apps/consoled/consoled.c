/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// The console protocol of <sys/svc/console.h> served from user space, relaying to the kernel
// console. It runs as a boot process serving the console (LKUSER_BOOT's /bin/consoled@console):
// born with the endpoint it serves under LK_TAG_SERVE_ENDPOINT and its own connection to the
// kernel console under LK_TAG_CONSOLE, while every process the kernel makes after it gets a
// connection to it under LK_TAG_CONSOLE (and may hand it down as it spawns). Its clients cannot
// tell it from the kernel's: it answers every request as svc/console.cpp does.
//
// One thread serves every call. A READ is answered later, by token: a second thread reads a
// line from the kernel console only while a READ waits, and hands it over through a call on an
// endpoint of the two threads' own, so WRITE goes on while a read blocks and nothing is read
// ahead of a reader. A READ whose caller has left (killed while it waited) is swept before
// each receive, so it does not hold its inflight slot until input comes. It also speaks the
// test ops of <consoletest.h>, and ends on CONSOLETEST_QUIT, printing CONSOLED: PASS if no call
// failed on its side. No libc: two threads, and newlib is not safe with threads.

#define TEST_NAME "CONSOLED"
#include <consoletest.h>
#include <lk/services.h>
#include <lktest.h>

#define HDR sizeof(struct lk_msg_hdr)
// the most a READ returns, as the kernel console
#define LINE_MAX 256
// READs waiting at once; more are refused with ERR_NO_RESOURCES, so that together with the
// reader's call and the one being served they fit in the process's 16 inflight slots
#define MAX_READS 8
#define WRITE_CHUNK 512
#define STACK_SIZE 8192

// the reader thread's ops on the line endpoint
#define LINE_COOKIE 1
// request: no payload; reply: struct line_want, once a READ waits and no input is left over
#define LINE_WANT 1
// request: what the kernel console's READ returned, nothing for end of input; reply: none
#define LINE_GOT 2

struct line_want {
    uint32_t max;
    uint32_t reserved;
};

struct pending_read {
    lk_token_t token;
    uint32_t max;
};

static lk_handle_t kcons_h, process_h, serve_h, line_ep_h, line_conn_h, group_h;
static uint8_t stack[STACK_SIZE] __attribute__((aligned(16)));

// The server thread's state; the reader thread touches none of it.
static struct pending_read reads[MAX_READS];
static uint32_t nreads;
// a line read and not yet all handed out, and an end of input not yet reported
static uint8_t input[LINE_MAX];
static size_t input_off, input_len;
static int input_eof;
// the reader's LINE_WANT, held until a READ waits with nothing left over
static lk_token_t want_token;
static struct consoletest_stats stats;

static void say(const char *s) {
    lk_console_write(kcons_h, s, test_strlen(s));
}

static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why) {
    say("CONSOLED: FAIL ");
    say(why);
    say("\n");
    lk_exit(1);
}

static size_t append(char *buf, size_t n, size_t cap, const char *s) {
    while (*s && n < cap) {
        buf[n++] = *s++;
    }
    return n;
}

static size_t append_dec(char *buf, size_t n, size_t cap, uint32_t v) {
    char digits[12];
    size_t d = 0;
    do {
        digits[d++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (d > 0 && n < cap) {
        buf[n++] = digits[--d];
    }
    return n;
}

// A reply carrying len bytes of p. ERR_NOT_FOUND if the caller has left.
static lk_status_t reply_with(lk_token_t t, lk_status_t status, const void *p, size_t len) {
    struct lk_msg_hdr hdr = {.status = status};
    const struct lk_iov iov[2] = {{&hdr, HDR}, {(void *)p, len}};
    const struct lk_reply_args a = {.iov = iov, .n = len ? 2 : 1};
    return lk_reply(t, &a);
}

static void count_reply(lk_status_t err) {
    if (err == ERR_NOT_FOUND) {
        stats.departed++;
    } else if (err < 0) {
        stats.errors++;
    }
}

// Hands left-over input to the waiting READs, oldest first, then, if one still waits and
// nothing is left over, lets the reader read a line for it. A READ whose caller has left gets
// nothing, and the input waits for the next.
static void deliver(void) {
    while (nreads > 0 && (input_off < input_len || input_eof)) {
        const struct pending_read r = reads[0];
        for (uint32_t i = 1; i < nreads; i++) {
            reads[i - 1] = reads[i];
        }
        nreads--;
        const size_t left = input_len - input_off;
        const size_t n = left < r.max ? left : r.max;
        const lk_status_t err = reply_with(r.token, NO_ERROR, input + input_off, n);
        if (err == ERR_NOT_FOUND) {
            stats.departed++;
            continue;
        }
        if (err < 0) {
            stats.errors++;
        } else {
            stats.reads++;
        }
        if (n == 0) {
            input_eof = 0;
        }
        input_off += n;
    }
    if (nreads > 0 && input_off == input_len && !input_eof && want_token) {
        const struct line_want w = {.max = reads[0].max};
        const lk_status_t err = reply_with(want_token, NO_ERROR, &w, sizeof(w));
        want_token = 0;
        if (err < 0) {
            fail("the reader left");
        }
    }
}

// Forgets the READs whose callers have left, which frees their inflight slots.
static void sweep(void) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < nreads; i++) {
        struct lk_msg_hdr h;
        const struct lk_iov iov = {&h, HDR};
        if (lk_msg_read(reads[i].token, 0, &iov, 1) == ERR_NOT_FOUND) {
            stats.departed++;
            continue;
        }
        reads[kept++] = reads[i];
    }
    nreads = kept;
}

// WRITE: the payload to the kernel console, from what recv copied, then read in place.
static lk_status_t relay_write(const struct lk_recv_info *info, const uint8_t *start, size_t got) {
    const size_t len = info->req_len - HDR;
    uint8_t buf[WRITE_CHUNK];
    for (size_t off = 0; off < len;) {
        size_t chunk = len - off < sizeof(buf) ? len - off : sizeof(buf);
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
        const lk_status_t err = lk_console_write(kcons_h, src, chunk);
        if (err < 0) {
            return err;
        }
        off += chunk;
    }
    stats.writes++;
    stats.write_bytes += len;
    return NO_ERROR;
}

// READ: answered now if there is no room or input is left over, else when the reader has a
// line. Returns 1 if the call waits.
static int serve_read(const struct lk_recv_info *info, const uint8_t *start, lk_status_t *status) {
    struct lk_console_read_req req;
    if (info->req_len != HDR + sizeof(req)) {
        *status = ERR_INVALID_ARGS;
        return 0;
    }
    for (size_t i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = start[HDR + i];
    }
    size_t max = info->reply_max - HDR;
    if (req.max < max) {
        max = req.max;
    }
    if (max > LINE_MAX) {
        max = LINE_MAX;
    }
    if (max == 0) {
        stats.reads++;
        *status = NO_ERROR;
        return 0;
    }
    if (nreads == MAX_READS) {
        *status = ERR_NO_RESOURCES;
        return 0;
    }
    reads[nreads++] = (struct pending_read){.token = info->token, .max = (uint32_t)max};
    deliver();
    return 1;
}

static void quit(void) __attribute__((noreturn));
static void quit(void) {
    char line[160];
    const size_t cap = sizeof(line) - 1;
    size_t n = append(line, 0, cap, "consoled: ");
    n = append_dec(line, n, cap, stats.writes);
    n = append(line, n, cap, " writes (");
    n = append_dec(line, n, cap, stats.write_bytes);
    n = append(line, n, cap, " bytes), ");
    n = append_dec(line, n, cap, stats.reads);
    n = append(line, n, cap, " reads, ");
    n = append_dec(line, n, cap, stats.winsizes);
    n = append(line, n, cap, " winsizes, ");
    n = append_dec(line, n, cap, stats.departed);
    n = append(line, n, cap, " departed, ");
    n = append_dec(line, n, cap, stats.errors);
    n = append(line, n, cap, " errors");
    line[n++] = '\n';
    lk_console_write(kcons_h, line, n);
    if (stats.errors != 0) {
        fail("calls failed");
    }
    say("CONSOLED: PASS\n");
    lk_exit(0);
}

static void serve_call(const struct lk_recv_info *info, const uint8_t *start, size_t got) {
    const size_t room = info->reply_max - HDR;
    lk_status_t status = NO_ERROR;

    // no op takes a handle; a moved one is this process's now, and a lent one goes at the reply
    if (info->nhandles != 0) {
        for (uint32_t i = 0; i < info->nhandles; i++) {
            lk_handle_close(info->handles[i]);
        }
        count_reply(reply_with(info->token, ERR_INVALID_ARGS, NULL, 0));
        return;
    }

    switch (info->op) {
        case LK_CONSOLE_WRITE:
            status = relay_write(info, start, got);
            break;
        case LK_CONSOLE_READ:
            if (serve_read(info, start, &status)) {
                return;
            }
            break;
        case LK_CONSOLE_WINSIZE: {
            struct lk_console_winsize ws;
            if (info->req_len != HDR) {
                status = ERR_INVALID_ARGS;
            } else if (room < sizeof(ws)) {
                status = ERR_NOT_ENOUGH_BUFFER;
            } else {
                status = lk_console_winsize(kcons_h, &ws.cols, &ws.rows);
            }
            if (status == NO_ERROR) {
                stats.winsizes++;
                count_reply(reply_with(info->token, NO_ERROR, &ws, sizeof(ws)));
                return;
            }
            break;
        }
        case CONSOLETEST_STATS:
            // the counts include every READ whose caller has left by now
            sweep();
            if (info->req_len != HDR) {
                status = ERR_INVALID_ARGS;
            } else if (room < sizeof(stats)) {
                status = ERR_NOT_ENOUGH_BUFFER;
            } else {
                count_reply(reply_with(info->token, NO_ERROR, &stats, sizeof(stats)));
                return;
            }
            break;
        case CONSOLETEST_QUIT:
            count_reply(reply_with(info->token, NO_ERROR, NULL, 0));
            quit();
        default:
            status = ERR_NOT_SUPPORTED;
            break;
    }
    count_reply(reply_with(info->token, status, NULL, 0));
}

static void serve_line(const struct lk_recv_info *info, const uint8_t *start, size_t got) {
    switch (info->op) {
        case LINE_WANT:
            want_token = info->token;
            deliver();
            return;
        case LINE_GOT: {
            const size_t n = info->req_len - HDR;
            if (n > LINE_MAX || got < HDR + n) {
                fail("a line too long");
            }
            for (size_t i = 0; i < n; i++) {
                input[i] = start[HDR + i];
            }
            input_off = 0;
            input_len = n;
            input_eof = n == 0;
            if (reply_with(info->token, NO_ERROR, NULL, 0) < 0) {
                fail("the reader left");
            }
            deliver();
            return;
        }
        default:
            fail("an unknown line op");
    }
}

static void serve(void) __attribute__((noreturn));
static void serve(void) {
    uint8_t start[HDR + LINE_MAX];
    for (;;) {
        sweep();
        struct lk_iov iov = {start, sizeof(start)};
        struct lk_recv_args a = {.buf = &iov, .nbuf = 1, .timeout_ns = LK_TIME_INFINITE};
        const lk_status_t err = lk_recv(group_h, &a);
        if (err < 0) {
            fail("recv");
        }
        // CONN_CLOSED as each client goes; nothing to do
        if (a.info.token == 0) {
            continue;
        }
        const size_t got = a.info.req_len < sizeof(start) ? a.info.req_len : sizeof(start);
        if (a.info.ep_cookie == LINE_COOKIE) {
            serve_line(&a.info, start, got);
        } else {
            serve_call(&a.info, start, got);
        }
    }
}

// Reads a line from the kernel console each time the server thread asks for one.
static void reader(void *arg) {
    uint8_t line[LINE_MAX];
    for (;;) {
        struct line_want w;
        if (lk_msg_call(line_conn_h, LINE_WANT, NULL, 0, &w, sizeof(w), NULL) != NO_ERROR) {
            fail("LINE_WANT");
        }
        size_t n = 0;
        if (lk_console_read(kcons_h, line, w.max, &n) != NO_ERROR) {
            fail("reading the kernel console");
        }
        if (lk_msg_call(line_conn_h, LINE_GOT, line, n, NULL, 0, NULL) != NO_ERROR) {
            fail("LINE_GOT");
        }
    }
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    kcons_h = lk_proc_args_find(args, LK_TAG_CONSOLE);
    process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    serve_h = lk_proc_args_find(args, LK_TAG_SERVE_ENDPOINT);
    if (!kcons_h || !process_h) {
        test_put("CONSOLED: FAIL no console or process service\n");
        lk_exit(1);
    }
    if (!serve_h) {
        fail("not started to serve the console (LKUSER_BOOT's /bin/consoled@console)");
    }

    const struct lk_endpoint_args ea = {.flags = 0, .max_active = 1, .cookie = LINE_COOKIE};
    if (lk_group_create(&group_h) != NO_ERROR || lk_group_add(group_h, serve_h) != NO_ERROR ||
        lk_endpoint_create(&ea, &line_ep_h) != NO_ERROR ||
        lk_group_add(group_h, line_ep_h) != NO_ERROR || lk_connect(line_ep_h, &line_conn_h) != NO_ERROR) {
        fail("setting up");
    }
    lk_handle_t t;
    if (lk_thread_start(process_h, reader, NULL, stack, sizeof(stack), &t) != NO_ERROR) {
        fail("starting the reader");
    }
    say("consoled: serving the console\n");
    serve();
}
