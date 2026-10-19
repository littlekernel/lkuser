/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// What lib/lku's newlib layer shares between its files.

#include <lk/ipc.h>
#include <stddef.h>

// The connections the process was born with, set by _start_c; LK_HANDLE_INVALID for any its
// spawner did not give it. The calls needing that service then fail with ENOSYS, except sbrk,
// whose callers expect ENOMEM; without a console, fds 0, 1 and 2 are not open.
extern lk_handle_t lku_console;
extern lk_handle_t lku_namespace;
extern lk_handle_t lku_process;
extern lk_handle_t lku_time;
extern lk_handle_t lku_vm;

// errno for a status
int lku_errno(lk_status_t err);
// sets errno for err, and returns -1 for the caller to return
int lku_fail(lk_status_t err);

// path as the namespace takes it, absolute, in buf if it has to be built: the working
// directory is always "/". NULL with errno set: ENOSYS without a namespace connection, else
// for an empty path or one that does not fit.
const char *lku_ns_path(const char *path, char *buf, size_t cap);

// binds fds 0, 1 and 2 to the console; called by _start_c before anything can print
void lku_fd_init(void);
