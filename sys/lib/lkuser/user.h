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

class proc;

// path, as user space names it, in the kernel's file system tree: the same path under the root
// the console sets with "lkuser root". ERR_TOO_BIG if the result does not fit in out_path_len.
// The caller normalizes path first if ".." must not climb out of the root.
status_t resolve_fs_path(const char *path, char *out_path, size_t out_path_len);

// The len bytes at path, a path a process sent, in the kernel's tree, out holding
// FS_MAX_PATH_LEN bytes. It is normalized before the root goes in front of it, so ".." cannot
// climb out of the root. ERR_BAD_PATH for an empty or relative path or one holding a NUL,
// ERR_TOO_BIG for one that does not fit.
status_t resolve_user_path(const char *path, size_t len, char *out);

// Loads the ELF image at path (a path in the kernel's tree) into p's address space and returns
// its entry point. Runs on any thread: the image is written with p's address space borrowed
// and the caller's put back.
status_t load_image(proc *p, const char *path, vaddr_t *entry);

} // namespace lkuser
