/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Start-up and the process-wide system calls for newlib programs: the startup block, exit,
// the heap behind sbrk, time and the process id, over the kernel services. The fd table and
// the file calls are in fd.c, directories in dirent.c. A program that does not use newlib
// defines its own _start_c, and none of this is linked in.

#include <errno.h>
#include <lk/services.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "lku_priv.h"

extern int main(int argc, char **argv);
extern void __libc_init_array(void);
extern void __libc_fini_array(void);

lk_handle_t lku_console = LK_HANDLE_INVALID;
lk_handle_t lku_namespace = LK_HANDLE_INVALID;
lk_handle_t lku_process = LK_HANDLE_INVALID;
lk_handle_t lku_time = LK_HANDLE_INVALID;
lk_handle_t lku_vm = LK_HANDLE_INVALID;

// Called by crt0 with the startup block.
void _start_c(const struct lk_proc_args *args) __attribute__((noreturn));
void _start_c(const struct lk_proc_args *args) {
    lku_console = lk_proc_args_find(args, LK_TAG_CONSOLE);
    lku_namespace = lk_proc_args_find(args, LK_TAG_NAMESPACE);
    lku_process = lk_proc_args_find(args, LK_TAG_PROCESS);
    lku_time = lk_proc_args_find(args, LK_TAG_TIME);
    lku_vm = lk_proc_args_find(args, LK_TAG_VM);
    lku_fd_init();

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

int lku_errno(lk_status_t err) {
    switch (err) {
        case ERR_NOT_FOUND:
            return ENOENT;
        case ERR_ALREADY_EXISTS:
            return EEXIST;
        case ERR_NOT_DIR:
            return ENOTDIR;
        case ERR_NOT_FILE:
            return EISDIR;
        case ERR_BAD_PATH:
            return EINVAL;
        case ERR_TOO_BIG:
            return ENAMETOOLONG;
        case ERR_BUSY:
            return EBUSY;
        case ERR_NOT_ALLOWED:
            return EPERM;
        case ERR_ACCESS_DENIED:
            return EACCES;
        case ERR_BAD_HANDLE:
            return EBADF;
        case ERR_NO_MEMORY:
            return ENOMEM;
        case ERR_NO_RESOURCES:
            return EMFILE;
        case ERR_INVALID_ARGS:
            return EINVAL;
        case ERR_NOT_SUPPORTED:
        case ERR_NOT_IMPLEMENTED:
            return ENOSYS;
        case ERR_FAULT:
            return EFAULT;
        case ERR_NOT_ENOUGH_BUFFER:
            return ERANGE;
        case ERR_CHANNEL_CLOSED:
            return EPIPE;
        case ERR_TIMED_OUT:
            return ETIMEDOUT;
        default:
            return EIO;
    }
}

int lku_fail(lk_status_t err) {
    errno = lku_errno(err);
    return -1;
}

void _exit(int code) {
    lk_exit(code);
}

// The heap. newlib's malloc grows it through sbrk, here from chunks of vm.ALLOC memory with a
// bump pointer. malloc copes with a break that jumps, taking the new space as another sbrk
// user's, as long as it only moves up; right after a jump it asks for an alignment correction
// that must follow on contiguously, so each chunk has a page of room beyond the request. A
// chunk the vm service places straight after the last one extends it, one placed below is
// refused, and the break never moves down (malloc copes with that too).
#define HEAP_CHUNK (64 * 1024)
#define HEAP_PAGE  4096

static uintptr_t heap_brk;
static uintptr_t heap_end;

void *_sbrk(ptrdiff_t incr) {
    if (incr < 0) {
        errno = ENOMEM;
        return (void *)-1;
    }
    const size_t n = (size_t)incr;
    if (n <= heap_end - heap_brk) {
        void *p = (void *)heap_brk;
        heap_brk += n;
        return p;
    }
    if (n > SIZE_MAX - HEAP_PAGE - HEAP_CHUNK) {
        errno = ENOMEM;
        return (void *)-1;
    }

    const size_t size = (n + HEAP_PAGE + HEAP_CHUNK - 1) & ~(size_t)(HEAP_CHUNK - 1);
    void *va;
    if (lk_vm_alloc(lku_vm, size, LK_VM_PROT_READ | LK_VM_PROT_WRITE, &va) < 0) {
        errno = ENOMEM;
        return (void *)-1;
    }
    const uintptr_t base = (uintptr_t)va;
    if (heap_end != 0 && base == heap_end) {
        void *p = (void *)heap_brk;
        heap_brk += n;
        heap_end += size;
        return p;
    }
    if (base < heap_end) {
        lk_vm_unmap(lku_vm, va);
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_brk = base + n;
    heap_end = base + size;
    return va;
}

int _kill(int pid, int sig) {
    errno = ENOSYS;
    return -1;
}

pid_t _getpid(void) {
    if (lku_process == LK_HANDLE_INVALID) {
        return lku_fail(ERR_NOT_SUPPORTED);
    }
    struct lk_process_info info;
    lk_status_t err = lk_process_info(lku_process, &info);
    if (err < 0) {
        return lku_fail(err);
    }
    return (pid_t)info.pid;
}

static int sleep_ns(uint64_t ns) {
    if (lku_time == LK_HANDLE_INVALID) {
        return lku_fail(ERR_NOT_SUPPORTED);
    }
    lk_status_t err = lk_time_sleep(lku_time, ns);
    return err < 0 ? lku_fail(err) : 0;
}

unsigned int sleep(unsigned int seconds) {
    return sleep_ns(seconds * 1000000000ull) < 0 ? seconds : 0;
}

int usleep(useconds_t useconds) {
    return sleep_ns(useconds * 1000ull);
}

// There is no wall clock: gettimeofday counts from boot. newlib declares clock_gettime and
// nanosleep only on targets with _POSIX_TIMERS, which riscv64-elf and aarch64-elf are not, so
// they are absent.
int _gettimeofday(struct timeval *tv, void *tz) {
    if (lku_time == LK_HANDLE_INVALID) {
        return lku_fail(ERR_NOT_SUPPORTED);
    }
    uint64_t ns;
    lk_status_t err = lk_time_clock_read(lku_time, LK_CLOCK_MONOTONIC, &ns);
    if (err < 0) {
        return lku_fail(err);
    }
    if (tv) {
        tv->tv_sec = (time_t)(ns / 1000000000u);
        tv->tv_usec = (suseconds_t)(ns % 1000000000u / 1000u);
    }
    return 0;
}
