/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "svc.h"

#include <stdio.h>

namespace lkuser {

namespace {

service service_table[kMaxServices];
size_t service_count;

} // namespace

void register_service(uint32_t tag, const char *name, kernel_handler_t handler) {
    if (service_count == kMaxServices) {
        printf("lkuser: no room for service %s\n", name);
        return;
    }
    lk::ref_ptr<endpoint> ep = endpoint::create_kernel(name, handler);
    if (!ep) {
        printf("lkuser: no memory for service %s\n", name);
        return;
    }
    service_table[service_count++] = {tag, std::move(ep)};
}

const service *services(size_t *count) {
    *count = service_count;
    return service_table;
}

} // namespace lkuser
