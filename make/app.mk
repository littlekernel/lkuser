# shared make file for different applications

#$(warning APP_NAME = $(APP_NAME))
#$(warning APP_BUILDDIR = $(APP_BUILDDIR))
#$(warning APP = $(APP))
#$(warning APP_SRCS = $(APP_SRCS))
#$(warning APP_CFLAGS = $(APP_CFLAGS))
#$(warning APP_LIBS = $(APP_LIBS))

_APP_CSRCS := $(filter %.c,$(APP_SRCS))
_APP_CPPSRCS := $(filter %.cpp,$(APP_SRCS))
_APP_ASMSRCS := $(filter %.S,$(APP_SRCS))

#$(warning _APP_CSRCS = $(_APP_CSRCS))
#$(warning _APP_CPPSRCS = $(_APP_CPPSRCS))
#$(warning _APP_ASMSRCS = $(_APP_ASMSRCS))

_APP_COBJS := $(call TOBUILDDIR,$(patsubst %.c,%.o,$(_APP_CSRCS)))
_APP_CPPOBJS := $(call TOBUILDDIR,$(patsubst %.cpp,%.o,$(_APP_CPPSRCS)))
_APP_ASMOBJS := $(call TOBUILDDIR,$(patsubst %.S,%.o,$(_APP_ASMSRCS)))

#$(warning _APP_COBJS = $(_APP_COBJS))
#$(warning _APP_CPPOBJS = $(_APP_CPPOBJS))
#$(warning _APP_ASMOBJS = $(_APP_ASMOBJS))

_APP_OBJS := $(_APP_COBJS) $(_APP_CPPOBJS) $(_APP_ASMOBJS) $(_APP_ARM_COBJS) $(_APP_ARM_CPPOBJS) $(_APP_ARM_ASMOBJS)

#$(warning _APP_OBJS = $(_APP_OBJS))

# make sure libc is built (and headers are generated) before compiling anything
$(_APP_OBJS): $(LIBC)

$(_APP_COBJS): $(BUILDDIR)/%.o: %.c
	@$(MKDIR)
	@echo compiling $<
	$(NOECHO)$(ARCH_CC) $(GLOBAL_OPTFLAGS) $(APP_OPTFLAGS) $(GLOBAL_COMPILEFLAGS) $(ARCH_COMPILEFLAGS) $(APP_COMPILEFLAGS) $(GLOBAL_CFLAGS) $(ARCH_CFLAGS) $(APP_CFLAGS) $(THUMBCFLAGS) $(GLOBAL_INCLUDES) $(APP_INCLUDES) -c $< -MD -MP -MT $@ -MF $(@:%o=%d) -o $@

$(APP): _APP_OBJS := $(_APP_OBJS)
$(APP): APP_LIBS := $(APP_LIBS)
$(APP): $(_APP_OBJS) $(ARCH_LINKER_SCRIPT) $(APP_LIBS) $(GLOBAL_LIBS)
	@$(MKDIR)
	@echo linking $@
	$(NOECHO)$(ARCH_LD) $(GLOBAL_LDFLAGS) $(ARCH_LDFLAGS) $(CRT0) $(_APP_OBJS) $(EXTRA_OBJS) $(APP_LIBS) $(GLOBAL_LIBS) $(LIBGCC) -o $@

$(APP).lst: $(APP)
	@$(MKDIR)
	@echo generating $@
	$(NOECHO)$(ARCH_OBJDUMP) -C -d -r $< > $@

$(APP).dump: $(APP)
	@$(MKDIR)
	@echo generating $@
	$(NOECHO)$(ARCH_OBJDUMP) -C -x $< > $@

$(APP).strip: $(APP)
	@$(MKDIR)
	@echo generating $@
	$(NOECHO)$(ARCH_STRIP) -d $< -o $@

# add ourself to the build list
APPS += $(APP)
APPS_EXTRADEPS += $(APP).lst $(APP).dump $(APP).strip

# add ourself to the list of files to add to the target fs
FS_LIST += $(APP).strip:bin/$(notdir $(APP))

# empty out various local variables
_APP_CSRCS :=
_APP_CPPSRCS :=
_APP_ASMSRCS :=
_APP_COBJS :=
_APP_CPPOBJS :=
_APP_ASMOBJS :=
_APP_OBJS :=
APP_SRCS :=
APP_OPTFLAGS :=
APP_CFLAGS :=
APP_COMPILEFLAGS :=
APP_INCLUDES :=
APP_LIBS :=
APP :=

