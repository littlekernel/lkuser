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
// on while the ones after it do; init waits for every one. An argument @<path> is a list: a
// file read through the namespace a line at a time, each line an app as above, cut at its
// first '=' (the harness's expected code), blank lines and lines starting with '#' skipped.
// An app that cannot be spawned, or a list that cannot be read, prints an INIT: FAIL line and
// the rest still run. Exits 0 after INIT: PASS, or 1. No libc; it prints through debug_write,
// so it never calls the console it hands down.

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

static lk_handle_t process_h;
static lk_handle_t ns_h;
// the apps run in the background, waited for once every entry has started
static lk_handle_t background[MAX_APPS];
static size_t nbackground;

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
static lk_status_t spawn_app(char *spec, bool *in_background, lk_handle_t *child) {
    size_t len = test_strlen(spec);
    *in_background = len > 0 && spec[len - 1] == '&';
    if (*in_background) {
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

// Spawns one app and, unless it runs in the background, waits for it to end.
static void run_entry(const char *entry) {
    char spec[SPEC_MAX];
    if (test_strlen(entry) >= sizeof(spec)) {
        put_fail("app too long:", entry, ERR_TOO_BIG);
        return;
    }
    for (size_t j = 0; (spec[j] = entry[j]); j++) {
    }
    bool in_background;
    lk_handle_t child;
    lk_status_t err = spawn_app(spec, &in_background, &child);
    if (err < 0) {
        put_fail("spawning", entry, err);
        return;
    }
    if (in_background && nbackground < MAX_APPS) {
        background[nbackground++] = child;
        return;
    }
    if (in_background) {
        put_fail("no room to run in the background:", entry, ERR_NO_RESOURCES);
    }
    err = lk_wait_one(child, LK_SIGNAL_TERMINATED, LK_TIME_INFINITE, NULL);
    if (err < 0) {
        put_fail("waiting for", entry, err);
    }
    lk_handle_close(child);
}

// One line of a list, NUL-terminated in place: cut at its '=', trailing blanks dropped.
static void run_line(char *line, const char *path) {
    size_t len = 0;
    while (line[len] && line[len] != '=') {
        len++;
    }
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r')) {
        len--;
    }
    line[len] = '\0';
    if (len == 0 || line[0] == '#') {
        return;
    }
    if (line[0] == '@') {
        put_fail("a list inside the list", path, ERR_NOT_SUPPORTED);
        return;
    }
    run_entry(line);
}

// Runs the list at path, a line at a time, so it may be any length.
static void run_list(const char *path) {
    if (ns_h == LK_HANDLE_INVALID) {
        put_fail("no namespace to read", path, ERR_NOT_FOUND);
        return;
    }
    lk_handle_t f;
    lk_status_t err = lk_ns_open(ns_h, path, LK_NS_OPEN_READ, &f, NULL);
    if (err < 0) {
        put_fail("opening", path, err);
        return;
    }
    char buf[256];
    char line[SPEC_MAX];
    size_t len = 0;
    bool too_long = false;
    for (uint64_t off = 0;;) {
        size_t n;
        err = lk_file_read(f, off, buf, sizeof(buf), &n);
        if (err < 0) {
            put_fail("reading", path, err);
            break;
        }
        for (size_t i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (len < sizeof(line) - 1) {
                    line[len++] = buf[i];
                } else {
                    too_long = true;
                }
                continue;
            }
            line[len] = '\0';
            if (too_long) {
                put_fail("app too long in", path, ERR_TOO_BIG);
            } else {
                run_line(line, path);
            }
            len = 0;
            too_long = false;
        }
        if (n == 0) {
            break;
        }
        off += n;
    }
    // a last line with no newline
    line[len] = '\0';
    if (too_long) {
        put_fail("app too long in", path, ERR_TOO_BIG);
    } else {
        run_line(line, path);
    }
    lk_handle_close(f);
}

void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    process_h = lk_proc_args_find(args, LK_TAG_PROCESS);
    ns_h = lk_proc_args_find(args, LK_TAG_NAMESPACE);
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

    for (uint32_t i = 1; i < args->nargs; i++) {
        const char *arg = lk_proc_args_arg(args, i);
        if (arg[0] == '@') {
            run_list(arg + 1);
        } else {
            run_entry(arg);
        }
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
