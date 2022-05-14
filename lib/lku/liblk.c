#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

#include <sys/lkuser_syscalls.h>

extern int main(void);

// TODO: implement __retarget_lock* routines from newlib

#define SYSCALL_FUNCTION_PTR 0

#if SYSCALL_FUNCTION_PTR

/* direct function pointer version */
static const struct lkuser_syscall_table *lk_syscalls;

#define LK_SYSCALL(func, args...) \
    lk_syscalls->func(args)

#elif ARCH_ARM

#define LK_SYSCALL(func, args...) \
    _lk_##func(args)

/* build a function that uses the svc instruction to call into system mode */
#define LK_SYSCALL_DEF(n, ret, name, args...) \
__attribute__((naked)) ret _lk_##name(args) { \
\
    asm volatile( \
        "mov r12, #" #n ";" \
        "svc #99;" \
        "bx lr;" \
        ::: "memory"); \
}

#include <sys/_syscalls.h>

#undef LK_SYSCALL_DEF

#elif ARCH_RISCV

#define LK_SYSCALL(func, args...) \
    _lk_##func(args)

/* build a function that uses the ecall instruction to call into supervisor mode */
#define LK_SYSCALL_DEF(n, ret, name, args...) \
__attribute__((naked)) ret _lk_##name(args) { \
\
    asm volatile( \
        "li t0, " #n ";" \
        "ecall;" \
        "ret;" \
        ::: "memory"); \
}

#include <sys/_syscalls.h>

#undef LK_SYSCALL_DEF

#else
#error define syscall mechanism for this arch
#endif

// Called from startup assembly
void _start_c(const struct lkuser_syscall_table *syscalls)
{
#if SYSCALL_FUNCTION_PTR
    lk_syscalls = syscalls;
#endif

    // register to call the fini array on exit
    extern void __libc_fini_array(void);
    atexit(__libc_fini_array);

    // call the init array
    extern void __libc_init_array(void);
    __libc_init_array();

    int ret = main();

    exit(ret);
}

/* make libc happy */
void _init(void) {}
void _fini(void) {}

extern int errno;

/* implement needed stuff to get it to compile */

int _fstat(int file, struct stat *st)
{
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file) { return 1; }

void *_sbrk(ptrdiff_t incr)
{
    return LK_SYSCALL(sbrk, incr);
}

/* newlib expects us to set errno in the error case */
int _open(const char *name, int flags, int mode)
{
    int err = LK_SYSCALL(open, name, flags, mode);
    if (err < 0) {
        errno = -err;
        return -1;
    }
    return err;
}

int _close(int file)
{
    int err = LK_SYSCALL(close, file);
    if (err < 0) {
        errno = -err;
        return -1;
    }
    return err;
}

int _read(int file, char *ptr, int len)
{
    int err = LK_SYSCALL(read, file, ptr, len);
    if (err < 0) {
        errno = -err;
        return -1;
    }
    return err;
}

int _write(int file, const char *ptr, int len)
{
    int err = LK_SYSCALL(write, file, ptr, len);
    if (err < 0) {
        errno = -err;
        return -1;
    }
    return err;
}

int _lseek(int file, _off_t pos, int whence)
{
    int err = LK_SYSCALL(lseek, file, pos, whence);
    if (err < 0) {
        errno = -err;
        return -1;
    }
    return err;
}

void _exit(int arg)
{
    LK_SYSCALL(exit, arg);

    __builtin_unreachable();
}

int usleep(useconds_t useconds)
{
    return LK_SYSCALL(sleep_usec, useconds);
}

int sleep(unsigned int seconds)
{
    return LK_SYSCALL(sleep_sec, seconds);
}

int _kill (int pid, int sig)
{
    // XXX sort this out
    LK_SYSCALL(exit, 0);

    __builtin_unreachable();
}

pid_t _getpid (void)
{
  return (pid_t)1;
}
