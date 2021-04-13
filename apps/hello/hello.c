#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <sys/lkuser_syscalls.h>

extern void foo(void);

volatile int bss_var;
volatile int data_var = 9;

// test that floating point is working
__attribute__((noinline))
double fadd(double a, double b) {
    return a + b;
}

int main(void)
{
    //foo();
    printf("Hello, World!\n");
    printf("printing something from bss %d (should be 0)\n", bss_var);
    printf("printing something from data %d (should be 9)\n", data_var);

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

    printf("printing some floating point %f (should be .1234)\n", 0.1234);
    printf("more float printing %f (should be .2464)\n", fadd(0.1234, 0.123));

    buf = malloc(4096);
    char *buf2 = malloc(4096);
    printf("malloced two buffers %p %p\n", buf, buf2);

    printf("copying some data (%p to %p)\n", buf, buf2);
    memcpy(buf2, buf, 4096);

    free(buf);
    free(buf2);

    printf("buffers are freed\n");

    return 99;
}

