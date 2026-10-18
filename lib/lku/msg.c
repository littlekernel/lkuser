/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// Calls built from a contiguous payload, and the clients of the kernel services on top.

#include <lk/ipc.h>
#include <lk/services.h>

lk_status_t lk_msg_call(lk_handle_t conn, uint32_t op, const void *req, size_t req_len,
                        void *reply, size_t reply_cap, size_t *reply_len) {
    if (req_len > UINT32_MAX) {
        return ERR_TOO_BIG;
    }
    struct lk_msg_hdr hdr = {.op = op, .len = (uint32_t)req_len};
    struct lk_iov req_iov[2] = {{&hdr, sizeof(hdr)}, {(void *)req, req_len}};
    struct lk_msg_hdr reply_hdr = {0};
    struct lk_iov reply_iov[2] = {{&reply_hdr, sizeof(reply_hdr)}, {reply, reply_cap}};
    struct lk_call_args args = {
        .req = req_iov,
        .nreq = req_len ? 2 : 1,
        .reply = reply_iov,
        .nreply = reply_cap ? 2 : 1,
        .timeout_ns = LK_TIME_INFINITE,
    };
    lk_status_t err = lk_call(conn, &args);
    if (err < 0) {
        return err;
    }
    if (reply_len) {
        *reply_len = reply_hdr.len;
    }
    return reply_hdr.status;
}

lk_status_t lk_console_write(lk_handle_t console, const void *buf, size_t len) {
    return lk_msg_call(console, LK_CONSOLE_WRITE, buf, len, NULL, 0, NULL);
}

lk_status_t lk_process_info(lk_handle_t process, struct lk_process_info *info) {
    return lk_msg_call(process, LK_PROCESS_INFO, NULL, 0, info, sizeof(*info), NULL);
}

lk_status_t lk_time_sleep(lk_handle_t time, uint64_t ns) {
    const struct lk_time_sleep_req req = {.ns = ns};
    return lk_msg_call(time, LK_TIME_SLEEP, &req, sizeof(req), NULL, 0, NULL);
}

lk_status_t lk_time_clock_read(lk_handle_t time, uint32_t clock, uint64_t *ns) {
    const struct lk_time_clock_read_req req = {.clock = clock};
    struct lk_time_clock_read_reply reply;
    lk_status_t err = lk_msg_call(time, LK_TIME_CLOCK_READ, &req, sizeof(req), &reply,
                                  sizeof(reply), NULL);
    if (err < 0) {
        return err;
    }
    *ns = reply.ns;
    return NO_ERROR;
}

lk_status_t lk_vm_alloc(lk_handle_t vm, size_t size, uint32_t prot, void **va) {
    const struct lk_vm_alloc_req req = {.size = size, .prot = prot};
    struct lk_vm_alloc_reply reply;
    lk_status_t err = lk_msg_call(vm, LK_VM_ALLOC, &req, sizeof(req), &reply, sizeof(reply), NULL);
    if (err < 0) {
        return err;
    }
    *va = (void *)(uintptr_t)reply.va;
    return NO_ERROR;
}

lk_status_t lk_vm_unmap(lk_handle_t vm, void *va) {
    const struct lk_vm_unmap_req req = {.va = (uintptr_t)va};
    return lk_msg_call(vm, LK_VM_UNMAP, &req, sizeof(req), NULL, 0, NULL);
}
