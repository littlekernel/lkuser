# allow local overrides of particular things
-include local.mk

include lk/make/macros.mk
NOECHO ?= @
# try to have the compiler output colorized error messages if available
export GCC_COLORS ?= 1

all: _all

# a list of files added by various modules in the form of
# <build file>:<fs path>
FS_LIST :=

APPS :=
APPS_EXTRADEPS :=
APP_RULES := $(shell find apps -name app.mk)
$(warning APP_RULES = $(APP_RULES))

LIBS :=
LIBS_EXTRADEPS :=
LIB_RULES := $(shell find lib -name lib.mk)
$(warning LIB_RULES = $(LIB_RULES))

CCACHE ?=
ARCH ?= arm

BUILDDIR := build-$(ARCH)
LK_TESTPROJECT := qemu-$(ARCH)-usertest

# some newlib stuff
NEWLIB_BUILD_DIR := build-newlib-$(ARCH)
NEWLIB_INSTALL_DIR := install-newlib-$(ARCH)
NEWLIB_INC_DIR := # arch.mk should set this
NEWLIB_ARCH_TARGET := # arch.mk should set this
LIBC := # arch.mk should set this and libm
LIBM :=

include arch/$(ARCH)/arch.mk

# compiler flags
GLOBAL_COMPILEFLAGS := -g -fno-builtin -finline -O2
GLOBAL_COMPILEFLAGS += -W -Wall -Wno-multichar -Wno-unused-parameter -Wno-unused-function -Wno-unused-label -Werror=return-type
GLOBAL_CFLAGS := --std=gnu99 -Werror-implicit-function-declaration -Wstrict-prototypes -Wwrite-strings
GLOBAL_CPPFLAGS := -fno-exceptions -fno-rtti -fno-threadsafe-statics
GLOBAL_ASMFLAGS := -DASSEMBLY
GLOBAL_LDFLAGS :=
GLOBAL_INCLUDES := -I$(NEWLIB_INC_DIR) -Isys/lib/lkuser/include
GLOBAL_LIBS := $(LIBC) $(LIBM)

GLOBAL_COMPILEFLAGS += -ffunction-sections -fdata-sections
GLOBAL_LDFLAGS += --gc-sections

ARCH_CC ?= $(CCACHE) $(TOOLCHAIN_PREFIX)gcc
ARCH_LD ?= $(TOOLCHAIN_PREFIX)ld
ARCH_AR ?= $(TOOLCHAIN_PREFIX)ar
ARCH_OBJDUMP ?= $(TOOLCHAIN_PREFIX)objdump
ARCH_OBJCOPY ?= $(TOOLCHAIN_PREFIX)objcopy
ARCH_CPPFILT ?= $(TOOLCHAIN_PREFIX)c++filt
ARCH_SIZE ?= $(TOOLCHAIN_PREFIX)size
ARCH_NM ?= $(TOOLCHAIN_PREFIX)nm
ARCH_STRIP ?= $(TOOLCHAIN_PREFIX)strip

LIBGCC := $(shell $(ARCH_CC) $(GLOBAL_COMPILEFLAGS) $(ARCH_COMPILEFLAGS) $(THUMBCFLAGS) -print-libgcc-file-name)
$(info LIBGCC = $(LIBGCC))

include $(APP_RULES)
$(warning APPS = $(APPS))

include $(LIB_RULES)
$(warning LIBS = $(LIBS))

_all: lk apps libs

lk:
	PROJECT=$(LK_TESTPROJECT) $(MAKE) -f makefile.lk

apps: $(APPS) $(APPS_EXTRADEPS)

libs: $(LIBS) $(LIBS_EXTRADEPS)

clean-apps:
	rm -rf -- "."/"$(BUILDDIR)"

clean: clean-apps
	PROJECT=$(LK_TESTPROJECT) $(MAKE) -f makefile.lk clean

spotless: clean clean-newlib
	$(MAKE) -f makefile.lk spotless

configure-newlib $(NEWLIB_BUILD_DIR)/.stamp:
	mkdir -p $(NEWLIB_BUILD_DIR)
	cd $(NEWLIB_BUILD_DIR) && ../newlib/configure --target $(NEWLIB_ARCH_TARGET) \
		--prefix=`pwd`/../$(NEWLIB_INSTALL_DIR) \
		--disable-newlib-supplied-syscalls \
		--enable-target-optspace
	$(MAKE) -C $(NEWLIB_BUILD_DIR) configure-host
	$(MAKE) -C $(NEWLIB_BUILD_DIR) configure-target
	touch $(NEWLIB_BUILD_DIR)/.stamp

build-newlib $(NEWLIB_INSTALL_DIR)/.stamp: $(NEWLIB_BUILD_DIR)/.stamp
	mkdir -p $(NEWLIB_INSTALL_DIR)
	$(MAKE) -C $(NEWLIB_BUILD_DIR)
	$(MAKE) -C $(NEWLIB_BUILD_DIR) install MAKEFLAGS=
	touch $(NEWLIB_INSTALL_DIR)/.stamp

$(LIBC) $(LIBM) $(CRT0): $(NEWLIB_INSTALL_DIR)/.stamp

newlib: build-newlib

clean-newlib:
	rm -rf -- ./"$(NEWLIB_BUILD_DIR)" ./"$(NEWLIB_INSTALL_DIR)"

FS_LIST_LEFT := $(foreach F,$(FS_LIST),$(firstword $(subst :, ,$(F))))
FS_LIST_RIGHT := $(foreach F,$(FS_LIST),$(lastword $(subst :, ,$(F))))
FS_LIST_DIRS := $(filter-out .,$(sort $(patsubst %/,%,$(dir $(FS_LIST_RIGHT)))))

$(info FS_LIST = $(FS_LIST))
$(info FS_LIST_LEFT = $(FS_LIST_LEFT))
$(info FS_LIST_RIGHT = $(FS_LIST_RIGHT))
$(info FS_LIST_DIRS = $(FS_LIST_DIRS))

# generate a directory within the build dir that holds all of the
# files referenced in FS_LIST.
ROOT_DIR := $(BUILDDIR)/root_dir
$(ROOT_DIR).stamp: $(FS_LIST_LEFT)
	@$(MKDIR)
	@echo building root fs image in $(ROOT_DIR)
	$(NOECHO)if [ -d "$(ROOT_DIR)" ]; then \
		rm -r -- "$(ROOT_DIR)";\
	fi
	$(NOECHO)mkdir -p -- $(addprefix $(ROOT_DIR)/,$(FS_LIST_DIRS))
	$(NOECHO)$(foreach F,$(FS_LIST),\
		cp --  "$(firstword $(subst :, ,$(F)))" "$(addprefix $(ROOT_DIR)/,$(lastword $(subst :, ,$(F))))"; \
	)
	$(NOECHO)touch $(ROOT_DIR).stamp
	$(NOECHO)#du -a $(ROOT_DIR)

# generate a FAT disk image with applications in it
# copies the entire tree that is stored in $(ROOT_DIR)
$(BUILDDIR)/root.fat: $(ROOT_DIR).stamp
	@$(MKDIR)
	@echo generating FAT image $@
	$(NOECHO)dd if=/dev/zero of=$@.tmp bs=1048576 count=64
	$(NOECHO)mformat -i $@.tmp -m f8 -v lkuser
	$(NOECHO)mcopy -v -i $@.tmp -s "$(ROOT_DIR)"/* ::
	$(NOECHO)mv $@.tmp $@
	$(NOECHO)#mdir -/ -i $@ ::

fs: $(BUILDDIR)/root.fat

list-toolchain:
	@echo TOOLCHAIN_PREFIX = ${TOOLCHAIN_PREFIX}

test: _all fs
ifeq ($(ARCH),arm)
	qemu-system-arm -m 512 -smp 1 -machine virt -cpu cortex-a15 -kernel build-$(LK_TESTPROJECT)/lk.elf -nographic -drive if=none,file=$(BUILDDIR)/root.fat,id=blk,format=raw -device virtio-blk-device,drive=blk
else ifeq ($(ARCH),riscv)
	qemu-system-riscv64 -m 512 -smp 1 -machine virt -cpu rv64 -bios default -kernel build-$(LK_TESTPROJECT)/lk.elf -nographic -drive if=none,file=$(BUILDDIR)/root.fat,id=blk,format=raw -device virtio-blk-device,drive=blk
endif

.PHONY: all _all apps fs lk clean clean-apps spotless newlib build-newlib configure-newlib clean-newlib list-toolchain

# vim: set noexpandtab ts=4 sw=4:
