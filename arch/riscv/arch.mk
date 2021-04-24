# arch stuff
TOOLCHAIN_PREFIX ?= riscv64-elf-
#ARCH_LINKER_SCRIPT := arch/riscv/linker.ld
ARCH_LINKER_SCRIPT := arch/riscv/elf64lriscv.xe

ARCH_COMPILEFLAGS := -DARCH_RISCV=1
ARCH_CFLAGS := -march=rv64imafdc -mabi=lp64d
ARCH_CPPFLAGS :=
ARCH_ASMFLAGS :=
ARCH_LDFLAGS :=

# newlib path stuff
NEWLIB_INC_DIR := $(NEWLIB_INSTALL_DIR)/riscv64-elf/include
NEWLIB_ARCH_TARGET := riscv64-elf

# libc revision we want
LIBC := $(NEWLIB_INSTALL_DIR)/riscv64-elf/lib/rv64imafdc/lp64d/libc.a
LIBM := $(NEWLIB_INSTALL_DIR)/riscv64-elf/lib/rv64imafdc/lp64d/libm.a
#CRT0 := $(NEWLIB_INSTALL_DIR)/riscv64-elf/lib/rv64imafdc/lp64d/crt0.o


