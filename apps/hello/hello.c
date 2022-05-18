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

void call_atexit(void) {
    printf("at exit here!\n");
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

    printf("registering atexit call\n");
    atexit(call_atexit);

    FILE *fp = fopen("/lku/data/testfile", "r");
    printf("fopen returns %p\n", fp);
    if (fp) {
        buf = malloc(4096);
        size_t err = fread(buf, 1, 4096, fp);
        printf("fread returns %lu\n", err);

        if (err > 0) {
            printf("file data:\n");
            for (size_t i = 0; i < err; i++) {
                putchar(buf[i]);
            }
            printf("\n");
        }

        free(buf);
    }

    printf("exiting main\n");
    return 99;
}

