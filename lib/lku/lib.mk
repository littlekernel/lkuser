LOCAL_DIR := $(GET_LOCAL_DIR)
#$(warning LOCAL_DIR $(LOCAL_DIR))

LIB_NAME := lku
LIB_BUILDDIR := $(call TOBUILDDIR, $(LOCAL_DIR))
LIB := $(LIB_BUILDDIR)/$(LIB_NAME).a

#$(warning LIB_NAME = $(LIB_NAME))
#$(warning LIB_BUILDDIR = $(LIB_BUILDDIR))
#$(warning LIB = $(LIB))

LIB_CFLAGS :=
LIB_SRCS := $(LOCAL_DIR)/liblk.c

include make/lib.mk
