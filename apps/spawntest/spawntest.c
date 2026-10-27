/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Processes and threads with no libc: spawning the image's bin/child with arguments and
// handles, waiting for it and reading its exit code, a child born with only the handles handed
// to it and their rights, every way a process ends (exit, thread_exit, a fault, a kill, from one
// thread or several), killing a child blocked in a sleep, a console read or a wait, the rights
// a process handle carries, the requests the process service refuses, and threads of its own.
// Prints SPAWNTEST: PASS; each failed check prints a SPAWNTEST: FAIL line and the exit code is 1.

#define TEST_NAME "SPAWNTEST"
#include <lk/services.h>
#include <lktest.h>

#define PAGE 4096
#define STACK_SIZE 8192
#define MS 1000000ull
#define CHILD TEST_IMAGE "/bin/child"
// no table has this many slots
#define NOT_A_HANDLE 0xffffffffu

static lk_handle_t console_h, process_h, vm_h, time_h, ns_h;

static void say(const char *s) {
    CHECK_EQ(lk_console_write(console_h, s, test_strlen(s)), NO_ERROR);
}

static uint64_t now_ns(void) {
    uint64_t ns = 0;
    CHECK_EQ(lk_time_clock_read(time_h, LK_CLOCK_MONOTONIC, &ns), NO_ERROR);
    return ns;
}

// /bin/child <mode> [arg], handed the services its modes use and hs[] under tags[]
static lk_status_t spawn(const char *mode, const char *arg, const lk_handle_t *hs,
                         const uint32_t *tags, size_t n, lk_handle_t *child, uint32_t *pid) {
    const char *argv[] = {CHILD, mode, arg};
    const struct test_handoff down[] = {
        {process_h, LK_TAG_PROCESS, LK_RIGHTS_ALL},
        {time_h, LK_TAG_TIME, LK_RIGHTS_ALL},
        {console_h, LK_TAG_CONSOLE, LK_RIGHTS_ALL},
        {vm_h, LK_TAG_VM, LK_RIGHTS_ALL},
    };
    return test_spawn(process_h, CHILD, arg ? 3 : 2, argv, down, 4, hs, tags, n, child, pid);
}

// Waits for the process to end, then returns its exit code.
static int wait_exit(lk_handle_t p) {
    uint32_t sig = 0;
    CHECK_EQ(lk_wait_one(p, LK_SIGNAL_TERMINATED, 10000 * MS, &sig), NO_ERROR);
    CHECK(sig & LK_SIGNAL_TERMINATED);
    struct lk_process_info info = {0};
    CHECK_EQ(lk_process_info_of(process_h, p, &info), NO_ERROR);
    CHECK_EQ(info.state, LK_PROCESS_EXITED);
    return info.exit_code;
}

// child <mode>, run to its end: its exit code
static int run(const char *mode) {
    lk_handle_t p = LK_HANDLE_INVALID;
    CHECK_EQ(spawn(mode, NULL, NULL, NULL, 0, &p, NULL), NO_ERROR);
    const int code = wait_exit(p);
    CHECK_EQ(lk_handle_close(p), NO_ERROR);
    return code;
}

static void test_startup(const struct lk_proc_args *args) {
    console_h = lk_proc_args_find(args, LK_TAG_CONSOLE);
    process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    vm_h = lk_proc_args_find(args, LK_TAG_VM);
    time_h = lk_proc_args_find(args, LK_TAG_TIME);
    ns_h = lk_proc_args_find(args, LK_TAG_NAMESPACE);
    CHECK(console_h != LK_HANDLE_INVALID);
    CHECK(process_h != LK_HANDLE_INVALID);
    CHECK(vm_h != LK_HANDLE_INVALID);
    CHECK(time_h != LK_HANDLE_INVALID);
    CHECK(ns_h != LK_HANDLE_INVALID);
    test_check_failures();
}

// A child that writes into a memory object the parent maps too, and exits 7; then one born with
// only the handles it was given.
static void test_memobj_child(void) {
    lk_handle_t m, dup, p;
    char *va = NULL;
    uint32_t pid = 0;
    CHECK_EQ(lk_vm_memobj_create(vm_h, PAGE, 0, &m, NULL), NO_ERROR);
    CHECK_EQ(lk_vm_memobj_map(vm_h, m, 0, 0, LK_VM_PROT_READ | LK_VM_PROT_WRITE, (void **)&va),
             NO_ERROR);

    // moved to the child, so it goes from here; the parent keeps the original
    CHECK_EQ(lk_handle_dup(m, LK_RIGHTS_ALL, &dup), NO_ERROR);
    const uint32_t tag = LK_TAG_USER_BASE;
    CHECK_EQ(spawn("memobj", "hello from the child", &dup, &tag, 1, &p, &pid), NO_ERROR);
    CHECK_EQ(lk_handle_close(dup), ERR_BAD_HANDLE);
    CHECK_EQ(wait_exit(p), 7);
    CHECK(va && test_streq(va, "hello from the child"));
    struct lk_process_info info = {0};
    CHECK_EQ(lk_process_info_of(process_h, p, &info), NO_ERROR);
    CHECK_EQ(info.pid, pid);
    CHECK_EQ(lk_handle_close(p), NO_ERROR);

    // a vm connection and the memory object, nothing else
    const struct test_handoff vm_only = {vm_h, LK_TAG_VM, LK_RIGHTS_ALL};
    CHECK_EQ(lk_handle_dup(m, LK_RIGHTS_ALL, &dup), NO_ERROR);
    const char *bare_argv[] = {CHILD, "bare"};
    CHECK_EQ(test_spawn(process_h, CHILD, 2, bare_argv, &vm_only, 1, &dup, &tag, 1, &p, NULL),
             NO_ERROR);
    CHECK_EQ(wait_exit(p), 9);
    CHECK(va && test_streq(va, "bare"));
    CHECK_EQ(lk_handle_close(p), NO_ERROR);

    CHECK_EQ(lk_vm_unmap(vm_h, va), NO_ERROR);
    CHECK_EQ(lk_handle_close(m), NO_ERROR);
    say("SPAWNTEST: memory object child ok\n");
}

// child count, handed down[] and n dups of a memory object: its exit code
static int count_handles(const struct test_handoff *down, size_t ndown, size_t n) {
    lk_handle_t m, hs[LK_MSG_MAX_HANDLES];
    uint32_t tags[LK_MSG_MAX_HANDLES];
    CHECK_EQ(lk_vm_memobj_create(vm_h, PAGE, 0, &m, NULL), NO_ERROR);
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ(lk_handle_dup(m, LK_RIGHTS_ALL, &hs[i]), NO_ERROR);
        tags[i] = LK_TAG_USER_BASE + i;
    }
    CHECK_EQ(lk_handle_close(m), NO_ERROR);
    const char *argv[] = {CHILD, "count"};
    lk_handle_t p = LK_HANDLE_INVALID;
    CHECK_EQ(test_spawn(process_h, CHILD, 2, argv, down, ndown, hs, tags, n, &p, NULL),
             NO_ERROR);
    const int code = wait_exit(p);
    CHECK_EQ(lk_handle_close(p), NO_ERROR);
    return code;
}

// A child is born with what it is handed and nothing else: no service comes with a spawn, and
// a handed-down dup keeps the rights it was made with.
static void test_handed_down(void) {
    const struct test_handoff services[] = {
        {process_h, LK_TAG_PROCESS, LK_RIGHTS_ALL},
        {vm_h, LK_TAG_VM, LK_RIGHTS_ALL},
        {time_h, LK_TAG_TIME, LK_RIGHTS_ALL},
        {console_h, LK_TAG_CONSOLE, LK_RIGHTS_ALL},
        {ns_h, LK_TAG_NAMESPACE, LK_RIGHTS_ALL},
    };
    CHECK_EQ(count_handles(NULL, 0, 0), 100);
    CHECK_EQ(count_handles(&services[3], 1, 0), 101);
    CHECK_EQ(count_handles(services, 5, 1), 106);
    CHECK_EQ(count_handles(services, 5, LK_MSG_MAX_HANDLES - 5), 100 + LK_MSG_MAX_HANDLES);

    const struct test_handoff no_impulse = {console_h, LK_TAG_CONSOLE,
                                            LK_RIGHTS_ALL & ~LK_RIGHT_IMPULSE};
    const char *argv[] = {CHILD, "impulse"};
    lk_handle_t p = LK_HANDLE_INVALID;
    CHECK_EQ(test_spawn(process_h, CHILD, 2, argv, &no_impulse, 1, NULL, NULL, 0, &p, NULL),
             NO_ERROR);
    CHECK_EQ(wait_exit(p), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(p), NO_ERROR);
    say("SPAWNTEST: handed down ok\n");
}

// every way out, with one thread or several
static void test_exits(void) {
    CHECK_EQ(run("thread_exit"), 0);
    CHECK_EQ(run("fault"), ERR_FAULT);
    CHECK_EQ(run("threads"), 5);
    CHECK_EQ(run("thread_fault"), ERR_FAULT);
    CHECK_EQ(run("spin"), 3);
    CHECK_EQ(run("last_thread"), 0);
    say("SPAWNTEST: exits ok\n");
}

// A child blocked in mode is killed, and ends at once with LK_PROCESS_KILLED.
static void kill_blocked(const char *mode) {
    lk_handle_t p = LK_HANDLE_INVALID, weak;
    CHECK_EQ(spawn(mode, NULL, NULL, NULL, 0, &p, NULL), NO_ERROR);

    // still running a while later
    uint32_t sig = 1;
    CHECK_EQ(lk_wait_one(p, LK_SIGNAL_TERMINATED, 50 * MS, &sig), ERR_TIMED_OUT);
    CHECK_EQ(sig, 0);
    struct lk_process_info info = {0};
    CHECK_EQ(lk_process_info_of(process_h, p, &info), NO_ERROR);
    CHECK_EQ(info.state, LK_PROCESS_RUNNING);

    // each operation needs its right
    CHECK_EQ(lk_handle_dup(p, LK_RIGHTS_ALL & ~LK_RIGHT_KILL, &weak), NO_ERROR);
    CHECK_EQ(lk_process_kill(process_h, weak), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(weak), NO_ERROR);
    CHECK_EQ(lk_handle_dup(p, LK_RIGHTS_ALL & ~LK_RIGHT_INFO, &weak), NO_ERROR);
    CHECK_EQ(lk_process_info_of(process_h, weak, &info), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(weak), NO_ERROR);
    CHECK_EQ(lk_handle_dup(p, LK_RIGHTS_ALL & ~LK_RIGHT_WAIT, &weak), NO_ERROR);
    CHECK_EQ(lk_wait_one(weak, LK_SIGNAL_TERMINATED, 0, NULL), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(weak), NO_ERROR);

    const uint64_t start = now_ns();
    CHECK_EQ(lk_process_kill(process_h, p), NO_ERROR);
    CHECK_EQ(wait_exit(p), LK_PROCESS_KILLED);
    // the child slept or waited for a minute; a kill takes far less than a second
    CHECK(now_ns() - start < 1000 * MS);
    // a process that has exited keeps its code
    CHECK_EQ(lk_process_kill(process_h, p), NO_ERROR);
    CHECK_EQ(wait_exit(p), LK_PROCESS_KILLED);
    CHECK_EQ(lk_handle_close(p), NO_ERROR);
}

static void test_kill(void) {
    kill_blocked("sleep");
    // the test's console has no input, so the read waits until killed
    kill_blocked("read");
    kill_blocked("wait");
    say("SPAWNTEST: kill ok\n");
}

// A spawn with one carried handle moved or lent, under tag: the call's status.
static lk_status_t spawn_carrying(lk_handle_t h, uint32_t flags, uint32_t tag) {
    const char *path = CHILD;
    const char args[] = CHILD "\0thread_exit";
    struct lk_process_spawn_req req = {
        .flags = 0,
        .path_len = sizeof(CHILD) - 1,
        .nargs = 2,
        .args_len = sizeof(args),
        .tags = {tag},
    };
    struct lk_process_spawn_reply reply;
    lk_handle_t ph = LK_HANDLE_INVALID;
    const struct lk_iov req_iov[3] = {
        {&req, sizeof(req)}, {(void *)path, sizeof(CHILD) - 1}, {(void *)args, sizeof(args)},
    };
    const struct lk_iov reply_iov = {&reply, sizeof(reply)};
    struct lk_msg m = {
        .op = LK_PROCESS_SPAWN, .req = req_iov, .nreq = 3, .reply = &reply_iov, .nreply = 1,
        .carry = &h, .ncarry = 1, .carry_flags = flags, .handles = &ph, .handles_max = 1,
    };
    lk_status_t err = lk_msg_callv(process_h, &m);
    if (err == NO_ERROR) {
        CHECK_EQ(wait_exit(ph), 0);
        CHECK_EQ(lk_handle_close(ph), NO_ERROR);
    }
    return err;
}

static void test_spawn_refused(void) {
    lk_handle_t h, weak, p = LK_HANDLE_INVALID;
    const char *argv[] = {CHILD, "thread_exit"};

    // the image: an absolute path to an ELF file
    CHECK_EQ(lk_process_spawn(process_h, CHILD + 1, 2, argv, 0, NULL, NULL, 0, &p, NULL),
             ERR_BAD_PATH);
    CHECK_EQ(lk_process_spawn(process_h, TEST_IMAGE "/bin/nothere", 2, argv, 0, NULL, NULL, 0,
                              &p, NULL),
             ERR_NOT_FOUND);
    // a file that is there, but no ELF
    struct lk_file_stat st;
    CHECK_EQ(lk_ns_stat(ns_h, TEST_IMAGE "/data/posixtest.txt", &st), NO_ERROR);
    CHECK(lk_process_spawn(process_h, TEST_IMAGE "/data/posixtest.txt", 2, argv, 0, NULL, NULL,
                           0, &p, NULL) < 0);
    // ".." stops at the root
    CHECK_EQ(lk_process_spawn(process_h, "/.." CHILD, 2, argv, 0, NULL, NULL, 0, &p, NULL),
             NO_ERROR);
    CHECK_EQ(wait_exit(p), 0);
    CHECK_EQ(lk_handle_close(p), NO_ERROR);
    // no flag is defined
    CHECK_EQ(lk_process_spawn(process_h, CHILD, 2, argv, 1, NULL, NULL, 0, &p, NULL),
             ERR_INVALID_ARGS);

    // carried handles: moved, under a nonzero tag; a refusal leaves them here
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL, &h), NO_ERROR);
    CHECK_EQ(spawn_carrying(h, LK_HANDLE_LEND(0), LK_TAG_USER_BASE), ERR_INVALID_ARGS);
    CHECK_EQ(spawn_carrying(h, 0, 0), ERR_INVALID_ARGS);
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL & ~LK_RIGHT_TRANSFER, &weak), NO_ERROR);
    CHECK_EQ(spawn_carrying(weak, 0, LK_TAG_USER_BASE), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(weak), NO_ERROR);
    lk_handle_t two[2];
    const uint32_t same_tags[2] = {LK_TAG_USER_BASE, LK_TAG_USER_BASE};
    CHECK_EQ(lk_handle_dup(console_h, LK_RIGHTS_ALL, &two[0]), NO_ERROR);
    two[1] = h;
    CHECK_EQ(lk_process_spawn(process_h, CHILD, 2, argv, 0, two, same_tags, 2, &p, NULL),
             ERR_INVALID_ARGS);
    CHECK_EQ(lk_handle_close(two[0]), NO_ERROR);
    // a spawn that goes through takes it, under a service's tag as under any other
    CHECK_EQ(spawn_carrying(h, 0, LK_TAG_CONSOLE), NO_ERROR);
    CHECK_EQ(lk_handle_close(h), ERR_BAD_HANDLE);

    // a request too short to hold one, and a kill naming no handle
    struct lk_process_info info;
    CHECK_EQ(lk_msg_call(process_h, LK_PROCESS_SPAWN, NULL, 0, &info, sizeof(info), NULL),
             ERR_INVALID_ARGS);
    CHECK_EQ(lk_process_kill(process_h, NOT_A_HANDLE), ERR_BAD_HANDLE);
    say("SPAWNTEST: refused spawns ok\n");
}

static volatile int thread_ran;
static uint8_t stack[STACK_SIZE] __attribute__((aligned(16)));

static void thread_body(void *arg) {
    lk_time_sleep(time_h, 10 * MS);
    thread_ran = *(int *)arg;
}

// a thread of this process, waited for by its handle
static void test_threads(void) {
    lk_handle_t t = LK_HANDLE_INVALID, weak;
    int value = 42;
    CHECK_EQ(lk_thread_start(process_h, thread_body, &value, stack, sizeof(stack), &t), NO_ERROR);
    uint32_t sig = 0;
    CHECK_EQ(lk_wait_one(t, LK_SIGNAL_TERMINATED, 10000 * MS, &sig), NO_ERROR);
    CHECK(sig & LK_SIGNAL_TERMINATED);
    CHECK_EQ(thread_ran, 42);
    // a thread handle is not a process handle, and has no INFO to show
    struct lk_process_info info;
    CHECK_EQ(lk_process_info_of(process_h, t, &info), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_dup(t, LK_RIGHTS_ALL & ~LK_RIGHT_WAIT, &weak), NO_ERROR);
    CHECK_EQ(lk_wait_one(weak, LK_SIGNAL_TERMINATED, 0, NULL), ERR_ACCESS_DENIED);
    CHECK_EQ(lk_handle_close(weak), NO_ERROR);
    CHECK_EQ(lk_handle_close(t), NO_ERROR);

    // where it starts must be inside the address space, and the stack aligned
    const uintptr_t sp = ((uintptr_t)stack + sizeof(stack)) & ~(uintptr_t)15;
    CHECK_EQ(lk_process_thread_create(process_h, 0, sp, &t, NULL), ERR_INVALID_ARGS);
    CHECK_EQ(lk_process_thread_create(process_h, (uintptr_t)&thread_body, sp - 8, &t, NULL),
             ERR_INVALID_ARGS);

    // wait() itself: one to LK_WAIT_MAX_ITEMS items, a handle with WAIT, a timeout (ipctest
    // waits on several)
    struct lk_wait_item items[LK_WAIT_MAX_ITEMS + 1];
    for (int i = 0; i <= LK_WAIT_MAX_ITEMS; i++) {
        items[i] = (struct lk_wait_item){console_h, LK_SIGNAL_TERMINATED};
    }
    struct lk_wait_args args = {.items = items, .n = 0, .timeout_ns = 0};
    CHECK_EQ(lk_wait(&args), ERR_INVALID_ARGS);
    args.n = LK_WAIT_MAX_ITEMS + 1;
    CHECK_EQ(lk_wait(&args), ERR_INVALID_ARGS);
    args.n = LK_WAIT_MAX_ITEMS;
    CHECK_EQ(lk_wait(&args), ERR_TIMED_OUT);
    CHECK_EQ(lk_wait_one(NOT_A_HANDLE, LK_SIGNAL_TERMINATED, 0, NULL), ERR_BAD_HANDLE);
    CHECK_EQ(lk_wait_one(console_h, LK_SIGNAL_TERMINATED, 0, NULL), ERR_TIMED_OUT);
    const uint64_t start = now_ns();
    CHECK_EQ(lk_wait_one(console_h, LK_SIGNAL_TERMINATED, 20 * MS, NULL), ERR_TIMED_OUT);
    CHECK(now_ns() - start >= 20 * MS);
    say("SPAWNTEST: threads ok\n");
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    test_put("SPAWNTEST: start\n");

    test_startup(args);
    test_memobj_child();
    test_handed_down();
    test_exits();
    test_kill();
    test_spawn_refused();
    test_threads();

    test_check_failures();
    say("SPAWNTEST: PASS\n");
    lk_exit(0);
}
