/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

// Where the tests find their files: the kernel mounts the boot image read-only at
// TEST_IMAGE and a memfs at TEST_TMP, which is where a test writes.
#define TEST_IMAGE "/init"
#define TEST_TMP "/tmp"
