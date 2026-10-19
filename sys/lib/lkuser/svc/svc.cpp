/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "svc.h"

#include <stdio.h>
#include <string.h>
#include <utility>

namespace lkuser {

namespace {

service service_table[kMaxServices];
size_t service_count;

} // namespace

lk::ref_ptr<endpoint> register_service(uint32_t tag, const char *name, kernel_handler_t handler,
                                       kernel_release_t release_hook) {
    if (service_count == kMaxServices) {
        printf("lkuser: no room for service %s\n", name);
        return nullptr;
    }
    lk::ref_ptr<endpoint> ep = endpoint::create_kernel(name, handler, release_hook);
    if (!ep) {
        printf("lkuser: no memory for service %s\n", name);
        return nullptr;
    }
    service_table[service_count++] = {tag, name, ep};
    return ep;
}

status_t serve_service(const char *name, lk::ref_ptr<endpoint> ep, uint32_t *tag) {
    for (size_t i = 0; i < service_count; i++) {
        service &s = service_table[i];
        if (strcmp(s.name, name) != 0) {
            continue;
        }
        if (s.tag == kNoTag || !s.ep->is_kernel()) {
            return ERR_INVALID_ARGS;
        }
        // the kernel's endpoint lives on while a connection names it
        s.ep = std::move(ep);
        *tag = s.tag;
        return NO_ERROR;
    }
    return ERR_NOT_FOUND;
}

const service *services(size_t *count) {
    *count = service_count;
    return service_table;
}

} // namespace lkuser
