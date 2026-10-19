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

lk_status_t lk_time_sleep(lk_handle_t time, uint64_t ns);
lk_status_t lk_time_clock_read(lk_handle_t time, uint32_t clock, uint64_t *ns);

// new memory, zeroed, readable and as prot (LK_VM_PROT_*) asks
lk_status_t lk_vm_alloc(lk_handle_t vm, size_t size, uint32_t prot, void **va);
// the region holding va
lk_status_t lk_vm_unmap(lk_handle_t vm, void *va);

// Paths are absolute and NUL-terminated. lk_ns_open's *file is a connection speaking the file
// protocol, to close with lk_handle_close; st may be NULL.
lk_status_t lk_ns_open(lk_handle_t ns, const char *path, uint32_t flags, lk_handle_t *file,
                       struct lk_file_stat *st);
lk_status_t lk_ns_stat(lk_handle_t ns, const char *path, struct lk_file_stat *st);
lk_status_t lk_ns_mkdir(lk_handle_t ns, const char *path);
lk_status_t lk_ns_unlink(lk_handle_t ns, const char *path);
lk_status_t lk_ns_rmdir(lk_handle_t ns, const char *path);

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

#ifdef __cplusplus
}
#endif
