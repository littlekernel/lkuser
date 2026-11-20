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
#include "syscalls.h"

#include <kernel/thread.h>
#include <lk/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <type_traits>
#include <utility>

#include "copy.h"
#include "proc.h"
#include "thread.h"

// user space's status codes are LK's
#define LK_ERR_DEF(name, value) static_assert(name == (value), #name " differs from LK's");
#include <sys/_errors.h>
#undef LK_ERR_DEF

namespace lkuser {

// Ends the process; its other threads leave at their next way out of the kernel.
void sys_exit(int code) {
    thread::current()->process()->exit(code);
    thread::exit_current();
}

// The last thread out ends the process with code 0.
void sys_thread_exit() {
    thread::exit_current();
}

// Writes the bytes as they are, with nothing added, straight to the kernel console.
status_t sys_debug_write(user_ptr<const char> buf, size_t len) {
    vmm_aspace_t *aspace = thread::current()->process()->aspace();
    // the whole range up front, so a bad length prints nothing
    if (!user_range_valid(aspace, buf.addr(), len)) {
        return ERR_FAULT;
    }

    char kbuf[128];
    for (size_t done = 0; done < len;) {
        const size_t chunk = MIN(len - done, sizeof(kbuf));
        status_t err = copy_from_user(aspace, kbuf, buf.addr() + done, chunk);
        if (err < 0) {
            return err;
        }
        fwrite(kbuf, 1, chunk, stdout);
        done += chunk;
    }
    return NO_ERROR;
}

namespace {

constexpr size_t kArgRegs = 6;

// A register as a handler argument: an integer keeps its low bits, whatever user space left
// above them, and a user pointer keeps the address.
template <typename T>
struct from_reg {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "a row argument is an integer or a pointer");
    static_assert(sizeof(T) <= sizeof(ulong), "a row argument fits one register");
    static T get(ulong r) { return static_cast<T>(r); }
};
template <typename T>
struct from_reg<user_ptr<T>> {
    static user_ptr<T> get(ulong r) { return user_ptr<T>(r); }
};

template <typename R, typename... A, size_t... I>
long invoke(R (*handler)(A...), [[maybe_unused]] const ulong *regs, std::index_sequence<I...>) {
    static_assert(sizeof...(A) <= kArgRegs, "a row takes at most six arguments");
    if constexpr (std::is_void_v<R>) {
        handler(from_reg<A>::get(regs[I])...);
        return NO_ERROR;
    } else {
        static_assert(std::is_same_v<R, status_t>, "a row returns void or a status");
        return handler(from_reg<A>::get(regs[I])...);
    }
}

template <typename R, typename... A>
long invoke(R (*handler)(A...), const ulong *regs) {
    return invoke(handler, regs, std::index_sequence_for<A...>{});
}

long dispatch(ulong num, const ulong *regs) {
    switch (num) {
#define LK_SYSCALL_DEF(n, ret, name, args...) \
        case n: \
            return invoke(&sys_##name, regs);
#include <sys/_syscalls.h>
#undef LK_SYSCALL_DEF
        default:
            return ERR_NOT_SUPPORTED;
    }
}

// After a fault's registers: the thread, whether it is its process's main thread, and the
// main stack's range, so an sp outside it shows.
void print_fault_thread(thread *t, proc *p) {
    const vaddr_t base = p->main_stack();
    printf("lkuser:   thread %u (%s), main stack %#lx-%#lx\n", t->tid(),
           t->tid() == p->main_tid() ? "the main thread" : "not the main thread",
           (unsigned long)base, (unsigned long)(base + proc::kMainStackSize));
}

} // namespace

} // namespace lkuser

#if ARCH_RISCV
#include <arch/riscv.h>
#include <arch/riscv/iframe.h>

// Number in a7, arguments in a0-a5, the status back in a0. The arch has already stepped epc
// past the ecall.
extern "C" void riscv_syscall_handler(struct riscv_short_iframe *frame) {
    lkuser::thread *t = lkuser::thread::current();
    if (!t) {
        riscv_syscall_unhandled(frame);
    }

    // handlers block, take mutexes and copy user memory, none of which works with
    // interrupts disabled
    arch_enable_ints();
    t->set_in_syscall(true);

    const ulong regs[lkuser::kArgRegs] = {
        frame->a0, frame->a1, frame->a2, frame->a3, frame->a4, frame->a5,
    };
    frame->a0 = static_cast<ulong>(lkuser::dispatch(frame->a7, regs));

    t->set_in_syscall(false);
    arch_disable_ints();
}

// Any other trap from user mode ends the process with ERR_FAULT: this thread leaves now, the
// others at their next way out of the kernel.
extern "C" void riscv_user_exception(long cause, ulong epc, struct riscv_short_iframe *frame) {
    lkuser::thread *t = lkuser::thread::current();
    if (!t) {
        riscv_user_exception_unhandled(cause, epc, frame);
    }

    // before interrupts are enabled, as another trap may overwrite it
    const ulong tval = riscv_csr_read(RISCV_CSR_XTVAL);
    lkuser::proc *p = t->process();
    printf("lkuser: process %u (%s) faulted: cause %ld, epc %#lx, tval %#lx\n", p->pid(),
           p->name(), cause, epc, tval);
    // the short frame is all of the user's registers it holds
#if __riscv_xlen == 64
#define R "%016lx"
#else
#define R "%08lx"
#endif
    printf("lkuser:   ra " R " sp " R " gp " R " tp " R "\n", frame->ra, frame->sp, frame->gp,
           frame->tp);
    printf("lkuser:   s0 " R " status " R "\n", frame->s0, frame->status);
    printf("lkuser:   a0 " R " a1 " R " a2 " R " a3 " R "\n", frame->a0, frame->a1, frame->a2,
           frame->a3);
    printf("lkuser:   a4 " R " a5 " R " a6 " R " a7 " R "\n", frame->a4, frame->a5, frame->a6,
           frame->a7);
    printf("lkuser:   t0 " R " t1 " R " t2 " R " t3 " R "\n", frame->t0, frame->t1, frame->t2,
           frame->t3);
    printf("lkuser:   t4 " R " t5 " R " t6 " R "\n", frame->t4, frame->t5, frame->t6);
#undef R
    lkuser::print_fault_thread(t, p);

    // exit takes mutexes, as the syscall path does
    arch_enable_ints();
    p->exit(ERR_FAULT);
    lkuser::thread::exit_current();
}

// A killed thread on its way to user mode, back from a syscall, a fault or an interrupt or on
// its first entry (frame NULL, which this never reads): its process has exited (exit, a fault
// or process.KILL), so the thread leaves too.
extern "C" void riscv_user_kill(struct riscv_short_iframe *frame) {
    // exit_current takes mutexes
    arch_enable_ints();
    if (!lkuser::thread::current()) {
        thread_exit(ERR_INTERRUPTED);
    }
    lkuser::thread::exit_current();
}

#elif ARCH_ARM64
#include <arch/arm64.h>

// Number in x8, arguments in x0-x5, the status back in x0. elr already points past the svc.
extern "C" void arm64_syscall(struct arm64_iframe_long *iframe, bool is_64bit) {
    lkuser::thread *t = lkuser::thread::current();
    // user space runs aarch64 only
    if (!t || !is_64bit) {
        arm64_syscall_unhandled(iframe, is_64bit);
    }

    // handlers block, take mutexes and copy user memory, none of which works with
    // interrupts disabled
    arch_enable_ints();
    t->set_in_syscall(true);

    const ulong regs[lkuser::kArgRegs] = {
        iframe->r[0], iframe->r[1], iframe->r[2], iframe->r[3], iframe->r[4], iframe->r[5],
    };
    iframe->r[0] = static_cast<ulong>(lkuser::dispatch(iframe->r[8], regs));

    // the way back to EL0 checks for a kill with interrupts disabled
    t->set_in_syscall(false);
    arch_disable_ints();
}

// Any other exception from EL0 ends the process with ERR_FAULT: this thread leaves now, the
// others at their next way out of the kernel.
extern "C" void arm64_user_exception(struct arm64_iframe_long *iframe, uint32_t esr,
                                     uint64_t far) {
    lkuser::thread *t = lkuser::thread::current();
    if (!t) {
        arm64_user_exception_unhandled(iframe, esr, far);
    }

    lkuser::proc *p = t->process();
    printf("lkuser: process %u (%s) faulted: esr %#x, elr %#llx, far %#llx\n", p->pid(),
           p->name(), esr, iframe->elr, far);
    for (int i = 0; i < 28; i += 4) {
        printf("lkuser:   x%-2d %016llx x%-2d %016llx x%-2d %016llx x%-2d %016llx\n", i,
               iframe->r[i], i + 1, iframe->r[i + 1], i + 2, iframe->r[i + 2], i + 3,
               iframe->r[i + 3]);
    }
    printf("lkuser:   x28 %016llx x29 %016llx lr  %016llx sp  %016llx\n", iframe->r[28],
           iframe->r[29], iframe->lr, iframe->usp);
    printf("lkuser:   spsr %016llx\n", iframe->spsr);
    lkuser::print_fault_thread(t, p);

    // exit takes mutexes, as the syscall path does
    arch_enable_ints();
    p->exit(ERR_FAULT);
    lkuser::thread::exit_current();
}

// A killed thread on its way to EL0, back from a syscall, a fault or an interrupt or on its
// first entry: its process has exited (exit, a fault or process.KILL), so the thread leaves too.
extern "C" void arm64_user_kill() {
    // exit_current takes mutexes
    arch_enable_ints();
    if (!lkuser::thread::current()) {
        thread_exit(ERR_INTERRUPTED);
    }
    lkuser::thread::exit_current();
}

#else
#error no syscall entry for this architecture
#endif
