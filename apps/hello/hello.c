#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

#include <sys/lkuser_syscalls.h>

int main(void)
{
    printf("Hello, World!\n");

    char *buf;

    buf = malloc(4096);

    fgets(buf, 4096, stdin);
    printf("line '%s'\n", buf);

    free(buf);

    return 99;
}

