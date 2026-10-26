# arch stuff
TOOLCHAIN_PREFIX ?= aarch64-elf-
ARCH_FAMILY := arm64

# atomics inline (LL/SC), not calls into libgcc's helpers, which pick LSE from a constructor
ARCH_COMPILEFLAGS := -DARCH_ARM64=1 -mno-outline-atomics
ARCH_CFLAGS :=
ARCH_CPPFLAGS :=
ARCH_ASMFLAGS :=
# ld's default 64KB max page size stays, so a 16KB or 64KB page kernel is not ruled out (untried)
ARCH_LDFLAGS := -Ttext-segment=0x01000000

# newlib path stuff
NEWLIB_INC_DIR := $(NEWLIB_INSTALL_DIR)/aarch64-elf/include
NEWLIB_ARCH_TARGET := aarch64-elf

# libc revision we want: aarch64-elf has the one, lp64
LIBC := $(NEWLIB_INSTALL_DIR)/aarch64-elf/lib/libc.a
LIBM := $(NEWLIB_INSTALL_DIR)/aarch64-elf/lib/libm.a
