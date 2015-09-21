LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

GLOBAL_INCLUDES += $(LOCAL_DIR)/include

MODULE_SRCS := \
    $(LOCAL_DIR)/user.c \
    $(LOCAL_DIR)/syscalls.c

MODULE_DEPS := \
    lib/elf \
    lib/bio

include make/module.mk
