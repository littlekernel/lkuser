/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// LK's status codes for user space, with LK's spelling: an enumerator per row of
// <sys/_errors.h>. User space only; the kernel has <lk/err.h>, whose macros these names
// would collide with.

enum {
#define LK_ERR_DEF(name, value) name = (value),
#include <sys/_errors.h>
#undef LK_ERR_DEF
};
