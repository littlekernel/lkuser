/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The console service: the kernel console behind the console protocol.

#include <kernel/wait.h>
#include <lib/cbuf.h>
#include <lib/io.h>
#include <stdio.h>
#include <sys/svc/console.h>

// READ takes characters from the console's input buffer, where a kill can interrupt the wait
#if !CONSOLE_HAS_INPUT_BUFFER
#error "the console service needs CONSOLE_HAS_INPUT_BUFFER"
#endif

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

// The next character typed: ERR_INTERRUPTED if a kill ends the wait.
int read_char() {
    char ch;
    ssize_t n = cbuf_read_etc(&console_input_cbuf, &ch, 1, true, WAIT_QUEUE_INTERRUPTIBLE);
    if (n < 0) {
        return static_cast<int>(n);
    }
    return static_cast<unsigned char>(ch);
}

// A line as a terminal in canonical mode gives it: echoed as typed, Backspace and DEL erase,
// Return or newline ends it as '\n', ^D ends it as it stands (on an empty line, end of input).
// LK's console hands over raw characters, so this is the line discipline. A line longer than
// the room comes back in pieces. The wait for a character is a kill point.
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
        const int ch = read_char();
        if (ch == ERR_INTERRUPTED) {
            return ERR_INTERRUPTED;
        }
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
    // no op takes a handle
    if (c.nhandles() != 0) {
        return ERR_INVALID_ARGS;
    }
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
