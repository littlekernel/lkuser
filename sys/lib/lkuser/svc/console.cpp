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

// A line as a terminal in canonical mode gives it: echoed as typed, Backspace and DEL erase,
// Return or newline ends it as '\n', ^D ends it as it stands (on an empty line, end of input).
// LK's console hands over raw characters, so this is the line discipline. A line longer than
// the room comes back in pieces.
lk_status_t console_read(kcall &c) {
    lk_console_read_req req;
    lk_status_t err = c.args(&req);
    if (err < 0) {
        return err;
    }

    char line[256];
    const size_t max = MIN(MIN(size_t{req.max}, c.reply_capacity()), sizeof(line));
    size_t len = 0;
    while (len < max) {
        const int ch = getchar();
        if (ch < 0 || ch == 0x04) {
            break;
        }
        if (ch == '\r' || ch == '\n') {
            line[len++] = '\n';
            putchar('\n');
            break;
        }
        if (ch == '\b' || ch == 0x7f) {
            if (len > 0) {
                len--;
                fputs("\b \b", stdout);
            }
            continue;
        }
        line[len++] = static_cast<char>(ch);
        putchar(ch);
    }

    ssize_t n = c.write(0, line, len);
    return n < 0 ? static_cast<lk_status_t>(n) : NO_ERROR;
}

// LK's console has no notion of its size, so this is the classic terminal's.
lk_status_t console_winsize(kcall &c) {
    if (c.len() != 0) {
        return ERR_INVALID_ARGS;
    }
    return c.reply(lk_console_winsize{.cols = 80, .rows = 24});
}

lk_status_t console_handler(kcall &c) {
    switch (c.op()) {
        case LK_CONSOLE_WRITE:
            return console_write(c);
        case LK_CONSOLE_READ:
            return console_read(c);
        case LK_CONSOLE_WINSIZE:
            return console_winsize(c);
        default:
            return ERR_NOT_SUPPORTED;
    }
}

} // namespace

} // namespace lkuser

LKUSER_SERVICE(console, LK_TAG_CONSOLE, &lkuser::console_handler);
