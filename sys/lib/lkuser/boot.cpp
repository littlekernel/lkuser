/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// The boot file systems, the image at /init and a memfs at /tmp, and the boot processes:
// LKUSER_BOOT, loaded from the image and started before the shell.

#include <kernel/thread.h>
#include <lib/cmdline.h>
#include <lib/fs.h>
#include <lk/err.h>
#include <lk/init.h>
#include <platform.h>
#include <stdio.h>
#include <string.h>
#include <utility>

#include "boot.h"
#include "endpoint.h"
#include "handle.h"
#include "proc.h"
#include "svc/svc.h"
#include "thread.h"
#include "user.h"

namespace lkuser {

namespace {

// the boot device, read-only: the boot processes' images and what user space reads of it
constexpr char kImageMount[] = "/init";
// a memfs, where user space writes
constexpr char kTmpMount[] = "/tmp";
constexpr size_t kMaxBoot = 8;
constexpr int kMaxArgs = 16;

// the boot processes not yet reaped, and one more while boot_load is starting them
int boot_live = 1;

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

// The words of the kernel command line's lkuser.<name>, name being path's file name, added to
// argv after its first *argc and cut up in place in buf: '+' and ' ' both separate them, as in
// lk.autorun. Nothing to add if the command line has no such variable.
status_t cmdline_args(const char *path, char *buf, size_t len, const char **argv, int *argc) {
    const char *slash = strrchr(path, '/');
    char var[FS_MAX_FILE_LEN + sizeof("lkuser.")];
    if (snprintf(var, sizeof(var), "lkuser.%s", slash ? slash + 1 : path) >= (int)sizeof(var)) {
        return ERR_TOO_BIG;
    }
    status_t err = cmdline_get_string(var, buf, len, nullptr);
    if (err == ERR_NOT_FOUND) {
        return NO_ERROR;
    }
    if (err < 0) {
        return err;
    }
    char *p = buf;
    for (;;) {
        while (*p == ' ' || *p == '+') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            return NO_ERROR;
        }
        if (*argc == kMaxArgs) {
            return ERR_TOO_BIG;
        }
        argv[(*argc)++] = p;
        while (*p && *p != ' ' && *p != '+') {
            p++;
        }
    }
}

// One LKUSER_BOOT entry, path[:arg...][@service], in text (cut up in place), the path in the
// kernel's tree: its process made and loaded, not started, with the arguments the entry gives
// it and then those of the command line's lkuser.<name>. With @service the process is born
// with a new endpoint under LK_TAG_SERVE_ENDPOINT, and the service points at that endpoint for
// every process the kernel makes later.
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
    char extra[256];
    status_t err = cmdline_args(argv[0], extra, sizeof(extra), argv, &argc);
    if (err < 0) {
        return err;
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
    err = proc::create(argc, argv, true, service ? &serve : nullptr, service ? 1 : 0, &p);
    if (err < 0) {
        return err;
    }
    err = load_image(p.get(), argv[0], &out->entry);
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

// Mounts the boot file systems, then makes every boot process, and points every service it
// serves at it, before any starts, so the service table is written while no process runs. An
// entry that fails is reported and left out; the ones after it still start.
int boot_load(void *arg) {
    status_t err =
        fs_mount(kImageMount, LKUSER_BOOT_FS, LKUSER_BOOT_DEV, FS_MOUNT_OPTION_READ_ONLY);
    if (err < 0) {
        printf("lkuser: boot: mounting %s on %s at %s failed: %d\n", LKUSER_BOOT_FS,
               LKUSER_BOOT_DEV, kImageMount, err);
        boot_reaped();
        return err;
    }
#if !LKUSER_BOOT_UNMOUNT
    err = fs_mount(kTmpMount, "memfs", nullptr, FS_MOUNT_OPTION_NONE);
    if (err < 0) {
        printf("lkuser: boot: mounting a memfs at %s failed: %d\n", kTmpMount, err);
    }
#endif

    char list[] = LKUSER_BOOT;
    boot_proc procs[kMaxBoot];
    size_t n = 0;
    char *rest = list[0] ? list : nullptr;
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
#if LKUSER_BOOT_UNMOUNT
    // no file system once the images are loaded
    err = fs_unmount(kImageMount);
    if (err < 0) {
        printf("lkuser: boot: unmounting %s failed: %d\n", kImageMount, err);
    }
#endif

    for (size_t i = 0; i < n; i++) {
        lk::ref_ptr<proc> &p = procs[i].p;
        p->set_boot();
        __atomic_add_fetch(&boot_live, 1, __ATOMIC_RELAXED);
        err = p->start(procs[i].entry);
        if (err < 0) {
            printf("lkuser: boot: starting %s failed: %d\n", p->name(), err);
            __atomic_sub_fetch(&boot_live, 1, __ATOMIC_RELAXED);
            continue;
        }
        printf("lkuser: boot: started %s as process %u\n", p->name(), p->pid());
        const uint32_t pid = p->pid();
        if (!add_background(std::move(p))) {
            printf("lkuser: boot: lkuser wait will not wait for process %u\n", pid);
        }
    }
    boot_reaped();
    return NO_ERROR;
}

// The loading reads the images through the file system and block drivers, deeper than the
// stack the init hooks run on allows on arm64, so it runs on a thread with a user thread's
// kernel stack.
void boot_init(uint level) {
    thread_t *t = thread_create("lkuser boot", &boot_load, nullptr, DEFAULT_PRIORITY,
                                thread::kKernelStackSize);
    if (!t) {
        printf("lkuser: boot: no thread to load the boot processes on\n");
        return;
    }
    thread_resume(t);
    thread_join(t, nullptr, INFINITE_TIME);
}

} // namespace

void boot_reaped() {
    if (__atomic_sub_fetch(&boot_live, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    bool halt = false;
    if (cmdline_get_bool("lkuser.halt", &halt) < 0 || !halt) {
        return;
    }
    // what lkuser wait would wait for holds the last references to the boot processes
    clear_background();
    printf("lkuser: halt: objects\n");
    object::for_each([](object &o) { o.dump(); }, true);
    printf("lkuser: halt: end\n");
    platform_halt(HALT_ACTION_SHUTDOWN, HALT_REASON_SW_RESET);
}

} // namespace lkuser

// after the services register (LK_INIT_LEVEL_THREADING) and the platform's block devices
// appear, before the shell starts at LK_INIT_LEVEL_APPS
LK_INIT_HOOK(lkuser_boot, &lkuser::boot_init, LK_INIT_LEVEL_APPS - 1);
