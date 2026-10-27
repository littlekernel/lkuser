LOCAL_DIR := $(GET_LOCAL_DIR)

APP_NAME := init
APP_BUILDDIR := $(call TOBUILDDIR, $(LOCAL_DIR))
APP := $(APP_BUILDDIR)/$(APP_NAME)

APP_LIBS := $(call TOBUILDDIR, lib/lku/lku.a)

APP_CFLAGS := -ffreestanding
APP_INCLUDES := -Iapps/include
APP_SRCS := $(LOCAL_DIR)/init.c

include make/app.mk
