/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// The mini profile's one process: started at boot (LKUSER_BOOT) in a build with only the console
// and process services compiled in and no file system once it is loaded. Static, with no libc
// and no heap: its thread's stack is in .bss. It checks that it was born with exactly those two
// services, that its handle table holds argv[1] slots (LKUSER_HANDLES), that the raw IPC rows
// work between two of its threads (an endpoint served by one, called by the other, an impulse
// first), and that nothing can be spawned by path. Prints MINITEST: PASS.

#define TEST_NAME "MINITEST"
#include <lk/services.h>
#include <lktest.h>

#define HDR sizeof(struct lk_msg_hdr)
#define STACK_SIZE 4096
#define OP_ECHO 1
#define OP_QUIT 2

static lk_handle_t console_h, process_h, ep_h;
static uint8_t stack[STACK_SIZE] __attribute__((aligned(16)));

static void say(const char *s) {
    CHECK_EQ(lk_console_write(console_h, s, test_strlen(s)), NO_ERROR);
}

static uint32_t parse_dec(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s++ - '0');
    }
    return v;
}

// Serves ep_h: OP_ECHO sends the payload back, OP_QUIT replies with the impulses seen so far and
// ends the thread.
static void server(void *arg) {
    uint32_t impulses = 0;
    for (;;) {
        uint8_t buf[HDR + 64];
        struct lk_iov iov = {buf, sizeof(buf)};
        struct lk_recv_args a = {.buf = &iov, .nbuf = 1, .timeout_ns = LK_TIME_INFINITE};
        if (lk_recv(ep_h, &a) != NO_ERROR) {
            test_put("MINITEST: FAIL recv\n");
            lk_exit(1);
        }
        if (a.info.token == 0) {
            impulses++;
            continue;
        }
        const size_t len = a.info.req_len - HDR;
        struct lk_msg_hdr hdr = {.status = NO_ERROR};
        struct lk_iov out[2] = {{&hdr, HDR}, {buf + HDR, 0}};
        if (a.info.op == OP_ECHO && len <= sizeof(buf) - HDR) {
            out[1].len = len;
        } else if (a.info.op == OP_QUIT) {
            out[1] = (struct lk_iov){&impulses, sizeof(impulses)};
        } else {
            hdr.status = ERR_NOT_SUPPORTED;
        }
        const struct lk_reply_args r = {.iov = out, .n = 2};
        if (lk_reply(a.info.token, &r) != NO_ERROR) {
            test_put("MINITEST: FAIL reply\n");
            lk_exit(1);
        }
        if (a.info.op == OP_QUIT) {
            return;
        }
    }
}

// the services the profile compiles in, and nothing else
static void test_startup(const struct lk_proc_args *args) {
    CHECK_EQ(args->nhandles, 2);
    CHECK(console_h != LK_HANDLE_INVALID);
    CHECK(process_h != LK_HANDLE_INVALID);
    CHECK_EQ(lk_proc_args_find(args, LK_TAG_VM), LK_HANDLE_INVALID);
    CHECK_EQ(lk_proc_args_find(args, LK_TAG_TIME), LK_HANDLE_INVALID);
    CHECK_EQ(lk_proc_args_find(args, LK_TAG_NAMESPACE), LK_HANDLE_INVALID);

    struct lk_process_info info = {0};
    CHECK_EQ(lk_process_info(process_h, &info), NO_ERROR);
    CHECK(info.pid != 0);
    CHECK_EQ(info.state, LK_PROCESS_RUNNING);
    say("MINITEST: startup ok\n");
}

// the table refuses once its slots are taken: the two startup handles and the dups
static void test_handle_table(uint32_t slots) {
    lk_handle_t dups[64];
    uint32_t n = 0;
    lk_status_t err = NO_ERROR;
    while (n < 64 && (err = lk_handle_dup(console_h, LK_RIGHTS_ALL, &dups[n])) == NO_ERROR) {
        n++;
    }
    CHECK_EQ(err, ERR_NO_RESOURCES);
    CHECK_EQ(n + 2, slots);
    while (n > 0) {
        CHECK_EQ(lk_handle_close(dups[--n]), NO_ERROR);
    }
    say("MINITEST: handle table ok\n");
}

static void test_ipc(void) {
    const struct lk_endpoint_args ea = {.flags = 0, .max_active = 1, .cookie = 7};
    lk_handle_t conn = LK_HANDLE_INVALID, t = LK_HANDLE_INVALID;
    CHECK_EQ(lk_endpoint_create(&ea, &ep_h), NO_ERROR);
    CHECK_EQ(lk_connect(ep_h, &conn), NO_ERROR);

    // queued before the server runs, and taken before any call
    struct lk_impulse imp = {.type = LK_IMPULSE_USER_BASE};
    CHECK_EQ(lk_impulse_send(conn, &imp), NO_ERROR);
    CHECK_EQ(lk_thread_start(process_h, server, NULL, stack, sizeof(stack), &t), NO_ERROR);

    char back[16] = {0};
    size_t len = 0;
    CHECK_EQ(lk_msg_call(conn, OP_ECHO, "mini echo", 9, back, sizeof(back), &len), NO_ERROR);
    CHECK_EQ(len, 9);
    CHECK(test_streq(back, "mini echo"));
    CHECK_EQ(lk_msg_call(conn, 99, NULL, 0, NULL, 0, NULL), ERR_NOT_SUPPORTED);
    uint32_t impulses = 0;
    CHECK_EQ(lk_msg_call(conn, OP_QUIT, NULL, 0, &impulses, sizeof(impulses), &len), NO_ERROR);
    CHECK_EQ(impulses, 1);

    CHECK_EQ(lk_wait_one(t, LK_SIGNAL_TERMINATED, LK_TIME_INFINITE, NULL), NO_ERROR);
    CHECK_EQ(lk_handle_close(t), NO_ERROR);
    CHECK_EQ(lk_handle_close(conn), NO_ERROR);
    CHECK_EQ(lk_handle_close(ep_h), NO_ERROR);
    say("MINITEST: ipc ok\n");
}

// the boot file system went away once the image was loaded
static void test_no_fs(const char *self) {
    const char *argv[] = {self};
    lk_handle_t child = LK_HANDLE_INVALID;
    CHECK_EQ(lk_process_spawn(process_h, self, 1, argv, 0, NULL, NULL, 0, &child, NULL),
             ERR_NOT_FOUND);
    CHECK_EQ(child, LK_HANDLE_INVALID);
    say("MINITEST: no file system ok\n");
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    console_h = lk_proc_args_find(args, LK_TAG_CONSOLE);
    process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    if (console_h == LK_HANDLE_INVALID || args->nargs != 2) {
        test_put("MINITEST: FAIL started without a console, or without the table's size\n");
        lk_exit(1);
    }
    say("MINITEST: start\n");

    test_startup(args);
    test_handle_table(parse_dec(lk_proc_args_arg(args, 1)));
    test_ipc();
    test_no_fs(lk_proc_args_arg(args, 0));

    test_check_failures();
    say("MINITEST: PASS\n");
    lk_exit(0);
}
