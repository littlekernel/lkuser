/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The echo test service: copies the request's payload into the reply, piece by piece, at any
// size. Test builds only.

#include <sys/svc/echo.h>

#include "svc.h"

namespace lkuser {

namespace {

lk_status_t echo_echo(kcall &c) {
    uint8_t buf[256];
    for (size_t off = 0; off < c.len();) {
        ssize_t n = c.read(off, buf, sizeof(buf));
        if (n < 0) {
            return static_cast<lk_status_t>(n);
        }
        ssize_t w = c.write(off, buf, n);
        if (w < 0) {
            return static_cast<lk_status_t>(w);
        }
        if (w < n) {
            return ERR_NOT_ENOUGH_BUFFER;
        }
        off += n;
    }
    return NO_ERROR;
}

lk_status_t echo_handler(kcall &c) {
    // no op takes a handle
    if (c.nhandles() != 0) {
        return ERR_INVALID_ARGS;
    }
    switch (c.op()) {
        case LK_ECHO_ECHO:
            return echo_echo(c);
        default:
            return ERR_NOT_SUPPORTED;
    }
}

} // namespace

} // namespace lkuser

LKUSER_SERVICE(echo, LK_TAG_TEST_ECHO, &lkuser::echo_handler);
