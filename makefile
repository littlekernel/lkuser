include lk/make/macros.mk

#all: lk $(APPS)
all: _all

APPS :=

APP_RULES := $(shell find apps -name app.mk)
$(warning APP_RULES = $(APP_RULES))

BUILDDIR := build

NOECHO ?= @

# some newlib stuff
NEWLIB_INSTALL_DIR := install-newlib
NEWLIB_INC_DIR := $(NEWLIB_INSTALL_DIR)/arm-eabi/include
NEWLIB_LIBC := $(NEWLIB_INSTALL_DIR)/arm-eabi/lib/thumb/thumb2/interwork/libc.a
NEWLIB_LIBM := $(NEWLIB_INSTALL_DIR)/arm-eabi/lib/thumb/thumb2/interwork/libm.a

LINKER_SCRIPT := linker.ld

# arch stuff
TOOLCHAIN_PREFIX := arm-eabi-
ARCH_CC := $(TOOLCHAIN_PREFIX)gcc
ARCH_LD := $(TOOLCHAIN_PREFIX)ld
ARCH_COMPILEFLAGS := -mthumb -march=armv7-m

# compiler flags
GLOBAL_COMPILEFLAGS := -g -fno-builtin -finline -O2
GLOBAL_COMPILEFLAGS += -W -Wall -Wno-multichar -Wno-unused-parameter -Wno-unused-function -Wno-unused-label -Werror=return-type
GLOBAL_CFLAGS := --std=gnu99 -Werror-implicit-function-declaration -Wstrict-prototypes -Wwrite-strings
GLOBAL_CPPFLAGS := -fno-exceptions -fno-rtti -fno-threadsafe-statics
GLOBAL_ASMFLAGS := -DASSEMBLY
GLOBAL_LDFLAGS :=
GLOBAL_INCLUDES += -I$(NEWLIB_INC_DIR)

LIBGCC := $(shell $(TOOLCHAIN_PREFIX)gcc $(GLOBAL_COMPILEFLAGS) $(ARCH_COMPILEFLAGS) $(THUMBCFLAGS) -print-libgcc-file-name)
$(info LIBGCC = $(LIBGCC))

include $(APP_RULES)
$(warning APPS = $(APPS))

_all: $(APPS)

lk:
	$(MAKE) -f makefile.lk

clean:
	$(MAKE) -f makefile.lk clean
	rm -rf $(BUILDDIR)

spotless: clean
	$(MAKE) -f makefile.lk spotless

configure-newlib build-newlib/Makefile:
	mkdir -p build-newlib
	cd build-newlib && ../newlib/configure --target arm-eabi --disable-newlib-supplied-syscalls --prefix=`pwd`/../install-newlib

build-newlib: build-newlib/Makefile
	mkdir -p install-newlib
	$(MAKE) -C build-newlib
	$(MAKE) -C build-newlib install MAKEFLAGS=

clean-newlib:
	rm -rf build-newlib install-newlib

.PHONY: all _all lk clean spotless build-newlib configure-newlib clean-newlib
