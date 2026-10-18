/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

// LK's status codes, one row per code: LK_ERR_DEF(name, value), with LK's spelling and
// values. Included with LK_ERR_DEF defined, by <sys/lk_err.h> for user space and by the
// kernel (syscalls.cpp), which checks every row against LK's <lk/err.h>, so a code that drifts
// from LK fails the build.

LK_ERR_DEF(NO_ERROR,              0)
LK_ERR_DEF(ERR_GENERIC,           -1)
LK_ERR_DEF(ERR_NOT_FOUND,         -2)
LK_ERR_DEF(ERR_NOT_READY,         -3)
LK_ERR_DEF(ERR_NO_MSG,            -4)
LK_ERR_DEF(ERR_NO_MEMORY,         -5)
LK_ERR_DEF(ERR_ALREADY_STARTED,   -6)
LK_ERR_DEF(ERR_NOT_VALID,         -7)
LK_ERR_DEF(ERR_INVALID_ARGS,      -8)
LK_ERR_DEF(ERR_NOT_ENOUGH_BUFFER, -9)
LK_ERR_DEF(ERR_NOT_SUSPENDED,     -10)
LK_ERR_DEF(ERR_OBJECT_DESTROYED,  -11)
LK_ERR_DEF(ERR_NOT_BLOCKED,       -12)
LK_ERR_DEF(ERR_TIMED_OUT,         -13)
LK_ERR_DEF(ERR_ALREADY_EXISTS,    -14)
LK_ERR_DEF(ERR_CHANNEL_CLOSED,    -15)
LK_ERR_DEF(ERR_OFFLINE,           -16)
LK_ERR_DEF(ERR_NOT_ALLOWED,       -17)
LK_ERR_DEF(ERR_BAD_PATH,          -18)
LK_ERR_DEF(ERR_ALREADY_MOUNTED,   -19)
LK_ERR_DEF(ERR_IO,                -20)
LK_ERR_DEF(ERR_NOT_DIR,           -21)
LK_ERR_DEF(ERR_NOT_FILE,          -22)
LK_ERR_DEF(ERR_RECURSE_TOO_DEEP,  -23)
LK_ERR_DEF(ERR_NOT_SUPPORTED,     -24)
LK_ERR_DEF(ERR_TOO_BIG,           -25)
LK_ERR_DEF(ERR_CANCELLED,         -26)
LK_ERR_DEF(ERR_NOT_IMPLEMENTED,   -27)
LK_ERR_DEF(ERR_CHECKSUM_FAIL,     -28)
LK_ERR_DEF(ERR_CRC_FAIL,          -29)
LK_ERR_DEF(ERR_CMD_UNKNOWN,       -30)
LK_ERR_DEF(ERR_BAD_STATE,         -31)
LK_ERR_DEF(ERR_BAD_LEN,           -32)
LK_ERR_DEF(ERR_BUSY,              -33)
LK_ERR_DEF(ERR_THREAD_DETACHED,   -34)
LK_ERR_DEF(ERR_I2C_NACK,          -35)
LK_ERR_DEF(ERR_ALREADY_EXPIRED,   -36)
LK_ERR_DEF(ERR_OUT_OF_RANGE,      -37)
LK_ERR_DEF(ERR_NOT_CONFIGURED,    -38)
LK_ERR_DEF(ERR_NOT_MOUNTED,       -39)
LK_ERR_DEF(ERR_FAULT,             -40)
LK_ERR_DEF(ERR_NO_RESOURCES,      -41)
LK_ERR_DEF(ERR_BAD_HANDLE,        -42)
LK_ERR_DEF(ERR_ACCESS_DENIED,     -43)
LK_ERR_DEF(ERR_PARTIAL_WRITE,     -44)
LK_ERR_DEF(ERR_NO_ROUTE,          -45)
LK_ERR_DEF(ERR_USER_BASE,         -16384)
