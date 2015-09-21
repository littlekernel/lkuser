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
	$(LOCAL_DIR)/hello.c

# most of below can go in a shared .mk
APP_CSRCS := $(filter %.c,$(APP_SRCS))
APP_CPPSRCS := $(filter %.cpp,$(APP_SRCS))
APP_ASMSRCS := $(filter %.S,$(APP_SRCS))

#$(warning APP_CSRCS = $(APP_CSRCS))
#$(warning APP_CPPSRCS = $(APP_CPPSRCS))
#$(warning APP_ASMSRCS = $(APP_ASMSRCS))

APP_COBJS := $(call TOBUILDDIR,$(patsubst %.c,%.o,$(APP_CSRCS)))
APP_CPPOBJS := $(call TOBUILDDIR,$(patsubst %.cpp,%.o,$(APP_CPPSRCS)))
APP_ASMOBJS := $(call TOBUILDDIR,$(patsubst %.S,%.o,$(APP_ASMSRCS)))

#$(warning APP_COBJS = $(APP_COBJS))
#$(warning APP_CPPOBJS = $(APP_CPPOBJS))
#$(warning APP_ASMOBJS = $(APP_ASMOBJS))

APP_OBJS := $(APP_COBJS) $(APP_CPPOBJS) $(APP_ASMOBJS) $(APP_ARM_COBJS) $(APP_ARM_CPPOBJS) $(APP_ARM_ASMOBJS)

$(APP_COBJS): $(BUILDDIR)/%.o: %.c $(LIBC)
	@$(MKDIR)
	@echo compiling $<
	$(NOECHO)$(ARCH_CC) $(GLOBAL_OPTFLAGS) $(APP_OPTFLAGS) $(GLOBAL_COMPILEFLAGS) $(ARCH_COMPILEFLAGS) $(APP_COMPILEFLAGS) $(GLOBAL_CFLAGS) $(ARCH_CFLAGS) $(APP_CFLAGS) $(THUMBCFLAGS) $(GLOBAL_INCLUDES) $(APP_INCLUDES) -c $< -MD -MP -MT $@ -MF $(@:%o=%d) -o $@

$(APP) $(APP).lst: $(APP_OBJS) $(ARCH_LINKER_SCRIPT) $(GLOBAL_LIBS)
	@$(MKDIR)
	@echo linking $@
	$(NOECHO)$(ARCH_LD) $(GLOBAL_LDFLAGS) -T $(ARCH_LINKER_SCRIPT) $(ARCH_LDFLAGS) $(APP_OBJS) $(EXTRA_OBJS) $(GLOBAL_LIBS) $(LIBGCC) -o $@
	@echo generating $@.strip
	$(NOECHO)$(ARCH_STRIP) $@ -o $@.strip
	@echo generating $@.lst
	$(NOECHO)$(ARCH_OBJDUMP) -d -Mreg-names-raw $@ > $@.lst
	@echo generating $@.dump
	$(NOECHO)$(ARCH_OBJDUMP) -x $@ > $@.dump

# add ourself to the build list
APPS += $(APP)
