/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// opendir and friends for newlib programs: a directory connection from namespace.OPEN and the
// index of the next entry, which READDIR takes, so rewinding and seeking are local.

#include <dirent.h>
#include <errno.h>
#include <lk/services.h>
#include <stdlib.h>

#include "lku_priv.h"

struct lku_dir {
    lk_handle_t h;
    uint32_t index;
    struct dirent ent;
};

DIR *opendir(const char *name) {
    char buf[256];
    const char *path = lku_ns_path(name, buf, sizeof(buf));
    if (!path) {
        return NULL;
    }
    DIR *d = malloc(sizeof(*d));
    if (!d) {
        errno = ENOMEM;
        return NULL;
    }
    lk_status_t err = lk_ns_open(lku_namespace, path, LK_NS_OPEN_DIR, &d->h, NULL);
    if (err < 0) {
        free(d);
        lku_fail(err);
        return NULL;
    }
    d->index = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    lk_status_t err =
        lk_file_readdir(d->h, d->index, d->ent.d_name, sizeof(d->ent.d_name), NULL);
    if (err == ERR_NOT_FOUND) {
        // the end, which leaves errno alone
        return NULL;
    }
    if (err < 0) {
        lku_fail(err);
        return NULL;
    }
    // counting from 1: a 0 inode reads as a deleted entry to some programs
    d->ent.d_ino = ++d->index;
    return &d->ent;
}

void rewinddir(DIR *d) {
    d->index = 0;
}

long telldir(DIR *d) {
    return (long)d->index;
}

void seekdir(DIR *d, long loc) {
    d->index = (uint32_t)loc;
}

int closedir(DIR *d) {
    lk_status_t err = lk_handle_close(d->h);
    free(d);
    return err < 0 ? lku_fail(err) : 0;
}
