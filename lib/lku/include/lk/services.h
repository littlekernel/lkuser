/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// Clients of the kernel services, one function per operation, each taking the connection to
// its service (the startup handle under the service's tag). Each returns the call's status if
// the call failed, else the service's.

#include <lk/ipc.h>
#include <sys/svc/console.h>
#include <sys/svc/process.h>
#include <sys/svc/time.h>
#include <sys/svc/vm.h>

#ifdef __cplusplus
extern "C" {
#endif

lk_status_t lk_console_write(lk_handle_t console, const void *buf, size_t len);

lk_status_t lk_process_info(lk_handle_t process, struct lk_process_info *info);

lk_status_t lk_time_sleep(lk_handle_t time, uint64_t ns);
lk_status_t lk_time_clock_read(lk_handle_t time, uint32_t clock, uint64_t *ns);

// new memory, zeroed, readable and as prot (LK_VM_PROT_*) asks
lk_status_t lk_vm_alloc(lk_handle_t vm, size_t size, uint32_t prot, void **va);
// the region holding va
lk_status_t lk_vm_unmap(lk_handle_t vm, void *va);

#ifdef __cplusplus
}
#endif
