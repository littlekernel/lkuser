/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lk/init.h>
#include <lktl/ref_ptr.h>
#include <stddef.h>
#include <stdint.h>

#include "../endpoint.h"

namespace lkuser {

// The kernel-resident services. Each svc/*.cpp registers one at boot, and every process the
// console starts is born with a connection to each that has a startup tag, under that tag.
// Which files are compiled in is the build's choice (LKUSER_SERVICES in rules.mk). A process
// started at boot may serve one in the kernel's place (LKUSER_BOOT's path@service): ep is then
// its user endpoint, for as long as the system runs, and dies with the server's handles.
struct service {
    uint32_t tag;
    const char *name;
    lk::ref_ptr<endpoint> ep;
};

constexpr size_t kMaxServices = 8;

// The tag of a service no process is born connected to: its connections are made by another
// service (file, by namespace.OPEN).
constexpr uint32_t kNoTag = 0;

// The table is written at boot only, before any process runs, so it needs no lock.

// Called from an init hook. Returns the service's endpoint, or null if it could not be
// registered.
lk::ref_ptr<endpoint> register_service(uint32_t tag, const char *name, kernel_handler_t handler,
                                       kernel_release_t release_hook = nullptr);

// Points the service called name at ep, a user endpoint, so that every process made from now
// on is born connected to ep under the service's tag, which goes to *tag. Called by the boot
// hook before it starts any process. ERR_NOT_FOUND for no such service, ERR_INVALID_ARGS for one
// with no tag or one already served.
status_t serve_service(const char *name, lk::ref_ptr<endpoint> ep, uint32_t *tag);

// the registered services, in registration order
const service *services(size_t *count);

} // namespace lkuser

// Registers handler as the service name under tag at boot.
#define LKUSER_SERVICE(name, tag, handler)                                       \
    static void lkuser_service_init_##name(uint level) {                         \
        lkuser::register_service(tag, #name, handler);                           \
    }                                                                            \
    LK_INIT_HOOK(lkuser_service_##name, lkuser_service_init_##name, LK_INIT_LEVEL_THREADING)
