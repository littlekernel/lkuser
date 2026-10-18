/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "object.h"

#include <lk/debug.h>

#include "proc.h"
#include "thread.h"

namespace lkuser {

void object::destroy(object *o) {
    switch (o->kind()) {
        case obj_kind::PROCESS:
            delete static_cast<proc *>(o);
            return;
        case obj_kind::THREAD:
            delete static_cast<thread *>(o);
            return;
        default:
            panic("lkuser: no destroy for object %p of kind %u\n", o, (unsigned)o->kind());
    }
}

} // namespace lkuser
