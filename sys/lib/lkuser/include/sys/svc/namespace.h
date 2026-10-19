/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// The namespace protocol (LK_TAG_NAMESPACE): one tree of paths. Every request is a
// struct lk_ns_path_req followed by path_len bytes of path, with no NUL. A path is absolute;
// ".." stops at the root. ERR_BAD_PATH for a relative path or one holding a NUL, ERR_TOO_BIG
// for one longer than the service handles.

#include <sys/lk_msg.h>
#include <sys/svc/file.h>

// reply: struct lk_file_stat and one handle, a connection speaking the file protocol (rights
// CALL IMPULSE WAIT DUP TRANSFER). Needs room for that handle and the stat, checked before
// anything is opened or created. ERR_NOT_FILE for a directory opened without LK_NS_OPEN_DIR,
// ERR_NOT_DIR for a file opened with it.
#define LK_NS_OPEN   1u
// reply: struct lk_file_stat
#define LK_NS_STAT   2u
// reply: no payload
#define LK_NS_MKDIR  3u
// a file; reply: no payload. ERR_BUSY while it is open.
#define LK_NS_UNLINK 4u
// an empty directory; reply: no payload
#define LK_NS_RMDIR  5u

// LK_NS_OPEN flags; the others take none. A file is opened for READ, WRITE or both.
#define LK_NS_OPEN_READ   (1u << 0)
#define LK_NS_OPEN_WRITE  (1u << 1)
#define LK_NS_OPEN_CREATE (1u << 2) // create the file, empty, if it does not exist
#define LK_NS_OPEN_EXCL   (1u << 3) // with CREATE: ERR_ALREADY_EXISTS if it does
#define LK_NS_OPEN_TRUNC  (1u << 4) // with WRITE: cut the file to length 0
#define LK_NS_OPEN_DIR    (1u << 5) // a directory, for READDIR; alone

struct lk_ns_path_req {
    uint32_t flags;
    uint32_t path_len;
};
LK_ABI_ASSERT(sizeof(struct lk_ns_path_req) == 8);
LK_ABI_ASSERT(offsetof(struct lk_ns_path_req, flags) == 0);
LK_ABI_ASSERT(offsetof(struct lk_ns_path_req, path_len) == 4);
