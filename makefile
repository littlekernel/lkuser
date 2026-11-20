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

# dependency files of every app and lib object
DEPS :=
LIB_RULES := $(shell find lib -name lib.mk)
$(warning LIB_RULES = $(LIB_RULES))

CCACHE ?=
ARCH ?= riscv64

BUILDDIR := build-$(ARCH)

# the kernel's profile, the project sys/project/qemu-$(ARCH)-$(PROFILE).mk:
#   usertest  every kernel service; init, started at boot, runs the apps (the default)
#   consoled  usertest with the console served from user space by consoled, from boot
#   mini      the console and process services only, and one static app started at boot
#   direct    no shell: the kernel starts init from an rofs image linked into it
PROFILE ?= usertest
PROFILES := usertest consoled mini direct
LK_TESTPROJECT := qemu-$(ARCH)-$(PROFILE)
# where each file starts in root.rofs, the direct profile's image: a page boundary
ROFS_ALIGN := 4096

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
# lib/lku's headers first: its <sys/dirent.h> stands in for newlib's
GLOBAL_INCLUDES := -Ilib/lku/include -I$(NEWLIB_INC_DIR) -Isys/lib/lkuser/include
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

-include $(DEPS)

_all: lk apps libs

# ARCH names lkuser's arch (riscv32), not LK's (riscv, which LK's project sets itself), so a
# command line ARCH stays out of LK's make
LK_MAKEOVERRIDES := $(filter-out ARCH=%,$(MAKEOVERRIDES))
lk clean spotless size-table: MAKEOVERRIDES = $(LK_MAKEOVERRIDES)

# the direct profile links root.rofs into the kernel, so its apps build first
ifeq ($(PROFILE),direct)
LK_MAKEARGS := ROFS_EMBED_IMAGE=$(abspath $(BUILDDIR)/root.rofs) ROFS_EMBED_ALIGN=$(ROFS_ALIGN)
lk: $(BUILDDIR)/root.rofs
size-table: $(BUILDDIR)/root.rofs
endif

lk:
	PROJECT=$(LK_TESTPROJECT) $(MAKE) -f makefile.lk $(LK_MAKEARGS)

# the executive's code per file, for the selected project (scripts/size-table.sh); + shares
# make's jobserver with the script's makes
size-table:
	+LK_MAKEARGS='$(LK_MAKEARGS)' scripts/size-table.sh $(LK_TESTPROJECT)

apps: $(APPS) $(APPS_EXTRADEPS)

libs: $(LIBS) $(LIBS_EXTRADEPS)

clean-apps:
	rm -rf -- "."/"$(BUILDDIR)"

clean: clean-apps
	PROJECT=$(LK_TESTPROJECT) $(MAKE) -f makefile.lk $(LK_MAKEARGS) clean

spotless: clean-apps clean-newlib
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

# the same files as an rofs image, which the direct profile links into its kernel
MKROFS := lk/scripts/mkrofs.py
$(BUILDDIR)/root.rofs: $(FS_LIST_LEFT) $(MKROFS)
	@$(MKDIR)
	@echo generating rofs image $@
	$(NOECHO)python3 $(MKROFS) -a $(ROFS_ALIGN) -o $@ $(FS_LIST)

fs: $(BUILDDIR)/root.fat

list-toolchain:
	@echo TOOLCHAIN_PREFIX = ${TOOLCHAIN_PREFIX}

test: _all fs
ifeq ($(ARCH),arm)
	qemu-system-arm -m 512 -smp 1 -machine virt -cpu cortex-a15 -kernel build-$(LK_TESTPROJECT)/lk.elf -nographic -drive if=none,file=$(BUILDDIR)/root.fat,id=blk,format=raw -device virtio-blk-device,drive=blk
else ifeq ($(ARCH),riscv64)
	qemu-system-riscv64 -m 512 -smp 1 -machine virt -cpu rv64 -bios default -kernel build-$(LK_TESTPROJECT)/lk.elf -nographic -drive if=none,file=$(BUILDDIR)/root.fat,id=blk,format=raw -device virtio-blk-device,drive=blk
else ifeq ($(ARCH),riscv32)
	qemu-system-riscv32 -m 512 -smp 1 -machine virt -cpu rv32 -bios default -kernel build-$(LK_TESTPROJECT)/lk.elf -nographic -drive if=none,file=$(BUILDDIR)/root.fat,id=blk,format=raw -device virtio-blk-device,drive=blk
else ifeq ($(ARCH),arm64)
	qemu-system-aarch64 -m 512 -smp 1 -machine virt -cpu cortex-a76 -kernel build-$(LK_TESTPROJECT)/lk.elf -nographic -drive if=none,file=$(BUILDDIR)/root.fat,id=blk,format=raw -device virtio-blk-device,drive=blk
endif

# apps run in order by test-auto, each as name[:arg...][&][=code]; each must exit with code
# (default 0), and one that exits 0 must print <NAME>: PASS (upper-cased). One with & runs in
# the background while the rest run, and must end by itself. TEST_BOOT names the processes the
# profile starts at boot (LKUSER_BOOT), as name[=code], checked the same way; consoled ends when
# consoletest:quit tells it to.
TEST_APPS_COMMON := rawtest:a:b:c calltest boottest posixtest spawntest faulttest=-40 ipctest echoserver& echotest
TEST_APPS_usertest := $(TEST_APPS_COMMON) consoletest
TEST_APPS_consoled := $(TEST_APPS_COMMON) consoletest:quit
# init runs the apps, and the kernel halts once the boot processes are done (the harness's
# -i); mini's boot process is its test, which the autorun waits for. Under the autorun init
# has nothing to run, and its lines can tear against the shell's starting up, so it is checked
# only with -i.
TEST_INIT_BOOT = $(if $(filter 1,$(TEST_INIT)),init)
TEST_BOOT_usertest = $(TEST_INIT_BOOT)
TEST_BOOT_consoled = consoled $(TEST_INIT_BOOT)
TEST_BOOT_mini := minitest
TEST_APPS_direct := $(TEST_APPS_usertest)
TEST_BOOT_direct := init
TEST_INIT_usertest := 1
TEST_INIT_consoled := 1
TEST_INIT_direct := 1
TEST_APPS ?= $(TEST_APPS_$(PROFILE))
TEST_BOOT ?= $(TEST_BOOT_$(PROFILE))
TEST_INIT ?= $(TEST_INIT_$(PROFILE))
# the disk the FAT profiles boot from; the direct one carries its image
TEST_DISK := $(if $(filter direct,$(PROFILE)),,$(BUILDDIR)/root.fat)
TEST_TIMEOUT ?= 60
TEST_SMP ?= 1
TEST_LOG := $(BUILDDIR)/test-auto$(if $(filter-out usertest,$(PROFILE)),-$(PROFILE)).log

test-auto: _all fs
	scripts/qemu-test.sh -a $(ARCH) -k build-$(LK_TESTPROJECT)/lk.elf \
		$(if $(TEST_DISK),-f $(TEST_DISK)) -l $(TEST_LOG) -t $(TEST_TIMEOUT) -s $(TEST_SMP) \
		$(if $(filter 1,$(TEST_INIT)),-i) $(foreach b,$(TEST_BOOT),-b '$(b)') \
		$(foreach a,$(TEST_APPS),'$(a)')

# every profile's gate in turn
test-auto-all:
	$(NOECHO)for p in $(PROFILES); do $(MAKE) --no-print-directory test-auto PROFILE=$$p || exit 1; done

.PHONY: all _all apps fs lk size-table clean clean-apps spotless newlib build-newlib configure-newlib clean-newlib list-toolchain test test-auto test-auto-all

# vim: set noexpandtab ts=4 sw=4:
