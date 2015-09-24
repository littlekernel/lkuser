#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#include <sys/lkuser_syscalls.h>

int main(void)
{
    printf("Hello, World!\n");

    printf("sleeping...");
    fflush(stdout);
    sleep(1);
    printf("done\n");

    char *buf;

    buf = malloc(4096);

    printf("enter something\n");
    fgets(buf, 4096, stdin);
    printf("line '%s'\n", buf);

    free(buf);

    return 99;
}

