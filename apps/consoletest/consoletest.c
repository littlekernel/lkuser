/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// The console protocol, whichever side of the kernel serves it: the same checks against the
// kernel console (usertest profile) and against apps/consoled (consoled profile), calltest
// having the transport's. WRITE in pieces and from several threads at once while a child waits
// in READ, the answers to requests the service refuses, and READ and WINSIZE clipped to the
// room. Given "quit" it also drives consoled's own ops: its counts, then CONSOLETEST_QUIT, after
// which the console is gone. Prints CONSOLETEST: PASS. No libc: it starts threads.

#define TEST_NAME "CONSOLETEST"
#include <consoletest.h>
#include <lk/services.h>
#include <lktest.h>

#define MS 1000000ull
#define CHILD "/bin/child"
#define WRITERS 3
#define LINES 10
#define STACK_SIZE 4096
#define LONG_LINES 11
#define LONG_LINE_LEN 100

static lk_handle_t console_h, process_h;
static uint8_t stacks[WRITERS][STACK_SIZE] __attribute__((aligned(16)));
static uint32_t writer_errors;
static char long_text[LONG_LINES * LONG_LINE_LEN];

static void say(const char *s) {
    CHECK_EQ(lk_console_write(console_h, s, test_strlen(s)), NO_ERROR);
}

static size_t put_num(char *p, uint32_t v) {
    char digits[12];
    size_t d = 0;
    do {
        digits[d++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    size_t n = 0;
    while (d > 0) {
        p[n++] = digits[--d];
    }
    return n;
}

static size_t put_str(char *p, const char *s) {
    size_t n = 0;
    while (s[n]) {
        p[n] = s[n];
        n++;
    }
    return n;
}

// op over the console with h lent: the call's status if it failed, else the service's
static lk_status_t call_lending(uint32_t op, const void *req, size_t req_len, void *reply,
                                size_t cap, lk_handle_t h) {
    const struct lk_iov rq = {(void *)req, req_len};
    const struct lk_iov rp = {reply, cap};
    struct lk_msg m = {
        .op = op, .req = &rq, .nreq = req_len ? 1 : 0, .reply = &rp, .nreply = cap ? 1 : 0,
        .carry = &h, .ncarry = 1, .carry_flags = LK_HANDLE_LEND(0),
    };
    return lk_msg_callv(console_h, &m);
}

// 1100 bytes in one call, cut into three pieces, the cuts falling inside lines
static void test_long_write(void) {
    for (uint32_t i = 0; i < LONG_LINES; i++) {
        char *line = long_text + i * LONG_LINE_LEN;
        size_t n = put_str(line, "CONSOLETEST: long write, line ");
        n += put_num(line + n, i + 1);
        line[n++] = ' ';
        while (n < LONG_LINE_LEN - 1) {
            line[n++] = '.';
        }
        line[n] = '\n';
    }
    const struct lk_iov pieces[] = {
        {long_text, 7},
        {long_text + 7, 593},
        {long_text + 600, sizeof(long_text) - 600},
    };
    struct lk_msg m = {.op = LK_CONSOLE_WRITE, .req = pieces, .nreq = 3};
    CHECK_EQ(lk_msg_callv(console_h, &m), NO_ERROR);
    CHECK_EQ(m.reply_len, 0);
}

// what the service refuses, and READ and WINSIZE as far as the room goes
static void test_refusals(void) {
    char buf[16];
    size_t len = 99;
    const struct lk_console_read_req rreq = {.max = 10};

    CHECK_EQ(lk_msg_call(console_h, 0x7777, "x", 1, NULL, 0, NULL), ERR_NOT_SUPPORTED);
    CHECK_EQ(lk_msg_call(console_h, LK_CONSOLE_READ, &rreq, 4, buf, sizeof(buf), NULL),
             ERR_INVALID_ARGS);
    CHECK_EQ(lk_console_read(console_h, buf, 0, &len), NO_ERROR);
    CHECK_EQ(len, 0);
    // max 10, but no room for any of it
    len = 99;
    CHECK_EQ(lk_msg_call(console_h, LK_CONSOLE_READ, &rreq, sizeof(rreq), NULL, 0, &len), NO_ERROR);
    CHECK_EQ(len, 0);

    uint32_t cols = 0, rows = 0;
    CHECK_EQ(lk_console_winsize(console_h, &cols, &rows), NO_ERROR);
    CHECK_EQ(cols, 80);
    CHECK_EQ(rows, 24);
    CHECK_EQ(lk_msg_call(console_h, LK_CONSOLE_WINSIZE, NULL, 0, buf, 4, NULL), ERR_NOT_ENOUGH_BUFFER);
    CHECK_EQ(lk_msg_call(console_h, LK_CONSOLE_WINSIZE, "x", 1, buf, sizeof(buf), NULL),
             ERR_INVALID_ARGS);

    // no op takes a handle; a lent one comes back either way
    lk_handle_t h;
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL, &h), NO_ERROR);
    CHECK_EQ(call_lending(LK_CONSOLE_WRITE, "x", 1, NULL, 0, h), ERR_INVALID_ARGS);
    CHECK_EQ(call_lending(LK_CONSOLE_READ, &rreq, sizeof(rreq), buf, sizeof(buf), h), ERR_INVALID_ARGS);
    CHECK_EQ(call_lending(LK_CONSOLE_WINSIZE, NULL, 0, buf, sizeof(buf), h), ERR_INVALID_ARGS);
    CHECK_EQ(call_lending(0x7777, NULL, 0, NULL, 0, h), ERR_INVALID_ARGS);
    CHECK_EQ(lk_console_write(h, "CONSOLETEST: lent handle still here\n", 36), NO_ERROR);
    CHECK_EQ(lk_handle_close(h), NO_ERROR);
    say("CONSOLETEST: refusals ok\n");
}

static void writer(void *arg) {
    const uint32_t id = (uint32_t)(uintptr_t)arg;
    for (uint32_t i = 0; i < LINES; i++) {
        char line[48];
        size_t n = put_str(line, "CONSOLETEST: writer ");
        n += put_num(line + n, id);
        n += put_str(line + n, " line ");
        n += put_num(line + n, i + 1);
        line[n++] = '\n';
        if (lk_console_write(console_h, line, n) != NO_ERROR) {
            __atomic_add_fetch(&writer_errors, 1, __ATOMIC_RELAXED);
        }
    }
}

// Several threads write at once while a child waits in READ (the gate's input is closed, so
// it waits until killed): no write waits for the read.
static void test_writers(void) {
    lk_handle_t child = LK_HANDLE_INVALID;
    const char *argv[] = {CHILD, "read"};
    CHECK_EQ(lk_process_spawn(process_h, CHILD, 2, argv, LK_PROCESS_SPAWN_SERVICES, NULL, NULL, 0,
                              &child, NULL),
             NO_ERROR);
    // long enough for the child to reach its read
    CHECK_EQ(lk_wait_one(child, LK_SIGNAL_TERMINATED, 50 * MS, NULL), ERR_TIMED_OUT);

    lk_handle_t t[WRITERS];
    for (uint32_t i = 0; i < WRITERS; i++) {
        CHECK_EQ(lk_thread_start(process_h, writer, (void *)(uintptr_t)(i + 1), stacks[i],
                                 STACK_SIZE, &t[i]),
                 NO_ERROR);
    }
    for (uint32_t i = 0; i < WRITERS; i++) {
        CHECK_EQ(lk_wait_one(t[i], LK_SIGNAL_TERMINATED, 10000 * MS, NULL), NO_ERROR);
        CHECK_EQ(lk_handle_close(t[i]), NO_ERROR);
    }
    CHECK_EQ(writer_errors, 0);

    // still reading
    CHECK_EQ(lk_wait_one(child, LK_SIGNAL_TERMINATED, 0, NULL), ERR_TIMED_OUT);
    CHECK_EQ(lk_process_kill(process_h, child), NO_ERROR);
    CHECK_EQ(lk_wait_one(child, LK_SIGNAL_TERMINATED, 10000 * MS, NULL), NO_ERROR);
    struct lk_process_info info = {0};
    CHECK_EQ(lk_process_info_of(process_h, child, &info), NO_ERROR);
    CHECK_EQ(info.exit_code, LK_PROCESS_KILLED);
    CHECK_EQ(lk_handle_close(child), NO_ERROR);
    say("CONSOLETEST: writers ok\n");
}

// consoled's own ops: its counts cover this test's calls, the read of the child killed above
// has been seen to leave, and nothing failed; then it quits, and the console with it
static void test_quit(void) {
    struct consoletest_stats st = {0};
    CHECK_EQ(lk_msg_call(console_h, CONSOLETEST_STATS, NULL, 0, &st, sizeof(st), NULL), NO_ERROR);
    CHECK(st.writes >= WRITERS * LINES + 3);
    CHECK(st.write_bytes >= sizeof(long_text));
    CHECK(st.reads >= 2);
    CHECK(st.winsizes >= 1);
    CHECK(st.departed >= 1);
    CHECK_EQ(st.errors, 0);
    say("CONSOLETEST: telling consoled to quit\n");
    CHECK_EQ(lk_msg_call(console_h, CONSOLETEST_QUIT, NULL, 0, NULL, 0, NULL), NO_ERROR);
    // the server's handles went with it, and its endpoint closed
    CHECK_EQ(lk_wait_one(console_h, LK_SIGNAL_PEER_CLOSED, 10000 * MS, NULL), NO_ERROR);
    CHECK_EQ(lk_console_write(console_h, "x", 1), ERR_CHANNEL_CLOSED);
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    console_h = lk_proc_args_find(args, LK_TAG_CONSOLE);
    process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    CHECK(console_h != LK_HANDLE_INVALID);
    CHECK(process_h != LK_HANDLE_INVALID);
    test_check_failures();
    const int quit = args->nargs > 1 && test_streq(lk_proc_args_arg(args, 1), "quit");

    // only consoled knows its test ops
    struct consoletest_stats st;
    const lk_status_t err = lk_msg_call(console_h, CONSOLETEST_STATS, NULL, 0, &st, sizeof(st), NULL);
    if (err == NO_ERROR) {
        say("CONSOLETEST: the console is served from user space\n");
    } else {
        CHECK_EQ(err, ERR_NOT_SUPPORTED);
        say("CONSOLETEST: the console is the kernel's\n");
    }
    CHECK(!quit || err == NO_ERROR);

    test_long_write();
    test_refusals();
    test_writers();
    if (quit) {
        test_quit();
    }
    test_check_failures();
    // through debug_write: after a quit there is no console
    test_put("CONSOLETEST: PASS\n");
    lk_exit(0);
}
