LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

GLOBAL_INCLUDES += $(LOCAL_DIR)/include

MODULE_SRCS += $(LOCAL_DIR)/copy.cpp
MODULE_SRCS += $(LOCAL_DIR)/object.cpp
MODULE_SRCS += $(LOCAL_DIR)/proc.cpp
MODULE_SRCS += $(LOCAL_DIR)/syscalls.cpp
MODULE_SRCS += $(LOCAL_DIR)/thread.cpp
MODULE_SRCS += $(LOCAL_DIR)/user.cpp

MODULE_DEPS += lib/elf
MODULE_DEPS += lib/fs
MODULE_DEPS += lib/libcpp
MODULE_DEPS += lib/lktl

include make/module.mk
