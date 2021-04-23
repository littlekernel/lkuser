#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

#include <sys/lkuser_syscalls.h>

extern int main(void);

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

void _start2(const struct lkuser_syscall_table *syscalls)
{
#if SYSCALL_FUNCTION_PTR
    lk_syscalls = syscalls;
#endif

    int ret = main();

    exit(ret);
}

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

int _open(const char *name, int flags, int mode)
{
    return LK_SYSCALL(open, name, flags, mode);
}

int _close(int file)
{
    return LK_SYSCALL(close, file);
}

int _read(int file, char *ptr, int len)
{
    return LK_SYSCALL(read, file, ptr, len);
}

int _write(int file, const char *ptr, int len)
{
    return LK_SYSCALL(write, file, ptr, len);
}

int _lseek(int file, _off_t pos, int whence)
{
    return LK_SYSCALL(lseek, file, pos, whence);
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

