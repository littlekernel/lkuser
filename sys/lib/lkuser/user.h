/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <sys/types.h>

namespace lkuser {

// path, as user space names it, in the kernel's file system tree: the same path under the root
// the console sets with "lkuser root". ERR_TOO_BIG if the result does not fit in out_path_len.
// The caller normalizes path first if ".." must not climb out of the root.
status_t resolve_fs_path(const char *path, char *out_path, size_t out_path_len);

} // namespace lkuser
