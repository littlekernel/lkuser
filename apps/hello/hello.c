#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

int main(void)
{
    printf("Hello, World!\n");

    return 0;
}

void _start(void)
{
    int ret = main();

    exit(ret);
}

/* implement needed stuff to get it to compile */
int _close(int file) { return -1; }

int _fstat(int file, struct stat *st)
{
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file) { return 1; }

int _lseek(int file, int ptr, int dir) { return 0; }

int _open(const char *name, int flags, int mode) { return -1; }

int _read(int file, char *ptr, int len) { return 0; }

void *_sbrk(ptrdiff_t incr)
{
    static char *heap_end = 0;
    char *prev_heap_end;

    extern char _heap_low; /* Defined by the linker */
    extern char _heap_top; /* Defined by the linker */

    if (heap_end == 0) {
        heap_end = &_heap_low;
    }
    prev_heap_end = heap_end;

    if (heap_end + incr > &_heap_top) {
        /* Out of heap */
        return 0;
    }

    heap_end += incr;
    return prev_heap_end;
}

int _write(int file, char *ptr, int len)
{
    return len;
}

void _exit(int arg)
{
    for (;;);
}

