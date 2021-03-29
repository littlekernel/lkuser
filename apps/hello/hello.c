#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <sys/lkuser_syscalls.h>

// test that floating point is working
__attribute__((noinline))
double fadd(double a, double b) {
    return a + b;
}

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

    printf("printing some floating point %f\n", 0.1234);
    printf("%f\n", fadd(0.1234, 0.123));

    buf = malloc(4096);
    char *buf2 = malloc(4096);

    printf("copying some data\n");
    memcpy(buf2, buf, 4096);

    free(buf);
    free(buf2);

    return 99;
}

