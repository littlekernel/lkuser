LOCAL_DIR := $(GET_LOCAL_DIR)

APP_NAME := boottest
APP_BUILDDIR := $(call TOBUILDDIR, $(LOCAL_DIR))
APP := $(APP_BUILDDIR)/$(APP_NAME)

APP_LIBS := $(call TOBUILDDIR, lib/lku/lku.a)

APP_CFLAGS :=
APP_SRCS := $(LOCAL_DIR)/boottest.c

include make/app.mk
