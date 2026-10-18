/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The process service: what the kernel knows about processes.

#include <sys/svc/process.h>

#include "../proc.h"
#include "svc.h"

namespace lkuser {

namespace {

// About the caller; a process handle carried in the request names another one, once calls
// can carry handles.
lk_status_t process_info(kcall &c) {
    if (c.len() != 0) {
        return ERR_INVALID_ARGS;
    }
    const lk_process_info info = {
        .pid = c.caller()->pid(),
        .state = LK_PROCESS_RUNNING,
        .exit_code = 0,
        .reserved = 0,
    };
    return c.reply(info);
}

lk_status_t process_handler(kcall &c) {
    switch (c.op()) {
        case LK_PROCESS_INFO:
            return process_info(c);
        default:
            return ERR_NOT_SUPPORTED;
    }
}

} // namespace

} // namespace lkuser

LKUSER_SERVICE(process, LK_TAG_PROCESS, &lkuser::process_handler);
