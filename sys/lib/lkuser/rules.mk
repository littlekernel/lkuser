LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

GLOBAL_INCLUDES += $(LOCAL_DIR)/include

# the kernel-resident services compiled in, one svc/<name>.cpp each; every process the
# kernel makes itself is born with a connection to each that has a startup tag (namespace needs
# file, which has none: its connections come from namespace.OPEN)
LKUSER_SERVICES ?= console process time vm namespace file

# The boot image: the LKUSER_BOOT_FS file system on LKUSER_BOOT_DEV, mounted read-only at /init
# at boot, before the shell if there is one, beside a memfs at /tmp. With LKUSER_BOOT_UNMOUNT set
# to 1 there is no file system once the boot processes are loaded: the image is unmounted
# before any starts, and there is no /tmp.
LKUSER_BOOT_FS ?= fat
LKUSER_BOOT_DEV ?= virtio0
LKUSER_BOOT_UNMOUNT ?= 0

# Processes started at boot, before the shell, in order: path[:arg...][@service] each, the path
# in the kernel's tree (/init/...), each also given the words of the kernel command line's
# lkuser.<name> (its file name) after its own arguments. With @service the process serves that
# service in the kernel's place: it is born with an endpoint under LK_TAG_SERVE_ENDPOINT, and
# every process the kernel makes after it is born connected to that endpoint under the
# service's tag.
LKUSER_BOOT ?=

# slots in each process's handle table
LKUSER_HANDLES ?= 64

# the defines cannot hold spaces, so the list is comma separated
MODULE_DEFINES += LKUSER_BOOT=\"$(subst $(SPACE),$(COMMA),$(strip $(LKUSER_BOOT)))\"
MODULE_DEFINES += LKUSER_BOOT_FS=\"$(LKUSER_BOOT_FS)\"
MODULE_DEFINES += LKUSER_BOOT_DEV=\"$(LKUSER_BOOT_DEV)\"
MODULE_DEFINES += LKUSER_BOOT_UNMOUNT=$(LKUSER_BOOT_UNMOUNT)
MODULE_DEFINES += LKUSER_HANDLES=$(LKUSER_HANDLES)

MODULE_SRCS += $(LOCAL_DIR)/boot.cpp
MODULE_SRCS += $(LOCAL_DIR)/copy.cpp
MODULE_SRCS += $(LOCAL_DIR)/endpoint.cpp
MODULE_SRCS += $(LOCAL_DIR)/group.cpp
MODULE_SRCS += $(LOCAL_DIR)/handle.cpp
MODULE_SRCS += $(LOCAL_DIR)/impulse.cpp
MODULE_SRCS += $(LOCAL_DIR)/ipc.cpp
MODULE_SRCS += $(LOCAL_DIR)/memobj.cpp
MODULE_SRCS += $(LOCAL_DIR)/msg.cpp
MODULE_SRCS += $(LOCAL_DIR)/object.cpp
MODULE_SRCS += $(LOCAL_DIR)/proc.cpp
MODULE_SRCS += $(LOCAL_DIR)/syscalls.cpp
MODULE_SRCS += $(LOCAL_DIR)/thread.cpp
MODULE_SRCS += $(LOCAL_DIR)/user.cpp
MODULE_SRCS += $(LOCAL_DIR)/wait.cpp
MODULE_SRCS += $(LOCAL_DIR)/svc/svc.cpp
MODULE_SRCS += $(foreach s,$(LKUSER_SERVICES),$(LOCAL_DIR)/svc/$(s).cpp)

MODULE_DEPS += lib/cmdline
MODULE_DEPS += lib/elf
MODULE_DEPS += lib/fs
MODULE_DEPS += lib/libcpp
MODULE_DEPS += lib/lktl
ifneq ($(LKUSER_BOOT_UNMOUNT),1)
MODULE_DEPS += lib/fs/memfs
endif

# console.READ waits on the console's input buffer, where a kill can reach it
ifneq ($(filter console,$(LKUSER_SERVICES)),)
GLOBAL_DEFINES += CONSOLE_HAS_INPUT_BUFFER=1
MODULE_DEPS += lib/cbuf
MODULE_DEPS += lib/io
endif

# the lkuser console command, under WITH_LIB_CONSOLE
MODULE_WEAK_DEPS += lib/console

include make/module.mk
