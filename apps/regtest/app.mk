LOCAL_DIR := $(GET_LOCAL_DIR)

APP_NAME := regtest
APP_BUILDDIR := $(call TOBUILDDIR, $(LOCAL_DIR))
APP := $(APP_BUILDDIR)/$(APP_NAME)

APP_LIBS := $(call TOBUILDDIR, lib/lku/lku.a)

# no libc: it starts threads, and newlib is not safe with threads
APP_CFLAGS := -ffreestanding
APP_INCLUDES := -Iapps/include
APP_SRCS := $(LOCAL_DIR)/regtest.c

include make/app.mk
