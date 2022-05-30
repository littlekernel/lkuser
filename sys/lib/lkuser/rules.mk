LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

GLOBAL_INCLUDES += $(LOCAL_DIR)/include

MODULE_SRCS += $(LOCAL_DIR)/fs.cpp
MODULE_SRCS += $(LOCAL_DIR)/handle.cpp
MODULE_SRCS += $(LOCAL_DIR)/proc.cpp
MODULE_SRCS += $(LOCAL_DIR)/syscalls.cpp
MODULE_SRCS += $(LOCAL_DIR)/thread.cpp
MODULE_SRCS += $(LOCAL_DIR)/user.cpp

MODULE_DEPS += lib/bio
MODULE_DEPS += lib/elf
MODULE_DEPS += lib/fs

MODULE_COMPILEFLAGS += -Wno-invalid-offsetof

include make/module.mk
