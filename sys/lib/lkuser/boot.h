/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

namespace lkuser {

// The reaper has freed a boot process. Once the last has gone, and if the kernel command line
// says lkuser.halt, the kernel prints the objects left between "lkuser: halt: objects" and
// "lkuser: halt: end" lines and powers off.
void boot_reaped();

} // namespace lkuser
