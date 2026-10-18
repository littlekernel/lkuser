/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The console service: the kernel console behind the console protocol.

#include <stdio.h>
#include <sys/svc/console.h>

#include "svc.h"

namespace lkuser {

namespace {

lk_status_t console_write(kcall &c) {
    char buf[128];
    for (size_t off = 0; off < c.len();) {
        ssize_t n = c.read(off, buf, sizeof(buf));
        if (n < 0) {
            return static_cast<lk_status_t>(n);
        }
        fwrite(buf, 1, n, stdout);
        off += n;
    }
    return NO_ERROR;
}

lk_status_t console_handler(kcall &c) {
    switch (c.op()) {
        case LK_CONSOLE_WRITE:
            return console_write(c);
        default:
            return ERR_NOT_SUPPORTED;
    }
}

} // namespace

} // namespace lkuser

LKUSER_SERVICE(console, LK_TAG_CONSOLE, &lkuser::console_handler);
