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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lk/trace.h>
#include <lk/list.h>
#include <kernel/vm.h>
#include <kernel/thread.h>
#include <lk/err.h>
#include <lib/elf.h>
#include <lib/bio.h>
#include <sys/lkuser_syscalls.h>

#include "lkuser_priv.h"

#define LOCAL_TRACE 0

using namespace lkuser;

void sys_exit(int retcode) {
    LTRACEF("retcode %d\n", retcode);

    auto *t = get_lkuser_thread();
    DEBUG_ASSERT(t);

    // XXX check that we're the last thread in this process
    t->get_proc()->exit(retcode);

    thread_exit(retcode);
}

int sys_write(int file, const char *ptr, int len) {
    LTRACEF("file %d, ptr %p, len %d\n", file, ptr, len);

    if (file == 1 || file == 2) {
        for (int i = 0; i < len; i++) {
            fputc(ptr[i], stdout);
        }
    }

    return len;
}

int sys_open(const char *name, int flags, int mode) {
    LTRACEF("name '%s', flags 0x%x, mode 0x%x\n", name, flags, mode);

    // open the file
    file_handle *handle {};
    status_t err = open_file(name, &handle);
    if (err < 0) {
        return err;
    }

    // find a slot for it in the process's table
    auto *t = get_lkuser_thread();
    proc *p = t->get_proc();
    auto &table = p->get_file_table();

    int fd = table.alloc_handle(handle);
    if (fd < 0) {
        delete handle;
        return fd;
    }

    LTRACEF("returning fd %d\n", fd);
    return fd;
}

int sys_close(int file) {
    LTRACEF("file %d\n", file);

    // close the file in the table
    auto *t = get_lkuser_thread();
    proc *p = t->get_proc();
    auto &table = p->get_file_table();

    return table.close_handle(file);
}

int sys_read(int file, char *ptr, int len) {
    LTRACEF("file %d, ptr %p, len %d\n", file, ptr, len);

    if (len <= 0)
        return 0;

    if (file == 0) {
        /* trying to read from stdin */
        int c = getchar();
        if (c >= 0) {
            /* translate \r -> \n */
            if (c == '\r') c = '\n';

            ptr[0] = c;
            return 1;
        }

        return 0;
    } else {
        /* bad file descriptor */
        auto *t = get_lkuser_thread();
        proc *p = t->get_proc();
        const auto &table = p->get_file_table();

        auto *handle = table.get_handle(file);
        if (!handle) {
            return ERR_INVALID_ARGS;
        }
        return handle->read(ptr, len);
    }
}

int sys_lseek(int file, long pos, int whence) {
    LTRACEF("file %d, pos %ld, whence %d\n", file, pos, whence);

    return -1;
}

void *sys_sbrk(long incr) {
    void *ptr;

    LTRACEF("incr %ld\n", incr);

    auto *t = get_lkuser_thread();
    proc *p = t->get_proc();
    auto ss = p->get_sbrk_state();

    if (incr < 0)
        return NULL;

    if (incr == 0)
        return (void *)ss.last_sbrk;

    if (ss.last_sbrk != 0 && ss.last_sbrk + incr <= ss.last_sbrk_top) {
        LTRACEF("still have space in the last allocation: last_sbrk 0x%lx, last_sbrk_top 0x%lx\n", ss.last_sbrk, ss.last_sbrk_top);
        ptr = (void *)ss.last_sbrk;
        ss.last_sbrk += incr;
        return ptr;
    }

#define HEAP_ALLOC_CHUNK_SIZE (PAGE_SIZE * 16)

    /* allocate a new chunk for the heap */
    size_t alloc_size = ROUNDUP(incr, HEAP_ALLOC_CHUNK_SIZE);
    status_t err = vmm_alloc(t->get_proc()->get_aspace(), "heap", alloc_size, &ptr, PAGE_SIZE_SHIFT,
                             0, ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_NO_EXECUTE);

    ss.last_sbrk = (uintptr_t)ptr + incr;
    ss.last_sbrk_top = (uintptr_t)ptr + HEAP_ALLOC_CHUNK_SIZE;

    LTRACEF("vmm_alloc returns %d, ptr at %p, last_sbrk now 0x%lx, top 0x%lx\n", err, ptr, ss.last_sbrk, ss.last_sbrk_top);

    return (err >= 0) ? ptr : NULL;
}

int sys_sleep_sec(unsigned long seconds) {
    LTRACEF("seconds %lu\n", seconds);

    thread_sleep(seconds * 1000U);
    return 0;
}

int sys_sleep_usec(unsigned long useconds) {
    LTRACEF("useconds %lu\n", useconds);

    thread_sleep(useconds / 1000U);
    return 0;
}

int sys_invalid_syscall(void) {
    LTRACEF("invalid syscall\n");
    return ERR_INVALID_ARGS;
}

const struct lkuser_syscall_table lkuser_syscalls = {
    .exit = &sys_exit,
    .open = &sys_open,
    .close = &sys_close,
    .write = &sys_write,
    .read = &sys_read,
    .lseek = &sys_lseek,
    .sbrk = &sys_sbrk,
    .sleep_sec = &sys_sleep_sec,
    .sleep_usec = &sys_sleep_usec,
};

#if ARCH_ARM
extern "C"
void arm_syscall_handler(struct arm_fault_frame *frame) {
    /* re-enable interrupts to maintain kernel preemptiveness */
    arch_enable_ints();

    LTRACEF("arm syscall: r12 %u\n", frame->r[12]);

    /* build a function pointer to call the routine.
     * the args are jammed into the function independent of if the function
     * uses them or not, which is safe for simple arg passing.
     */
    int64_t (*sfunc)(unsigned long a, unsigned long b, unsigned long c, unsigned long d);

    switch (frame->r[12]) {
#define LK_SYSCALL_DEF(n, ret, name, args...) \
        case n: sfunc = reinterpret_cast<decltype(sfunc)>((uintptr_t)sys_##name); break;
#include <sys/_syscalls.h>
#undef LK_SYSCALL_DEF
        default:
            sfunc = reinterpret_cast<decltype(sfunc)>((uintptr_t)sys_invalid_syscall);
    }

    LTRACEF("func %p\n", sfunc);

    /* call the routine */
    uint64_t ret = sfunc(frame->r[0], frame->r[1], frame->r[2], frame->r[3]);

    /* unpack the 64bit return back into r0 and r1 */
    frame->r[0] = ret & 0xffffffff;
    frame->r[1] = (ret >> 32) & 0xffffffff;
}
#endif
#if ARCH_RISCV
#include <arch/riscv/iframe.h>
extern "C"
void riscv_syscall_handler(struct riscv_short_iframe *frame) {
    /* re-enable interrupts to maintain kernel preemptiveness */
    arch_enable_ints();

    LTRACEF("riscv syscall: t0 %lu\n", frame->t0);

    /* build a function pointer to call the routine.
     * the args are jammed into the function independent of if the function
     * uses them or not, which is safe for simple arg passing.
     */
    int64_t (*sfunc)(unsigned long a, unsigned long b, unsigned long c, unsigned long d);

    switch (frame->t0) {
#define LK_SYSCALL_DEF(n, ret, name, args...) \
        case n: sfunc = reinterpret_cast<decltype(sfunc)>((uintptr_t)sys_##name); break;
#include <sys/_syscalls.h>
#undef LK_SYSCALL_DEF
        default:
            sfunc = reinterpret_cast<decltype(sfunc)>((uintptr_t)sys_invalid_syscall);
    }

    LTRACEF("func %p\n", sfunc);

    /* call the routine */
    uint64_t ret = sfunc(frame->a0, frame->a1, frame->a2, frame->a3);

#if __riscv_xlen__ == 32
    /* unpack the 64bit return back into a0 and a1 */
    frame->a0 = ret & 0xffffffff;
    frame->a1 = (ret >> 32) & 0xffffffff;
#else
    frame->a0 = ret;
#endif

    /* bump the PC forward over the ecall */
    frame->epc += 4;

    arch_disable_ints();
}
#endif


