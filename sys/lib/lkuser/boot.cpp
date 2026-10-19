/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The boot processes: LKUSER_BOOT, loaded from the boot device and started before the shell.

#include <lib/fs.h>
#include <lk/err.h>
#include <lk/init.h>
#include <stdio.h>
#include <string.h>
#include <utility>

#include "endpoint.h"
#include "handle.h"
#include "proc.h"
#include "svc/svc.h"
#include "user.h"

namespace lkuser {

namespace {

// where the boot device is mounted, read-only, while the images load; no process sees it
constexpr char kBootMount[] = "/boot";
constexpr size_t kMaxBoot = 8;
constexpr int kMaxArgs = 16;

struct boot_proc {
    lk::ref_ptr<proc> p;
    vaddr_t entry;
};

// The text up to the next sep or the end of *s, cut off there, *s moving past it; null once
// *s is used up. Empty fields come back as they are.
char *next_field(char **s, char sep) {
    char *field = *s;
    if (!field) {
        return nullptr;
    }
    char *end = strchr(field, sep);
    if (end) {
        *end++ = '\0';
    }
    *s = end;
    return field;
}

// One LKUSER_BOOT entry, path[:arg...][@service], in text (cut up in place): its process made
// and loaded, not started. With @service the process is born with a new endpoint under
// LK_TAG_SERVE_ENDPOINT, and the service points at that endpoint for every process made later.
status_t make_one(char *text, boot_proc *out) {
    char *service = strchr(text, '@');
    if (service) {
        *service++ = '\0';
    }
    const char *argv[kMaxArgs];
    int argc = 0;
    for (char *a; (a = next_field(&text, ':'));) {
        if (argc == kMaxArgs) {
            return ERR_TOO_BIG;
        }
        argv[argc++] = a;
    }
    if (argv[0][0] != '/') {
        return ERR_INVALID_ARGS;
    }
    char path[FS_MAX_PATH_LEN];
    if (snprintf(path, sizeof(path), "%s%s", kBootMount, argv[0]) >= (int)sizeof(path)) {
        return ERR_TOO_BIG;
    }

    // as many calls in progress as a process can hold
    lk::ref_ptr<endpoint> ep;
    startup_handle serve = {};
    if (service) {
        ep = endpoint::create_user(0, LKUSER_INFLIGHT, 0);
        if (!ep) {
            return ERR_NO_MEMORY;
        }
        serve = {LK_TAG_SERVE_ENDPOINT, ep, kEndpointRights};
    }
    lk::ref_ptr<proc> p;
    status_t err = proc::create(argc, argv, true, service ? &serve : nullptr, service ? 1 : 0, &p);
    if (err < 0) {
        return err;
    }
    err = load_image(p.get(), path, &out->entry);
    if (err < 0) {
        return err;
    }
    if (service) {
        uint32_t tag;
        err = serve_service(service, std::move(ep), &tag);
        if (err < 0) {
            return err;
        }
        printf("lkuser: boot: %s serves %s (tag %u)\n", argv[0], service, tag);
    }
    out->p = std::move(p);
    return NO_ERROR;
}

// Every process is made, and every service it serves pointed at it, before any starts, so the
// service table is written while no process runs. An entry that fails is reported and left
// out; the ones after it still start.
void boot_init(uint level) {
    char list[] = LKUSER_BOOT;
    if (list[0] == '\0') {
        return;
    }

    status_t err = fs_mount(kBootMount, LKUSER_BOOT_FS, LKUSER_BOOT_DEV, FS_MOUNT_OPTION_READ_ONLY);
    if (err < 0) {
        printf("lkuser: boot: mounting %s on %s failed: %d\n", LKUSER_BOOT_FS, LKUSER_BOOT_DEV, err);
        return;
    }
    boot_proc procs[kMaxBoot];
    size_t n = 0;
    char *rest = list;
    for (char *e; (e = next_field(&rest, ','));) {
        char shown[FS_MAX_PATH_LEN];
        strlcpy(shown, e, sizeof(shown));
        err = n < kMaxBoot ? make_one(e, &procs[n]) : ERR_NO_RESOURCES;
        if (err < 0) {
            printf("lkuser: boot: %s failed: %d\n", shown, err);
            continue;
        }
        n++;
    }
    fs_unmount(kBootMount);

    for (size_t i = 0; i < n; i++) {
        lk::ref_ptr<proc> &p = procs[i].p;
        err = p->start(procs[i].entry);
        if (err < 0) {
            printf("lkuser: boot: starting %s failed: %d\n", p->name(), err);
            continue;
        }
        printf("lkuser: boot: started %s as process %u\n", p->name(), p->pid());
        const uint32_t pid = p->pid();
        if (!add_background(std::move(p))) {
            printf("lkuser: boot: lkuser wait will not wait for process %u\n", pid);
        }
    }
}

} // namespace

} // namespace lkuser

// after the services register (LK_INIT_LEVEL_THREADING) and the platform's block devices
// appear, before the shell starts at LK_INIT_LEVEL_APPS
LK_INIT_HOOK(lkuser_boot, &lkuser::boot_init, LK_INIT_LEVEL_APPS - 1);
