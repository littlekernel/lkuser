# arch stuff
TOOLCHAIN_PREFIX ?= arm-eabi-
ARCH_LINKER_SCRIPT := arch/arm/linker.ld

ARCH_COMPILEFLAGS := -mthumb -march=armv7-a -mfpu=neon -mfloat-abi=hard -DARCH_ARM=1
ARCH_CFLAGS :=
ARCH_CPPFLAGS :=
ARCH_ASMFLAGS :=
ARCH_LDFLAGS := -z max-page-size=4096

# libc revision we want
LIBC := $(NEWLIB_INSTALL_DIR)/arm-eabi/lib/thumb/v7-a+simd/hard/libc.a
LIBM := $(NEWLIB_INSTALL_DIR)/arm-eabi/lib/thumb/v7-a+simd/hard/libm.a

