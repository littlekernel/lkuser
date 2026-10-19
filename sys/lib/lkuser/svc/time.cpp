/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The time service: sleeping and clocks, in nanoseconds, over LK's millisecond timers and
// microsecond clock.

#include <kernel/thread.h>
#include <platform/time.h>
#include <sys/svc/time.h>

#include "svc.h"

namespace lkuser {

namespace {

// LK's timers count whole milliseconds from a truncated now, so one sleep can end up to a
// millisecond early by the microsecond clock CLOCK_READ reports. The sleep therefore runs until
// that clock passes a deadline, rounded up, so it is never shorter than asked. A kill ends it.
lk_status_t time_sleep(kcall &c) {
    lk_time_sleep_req req;
    lk_status_t err = c.args(&req);
    if (err < 0) {
        return err;
    }
    const uint64_t us = req.ns / 1000 + (req.ns % 1000 != 0);
    const lk_bigtime_t start = current_time_hires();
    const lk_bigtime_t deadline = us > UINT64_MAX - start ? UINT64_MAX : start + us;
    for (lk_bigtime_t now = start; now < deadline; now = current_time_hires()) {
        const uint64_t left = deadline - now;
        const uint64_t ms = left / 1000 + (left % 1000 != 0);
        // LK sleeps at most ~49 days at once
        err = thread_sleep_etc(static_cast<lk_time_t>(MIN(ms, uint64_t{INFINITE_TIME - 1})),
                               WAIT_QUEUE_INTERRUPTIBLE);
        if (err < 0) {
            return err;
        }
    }
    return NO_ERROR;
}

lk_status_t time_clock_read(kcall &c) {
    lk_time_clock_read_req req;
    lk_status_t err = c.args(&req);
    if (err < 0) {
        return err;
    }
    if (req.clock != LK_CLOCK_MONOTONIC) {
        return ERR_NOT_SUPPORTED;
    }
    const lk_time_clock_read_reply reply = {.ns = current_time_hires() * 1000};
    return c.reply(reply);
}

lk_status_t time_handler(kcall &c) {
    // no op takes a handle
    if (c.nhandles() != 0) {
        return ERR_INVALID_ARGS;
    }
    switch (c.op()) {
        case LK_TIME_SLEEP:
            return time_sleep(c);
        case LK_TIME_CLOCK_READ:
            return time_clock_read(c);
        default:
            return ERR_NOT_SUPPORTED;
    }
}

} // namespace

} // namespace lkuser

LKUSER_SERVICE(time, LK_TAG_TIME, &lkuser::time_handler);
