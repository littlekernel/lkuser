/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// The first process: started at boot and born with a connection to every kernel service with
// a tag (or to the user endpoint serving one in the kernel's place), it makes the rest. Each
// argument after argv[0] is an app, name[:arg...][&]: init spawns TEST_IMAGE/bin/<name> with
// the arguments after the name and a dup of every handle init was born with, under the same
// tag. An app runs to its end before the next one starts, unless it ends in '&', when it runs
// on while the ones after it do; init waits for every one. An app that cannot be spawned
// prints an INIT: FAIL line and the rest still run. Exits 0 after INIT: PASS, or 1. No libc;
// it prints through debug_write, so it never calls the console it hands down.

#define TEST_NAME "INIT"
#include <lk/services.h>
#include <lktest.h>
#include <stdbool.h>

#define MAX_ARGS 16
#define MAX_APPS 16
#define SPEC_MAX 128

// what the apps are born with: a dup of each of init's own handles
static struct test_handoff down[LK_MSG_MAX_HANDLES];
static size_t ndown;

static void put_fail(const char *what, const char *app, lk_status_t err) {
    test_put("INIT: FAIL ");
    test_put(what);
    test_put(" ");
    test_put(app);
    test_put(": ");
    test_put_dec(err);
    test_put("\n");
    test_failures++;
}

// Spawns the app spec names (cut up in place); *background says whether it ended in '&'.
static lk_status_t spawn_app(lk_handle_t process_h, char *spec, bool *background,
                             lk_handle_t *child) {
    size_t len = test_strlen(spec);
    *background = len > 0 && spec[len - 1] == '&';
    if (*background) {
        spec[--len] = '\0';
    }
    const char *argv[MAX_ARGS];
    int argc = 0;
    for (char *field = spec; field; argc++) {
        if (argc == MAX_ARGS) {
            return ERR_TOO_BIG;
        }
        char *colon = field;
        while (*colon && *colon != ':') {
            colon++;
        }
        const char next = *colon;
        *colon = '\0';
        argv[argc] = field;
        field = next ? colon + 1 : NULL;
    }
    // the name, argv[0], becomes its path in the image
    if (argv[0][0] == '\0') {
        return ERR_INVALID_ARGS;
    }
    char path[SPEC_MAX + sizeof(TEST_IMAGE "/bin/")];
    size_t n = 0;
    for (const char *s = TEST_IMAGE "/bin/"; *s; s++) {
        path[n++] = *s;
    }
    for (const char *s = argv[0]; *s; s++) {
        path[n++] = *s;
    }
    path[n] = '\0';
    argv[0] = path;
    return test_spawn(process_h, path, argc, argv, down, ndown, NULL, NULL, 0, child, NULL);
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    const lk_handle_t process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    if (process_h == LK_HANDLE_INVALID) {
        test_put("INIT: FAIL born without a process connection\n");
        lk_exit(1);
    }
    const struct lk_proc_handle *born = lk_proc_args_handles(args);
    for (uint32_t i = 0; i < args->nhandles; i++) {
        if (ndown == LK_MSG_MAX_HANDLES) {
            test_put("INIT: FAIL born with more handles than a spawn carries\n");
            lk_exit(1);
        }
        down[ndown++] = (struct test_handoff){born[i].handle, born[i].tag, LK_RIGHTS_ALL};
    }

    lk_handle_t background[MAX_APPS];
    size_t nbackground = 0;
    for (uint32_t i = 1; i < args->nargs; i++) {
        const char *arg = lk_proc_args_arg(args, i);
        char spec[SPEC_MAX];
        if (test_strlen(arg) >= sizeof(spec)) {
            put_fail("app too long:", arg, ERR_TOO_BIG);
            continue;
        }
        for (size_t j = 0; (spec[j] = arg[j]); j++) {
        }
        bool in_background;
        lk_handle_t child;
        lk_status_t err = spawn_app(process_h, spec, &in_background, &child);
        if (err < 0) {
            put_fail("spawning", arg, err);
            continue;
        }
        if (in_background && nbackground < MAX_APPS) {
            background[nbackground++] = child;
            continue;
        }
        if (in_background) {
            put_fail("no room to run in the background:", arg, ERR_NO_RESOURCES);
        }
        err = lk_wait_one(child, LK_SIGNAL_TERMINATED, LK_TIME_INFINITE, NULL);
        if (err < 0) {
            put_fail("waiting for", arg, err);
        }
        lk_handle_close(child);
    }
    for (size_t i = 0; i < nbackground; i++) {
        const lk_status_t err =
            lk_wait_one(background[i], LK_SIGNAL_TERMINATED, LK_TIME_INFINITE, NULL);
        if (err < 0) {
            put_fail("waiting for", "a background app", err);
        }
        lk_handle_close(background[i]);
    }

    test_check_failures();
    test_put("INIT: PASS\n");
    lk_exit(0);
}
