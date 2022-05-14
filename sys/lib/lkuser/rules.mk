LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

GLOBAL_INCLUDES += $(LOCAL_DIR)/include

MODULE_SRCS += $(LOCAL_DIR)/user.cpp
MODULE_SRCS += $(LOCAL_DIR)/syscalls.cpp

MODULE_DEPS += lib/bio
MODULE_DEPS += lib/elf
MODULE_DEPS += lib/fs

include make/module.mk
