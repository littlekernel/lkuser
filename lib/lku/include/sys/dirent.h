/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// Directories for newlib programs. newlib's <dirent.h> declares opendir and friends and takes
// the types from here, in place of its own <sys/dirent.h>, which refuses to compile; lib/lku's
// include directory comes first so this one is found. lib/lku implements them over the file
// protocol's READDIR.

#include <sys/types.h>

#define MAXNAMLEN 255 // LK_NAME_MAX

struct dirent {
    ino_t d_ino; // the entry's position, counting from 1: no file system inode behind it
    char d_name[MAXNAMLEN + 1];
};

typedef struct lku_dir DIR;
