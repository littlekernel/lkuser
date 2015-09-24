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

/* included from other files to define a syscall api */
LK_SYSCALL_DEF(0, void,  exit,      int retcode)
LK_SYSCALL_DEF(1, int,   open,      const char * name, int flags, int mode)
LK_SYSCALL_DEF(2, int,   close,     int file)
LK_SYSCALL_DEF(3, int,   write,     int file, const char *ptr, int len)
LK_SYSCALL_DEF(4, int,   read,      int file, char *ptr, int len)
LK_SYSCALL_DEF(5, int,   lseek,     int file, long pos, int whence)
LK_SYSCALL_DEF(6, void*, sbrk,      long incr)
LK_SYSCALL_DEF(7, int,   sleep_sec, unsigned long useconds)
LK_SYSCALL_DEF(8, int,   sleep_usec, unsigned long useconds)

