LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

GLOBAL_INCLUDES += $(LOCAL_DIR)/include

# the kernel-resident services compiled in, one svc/<name>.cpp each; every process the
# console starts is born with a connection to each that has a startup tag (namespace needs
# file, which has none: its connections come from namespace.OPEN)
LKUSER_SERVICES ?= console process time vm namespace file

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

MODULE_DEPS += lib/elf
MODULE_DEPS += lib/fs
MODULE_DEPS += lib/libcpp
MODULE_DEPS += lib/lktl

# console.READ waits on the console's input buffer, where a kill can reach it
ifneq ($(filter console,$(LKUSER_SERVICES)),)
GLOBAL_DEFINES += CONSOLE_HAS_INPUT_BUFFER=1
MODULE_DEPS += lib/cbuf
MODULE_DEPS += lib/io
endif

# the lkuser console command, under WITH_LIB_CONSOLE
MODULE_WEAK_DEPS += lib/console

include make/module.mk
