/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// Clients of the kernel services, one function per operation, each taking the connection to
// its service (the startup handle under the service's tag; for the file protocol, the
// connection lk_ns_open returned). Each returns the call's status if the call failed, else the
// service's.

#include <lk/ipc.h>
#include <sys/svc/console.h>
#include <sys/svc/file.h>
#include <sys/svc/namespace.h>
#include <sys/svc/process.h>
#include <sys/svc/time.h>
#include <sys/svc/vm.h>

#ifdef __cplusplus
extern "C" {
#endif

lk_status_t lk_console_write(lk_handle_t console, const void *buf, size_t len);
// a line, or as much of it as fits; *len is 0 at end of input
lk_status_t lk_console_read(lk_handle_t console, void *buf, size_t max, size_t *len);
lk_status_t lk_console_winsize(lk_handle_t console, uint32_t *cols, uint32_t *rows);

lk_status_t lk_process_info(lk_handle_t process, struct lk_process_info *info);
// about another process, by a handle to it (lent)
lk_status_t lk_process_info_of(lk_handle_t process, lk_handle_t proc, struct lk_process_info *info);
// ends every thread of proc (lent); it exits with LK_PROCESS_KILLED
lk_status_t lk_process_kill(lk_handle_t process, lk_handle_t proc);
// /path as argv[0..argc), flags LK_PROCESS_SPAWN_*. The child is born with handles[i] under
// tags[i], moved out of this process. *child (if not NULL) gets a handle to it, *pid its pid.
lk_status_t lk_process_spawn(lk_handle_t process, const char *path, int argc,
                             const char *const *argv, uint32_t flags, const lk_handle_t *handles,
                             const uint32_t *tags, size_t nhandles, lk_handle_t *child,
                             uint32_t *pid);
// a thread starting at entry with sp; *thread gets a handle to it
lk_status_t lk_process_thread_create(lk_handle_t process, uintptr_t entry, uintptr_t sp,
                                     lk_handle_t *thread, uint32_t *tid);
// A thread running fn(arg) on [stack, stack + stack_size), which leaves with lk_thread_exit
// when fn returns. The process has no thread-local storage: newlib is not safe in it.
lk_status_t lk_thread_start(lk_handle_t process, void (*fn)(void *), void *arg, void *stack,
                            size_t stack_size, lk_handle_t *thread);

lk_status_t lk_time_sleep(lk_handle_t time, uint64_t ns);
lk_status_t lk_time_clock_read(lk_handle_t time, uint32_t clock, uint64_t *ns);

// new memory, zeroed, readable and as prot (LK_VM_PROT_*) asks
lk_status_t lk_vm_alloc(lk_handle_t vm, size_t size, uint32_t prot, void **va);
// the region holding va
lk_status_t lk_vm_unmap(lk_handle_t vm, void *va);
// a new memory object of size bytes (rounded up to pages, *actual if not NULL), flags
// LK_VM_MEMOBJ_*; *memobj gets a handle to it
lk_status_t lk_vm_memobj_create(lk_handle_t vm, size_t size, uint32_t flags, lk_handle_t *memobj,
                                size_t *actual);
// maps [off, off + len) of memobj (lent; len 0 to its end) with prot
lk_status_t lk_vm_memobj_map(lk_handle_t vm, lk_handle_t memobj, size_t off, size_t len,
                             uint32_t prot, void **va);

// Paths are absolute and NUL-terminated. lk_ns_open's *file is a connection speaking the file
// protocol, to close with lk_handle_close; st may be NULL.
lk_status_t lk_ns_open(lk_handle_t ns, const char *path, uint32_t flags, lk_handle_t *file,
                       struct lk_file_stat *st);
lk_status_t lk_ns_stat(lk_handle_t ns, const char *path, struct lk_file_stat *st);
lk_status_t lk_ns_mkdir(lk_handle_t ns, const char *path);
lk_status_t lk_ns_unlink(lk_handle_t ns, const char *path);
lk_status_t lk_ns_rmdir(lk_handle_t ns, const char *path);
// path is served by the endpoint ep names (moved: a spare CONNECT handle)
lk_status_t lk_ns_mount(lk_handle_t ns, const char *path, lk_handle_t ep);
// the mount at path goes; ep (lent) names its endpoint
lk_status_t lk_ns_unmount(lk_handle_t ns, const char *path, lk_handle_t ep);
// *conn: a new connection to the server mounted over path; rest[cap] gets the rest of the
// path below its mount point, NUL-terminated (the service refuses a rest longer than cap - 1)
lk_status_t lk_ns_lookup(lk_handle_t ns, const char *path, lk_handle_t *conn, char *rest,
                         size_t cap, size_t *rest_len);

// Positional: nothing keeps an offset. A length past 32 bits is cut short.
lk_status_t lk_file_read(lk_handle_t file, uint64_t off, void *buf, size_t len, size_t *nread);
// flags: LK_FILE_WRITE_*; *end (if not NULL) is the offset after the last byte written
lk_status_t lk_file_write(lk_handle_t file, uint64_t off, uint32_t flags, const void *buf,
                          size_t len, size_t *nwritten, uint64_t *end);
lk_status_t lk_file_fstat(lk_handle_t file, struct lk_file_stat *st);
lk_status_t lk_file_truncate(lk_handle_t file, uint64_t size);
// entry index of the directory, its name NUL-terminated in name[cap]; ERR_NOT_FOUND past the
// end, ERR_NOT_ENOUGH_BUFFER for a name longer than cap - 1
lk_status_t lk_file_readdir(lk_handle_t dir, uint32_t index, char *name, size_t cap,
                            size_t *name_len);
// Between the file at off and memobj (lent) at mem_off, len bytes at most: *count moved.
lk_status_t lk_file_read_into(lk_handle_t file, uint64_t off, lk_handle_t memobj, size_t mem_off,
                              size_t len, size_t *count);
lk_status_t lk_file_write_from(lk_handle_t file, uint64_t off, uint32_t flags, lk_handle_t memobj,
                               size_t mem_off, size_t len, size_t *count, uint64_t *end);

#ifdef __cplusplus
}
#endif
