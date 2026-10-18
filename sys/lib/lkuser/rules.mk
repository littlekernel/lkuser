LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

GLOBAL_INCLUDES += $(LOCAL_DIR)/include

# the kernel-resident services compiled in, one svc/<name>.cpp each; every process the
# console starts is born with a connection to each
LKUSER_SERVICES ?= console process time vm

MODULE_SRCS += $(LOCAL_DIR)/copy.cpp
MODULE_SRCS += $(LOCAL_DIR)/endpoint.cpp
MODULE_SRCS += $(LOCAL_DIR)/handle.cpp
MODULE_SRCS += $(LOCAL_DIR)/msg.cpp
MODULE_SRCS += $(LOCAL_DIR)/object.cpp
MODULE_SRCS += $(LOCAL_DIR)/proc.cpp
MODULE_SRCS += $(LOCAL_DIR)/syscalls.cpp
MODULE_SRCS += $(LOCAL_DIR)/thread.cpp
MODULE_SRCS += $(LOCAL_DIR)/user.cpp
MODULE_SRCS += $(LOCAL_DIR)/svc/svc.cpp
MODULE_SRCS += $(foreach s,$(LKUSER_SERVICES),$(LOCAL_DIR)/svc/$(s).cpp)

MODULE_DEPS += lib/elf
MODULE_DEPS += lib/fs
MODULE_DEPS += lib/libcpp
MODULE_DEPS += lib/lktl

# the lkuser console command, under WITH_LIB_CONSOLE
MODULE_WEAK_DEPS += lib/console

include make/module.mk
