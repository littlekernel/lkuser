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
// Which files are compiled in is the build's choice (LKUSER_SERVICES in rules.mk).
struct service {
    uint32_t tag;
    lk::ref_ptr<endpoint> ep;
};

constexpr size_t kMaxServices = 8;

// The tag of a service no process is born connected to: its connections are made by another
// service (file, by namespace.OPEN).
constexpr uint32_t kNoTag = 0;

// Called from an init hook, before any process exists, so the table needs no lock. Returns
// the service's endpoint, or null if it could not be registered.
lk::ref_ptr<endpoint> register_service(uint32_t tag, const char *name, kernel_handler_t handler,
                                       kernel_release_t release_hook = nullptr);

// the registered services, in registration order
const service *services(size_t *count);

} // namespace lkuser

// Registers handler as the service name under tag at boot.
#define LKUSER_SERVICE(name, tag, handler)                                       \
    static void lkuser_service_init_##name(uint level) {                         \
        lkuser::register_service(tag, #name, handler);                           \
    }                                                                            \
    LK_INIT_HOOK(lkuser_service_##name, lkuser_service_init_##name, LK_INIT_LEVEL_THREADING)
