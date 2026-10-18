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

void sys_exit(int code) {
    thread::current()->process()->exit(code);
    thread_exit(code);
}

void sys_thread_exit() {
    proc *p = thread::current()->process();
    if (p->thread_exiting()) {
        p->exit(0);
    }
    thread_exit(0);
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

// Rows this executive does not serve.
status_t sys_recv(lk_handle_t, user_ptr<lk_recv_args>) { return ERR_NOT_SUPPORTED; }
status_t sys_reply(lk_token_t, user_ptr<const lk_reply_args>) { return ERR_NOT_SUPPORTED; }
status_t sys_impulse_send(lk_handle_t, user_ptr<const lk_impulse>) { return ERR_NOT_SUPPORTED; }
status_t sys_connection_accept(lk_token_t, uintptr_t, user_ptr<lk_handle_t>) { return ERR_NOT_SUPPORTED; }
status_t sys_connection_refuse(lk_token_t) { return ERR_NOT_SUPPORTED; }
status_t sys_connection_set_cookie(lk_handle_t, uintptr_t) { return ERR_NOT_SUPPORTED; }
status_t sys_connection_signal(lk_handle_t, uint32_t, uint32_t) { return ERR_NOT_SUPPORTED; }
status_t sys_group_create(user_ptr<lk_handle_t>) { return ERR_NOT_SUPPORTED; }
status_t sys_group_add(lk_handle_t, lk_handle_t) { return ERR_NOT_SUPPORTED; }
status_t sys_group_remove(lk_handle_t, lk_handle_t) { return ERR_NOT_SUPPORTED; }
status_t sys_wait(user_ptr<lk_wait_args>) { return ERR_NOT_SUPPORTED; }

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

// Any other trap from user mode ends the process with ERR_FAULT. Every process has one thread,
// so ending this one is all it takes.
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

    // exit takes mutexes, as the syscall path does
    arch_enable_ints();
    p->exit(ERR_FAULT);
    thread_exit(ERR_FAULT);
}
#endif
