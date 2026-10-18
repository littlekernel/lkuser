LOCAL_DIR := $(GET_LOCAL_DIR)

APP_NAME := rawtest
APP_BUILDDIR := $(call TOBUILDDIR, $(LOCAL_DIR))
APP := $(APP_BUILDDIR)/$(APP_NAME)

APP_LIBS := $(call TOBUILDDIR, lib/lku/lku.a)

# no libc: rawtest defines _start_c itself, so newlib's start-up and everything it pulls in
# stays out of the link
APP_CFLAGS := -ffreestanding
APP_INCLUDES := -Iapps/include
APP_SRCS := $(LOCAL_DIR)/rawtest.c

include make/app.mk
