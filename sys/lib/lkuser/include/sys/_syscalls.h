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

// The raw syscall table, one row per syscall: LK_SYSCALL_DEF(number, return type, name,
// arguments...). Included with LK_SYSCALL_DEF defined by the user-space stubs (lib/lku) and by
// the kernel's dispatcher (syscalls.cpp), after <sys/lk_msg.h>; both sides expand the same
// rows, so numbers and signatures cannot skew.
//
// Trap ABI (riscv): number in a7, arguments in a0-a5, the result in a0, LK's NO_ERROR or a
// negative ERR_*. Arguments follow the argument-width rule of <sys/lk_abi.h>: each fits one
// register today, and times travel in the argument structures. A row without a kernel handler
// returns ERR_NOT_SUPPORTED, as does a number past the end of the table.

LK_SYSCALL_DEF(0,  void,        exit,                  int code)
LK_SYSCALL_DEF(1,  void,        thread_exit,           void)
LK_SYSCALL_DEF(2,  lk_status_t, handle_close,          lk_handle_t h)
LK_SYSCALL_DEF(3,  lk_status_t, handle_dup,            lk_handle_t h, uint32_t rights_mask, lk_handle_t *out)
LK_SYSCALL_DEF(4,  lk_status_t, call,                  lk_handle_t conn, struct lk_call_args *args)
LK_SYSCALL_DEF(5,  lk_status_t, recv,                  lk_handle_t h, struct lk_recv_args *args)
LK_SYSCALL_DEF(6,  lk_status_t, msg_read,              lk_token_t token, size_t off, const struct lk_iov *iov, size_t n)
LK_SYSCALL_DEF(7,  lk_status_t, msg_write,             lk_token_t token, size_t off, const struct lk_iov *iov, size_t n)
LK_SYSCALL_DEF(8,  lk_status_t, reply,                 lk_token_t token, const struct lk_reply_args *args)
LK_SYSCALL_DEF(9,  lk_status_t, impulse_send,          lk_handle_t conn, const struct lk_impulse *imp)
LK_SYSCALL_DEF(10, lk_status_t, endpoint_create,       const struct lk_endpoint_args *args, lk_handle_t *out)
LK_SYSCALL_DEF(11, lk_status_t, connect,               lk_handle_t ep, lk_handle_t *out)
LK_SYSCALL_DEF(12, lk_status_t, connection_accept,     lk_token_t token, uintptr_t cookie, lk_handle_t *peer_out)
LK_SYSCALL_DEF(13, lk_status_t, connection_refuse,     lk_token_t token)
LK_SYSCALL_DEF(14, lk_status_t, connection_set_cookie, lk_handle_t peer, uintptr_t cookie)
LK_SYSCALL_DEF(15, lk_status_t, connection_signal,     lk_handle_t peer, uint32_t set, uint32_t clear)
LK_SYSCALL_DEF(16, lk_status_t, group_create,          lk_handle_t *out)
LK_SYSCALL_DEF(17, lk_status_t, group_add,             lk_handle_t group, lk_handle_t ep)
LK_SYSCALL_DEF(18, lk_status_t, group_remove,          lk_handle_t group, lk_handle_t ep)
LK_SYSCALL_DEF(19, lk_status_t, wait,                  struct lk_wait_args *args)
LK_SYSCALL_DEF(20, lk_status_t, debug_write,           const char *buf, size_t len)
