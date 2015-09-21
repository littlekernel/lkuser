LOCAL_DIR := $(GET_LOCAL_DIR)
$(warning LOCAL_DIR $(LOCAL_DIR))

APP_NAME := hello
APP_BUILDDIR := $(call TOBUILDDIR, $(LOCAL_DIR))
APP := $(APP_BUILDDIR)/$(APP_NAME)

$(warning APP_NAME = $(APP_NAME))
$(warning APP_BUILDDIR = $(APP_BUILDDIR))
$(warning APP = $(APP))

APP_CFLAGS :=
APP_SRCS := \
	$(LOCAL_DIR)/hello.c

APP_CSRCS := $(filter %.c,$(APP_SRCS))
APP_CPPSRCS := $(filter %.cpp,$(APP_SRCS))
APP_ASMSRCS := $(filter %.S,$(APP_SRCS))

$(warning APP_CSRCS = $(APP_CSRCS))
$(warning APP_CPPSRCS = $(APP_CPPSRCS))
$(warning APP_ASMSRCS = $(APP_ASMSRCS))

APP_COBJS := $(call TOBUILDDIR,$(patsubst %.c,%.o,$(APP_CSRCS)))
APP_CPPOBJS := $(call TOBUILDDIR,$(patsubst %.cpp,%.o,$(APP_CPPSRCS)))
APP_ASMOBJS := $(call TOBUILDDIR,$(patsubst %.S,%.o,$(APP_ASMSRCS)))

$(warning APP_COBJS = $(APP_COBJS))
$(warning APP_CPPOBJS = $(APP_CPPOBJS))
$(warning APP_ASMOBJS = $(APP_ASMOBJS))

APP_OBJS := $(APP_COBJS) $(APP_CPPOBJS) $(APP_ASMOBJS) $(APP_ARM_COBJS) $(APP_ARM_CPPOBJS) $(APP_ARM_ASMOBJS)

$(APP_COBJS): $(BUILDDIR)/%.o: %.c
	@$(MKDIR)
	@echo compiling $<
	$(NOECHO)$(ARCH_CC) $(GLOBAL_OPTFLAGS) $(APP_OPTFLAGS) $(GLOBAL_COMPILEFLAGS) $(ARCH_COMPILEFLAGS) $(APP_COMPILEFLAGS) $(GLOBAL_CFLAGS) $(ARCH_CFLAGS) $(APP_CFLAGS) $(THUMBCFLAGS) $(GLOBAL_INCLUDES) $(APP_INCLUDES) -c $< -MD -MP -MT $@ -MF $(@:%o=%d) -o $@

$(APP): $(APP_OBJS) $(LINKER_SCRIPT)
	@$(MKDIR)
	@echo linking $@
	$(NOECHO)$(ARCH_LD) $(GLOBAL_LDFLAGS) -T $(LINKER_SCRIPT) $(APP_OBJS) $(EXTRA_OBJS) $(NEWLIB_LIBC) $(LIBGCC) -o $@


# add ourself to the build list
APPS += $(APP)
