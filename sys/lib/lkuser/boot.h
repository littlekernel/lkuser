/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

namespace lkuser {

// The reaper has freed a boot process. With lkuser.halt on the kernel command line the boot hook
// waits for the last, then prints the objects left between "lkuser: halt: objects" and
// "lkuser: halt: end" lines and powers off, so the init levels after it, the shell's among
// them, never run.
void boot_reaped();

} // namespace lkuser
