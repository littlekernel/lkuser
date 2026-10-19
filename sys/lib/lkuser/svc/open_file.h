/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// How namespace.OPEN hands what it opened to the file service: a connection to the file
// endpoint whose cookie is the open-file record. The connection owns the record, which closes
// the file system's handle when the connection is destroyed.

#include <lib/fs.h>
#include <lktl/ref_ptr.h>

#include "../endpoint.h"

namespace lkuser {

// fh, opened with flags (LK_NS_OPEN_*). Takes fh: on failure it is closed.
lk_status_t make_file_connection(filehandle *fh, uint32_t flags, lk::ref_ptr<connection> *out);

// dh, opened from path, which READDIR opens again to go back. Takes dh likewise.
lk_status_t make_dir_connection(dirhandle *dh, const char *path, lk::ref_ptr<connection> *out);

} // namespace lkuser
