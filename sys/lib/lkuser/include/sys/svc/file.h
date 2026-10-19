/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// The file protocol: an open file or directory, spoken over the connection namespace.OPEN
// returns. Closing the connection's last handle closes the file. I/O is positional: the client
// keeps the offset and every READ and WRITE says where. Offsets and sizes are at most
// INT64_MAX.

#include <sys/lk_msg.h>

// request: struct lk_file_read_req; reply payload: the bytes, fewer at the end of the file.
// Needs a file opened for LK_NS_OPEN_READ.
#define LK_FILE_READ     1u
// request: struct lk_file_write_req, then the bytes; reply: struct lk_file_write_reply. Needs
// a file opened for LK_NS_OPEN_WRITE.
#define LK_FILE_WRITE    2u
// request: no payload; reply: struct lk_file_stat
#define LK_FILE_STAT     3u
// request: struct lk_file_truncate_req; reply: no payload. Needs LK_NS_OPEN_WRITE.
#define LK_FILE_TRUNCATE 4u
// request: struct lk_file_readdir_req; reply: struct lk_file_dirent, then the name.
// ERR_NOT_FOUND past the last entry. Needs a directory opened with LK_NS_OPEN_DIR.
#define LK_FILE_READDIR  5u
// request: struct lk_file_io_req and a memory object with LK_RIGHT_WRITE (lent, or moved);
// reply: struct lk_file_io_reply. The file's bytes from off go into the object at mem_off,
// fewer at the end of the file. Needs LK_NS_OPEN_READ.
#define LK_FILE_READ_INTO  6u
// request: struct lk_file_io_req and a memory object with LK_RIGHT_READ; reply: struct
// lk_file_io_reply. The object's bytes from mem_off go into the file at off, or at its end
// with LK_FILE_WRITE_APPEND. Needs LK_NS_OPEN_WRITE.
#define LK_FILE_WRITE_FROM 7u

#define LK_FILE_TYPE_FILE 1u
#define LK_FILE_TYPE_DIR  2u

// the longest name READDIR returns
#define LK_NAME_MAX 255

struct lk_file_stat {
    uint64_t size; // bytes; 0 for a directory
    uint32_t type; // LK_FILE_TYPE_*
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_file_stat) == 16);
LK_ABI_ASSERT(offsetof(struct lk_file_stat, size) == 0);
LK_ABI_ASSERT(offsetof(struct lk_file_stat, type) == 8);
LK_ABI_ASSERT(offsetof(struct lk_file_stat, reserved) == 12);

struct lk_file_read_req {
    uint64_t off;
    uint32_t len; // bytes at most; the reply's room limits it too
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_file_read_req) == 16);
LK_ABI_ASSERT(offsetof(struct lk_file_read_req, off) == 0);
LK_ABI_ASSERT(offsetof(struct lk_file_read_req, len) == 8);
LK_ABI_ASSERT(offsetof(struct lk_file_read_req, reserved) == 12);

// at the file's end, wherever that is when the service writes; off is ignored
#define LK_FILE_WRITE_APPEND (1u << 0)

struct lk_file_write_req {
    uint64_t off;
    uint32_t flags; // LK_FILE_WRITE_*
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_file_write_req) == 16);
LK_ABI_ASSERT(offsetof(struct lk_file_write_req, off) == 0);
LK_ABI_ASSERT(offsetof(struct lk_file_write_req, flags) == 8);
LK_ABI_ASSERT(offsetof(struct lk_file_write_req, reserved) == 12);

struct lk_file_write_reply {
    uint64_t end;     // the offset just past the last byte written
    uint32_t written; // fewer than sent only if the file system stopped early
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_file_write_reply) == 16);
LK_ABI_ASSERT(offsetof(struct lk_file_write_reply, end) == 0);
LK_ABI_ASSERT(offsetof(struct lk_file_write_reply, written) == 8);
LK_ABI_ASSERT(offsetof(struct lk_file_write_reply, reserved) == 12);

struct lk_file_truncate_req {
    uint64_t size;
};
LK_ABI_ASSERT(sizeof(struct lk_file_truncate_req) == 8);
LK_ABI_ASSERT(offsetof(struct lk_file_truncate_req, size) == 0);

struct lk_file_readdir_req {
    uint32_t index; // entries count from 0 in the order the file system lists them
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_file_readdir_req) == 8);
LK_ABI_ASSERT(offsetof(struct lk_file_readdir_req, index) == 0);
LK_ABI_ASSERT(offsetof(struct lk_file_readdir_req, reserved) == 4);

struct lk_file_dirent {
    uint32_t name_len; // the name follows, without a NUL
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_file_dirent) == 8);
LK_ABI_ASSERT(offsetof(struct lk_file_dirent, name_len) == 0);
LK_ABI_ASSERT(offsetof(struct lk_file_dirent, reserved) == 4);

struct lk_file_io_req {
    uint64_t off;     // in the file
    uint64_t mem_off; // in the memory object
    uint32_t len;     // bytes at most; mem_off + len inside the object
    uint32_t flags;   // WRITE_FROM: LK_FILE_WRITE_*; READ_INTO: 0
};
LK_ABI_ASSERT(sizeof(struct lk_file_io_req) == 24);
LK_ABI_ASSERT(offsetof(struct lk_file_io_req, off) == 0);
LK_ABI_ASSERT(offsetof(struct lk_file_io_req, mem_off) == 8);
LK_ABI_ASSERT(offsetof(struct lk_file_io_req, len) == 16);
LK_ABI_ASSERT(offsetof(struct lk_file_io_req, flags) == 20);

struct lk_file_io_reply {
    uint64_t end;   // the file offset just past the last byte moved
    uint32_t count; // bytes moved
    uint32_t reserved;
};
LK_ABI_ASSERT(sizeof(struct lk_file_io_reply) == 16);
LK_ABI_ASSERT(offsetof(struct lk_file_io_reply, end) == 0);
LK_ABI_ASSERT(offsetof(struct lk_file_io_reply, count) == 8);
LK_ABI_ASSERT(offsetof(struct lk_file_io_reply, reserved) == 12);
