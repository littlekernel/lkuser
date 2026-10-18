/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Start-up and the system call layer for newlib programs. A program that does not use
// newlib defines its own _start_c, and nothing here is linked in.
//
// Only _exit reaches the kernel. Everything else fails with ENOSYS (_sbrk with ENOMEM), as
// files, the console and memory are services this layer does not call.

#include <errno.h>
#include <lk/ipc.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern int main(int argc, char **argv);
extern void __libc_init_array(void);
extern void __libc_fini_array(void);

// Called by crt0 with the startup block.
void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    // _start_c never returns, so its frame holds argv for the life of the program
    const int argc = (int)args->nargs;
    char *argv[argc + 1];
    for (int i = 0; i < argc; i++) {
        argv[i] = (char *)lk_proc_args_arg(args, i);
    }
    argv[argc] = NULL;

    atexit(__libc_fini_array);
    __libc_init_array();

    exit(main(argc, argv));
}

// __libc_init_array and __libc_fini_array call these
void _init(void) {}
void _fini(void) {}

void _exit(int code) {
    lk_exit(code);
}

void *_sbrk(ptrdiff_t incr) {
    errno = ENOMEM;
    return (void *)-1;
}

int _open(const char *name, int flags, int mode) {
    errno = ENOSYS;
    return -1;
}

int _close(int file) {
    errno = ENOSYS;
    return -1;
}

int _read(int file, char *ptr, int len) {
    errno = ENOSYS;
    return -1;
}

int _write(int file, const char *ptr, int len) {
    errno = ENOSYS;
    return -1;
}

int _lseek(int file, _off_t pos, int whence) {
    errno = ENOSYS;
    return -1;
}

int _fstat(int file, struct stat *st) {
    errno = ENOSYS;
    return -1;
}

int _isatty(int file) {
    errno = ENOSYS;
    return 0;
}

int _kill(int pid, int sig) {
    errno = ENOSYS;
    return -1;
}

pid_t _getpid(void) {
    errno = ENOSYS;
    return -1;
}

int usleep(useconds_t useconds) {
    errno = ENOSYS;
    return -1;
}

unsigned int sleep(unsigned int seconds) {
    errno = ENOSYS;
    return seconds;
}
