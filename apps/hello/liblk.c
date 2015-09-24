#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

#include <sys/lkuser_syscalls.h>

extern int main(void);

static const struct lkuser_syscall_table *lk_syscalls;

void _start(const struct lkuser_syscall_table *syscalls)
{
    lk_syscalls = syscalls;

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
    return lk_syscalls->sbrk(incr);
}

int _open(const char *name, int flags, int mode)
{
    return lk_syscalls->open(name, flags, mode);
}

int _close(int file)
{
    return lk_syscalls->close(file);
}

int _read(int file, char *ptr, int len)
{
    return lk_syscalls->read(file, ptr, len);
}

int _write(int file, const char *ptr, int len)
{
    return lk_syscalls->write(file, ptr, len);
}

int _lseek(int file, _off_t pos, int whence)
{
    return lk_syscalls->lseek(file, pos, whence);
}

void _exit(int arg)
{
    lk_syscalls->exit(arg);
}

int usleep(useconds_t useconds)
{
    return lk_syscalls->sleep_usec(useconds);
}

int sleep(unsigned int seconds)
{
    return lk_syscalls->sleep_sec(seconds);
}


