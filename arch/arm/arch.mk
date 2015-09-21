# arch stuff
TOOLCHAIN_PREFIX ?= arm-eabi-
ARCH_LINKER_SCRIPT := arch/arm/linker.ld

ARCH_COMPILEFLAGS := -mthumb -march=armv7-m
ARCH_CFLAGS :=
ARCH_CPPFLAGS :=
ARCH_ASMFLAGS :=
ARCH_LDFLAGS := -z max-page-size=4096

