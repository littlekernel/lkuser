LOCAL_DIR := $(GET_LOCAL_DIR)

APP_NAME := posixtest
APP_BUILDDIR := $(call TOBUILDDIR, $(LOCAL_DIR))
APP := $(APP_BUILDDIR)/$(APP_NAME)

APP_LIBS := $(call TOBUILDDIR, lib/lku/lku.a)

APP_CFLAGS :=
APP_SRCS := $(LOCAL_DIR)/posixtest.c

# the file the test reads; posixtest.c holds the same text
FS_LIST += $(LOCAL_DIR)/posixtest.txt:data/posixtest.txt

include make/app.mk
