LOCAL_DIR := $(GET_LOCAL_DIR)
#$(warning LOCAL_DIR $(LOCAL_DIR))

APP_NAME := hello
APP_BUILDDIR := $(call TOBUILDDIR, $(LOCAL_DIR))
APP := $(APP_BUILDDIR)/$(APP_NAME)

#$(warning APP_NAME = $(APP_NAME))
#$(warning APP_BUILDDIR = $(APP_BUILDDIR))
#$(warning APP = $(APP))

APP_CFLAGS :=
APP_SRCS := \
	$(LOCAL_DIR)/hello.c \
	$(LOCAL_DIR)/liblk.c

include make/app.mk
