/*
 * Copyright (c) 2015 Travis Geiselbrecht
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "lkuser_priv.h"

#include <trace.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <list.h>
#include <kernel/vm.h>
#include <kernel/thread.h>
#include <err.h>
#include <lib/elf.h>
#include <lib/bio.h>
#include <sys/lkuser_syscalls.h>

#define LOCAL_TRACE 0

void sys_exit(int retcode) __NO_RETURN;
void sys_exit(int retcode)
{
    LTRACEF("retcode %d\n", retcode);

    lkuser_state_t *s = get_lkuser_state();

    // XXX check that we're the last thread in this process
    s->state = STATE_DEAD;
    s->retcode = retcode;
    event_signal(&s->event, true);

    thread_exit(retcode);
}

int  sys_write(int file, char *ptr, int len)
{
    LTRACEF("file %d, ptr %p, len %d\n", file, ptr, len);

    if (file == 1 || file == 2) {
        for (int i = 0; i < len; i++) {
            fputc(ptr[i], stdout);
        }
    }

    return len;
}

const struct lkuser_syscall_table lkuser_syscalls = {
    .exit = &sys_exit,
    .write = &sys_write,
};

