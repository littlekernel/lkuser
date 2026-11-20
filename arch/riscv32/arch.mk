# arch stuff
# riscv32 builds with the riscv64 toolchain and its rv32imac/ilp32 multilib, against the newlib
# the riscv64 build installs, which carries every multilib, so it never configures one of its own
TOOLCHAIN_PREFIX ?= riscv64-elf-
ARCH_FAMILY := riscv

# -march and -mabi go on every compile, the assembler and the libgcc lookup included
ARCH_COMPILEFLAGS := -DARCH_RISCV=1 -march=rv32imac -mabi=ilp32
ARCH_CFLAGS :=
ARCH_CPPFLAGS :=
ARCH_ASMFLAGS :=
ARCH_LDFLAGS := -m elf32lriscv -Ttext-segment=0x01000000

# newlib path stuff
NEWLIB_BUILD_DIR := build-newlib-riscv64
NEWLIB_INSTALL_DIR := install-newlib-riscv64
NEWLIB_INC_DIR := $(NEWLIB_INSTALL_DIR)/riscv64-elf/include
NEWLIB_ARCH_TARGET := riscv64-elf

# libc revision we want
MULTILIB := rv32imac/ilp32
LIBC := $(NEWLIB_INSTALL_DIR)/riscv64-elf/lib/$(MULTILIB)/libc.a
LIBM := $(NEWLIB_INSTALL_DIR)/riscv64-elf/lib/$(MULTILIB)/libm.a
